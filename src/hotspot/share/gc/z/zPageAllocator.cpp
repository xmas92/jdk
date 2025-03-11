/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "gc/shared/gcLogPrecious.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zAllocationFlags.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zCommitter.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zFuture.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zGranuleMap.inline.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zMemory.hpp"
#include "gc/z/zMemory.inline.hpp"
#include "gc/z/zNUMA.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageAllocator.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zPhysicalMemoryManager.hpp"
#include "gc/z/zSafeDelete.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncommitter.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "gc/z/zValue.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zWorkers.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"
#include "runtime/globals.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <cmath>
#include <cstddef>

static const ZStatCounter       ZCounterMutatorAllocationRate("Memory", "Allocation Rate", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterDefragment("Memory", "Defragment", ZStatUnitOpsPerSecond);
static const ZStatCriticalPhase ZCriticalPhaseAllocationStall("Allocation Stall");

static void sort_zbacking_index_ptrs(void* at, size_t size) {
  qsort(at, size, sizeof(zbacking_index),
    [](const void* a, const void* b) -> int {
      return *static_cast<const zbacking_index*>(a) < *static_cast<const zbacking_index*>(b) ? -1 : 1;
    });
}

class ZSegmentStash {
private:
  ZGranuleMap<zbacking_index>* const _physical_mappings;
  ZArray<zbacking_index>             _stash;

  void sort_stashed_segments() {
    sort_zbacking_index_ptrs(_stash.adr_at(0), (size_t)_stash.length());
  }

  void copy_to_stash(int index, const ZMemoryRange& vmem) {
    zbacking_index* const dest = _stash.adr_at(index);
    const zbacking_index* const src = _physical_mappings->get_addr(vmem.start());
    const size_t num_granules = vmem.size_in_granules();

    // Copy to stash
    ZUtils::copy_disjoint(dest, src, num_granules);
  }

  void copy_from_stash(int index, const ZMemoryRange& vmem) {
    zbacking_index* const dest = _physical_mappings->get_addr(vmem.start());
    const zbacking_index* const src = _stash.adr_at(index);
    const size_t num_granules = vmem.size_in_granules();

    // Copy from stash
    ZUtils::copy_disjoint(dest, src, num_granules);
  }

public:
  ZSegmentStash(ZGranuleMap<zbacking_index>* physical_mappings, int num_granules)
    : _physical_mappings(physical_mappings),
      _stash(num_granules, num_granules, zbacking_index::zero) {}

  void stash(const ZMemoryRange& vmem) {
    copy_to_stash(0, vmem);
    sort_stashed_segments();
  }

  void stash(ZArray<ZMemoryRange>* mappings) {
    int stash_index = 0;
    ZArrayIterator<ZMemoryRange> iter(mappings);
    for (ZMemoryRange vmem; iter.next(&vmem);) {
      const size_t num_granules = vmem.size_in_granules();
      copy_to_stash(stash_index, vmem);
      stash_index += (int)num_granules;
    }
    sort_stashed_segments();
  }

  void pop(ZArray<ZMemoryRange>* mappings, size_t num_mappings) {
    int stash_index = 0;
    const int pop_start_index = mappings->length() - (int)num_mappings;
    ZArrayIterator<ZMemoryRange> iter(mappings, pop_start_index);
    for (ZMemoryRange vmem; iter.next(&vmem);) {
      const size_t num_granules = vmem.size_in_granules();
      const size_t granules_left = _stash.length() - stash_index;

      // If we run out of segments in the stash, we finish early
      if (num_granules >= granules_left) {
        const ZMemoryRange truncated_vmem(vmem.start(), granules_left * ZGranuleSize);
        copy_from_stash(stash_index, truncated_vmem);
        return;
      }

      copy_from_stash(stash_index, vmem);
      stash_index += (int)num_granules;
    }
  };
};

class ZMemoryAllocation;

class ZMemoryAllocationData : public StackObj {
private:
  ZArray<ZMemoryRange>      _claimed_mappings;
  ZArray<ZMemoryRange>*     _multi_numa_claimed_mappings;
  ZArray<ZMemoryAllocation> _multi_numa_allocations;
  bool                      _is_multi_numa_allocation;

  static int get_multi_numa_count() {
    // We may have two allocations per NUMA node
    return ZNUMA::count() * 2;
  }

public:
  ZMemoryAllocationData()
    : _claimed_mappings(1),
      _multi_numa_claimed_mappings(nullptr),
      _multi_numa_allocations(0),
      _is_multi_numa_allocation(false) {}

  ZArray<ZMemoryRange>* claimed_mappings() {
    return &_claimed_mappings;
  }

  bool is_multi_numa_allocation() const {
    return _is_multi_numa_allocation;
  }

  ZArray<ZMemoryAllocation>* multi_numa_allocations() {
    return &_multi_numa_allocations;
  }

  const ZArray<ZMemoryAllocation>* multi_numa_allocations() const {
    return &_multi_numa_allocations;
  }

  ~ZMemoryAllocationData();

  void reset();

  void set_multi_numa_allocation();

  ZMemoryAllocation* get_next_multi_numa_allocation(size_t size);
  void remove_last_multi_numa_allocation();
};

class ZMemoryAllocation {
private:
  size_t                _size;
  ZArray<ZMemoryRange>* _claimed_mappings;
  size_t                _current_max_capacity;
  size_t                _harvested;
  size_t                _committed;
  int                   _numa_id;
  bool                  _commit_failed;
  bool                  _commit_only;
  bool                  _cache_only;

public:
  // All fields are mutable and we have a default empty constructor to enable
  // the usage of this class in a ZArray
  ZMemoryAllocation() = default;
  ZMemoryAllocation(ZArray<ZMemoryRange>* claimed_mappings, size_t size)
    : _size(size),
      _claimed_mappings(claimed_mappings),
      _current_max_capacity(0),
      _harvested(0),
      _committed(0),
      _numa_id(-1),
      _commit_failed(false),
      _commit_only(false),
      _cache_only(false) {}

  void reset() {
    _current_max_capacity = 0;
    _harvested = 0;
    _committed = 0;
    _numa_id = -1;
    _commit_failed = false;
    _commit_only = false;
    _cache_only = false;
  }

  size_t size() const {
    return _size;
  }

  size_t current_max_capacity() const {
    return _current_max_capacity;
  }

  void set_current_max_capacity(size_t current_max_capacity) {
    _current_max_capacity = current_max_capacity;
  }

  size_t harvested() const {
    return _harvested;
  }

  void set_harvested(size_t harvested) {
    _harvested = harvested;
  }

  size_t committed() const {
    return _committed;
  }

  void set_committed(size_t committed) {
    _committed = committed;
  }

  int numa_id() const {
    return _numa_id;
  }

  void set_numa_id(int numa_id) {
    _numa_id = numa_id;
  }

  bool commit_failed() const {
    return _commit_failed;
  }

  void set_commit_failed() {
    _commit_failed = true;
  }

  bool commit_only() const {
    return _commit_only;
  }

  void set_commit_only() {
    _commit_only = true;
  }

  bool cache_only() const {
    return _cache_only;
  }

  void set_cache_only() {
    _cache_only = true;
  }

  ZArray<ZMemoryRange>* claimed_mappings() {
    return _claimed_mappings;
  }
};

class ZPageAllocation : public StackObj {
  friend class ZList<ZPageAllocation>;

private:
  const ZPageType            _type;
  const size_t               _size;
  const ZAllocationFlags     _flags;
  const uint32_t             _young_seqnum;
  const uint32_t             _old_seqnum;
  int const                  _initiating_numa_id;
  ZMemoryAllocationData      _allocation_data;
  ZMemoryAllocation          _allocation;
  ZListNode<ZPageAllocation> _node;
  ZFuture<bool>              _stall_result;
  bool                       _commit_failed;
  bool                       _is_retrying;
  bool                       _is_stalling;
  bool                       _retry_with_stall;

public:
  ZPageAllocation(ZPageType type, size_t size, ZAllocationFlags flags)
    : _type(type),
      _size(size),
      _flags(flags),
      _young_seqnum(ZGeneration::young()->seqnum()),
      _old_seqnum(ZGeneration::old()->seqnum()),
      _initiating_numa_id(ZNUMA::id()),
      _allocation_data(),
      _allocation(_allocation_data.claimed_mappings(), size),
      _node(),
      _stall_result(),
      _is_retrying(false),
      _is_stalling(false),
      _retry_with_stall(false) {}

  void reset_for_retry() {
    assert(!_is_retrying, "only retry once");
    assert(!_allocation.commit_only(), "unexpected allocation");

    // Reset the underlying allocation data
    _allocation.reset();
    _allocation_data.reset();

    // When retrying a page allocation, only take from the cache
    _is_retrying = true;
    _allocation.set_cache_only();
  }

  void reset_for_retry_with_stall() {
    assert(!_retry_with_stall, "only retry with stall once");
    assert(!_is_stalling, "do not retry with stall if already stalling");
    assert(!_allocation.commit_only(), "unexpected allocation");

    // Reset the underlying allocation data
    _allocation.reset();
    _allocation_data.reset();

    // Retry allocation by stalling
    _is_retrying = true;
    _retry_with_stall = true;
  }

  void reset_for_stall() {
    assert(!_is_stalling, "only stall once");
    assert(!_allocation.commit_only(), "unexpected allocation");

    // Reset the underlying allocation data
    _allocation.reset();
    _allocation_data.reset();

    // Set the allocation as stalling
    _is_stalling = true;

    // Allow the stalling allocation to retry
    _is_retrying = false;
  }

  ZPageType type() const {
    return _type;
  }

  size_t size() const {
    return _size;
  }

  ZAllocationFlags flags() const {
    return _flags;
  }

  uint32_t young_seqnum() const {
    return _young_seqnum;
  }

  uint32_t old_seqnum() const {
    return _old_seqnum;
  }

  int initiating_numa_id() const {
    return _initiating_numa_id;
  }

  ZMemoryAllocation* memory_allocation() {
    return &_allocation;
  }

  bool is_multi_numa_allocation() const {
    return _allocation_data.is_multi_numa_allocation();
  }

  void set_multi_numa_allocation() {
    _allocation_data.set_multi_numa_allocation();
  }

  ZMemoryAllocation* get_next_multi_numa_allocation(size_t size) {
    return _allocation_data.get_next_multi_numa_allocation(size);
  }

  void remove_last_multi_numa_allocation() {
    return _allocation_data.remove_last_multi_numa_allocation();
  }

  ZArray<ZMemoryAllocation>* multi_numa_allocations() {
    return _allocation_data.multi_numa_allocations();
  }

  const ZArray<ZMemoryAllocation>* multi_numa_allocations() const {
    return _allocation_data.multi_numa_allocations();
  }

  ZMemoryRange pop_final_mapping() {
    ZMemoryAllocation* const allocation = memory_allocation();
    ZArray<ZMemoryRange>* const mappings = allocation->claimed_mappings();

    assert(mappings->length() == 1, "must contain one mapping");
    assert(mappings->first().size() == _size, "must be complete");

    return mappings->pop();
  }

  bool wait() {
    return _stall_result.get();
  }

  void satisfy(bool result) {
    _stall_result.set(result);
  }

  bool gc_relocation() const {
    return _flags.gc_relocation();
  }

  bool is_retrying() const {
    return _is_retrying;
  }

  bool is_stalling() const {
    return _is_stalling;
  }

  bool retry_with_stall() const {
    return _retry_with_stall;
  }
};

ZMemoryAllocationData::~ZMemoryAllocationData() {
  if (_multi_numa_claimed_mappings != nullptr) {
    const int length = get_multi_numa_count();
    ZArrayMutableIterator<ZArray<ZMemoryRange>> iter(_multi_numa_claimed_mappings, length);
    for (ZArray<ZMemoryRange>* claimed_mappings; iter.next_addr(&claimed_mappings);) {
      claimed_mappings->~ZArray<ZMemoryRange>();
    }
    FREE_C_HEAP_ARRAY(ZArray<ZMemoryRange>, _multi_numa_claimed_mappings);
  }
}

void ZMemoryAllocationData::reset() {
  // Clear mappings
  _claimed_mappings.clear();

  // Clear multi numa allocations and mappings, but do not deallocate, it will
  // more than likely be a multi numa allocation the next time around
  _multi_numa_allocations.clear();
  if (_multi_numa_claimed_mappings != nullptr) {
    const int length = get_multi_numa_count();
    ZArrayMutableIterator<ZArray<ZMemoryRange>> iter(_multi_numa_claimed_mappings, length);
    for (ZArray<ZMemoryRange>* claimed_mappings; iter.next_addr(&claimed_mappings);) {
      claimed_mappings->clear();
    }
  }
  _is_multi_numa_allocation = false;
}

void ZMemoryAllocationData::set_multi_numa_allocation() {
  _is_multi_numa_allocation = true;

  // Allocate storage for multi numa allocations and mappings
  const int length = get_multi_numa_count();
  _multi_numa_allocations.reserve(length);

  if (_multi_numa_claimed_mappings == nullptr) {
    // ZArray<ZMemoryRange> _multi_numa_claimed_mappings[length];
    void* const multi_numa_claimed_mappings_memory = NEW_C_HEAP_ARRAY(ZArray<ZMemoryRange>, length, mtGC);
    _multi_numa_claimed_mappings = ::new (multi_numa_claimed_mappings_memory) ZArray<ZMemoryRange>[length];
  }
}

ZMemoryAllocation* ZMemoryAllocationData::get_next_multi_numa_allocation(size_t size) {
  assert(_is_multi_numa_allocation, "not flipped to multi numa allocation");
  const int next_index = _multi_numa_allocations.length();

  assert(next_index < get_multi_numa_count(), "to many partial allocations");

  ZArray<ZMemoryRange>* const claimed_mappings = &_multi_numa_claimed_mappings[next_index];
  _multi_numa_allocations.push(ZMemoryAllocation(claimed_mappings, size));
  return &_multi_numa_allocations.last();
}

void ZMemoryAllocationData::remove_last_multi_numa_allocation() {
  _multi_numa_allocations.pop();
}

const ZVirtualMemoryManager& ZCacheState::virtual_memory_manager() const {
  return _page_allocator->_virtual;
}

ZVirtualMemoryManager& ZCacheState::virtual_memory_manager() {
  return _page_allocator->_virtual;
}

const ZPhysicalMemoryManager& ZCacheState::physical_memory_manager() const {
  return _page_allocator->_physical;
}

ZPhysicalMemoryManager& ZCacheState::physical_memory_manager() {
  return _page_allocator->_physical;
}

const ZGranuleMap<zbacking_index>& ZCacheState::physical_mappings() const {
  return _page_allocator->_physical_mappings;
}

ZGranuleMap<zbacking_index>& ZCacheState::physical_mappings() {
  return _page_allocator->_physical_mappings;
}

const zbacking_index* ZCacheState::physical_mappings_addr(const ZMemoryRange& vmem) const {
  const ZGranuleMap<zbacking_index>& mappings = physical_mappings();
  return mappings.get_addr(vmem.start());
}

zbacking_index* ZCacheState::physical_mappings_addr(const ZMemoryRange& vmem) {
  ZGranuleMap<zbacking_index>& mappings = physical_mappings();
  return mappings.get_addr(vmem.start());
}

ZLock* ZCacheState::lock() const {
  return &_page_allocator->_lock;
}

ZCacheState::ZCacheState(uint32_t numa_id, ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _cache(),
    _uncommitter(numa_id, page_allocator),
    _committer(numa_id, page_allocator),
    _min_capacity(ZNUMA::calculate_share(numa_id, page_allocator->min_capacity())),
    _initial_capacity(ZNUMA::calculate_share(numa_id, page_allocator->initial_capacity())),
    _static_max_capacity(ZNUMA::calculate_share(numa_id, page_allocator->static_max_capacity())),
    _committed(0),
    _observed_max_committed(0),
    _heuristic_max_capacity(_initial_capacity),
    _capacity(0),
    _claimed(0),
    _used(0),
    _used_generations{0,0},
    _collection_stats{{0, 0},{0, 0}},
    _last_commit(0.0),
    _last_uncommit(0.0),
    _to_uncommit(0),
    _numa_id(numa_id) {}

void ZCacheState::set_heuristic_max_capacity(size_t heuristic_max_capacity) {
  Atomic::store(&_heuristic_max_capacity, heuristic_max_capacity);
}

size_t ZCacheState::dynamic_max_capacity() const {
  if (ZAdaptiveHeap::explicit_max_capacity()) {
    return _static_max_capacity;
  }

  const size_t max = align_down(size_t(os::physical_memory() * (1.0 - ZMemoryCriticalThreshold)), ZGranuleSize);
  const size_t max_share = ZNUMA::calculate_share(_numa_id, max);
  return MAX2(max_share, _min_capacity);
}

size_t ZCacheState::current_max_capacity(ZPageAllocation* allocation) const {
  const size_t current_max_capacity = ZCacheState::current_max_capacity();

  if (allocation->is_stalling()) {
    // Stalling allocations uses at most the observed max capacity
    const size_t observed_max_capacity = Atomic::load(&_observed_max_committed);

    return MIN2(current_max_capacity, observed_max_capacity);
  }

  return current_max_capacity;
}

size_t ZCacheState::current_max_capacity() const {
  if (ZAdaptiveHeap::explicit_max_capacity()) {
    return _static_max_capacity;
  }

  // Calculate current max capacity based on machine usage
  return ZAdaptiveHeap::current_max_capacity(_capacity, dynamic_max_capacity());
}

static size_t calculate_heuristic_max_capacity(size_t soft_max_capacity, size_t heuristic_max_capacity, size_t curr_max_capacity) {
  const size_t lowest_soft_capacity = soft_max_capacity == 0 ? heuristic_max_capacity
                                                             : MIN2(soft_max_capacity, heuristic_max_capacity);
  return MIN2(lowest_soft_capacity, curr_max_capacity);
}

size_t ZCacheState::heuristic_max_capacity() const {
  // Note that SoftMaxHeapSize is a manageable flag
  const size_t soft_max_capacity = ZNUMA::calculate_share(_numa_id, align_down(Atomic::load(&SoftMaxHeapSize), ZGranuleSize));
  const size_t heuristic_max_capacity = Atomic::load(&_heuristic_max_capacity);
  const size_t curr_max_capacity = current_max_capacity();
  return calculate_heuristic_max_capacity(soft_max_capacity, heuristic_max_capacity, curr_max_capacity);
}

size_t ZCacheState::capacity() const {
  return Atomic::load(&_capacity);
}

size_t ZCacheState::available(size_t current_max_capacity) const {
  const size_t unavailable = _used + _claimed;

  if (unavailable > current_max_capacity) {
    return 0;
  }

  return current_max_capacity - unavailable;
}

size_t ZCacheState::available(ZMemoryAllocation* allocation) const {
  assert(_capacity == _used + _claimed + _cache.size(), "Should be consistent"
         " _capacity: %zx _used: %zx _claimed: %zx _cache.size(): %zx",
         _capacity, _used, _claimed, _cache.size());
  const size_t current_max_capacity = allocation->current_max_capacity();
  const size_t unavailable = _used + _claimed;

  if (unavailable > current_max_capacity) {
    return 0;
  }

  const size_t cached = _cache.size();
  const size_t available_with_cache = current_max_capacity - unavailable;

  if (allocation->commit_only()) {
    // Commit only does not use the cache

    if (available_with_cache < cached) {
      return 0;
    }

    return available_with_cache - cached;
  }

  if (allocation->cache_only()) {
    // Only the cache is used for retries

    return MIN2(cached, available_with_cache);
  }

  return available_with_cache;
}

bool ZCacheState::is_allocation_allowed(ZMemoryAllocation* allocation) const {
  const size_t size = allocation->size();
  const size_t available = ZCacheState::available(allocation);

  return size <= available;
}

size_t ZCacheState::increase_capacity(ZMemoryAllocation* allocation) {
  const size_t size = allocation->size();
  const size_t current_max_capacity = allocation->current_max_capacity();
  const size_t capacity = _capacity;

  if (current_max_capacity < capacity) {
    return 0;
  }

  const size_t increased = MIN2(size, current_max_capacity - capacity);

  if (increased > 0) {
    // Update atomically since we have concurrent readers
    Atomic::add(&_capacity, increased);

    _last_commit = os::elapsedTime();
    _last_uncommit = 0;
    _cache.reset_min();
  }

  return increased;
}

void ZCacheState::decrease_capacity(size_t size) {
  // Update state atomically since we have concurrent readers
  Atomic::sub(&_capacity, size);
}

void ZCacheState::increase_committed(size_t increment, bool commit_failed) {
  _committed += increment;

  if (commit_failed) {
    // Commit failed, reset max
    _observed_max_committed = _committed;
  } else if (_committed < _observed_max_committed) {
    // Commit successful update max
    _observed_max_committed = _committed;
  }
}

void ZCacheState::decrease_committed(size_t decrement) {
  _committed -= decrement;
}

void ZCacheState::increase_used(size_t size) {
  // We don't track generation usage here because this page
  // could be allocated by a thread that satisfies a stalling
  // allocation. The stalled thread can wake up and potentially
  // realize that the page alloc should be undone. If the alloc
  // and the undo gets separated by a safepoint, the generation
  // statistics could se a decreasing used value between mark
  // start and mark end.

  // Update atomically since we have concurrent readers
  const size_t used = Atomic::add(&_used, size);

  // Update used high
  for (auto& stats : _collection_stats) {
    if (used > stats._used_high) {
      stats._used_high = used;
    }
  }
}

void ZCacheState::decrease_used(size_t size) {
  // Update atomically since we have concurrent readers
  const size_t used = Atomic::sub(&_used, size);

  // Update used low
  for (auto& stats : _collection_stats) {
    if (used < stats._used_low) {
      stats._used_low = used;
    }
  }
}

void ZCacheState::increase_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::add(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZCacheState::decrease_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::sub(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZCacheState::reset_statistics(ZGenerationId id) {
  _collection_stats[(int)id]._used_high = _used;
  _collection_stats[(int)id]._used_low = _used;
}

void ZCacheState::claim_mapped_or_increase_capacity(ZMemoryAllocation* allocation) {
  assert(is_allocation_allowed(allocation), "must be allowed");

  const size_t size = allocation->size();
  const size_t current_max_capacity = allocation->current_max_capacity();
  ZArray<ZMemoryRange>* mappings = allocation->claimed_mappings();

  // Try to allocate a contiguous mapping.
  ZMemoryRange mapping = _cache.remove_contiguous(size);
  if (!mapping.is_null()) {
    mappings->append(mapping);
    return;
  }

  // If we've failed to allocate a contiguous range from the mapped cache,
  // there is still a possibility that the cache holds enough memory for the
  // allocation dispersed over more than one mapping if the capacity cannot be
  // increased to satisfy the allocation.

  // Try increase capacity
  const bool is_retry = allocation->cache_only();
  const size_t increased = is_retry ? 0 : increase_capacity(allocation);

  // Could not increase capacity enough to satisfy the allocation completely.
  // Try removing multiple mappings from the mapped cache. We only remove if
  // cache has enough remaining to cover the request.
  const size_t remaining = size - increased;
  if (remaining > 0) {
    const size_t removed = _cache.remove_discontiguous(mappings, remaining);
    allocation->set_harvested(removed);
    assert(removed == remaining, "must be %zu != %zu", removed, remaining);
  }
}

bool ZCacheState::claim_physical(ZMemoryAllocation* allocation) {
  if (!is_allocation_allowed(allocation)) {
    // Out of memory
    return false;
  }

  claim_mapped_or_increase_capacity(allocation);

  const size_t size = allocation->size();

  // Updated used statistics
  increase_used(size);

  // Success
  return true;
}

bool ZCacheState::claim_committed(ZMemoryAllocation* allocation) {
  const size_t current_max_capacity = ZCacheState::current_max_capacity();
  allocation->set_current_max_capacity(current_max_capacity);
  allocation->set_commit_only();

  if (!is_allocation_allowed(allocation)) {
    return false;
  }

  const size_t size = allocation->size();

  // Try increase capacity
  const size_t increased = increase_capacity(allocation);
  guarantee(increased == size, "should always succeed");

  // Updated used statistics
  increase_used(size);

  return true;
}

bool ZCacheState::claim_cached(ZMemoryAllocation* allocation) {
  Unimplemented();
}

ZMemoryRange ZCacheState::get_primed_mapping() const {
  const ZMemoryRange vmem = _cache.first();

  assert(!vmem.is_null(), "mapping must exist");
  assert(vmem.size() == _cache.size(), "only one mapping must exist");

  return vmem;
}

ZMappedCache* ZCacheState::cache() {
  return &_cache;
}

int ZCacheState::numa_id() const {
  return _numa_id;
}

template <typename Decider>
size_t ZCacheState::uncommit(ZPageAllocator* page_allocator, size_t limit, Decider decide) {
  ZArray<ZMemoryRange> flushed_mappings;
  size_t flushed = 0;

  {
    // We need to join the suspendible thread set while manipulating capacity and
    // used, to make sure GC safepoints will have a consistent view.
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&page_allocator->_lock);

    if (!decide()) {
      // Decided not to uncommit
      return 0;
    }

    // Never uncommit below min capacity. We flush out and uncommit chunks at
    // a time (~0.8% of the max capacity, but at least one granule and at most
    // 256M), in case demand for memory increases while we are uncommitting.
    const size_t retain = MAX2(_used, _min_capacity / ZNUMA::count());
    const size_t release = _capacity - retain;
    const size_t flush = MIN3(release, limit, _to_uncommit);

    if (flush == 0) {
      // Nothing to flush
      return 0;
    }

    // Flush memory from the mapped cache to uncommit
    flushed = _cache.remove_from_min(&flushed_mappings, flush);
    if (flushed == 0) {
      // Nothing flushed
      return 0;
    }

    // Record flushed memory as claimed and how much we've flushed for this NUMA node
    Atomic::add(&_claimed, flushed);
    _to_uncommit -= flushed;
  }

  // Unmap and uncommit flushed memory
  ZArrayIterator<ZMemoryRange> it(&flushed_mappings);
  for (ZMemoryRange vmem; it.next(&vmem);) {
    page_allocator->unmap_virtual(vmem);
    uncommit_physical(vmem);
    free_physical(vmem);
    page_allocator->free_virtual(vmem);
  }

  {
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(& page_allocator->_lock);

    // Adjust claimed and capacity to reflect the uncommit
    Atomic::sub(&_claimed, flushed);
    decrease_capacity(flushed);
  }

  return flushed;
}

const ZUncommitter& ZCacheState::uncommitter() const {
  return _uncommitter;
}

ZUncommitter& ZCacheState::uncommitter() {
  return _uncommitter;
}

const ZCommitter& ZCacheState::committer() const {
  return _committer;
}

ZCommitter& ZCacheState::committer() {
  return _committer;
}

void ZCacheState::threads_do(ThreadClosure* tc) const {
  tc->do_thread(const_cast<ZUncommitter*>(&_uncommitter));
  tc->do_thread(const_cast<ZCommitter*>(&_committer));
}

void ZCacheState::alloc_physical(const ZMemoryRange& vmem) {
  ZPhysicalMemoryManager& manager = physical_memory_manager();
  zbacking_index* const pmem = physical_mappings_addr(vmem);
  size_t const size = vmem.size();

  // Alloc physical memory
  manager.alloc(pmem, size, _numa_id);
}

void ZCacheState::free_physical(const ZMemoryRange& vmem) {
  ZPhysicalMemoryManager& manager = physical_memory_manager();
  zbacking_index* const pmem = physical_mappings_addr(vmem);
  size_t const size = vmem.size();

  // Free physical memory
  manager.free(pmem, size, _numa_id);
}

size_t ZCacheState::commit_physical(const ZMemoryRange& vmem) {
  ZPhysicalMemoryManager& manager = physical_memory_manager();
  zbacking_index* const pmem = physical_mappings_addr(vmem);
  size_t const size = vmem.size();

  // Commit physical memory
  const size_t committed = manager.commit(pmem, size, _numa_id);

  // Keep track of the committed memory
  {
    ZLocker<ZLock> locker(lock());
    const bool commit_failed = committed != size;
    increase_committed(committed, commit_failed);
  }

  return committed;
}

void ZCacheState::uncommit_physical(const ZMemoryRange& vmem) {
  assert(ZUncommit, "should not uncommit when uncommit is disabled");

  ZPhysicalMemoryManager& manager = physical_memory_manager();
  zbacking_index* const pmem = physical_mappings_addr(vmem);
  size_t const size = vmem.size();

  // Uncommit physical memory
  const size_t uncommitted = manager.uncommit(pmem, size);

  // Keep track of the committed memory
  {
    ZLocker<ZLock> locker(lock());
    decrease_committed(uncommitted);
  }
}

class MultiNUMATracker : CHeapObj<mtGC> {
private:
  struct Element {
    ZMemoryRange _range;
    int _numa_id;
  };

  ZArray<Element> _map;

  MultiNUMATracker(int capacity)
    : _map(capacity) {}

  const ZArray<Element>* map() const {
    return &_map;
  }
public:
  static void install_tracker(const ZPageAllocation* allocation, ZPage* page) {
    if (!allocation->is_multi_numa_allocation()) {
      return;
    }

    const ZArray<ZMemoryAllocation>* const partial_allocations = allocation->multi_numa_allocations();
    MultiNUMATracker* const tracker = new MultiNUMATracker(partial_allocations->length());

    // Each partial allocation is mapped to the virtual memory in order
    ZMemoryRange vmem = page->virtual_memory();
    ZArrayIterator<ZMemoryAllocation> iter(partial_allocations);
    for (const ZMemoryAllocation* partial_allocation; iter.next_addr(&partial_allocation);) {
      // Track each separate mapping's numa node
      const ZMemoryRange partial_vmem = vmem.split_from_front(partial_allocation->size());
      const int numa_id = partial_allocation->numa_id();
      tracker->_map.push({partial_vmem, numa_id});
    }

    // Install the tracker
    page->set_multi_numa_tracker(tracker);
  }

  static void free_and_destroy(ZPageAllocator* allocator, ZPage* page) {
    const int numa_nodes = ZNUMA::count();

    // Extract data and destroy page
    const ZMemoryRange vmem = page->virtual_memory();
    const ZGenerationId id = page->generation_id();
    MultiNUMATracker* const tracker = page->multi_numa_tracker();
    allocator->safe_destroy_page(page);

    // Keep track of to be inserted mappings
    struct PerNUMAData : public CHeapObj<mtGC> {
      ZArray<ZMemoryRange> _mappings{};
      size_t _mapped = 0;
      size_t _uncommitted = 0;
    };
    PerNUMAData* const per_numa_mappings = new PerNUMAData[numa_nodes];

    // Remap memory back to original numa node
    ZArrayIterator<Element> iter(tracker->map());
    for (Element partial_allocation{}; iter.next(&partial_allocation);) {
      ZMemoryRange remaining_vmem = partial_allocation._range;
      const int numa_id = partial_allocation._numa_id;
      PerNUMAData& numa_data = per_numa_mappings[numa_id];
      ZArray<ZMemoryRange>* const numa_memory_mappings = &numa_data._mappings;
      const size_t size = remaining_vmem.size();
      ZCacheState& state = allocator->state_from_numa_id(numa_id);

      // Allocate new virtual address ranges
      const int start_index = numa_memory_mappings->length();
      const size_t allocated = allocator->_virtual.alloc_low_address_many_at_most(remaining_vmem.size(), numa_id, numa_memory_mappings);

      // Remap to the newly allocated virtual address ranges
      size_t mapped = 0;
      ZArrayIterator<ZMemoryRange> iter(numa_memory_mappings, start_index);
      for (ZMemoryRange to_vmem; iter.next(&to_vmem);) {
        ZMemoryRange from_vmem = remaining_vmem.split_from_front(to_vmem.size());

        // Copy physical segments
        allocator->copy_physical_segments(to_vmem.start(), from_vmem);

        // Unmap from_vmem
        allocator->unmap_virtual(from_vmem);

        // Map to_vmem
        allocator->map_virtual_to_physical(to_vmem, numa_id);

        mapped += to_vmem.size();
      }

      assert(allocated == mapped, "must have mapped all allocated");
      assert(size == mapped + remaining_vmem.size(), "must cover whole range");

      if (remaining_vmem.size() != 0) {
        // Failed to get vmem for all memory, unmap, uncommit and free the remaining
        allocator->unmap_virtual(remaining_vmem);
        state.uncommit_physical(remaining_vmem);
        state.free_physical(remaining_vmem);
      }

      // Keep track of the per numa data
      numa_data._mapped += mapped;
      numa_data._uncommitted += remaining_vmem.size();
    }

    // Free the virtual memory
    allocator->free_virtual(vmem);

    {
      ZLocker<ZLock> locker(&allocator->_lock);

      ZPerNUMAIterator<ZCacheState> iter = allocator->state_iterator();
      for (ZCacheState* state; iter.next(&state);) {
        const int numa_id = state->numa_id();
        PerNUMAData& numa_data = per_numa_mappings[numa_id];

        // Reinsert mappings
        ZArrayIterator<ZMemoryRange> iter(&numa_data._mappings);
        for (ZMemoryRange mapping; iter.next(&mapping);) {
          state->cache()->insert(mapping);
        }

        // Update accounting
        state->decrease_used(numa_data._mapped + numa_data._uncommitted);
        state->decrease_used_generation(id, numa_data._mapped + numa_data._uncommitted);
        state->decrease_capacity(numa_data._uncommitted);
      }

      // Try satisfy stalled allocations
      allocator->satisfy_stalled();
    }

    // Free up the allocated memory
    delete[] per_numa_mappings;
    delete tracker;
  }

  static void promote(ZPageAllocator* allocator, const ZPage* from, const ZPage* to) {
    MultiNUMATracker* const tracker = from->multi_numa_tracker();
    assert(tracker == to->multi_numa_tracker(), "should have the same tracker");

    ZArrayIterator<Element> iter(tracker->map());
    for (Element partial_allocation{}; iter.next(&partial_allocation);) {
      const size_t size = partial_allocation._range.size();
      const int numa_id = partial_allocation._numa_id;
      ZCacheState& state = allocator->state_from_numa_id(numa_id);

      state.decrease_used_generation(ZGenerationId::young, size);
      state.increase_used_generation(ZGenerationId::old, size);
    }
  }
};

ZPageAllocator::ZPageAllocator(size_t min_capacity,
                               size_t initial_capacity,
                               size_t soft_max_capacity,
                               size_t initial_max_capacity,
                               size_t static_max_capacity)
  : _lock(),
    _virtual(static_max_capacity),
    _physical(static_max_capacity),
    _physical_mappings(ZAddressOffsetMax),
    _min_capacity(min_capacity),
    _initial_capacity(initial_capacity),
    _static_max_capacity(static_max_capacity),
    _heuristic_max_capacity(initial_capacity),
    _states(ZValueIdTagType{}, this),
    _stalled(),
    _safe_destroy(),
    _initialized(false) {

  if (!_virtual.is_initialized() || !_physical.is_initialized()) {
    return;
  }

  log_info_p(gc, init)("Min Capacity: %zuM", min_capacity / M);
  log_info_p(gc, init)("Initial Capacity: %zuM", initial_capacity / M);
  log_info_p(gc, init)("Max Capacity: %zuM", initial_max_capacity / M);
  if (soft_max_capacity != 0) {
    log_info_p(gc, init)("Soft Max Capacity: %zuM", soft_max_capacity / M);
  }
  if (ZPageSizeMedium > 0) {
    log_info_p(gc, init)("Medium Page Size: %zuM", ZPageSizeMedium / M);
  } else {
    log_info_p(gc, init)("Medium Page Size: N/A");
  }
  log_info_p(gc, init)("Pre-touch: %s", AlwaysPreTouch ? "Enabled" : "Disabled");

  // Warn if system limits could stop us from reaching desired capacity
  size_t expected_capacity = ZAdaptiveHeap::explicit_max_capacity() ? initial_max_capacity
                                                                    : initial_capacity;
  _physical.warn_commit_limits(expected_capacity, initial_max_capacity);

  // Check if uncommit should and can be enabled
  _physical.try_enable_uncommit(min_capacity, static_max_capacity);

  // Print heap adaptation status
  ZAdaptiveHeap::print();

  // Successfully initialized
  _initialized = true;
}

bool ZPageAllocator::is_initialized() const {
  return _initialized;
}

static void pretouch_memory(zoffset start, size_t size) {
  // At this point we know that we have a valid zoffset / zaddress.
  const zaddress zaddr = ZOffset::address(start);
  const uintptr_t addr = untype(zaddr);
  const size_t page_size = ZLargePages::is_explicit() ? ZGranuleSize : os::vm_page_size();
  os::pretouch_memory((void*)addr, (void*)(addr + size), page_size);
}

class ZPreTouchTask : public ZTask {
private:
  volatile uintptr_t _current;
  const uintptr_t    _end;

public:
  ZPreTouchTask(zoffset start, zoffset_end end)
    : ZTask("ZPreTouchTask"),
      _current(untype(start)),
      _end(untype(end)) {}

  virtual void work() {
    const size_t size = ZGranuleSize;

    for (;;) {
      // Claim an offset for this thread
      const uintptr_t claimed = Atomic::fetch_then_add(&_current, size);
      if (claimed >= _end) {
        // Done
        break;
      }

      // At this point we know that we have a valid zoffset / zaddress.
      const zoffset offset = to_zoffset(claimed);

      // Pre-touch the granule
      pretouch_memory(offset, size);
    }
  }
};

bool ZPageAllocator::prime_state_cache(ZWorkers* workers, int numa_id, size_t to_prime) {
  if (to_prime == 0) {
    return true;
  }

  const size_t committed = commit(numa_id, to_prime);

  if (committed != to_prime) {
    // This is a failure state. We do not cleanup the maybe partially committed memory.
    return false;
  }

  // Retrieve the primed mapping
  const ZMemoryRange vmem =_states.get(numa_id).get_primed_mapping();

  if (ZNUMA::is_enabled()) {
    // Check if memory ended up on desired NUMA node or not
    const int actual_id = ZNUMA::memory_id(untype(ZOffset::address(vmem.start())));
    if (actual_id != numa_id) {
      log_debug(gc, heap)("NUMA Mismatch: desired %d, actual %d", numa_id, actual_id);
    }
  }

  if (AlwaysPreTouch) {
    // Pre-touch memory
    ZPreTouchTask task(vmem.start(), vmem.end());
    workers->run_all(&task);
  }

  return true;
}

bool ZPageAllocator::prime_cache(ZWorkers* workers, size_t size) {
  const int numa_nodes = ZNUMA::count();
  for (int numa_id = 0; numa_id < numa_nodes; ++numa_id) {
    const size_t to_prime = ZNUMA::calculate_share(numa_id, size);
    if (!prime_state_cache(workers, numa_id, to_prime)) {
      return false;
    }
  }

  return true;
}

size_t ZPageAllocator::initial_capacity() const {
  return _initial_capacity;
}

size_t ZPageAllocator::min_capacity() const {
  return _min_capacity;
}

size_t ZPageAllocator::static_max_capacity() const {
  return _static_max_capacity;
}

size_t ZPageAllocator::dynamic_max_capacity() const {
  size_t total_dynamic_max_capacity = 0;
  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    total_dynamic_max_capacity += state->dynamic_max_capacity();
  }
  return total_dynamic_max_capacity;
}

size_t ZPageAllocator::current_max_capacity() const {
  size_t total_current_max_capacity = 0;
  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    total_current_max_capacity += state->current_max_capacity();
  }
  return total_current_max_capacity;
}

size_t ZPageAllocator::heuristic_max_capacity() const {
  // Note that SoftMaxHeapSize is a manageable flag
  const size_t soft_max_capacity = align_down(Atomic::load(&SoftMaxHeapSize), ZGranuleSize);
  const size_t heuristic_max_capacity = Atomic::load(&_heuristic_max_capacity);
  const size_t curr_max_capacity = current_max_capacity();
  return calculate_heuristic_max_capacity(soft_max_capacity, heuristic_max_capacity, curr_max_capacity);
}

void ZPageAllocator::adapt_heuristic_max_capacity(ZGenerationId generation) {
  const size_t soft_max_capacity = align_down(Atomic::load(&SoftMaxHeapSize), ZGranuleSize);
  const size_t heuristic_max_capacity = Atomic::load(&_heuristic_max_capacity);
  const size_t min_capacity = _min_capacity;
  const size_t used = ZPageAllocator::used();
  const size_t capacity = MAX2(ZPageAllocator::capacity(), used);
  const size_t curr_max_capacity = MAX2(capacity, current_max_capacity());
  const size_t highest_soft_capacity = soft_max_capacity == 0 ? curr_max_capacity
                                                              : MIN2(soft_max_capacity, curr_max_capacity);
  const double alloc_rate = ZStatMutatorAllocRate::stats()._avg;

  ZHeapResizeMetrics metrics = {
    highest_soft_capacity,
    curr_max_capacity,
    heuristic_max_capacity,
    min_capacity,
    capacity,
    used,
    alloc_rate
  };

  const size_t selected_capacity = ZAdaptiveHeap::compute_heap_size(&metrics, generation);

  // Update heuristic max capacity
  Atomic::store(&_heuristic_max_capacity, selected_capacity);
  ZPerNUMAIterator<ZCacheState> iter = state_iterator();
  for (ZCacheState* state; iter.next(&state);) {
    const int numa_id = state->numa_id();
    // Update per state heuristic max capacity
    const size_t selected_capacity_share = ZNUMA::calculate_share(numa_id, selected_capacity);
    state->set_heuristic_max_capacity(selected_capacity_share);

    // Update committer target capacity
    ZCommitter& committer = state->committer();
    committer.heap_resized(state->capacity(), selected_capacity_share);
  }

  // Complain about misconfigurations
  _physical.warn_commit_limits(selected_capacity, dynamic_max_capacity());
}

size_t ZPageAllocator::capacity() const {
  size_t capacity = 0;
  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    capacity += Atomic::load(&state->_capacity);
  }
  return capacity;
}

size_t ZPageAllocator::used() const {
  size_t used = 0;
  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    used += Atomic::load(&state->_used);
  }
  return used;
}

size_t ZPageAllocator::used_generation(ZGenerationId id) const {
  size_t used_generation = 0;
  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    used_generation += Atomic::load(&state->_used_generations[(int)id]);
  }
  return used_generation;
}

size_t ZPageAllocator::unused() const {
  ssize_t capacity = 0;
  ssize_t used = 0;
  ssize_t claimed = 0;

  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    capacity += (ssize_t)Atomic::load(&state->_capacity);
    used += (ssize_t)Atomic::load(&state->_used);
    claimed += (ssize_t)Atomic::load(&state->_claimed);
  }

  const ssize_t unused = capacity - used - claimed;
  return unused > 0 ? (size_t)unused : 0;
}

void ZPageAllocator::promote_used(const ZPage* from, const ZPage* to) {
  assert(from->size() == to->size(), "pages are the same size");
  const size_t size = from->size();
  if (from->is_multi_numa()) {
    MultiNUMATracker::promote(this, from, to);
  } else {
    // TODO: virtual_memory are the same, this is only used for inplace and flip promotion
    //       for relocation promotions these counters are accounted for in alloc and free
    state_from_vmem(from->virtual_memory()).decrease_used_generation(ZGenerationId::young, size);
    state_from_vmem(to->virtual_memory()).increase_used_generation(ZGenerationId::old, size);
  }
}

ZPageAllocatorStats ZPageAllocator::stats(ZGeneration* generation) const {
  ZLocker<ZLock> locker(&_lock);

  ZPageAllocatorStats stats(_min_capacity,
                            heuristic_max_capacity(),
                            generation->freed(),
                            generation->promoted(),
                            generation->compacted(),
                            _stalled.size());

  // Aggregate per ZCacheState stats
  const int gen_id = (int)generation->id();
  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    stats.increment_stats(state->_capacity,
                          state->_used,
                          state->_collection_stats[gen_id]._used_high,
                          state->_collection_stats[gen_id]._used_low,
                          state->_used_generations[gen_id]);
  }

  return stats;
}

void ZPageAllocator::reset_statistics(ZGenerationId id) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");

  ZPerNUMAIterator<ZCacheState> iter(&_states);
  for (ZCacheState* state; iter.next(&state);) {
    state->reset_statistics(id);
  }
}

ZCacheState& ZPageAllocator::state_from_numa_id(int numa_id) {
  return _states.get(numa_id);
}

ZCacheState& ZPageAllocator::state_from_vmem(const ZMemoryRange& vmem) {
  return state_from_numa_id(_virtual.get_numa_id(vmem));
}

size_t ZPageAllocator::count_segments_physical(const ZMemoryRange& vmem) {
  return _physical.count_segments(_physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::sort_segments_physical(const ZMemoryRange& vmem) {
  sort_zbacking_index_ptrs(_physical_mappings.get_addr(vmem.start()), vmem.size_in_granules());
}

void ZPageAllocator::heat_memory(zoffset start, size_t size) const {
  pretouch_memory(start, size);
  if (ZLargePages::is_collapse()) {
    _physical.collapse(start, size);
  }
}

void ZPageAllocator::map_virtual_to_physical(const ZMemoryRange& vmem, int numa_id) {
  // Map virtual memory to physical memory
  _physical.map(vmem.start(), _physical_mappings.get_addr(vmem.start()), vmem.size(), numa_id);
}

void ZPageAllocator::unmap_virtual(const ZMemoryRange& vmem) {
  const int numa_id = _virtual.get_numa_id(vmem);
  // Make sure we don't try to pretouch unmapped pages
  ZCacheState& state = _states.get(numa_id);
  ZCommitter& committer = state.committer();
  committer.remove_heating_request(vmem);

  // Unmap virtual memory from physical memory
  _physical.unmap(vmem.start(), _physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::free_virtual(const ZMemoryRange& vmem) {
  // Free virtual memory
  _virtual.free(vmem);
}

void ZPageAllocator::free_virtual(const ZMemoryRange& vmem, int numa_id) {
  // Free virtual memory
  _virtual.free(vmem, numa_id);
}

void ZPageAllocator::remap_and_defragment_mapping(const ZMemoryRange& vmem, ZArray<ZMemoryRange>* entries) {
  // If no lower address can be found, don't remap/defrag
  if (_virtual.lowest_available_address(_virtual.get_numa_id(vmem)) > vmem.start()) {
    entries->append(vmem);
    return;
  }

  ZStatInc(ZCounterDefragment);

  // Synchronously unmap the virtual memory
  unmap_virtual(vmem);

  // Stash segments
  ZSegmentStash segments(&_physical_mappings, (int)vmem.size_in_granules());
  segments.stash(vmem);

  // Shuffle vmem
  const int start_index = entries->length();
  const int num_ranges = _virtual.shuffle_vmem_to_low_addresses(vmem, entries);

  // Restore segments
  segments.pop(entries, num_ranges);

  // The entries array may contain entries from other defragmentations as well,
  // so we only operate on the last ranges that we have just inserted
  const int numa_id = _virtual.get_numa_id(vmem);
  ZArrayIterator<ZMemoryRange> iter(entries, start_index);
  for (ZMemoryRange v; iter.next(&v);) {
    map_virtual_to_physical(v, numa_id);
    pretouch_memory(v.start(), v.size());
  }
}

static void check_out_of_memory_during_initialization() {
  if (!is_init_completed()) {
    vm_exit_during_initialization("java.lang.OutOfMemoryError", "Java heap too small");
  }
}

bool ZPageAllocator::alloc_page_stall(ZPageAllocation* allocation) {
  ZStatTimer timer(ZCriticalPhaseAllocationStall);
  EventZAllocationStall event;

  // We can only block if the VM is fully initialized
  check_out_of_memory_during_initialization();

  // Start asynchronous minor GC
  const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, 0);
  ZDriver::minor()->collect(request);

  // Wait for allocation to complete or fail
  const bool result = allocation->wait();

  {
    // Guard deletion of underlying semaphore. This is a workaround for
    // a bug in sem_post() in glibc < 2.21, where it's not safe to destroy
    // the semaphore immediately after returning from sem_wait(). The
    // reason is that sem_post() can touch the semaphore after a waiting
    // thread have returned from sem_wait(). To avoid this race we are
    // forcing the waiting thread to acquire/release the lock held by the
    // posting thread. https://sourceware.org/bugzilla/show_bug.cgi?id=12674
    ZLocker<ZLock> locker(&_lock);
  }

  // Send event
  event.commit((u8)allocation->type(), allocation->size());

  return result;
}

bool ZPageAllocator::claim_physical_multi_numa(ZPageAllocation* allocation) {
  // Start at the allocating thread's affinity
  const int start_node = allocation->initiating_numa_id();
  const int numa_nodes = ZNUMA::count();

  const size_t size = allocation->size();
  size_t remaining = size;
  const size_t split_size = align_up(size / numa_nodes, ZGranuleSize);

  // Flip allocation to multi numa allocation
  allocation->set_multi_numa_allocation();

  // Loops over every node and allocates get_alloc_size per node
  const auto do_claim_each_node = [&](auto get_alloc_size) {
    for (int i = 0; i < numa_nodes; ++i) {
      uint32_t current_node = (start_node + i) % numa_nodes;
      ZCacheState& state = _states.get(current_node);
      const size_t current_max_capacity = state.current_max_capacity(allocation);
      size_t alloc_size = get_alloc_size(state, current_max_capacity);

      if (alloc_size == 0) {
        // Nothing to allocate on current node
        continue;
      }

      ZMemoryAllocation* partial_allocation = allocation->get_next_multi_numa_allocation(alloc_size);
      partial_allocation->set_current_max_capacity(current_max_capacity);

      if (!state.claim_physical(partial_allocation)) {
        // Claiming failed
        allocation->remove_last_multi_numa_allocation();
        return false;
      }

      // Record which state the allocation was made on
      partial_allocation->set_numa_id(current_node);

      // Update remaining
      remaining -= alloc_size;

      if (remaining == 0) {
        // All memory claimed
        return true;
      }
    }
    return true;
  };

  // Try to claim upto split_size on each node
  const auto get_even_alloc_size = [&](ZCacheState& state, size_t current_max_capacity) {
    return MIN3(split_size, state.available(current_max_capacity), remaining);
  };
  if (!do_claim_each_node(get_even_alloc_size)) {
    // Claiming failed
    return false;
  }

  if (remaining == 0) {
    // All memory claimed
    return true;
  }

  // Else try claim the remaining
  const auto get_rest_alloc_size = [&](ZCacheState& state, size_t current_max_capacity) {
    return MIN2(state.available(current_max_capacity), remaining);
  };
  if (!do_claim_each_node(get_rest_alloc_size)) {
    // Claiming failed
    return false;
  }

  return remaining == 0;
}

bool ZPageAllocator::claim_physical_round_robin(ZPageAllocation* allocation) {
  // Start at the allocating thread's affinity
  ZMemoryAllocation* const memory_allocation = allocation->memory_allocation();
  const int start_node = allocation->initiating_numa_id();
  const int numa_nodes = ZNUMA::count();
  size_t total_available = 0;
  int current_node = start_node;

  for (int i = 0; i < numa_nodes; ++i) {
    uint32_t current_node = (start_node + i) % numa_nodes;
    ZCacheState& state = _states.get(current_node);

    // Get current max capacity for state
    const size_t current_max_capacity = state.current_max_capacity(allocation);
    memory_allocation->set_current_max_capacity(current_max_capacity);

    if (state.claim_physical(memory_allocation)) {
      // Success, record which state the allocation was made on
      memory_allocation->set_numa_id(current_node);

      return true;
    }

    // Keep track of total availability for a potential multi NUMA allocation
    // TODO: available requires that current_max_capacity has been set.
    total_available += state.available(memory_allocation);
  }

  if (numa_nodes > 1 && total_available >= allocation->size()) {
    if (!claim_physical_multi_numa(allocation)) {
      // May have partially succeeded, undo any partial allocations
      free_memory_alloc_failed_multi_numa(allocation);

      return false;
    }

    return true;
  }

  return false;
}

bool ZPageAllocator::claim_physical_or_stall(ZPageAllocation* allocation) {
  if (allocation->retry_with_stall()) {
    // Reset the allocation for stall
    allocation->reset_for_stall();

    // Enqueue allocation request
    ZLocker<ZLock> locker(&_lock);
    _stalled.insert_last(allocation);
  } else {
    ZLocker<ZLock> locker(&_lock);
    // Try to claim memory
    if (claim_physical_round_robin(allocation)) {
      return true;
    }

    // Failed to claim memory
    if (allocation->flags().non_blocking()) {
      // Don't stall
      return false;
    }

    if (allocation->is_stalling()) {
      // Don't stall for a retrying allocation which already stalled
      assert(allocation->is_retrying(), "must be");
      return false;
    }

    // Reset the allocation for stall
    allocation->reset_for_stall();

    // Enqueue allocation request
    _stalled.insert_last(allocation);
  }

  // Stall
  return alloc_page_stall(allocation);
}

void ZPageAllocator::harvest_claimed_physical(ZMemoryAllocation* allocation) {
  const int num_mappings_harvested = allocation->claimed_mappings()->length();

  const int num_granules = (int)(allocation->harvested() >> ZGranuleSizeShift);
  ZSegmentStash segments(&_physical_mappings, num_granules);

  // Unmap virtual memory
  ZArrayIterator<ZMemoryRange> iter(allocation->claimed_mappings());
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    unmap_virtual(vmem);
  }

  // Stash segments
  segments.stash(allocation->claimed_mappings());

  // Shuffle vmem. We attempt to allocate enough memory to cover the entire allocation
  // size, not just the harvested memory.
  _virtual.shuffle_vmem_to_low_addresses_contiguous(allocation->size(), allocation->claimed_mappings());

  // Restore segments
  segments.pop(allocation->claimed_mappings(), allocation->claimed_mappings()->length());

  const size_t harvested = allocation->harvested();
  if (harvested > 0) {
    log_debug(gc, heap)("Mapped Cache Harvest: %zuM from %d mappings", harvested / M, num_mappings_harvested);
  }
}

bool ZPageAllocator::is_alloc_satisfied(ZPageAllocation* allocation) const {
  return is_alloc_satisfied(allocation->memory_allocation());
}

bool ZPageAllocator::is_alloc_satisfied(ZMemoryAllocation* allocation) const {
  // The allocation is immediately satisfied if the list of mappings contains
  // exactly one mapping and is of the correct size.

  if (allocation->claimed_mappings()->length() != 1) {
    // No mapping(s) or not a contiguous mapping
    return false;
  }

  const ZMemoryRange& vmem = allocation->claimed_mappings()->first();
  if (vmem.size() != allocation->size()) {
    // Not correct sized mapping
    return false;
  }

  // Allocation immediately satisfied
  return true;
}

void ZPageAllocator::copy_physical_segments(zoffset to, const ZMemoryRange& from) {
  zbacking_index* const dest = _physical_mappings.get_addr(to);
  const zbacking_index* const src = _physical_mappings.get_addr(from.start());
  const size_t num_granules = from.size_in_granules();

  ZUtils::copy_disjoint(dest, src, num_granules);
}

void ZPageAllocator::copy_claimed_physical_multi_numa(ZPageAllocation* allocation, const ZMemoryRange& vmem) {
  // Start at the new dest offset
  zoffset allocation_destination_offset = vmem.start();
  size_t total_harvested = 0;

  ZArrayMutableIterator<ZMemoryAllocation> allocation_iter(allocation->multi_numa_allocations());
  for (ZMemoryAllocation* partial_allocation; allocation_iter.next_addr(&partial_allocation);) {
    zoffset partial_vmem_destination_offset = allocation_destination_offset;
    size_t harvested = 0;

    // Iterate over all claimed mappings and copy physical segments into the partial_allocations
    // destination offset
    ZArrayIterator<ZMemoryRange> iter(partial_allocation->claimed_mappings());
    for (ZMemoryRange partial_vmem; iter.next(&partial_vmem);) {
      // Copy physical segments
      copy_physical_segments(partial_vmem_destination_offset, partial_vmem);

      // Keep track of amount harvested and advance to next partial_vmem's offset
      harvested += partial_vmem.size();
      partial_vmem_destination_offset += partial_vmem.size();
    }

    // Register amount harvested and advance to next allocation's offset
    total_harvested += harvested;
    partial_allocation->set_harvested(harvested);
    allocation_destination_offset += partial_allocation->size();
  }

  allocation->memory_allocation()->set_harvested(total_harvested);
}

bool ZPageAllocator::claim_virtual_memory_multi_numa(ZPageAllocation* allocation) {
  const int numa_nodes = ZNUMA::count();
  const size_t size = allocation->size();
  ZMemoryAllocation* const memory_allocation = allocation->memory_allocation();

  for (int numa_id = 0; numa_id < numa_nodes; ++numa_id) {
    ZMemoryRange vmem = _virtual.alloc(size, numa_id, false /* force_low_address */);
    if (!vmem.is_null()) {
      // Found an address range
      memory_allocation->claimed_mappings()->append(vmem);

      // Copy claimed multi numa mappings, we leave the old mappings mapped
      // until after we have committed. In case committing fails we can simply
      // reinsert the inital mappings.
      copy_claimed_physical_multi_numa(allocation, vmem);

      return true;
    }
  }
  return false;
}

bool ZPageAllocator::claim_virtual_memory(ZPageAllocation* allocation) {
  if (allocation->is_multi_numa_allocation()) {
    return claim_virtual_memory_multi_numa(allocation);
  }

  return claim_virtual_memory(allocation->memory_allocation());
}

bool ZPageAllocator::claim_virtual_memory(ZMemoryAllocation* allocation) {
  if (allocation->harvested() > 0) {
    // If we have harvested anything, we claim virtual memory from the harvested
    // mappings, and perhaps also allocate more to match the allocation request.
    harvest_claimed_physical(allocation);
  } else {
    // If we have not harvested anything, we only increased capacity. Allocate
    // new virtual memory from the manager.
    const ZMemoryRange vmem = _virtual.alloc(allocation->size(), allocation->numa_id(), true /* force_low_address */);
    if (!vmem.is_null()) {
      allocation->claimed_mappings()->append(vmem);
    }
  }

  // If the virtual memory covers the allocation request, we're done.
  if (is_alloc_satisfied(allocation)) {
    return true;
  }

  // Before returning harvested memory to the cache it must be mapped.
  if (allocation->harvested() > 0) {
    ZArrayIterator<ZMemoryRange> iter(allocation->claimed_mappings());
    for (ZMemoryRange vmem; iter.next(&vmem);) {
      map_virtual_to_physical(vmem, allocation->numa_id());
    }
  }

  // Failed to allocate enough to virtual memory from the manager.
  return false;
}

void ZPageAllocator::allocate_remaining_physical_multi_numa(ZPageAllocation* allocation, const ZMemoryRange& vmem) {
  ZMemoryRange remaining_vmem = vmem;
  ZArrayMutableIterator<ZMemoryAllocation> allocation_iter(allocation->multi_numa_allocations());
  for (ZMemoryAllocation* partial_allocation; allocation_iter.next_addr(&partial_allocation);) {
    ZMemoryRange partial_allocation_vmem = remaining_vmem.split_from_front(partial_allocation->size());
    allocate_remaining_physical(partial_allocation, partial_allocation_vmem);
  }
}

void ZPageAllocator::allocate_remaining_physical(ZMemoryAllocation* allocation, const ZMemoryRange& vmem) {
  const size_t remaining_physical = allocation->size() - allocation->harvested();
  if (remaining_physical > 0) {
    ZMemoryRange uncommitted_range = ZMemoryRange(vmem.start() + allocation->harvested(), remaining_physical);
    ZCacheState& state = state_from_numa_id(allocation->numa_id());
    state.alloc_physical(uncommitted_range);
  }
}

void ZPageAllocator::allocate_remaining_physical(ZPageAllocation* allocation, const ZMemoryRange& vmem) {
  assert(allocation->size() == vmem.size(), "vmem should be the final mapping");

  if (allocation->is_multi_numa_allocation()) {
    allocate_remaining_physical_multi_numa(allocation, vmem);
  } else {
    allocate_remaining_physical(allocation->memory_allocation(), vmem);
  }
}

bool ZPageAllocator::commit_and_map_memory_multi_numa(ZPageAllocation* allocation, const ZMemoryRange& vmem) {
  // Helper to loop over each ZMemoryAllocation and its associated partial_vmem
  const auto for_each_partial_vmem = [&](auto body_fn) {
    ZMemoryRange remaining_vmem = vmem;

    ZArrayMutableIterator<ZMemoryAllocation> allocation_iter(allocation->multi_numa_allocations());
    for (ZMemoryAllocation* partial_allocation; allocation_iter.next_addr(&partial_allocation);) {
      // Split of partial allocation's memory range
      ZMemoryRange partial_vmem = remaining_vmem.split_from_front(partial_allocation->size());

      body_fn(partial_allocation, partial_vmem);
    }

    assert(remaining_vmem.size() == 0, "all memory should be acountted for");
  };

  // First commit all uncommitted parts
  bool commit_failed = false;
  size_t total_committed = 0;

  for_each_partial_vmem([&](ZMemoryAllocation* partial_allocation, ZMemoryRange partial_vmem) {
    if (commit_failed) {
      // Skip committing the rest after a commit failed.
      return; // continue;
    }

    // Remove the harvested part
    partial_vmem.shrink_from_front(partial_allocation->harvested());

    // Try to commit
    ZCacheState& state = state_from_numa_id(partial_allocation->numa_id());
    const size_t to_commit = partial_vmem.size();
    const size_t committed = state.commit_physical(partial_vmem);

    // Keep track of the committed amount
    partial_allocation->set_committed(committed);

    if (committed != to_commit) {
      commit_failed = true;
      partial_allocation->set_commit_failed();

      // Free uncommitted physical segments
      const ZMemoryRange uncommitted = partial_vmem.split_from_back(to_commit - committed);
      state.free_physical(uncommitted);
    }

    // Account for all committed
    total_committed += committed;
  });

  if (!commit_failed) {
    // All memory has been committed, now unmap the original mappings and create the final mapping
    for_each_partial_vmem([&](ZMemoryAllocation* partial_allocation, ZMemoryRange partial_vmem) {
      const int numa_id = partial_allocation->numa_id();
      ZArray<ZMemoryRange>* const mappings = partial_allocation->claimed_mappings();

      // Unmap original mappings
      while (!mappings->is_empty()) {
        ZMemoryRange to_unmap = mappings->pop();
        unmap_virtual(to_unmap);
        free_virtual(to_unmap, numa_id);
      }

      // Sort physical segments
      sort_segments_physical(partial_vmem);

      // Map the partial_allocation to partial_vmem
      map_virtual_to_physical(partial_vmem, numa_id);
    });

    // Keep track of the total committed memory
    allocation->memory_allocation()->set_committed(total_committed);

    return true;
  }

  // Deal with a failed commit
  // All harvested mappings still remain, but we may have unmapped commited
  // memory for each partial_allocation. Try to map this on the correct node,
  // and in the case that no virtual memory can be found, simply uncommit.
  for_each_partial_vmem([&](ZMemoryAllocation* partial_allocation, ZMemoryRange partial_vmem) {
    const size_t committed = partial_allocation->committed();

    if (committed == 0) {
      // Nothing committed, nothing to handle
      return; // continue;
    }

    // Remove the harvested part
    partial_vmem.shrink_from_front(partial_allocation->harvested());

    const int numa_id = partial_allocation->numa_id();
    ZCacheState& state = state_from_numa_id(numa_id);
    ZArray<ZMemoryRange>* const mappings = partial_allocation->claimed_mappings();
    // Keep track of the start index
    const int start_index = mappings->length();

    // Try to allocated virtual memory for the committed part
    const size_t to_map = _virtual.alloc_low_address_many_at_most(committed, numa_id, mappings);

    if (to_map != committed) {
      // Uncommit any memory that is unmappable due to no virtual memory
      // We do not track this, so if the partial_allocation failed to commit,
      // the unmappable memory will also count toward the reduction in
      // current max capacity

      const ZMemoryRange unmappable = partial_vmem.split_from_back(committed - to_map);
      state.uncommit_physical(unmappable);
      state.free_physical(unmappable);

      // Keep track of the total committed memory
      total_committed -= unmappable.size();
    }

    ZArrayIterator<ZMemoryRange> iter(mappings, start_index);
    for (ZMemoryRange to_map; iter.next(&to_map);) {
      const ZMemoryRange from = partial_vmem.split_from_front(to_map.size());

      // Copy physical mappings
      copy_physical_segments(to_map.start(), from);

      // Map memory
      map_virtual_to_physical(to_map, numa_id);
    }

    assert(partial_vmem.size() == 0, "all memory should be acountted for");
  });

  // Keep track of the total committed memory
  allocation->memory_allocation()->set_committed(total_committed);

  // Free the unused virtual mapping
  free_virtual(vmem);

  // All memory has been accounted for and are in the partial_allocation's
  // claimed_mappings
  return false;
}

bool ZPageAllocator::commit_and_map_memory(ZPageAllocation* allocation, const ZMemoryRange& vmem) {
  assert(allocation->size() == vmem.size(), "vmem should be the final mapping");

  if (allocation->is_multi_numa_allocation()) {
    return commit_and_map_memory_multi_numa(allocation, vmem);
  } else {
    return commit_and_map_memory(allocation->memory_allocation(), vmem);
  }
}

bool ZPageAllocator::commit_and_map_memory(ZMemoryAllocation* allocation, const ZMemoryRange& vmem) {
  ZCacheState& state = state_from_numa_id(allocation->numa_id());
  size_t const committed_size = allocation->harvested();
  ZMemoryRange to_be_committed_vmem = vmem;
  ZMemoryRange committed_vmem = to_be_committed_vmem.split_from_front(committed_size);

  // Try to commit all physical memory, commit_physical frees both the virtual
  // and physical parts that correspond to the memory that failed to be committed.
  const size_t committed = state.commit_physical(to_be_committed_vmem);

  // Keep track of the committed amount
  allocation->set_committed(committed);

  if (committed != to_be_committed_vmem.size()) {
    // Free the uncommitted memory and update vmem with the committed memory
    ZMemoryRange not_commited_vmem = to_be_committed_vmem.split_from_back(committed);
    state.free_physical(not_commited_vmem);
    free_virtual(not_commited_vmem);
    allocation->set_commit_failed();
  }
  committed_vmem.grow_from_back(committed);

  // We have not managed to get any committed memory at all, meaning this allocation
  // failed to commit memory on capacity increase alone and nothing harvested.
  if (committed_vmem.size() == 0)  {
    return false;
  }

  sort_segments_physical(committed_vmem);
  map_virtual_to_physical(committed_vmem, allocation->numa_id());
  allocation->claimed_mappings()->append(committed_vmem);

  if (ZNUMA::is_enabled()) {
    // Check if memory ended up on desired NUMA node or not
    const int actual_id = ZNUMA::memory_id(untype(ZOffset::address(vmem.start())));
    if (actual_id != allocation->numa_id()) {
      log_debug(gc, heap)("NUMA Mismatch: desired %d, actual %d", allocation->numa_id(), actual_id);
    }
  }

  if (committed_vmem.size() != vmem.size()) {
    log_trace(gc, page)("Split memory [" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT "]",
        untype(committed_vmem.start()),
        untype(committed_vmem.end()),
        untype(vmem.end()));
    return false;
  }

  return true;
}

ZPage* ZPageAllocator::alloc_page_inner(ZPageAllocation* allocation) {
  // Claim physical memory by taking it from the mapped cache or by increasing
  // capacity, which allows us to allocate from the underlying memory manager
  // later on. Note that this call might block in a safepoint if the non-blocking
  // flag is not set.
  if (!claim_physical_or_stall(allocation)) {
    // Out of memory
    return nullptr;
  }

  // If we have claimed a large enough contiguous mapping from the cache,
  // we're done.
  if (is_alloc_satisfied(allocation)) {
    const ZMemoryRange vmem = allocation->pop_final_mapping();
    return new ZPage(allocation->type(), vmem);
  }

  // Claim virtual memory, either by harvesting or by allocating from the
  // virtual manager.
  if (!claim_virtual_memory(allocation)) {
    log_error(gc)("Out of address space");
    return free_memory_alloc_failed_and_retry(allocation);
  }

  const ZMemoryRange vmem = allocation->pop_final_mapping();

  // Allocate any remaining physical memory. Capacity and used has already been
  // adjusted, we just need to fetch the memory, which is guaranteed to succeed.
  allocate_remaining_physical(allocation, vmem);

  if (!commit_and_map_memory(allocation, vmem)) {
    return free_memory_alloc_failed_and_retry(allocation);
  }

  return new ZPage(allocation->type(), vmem);
}

void ZPageAllocator::increase_used_generation(const ZMemoryAllocation* allocation, ZGenerationId id) {
  const int numa_id = allocation->numa_id();
  const size_t size = allocation->size();
  _states.get(numa_id).increase_used_generation(id, size);
}

void ZPageAllocator::alloc_page_age_update(ZPageAllocation* allocation, ZPage* page, ZPageAge age) {
  // The generation's used is tracked here when the page is handed out
  // to the allocating thread. The overall heap "used" is tracked in
  // the lower-level allocation code.
  const ZGenerationId id = age == ZPageAge::old ? ZGenerationId::old : ZGenerationId::young;
  if (allocation->is_multi_numa_allocation()) {
    ZArrayIterator<ZMemoryAllocation> allocation_iter(allocation->multi_numa_allocations());
    for (const ZMemoryAllocation* partial_allocation; allocation_iter.next_addr(&partial_allocation);) {
      increase_used_generation(partial_allocation, id);
    }
  } else {
    increase_used_generation(allocation->memory_allocation(), id);
  }

  // Reset page. This updates the page's sequence number and must
  // be done after we potentially blocked in a safepoint (stalled)
  // where the global sequence number was updated.
  page->reset(age);
  if (age == ZPageAge::old) {
    page->remset_alloc();
  }
}

ZPage* ZPageAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags, ZPageAge age) {
  EventZPageAllocation event;

  ZPageAllocation allocation(type, size, flags);
  ZPage* const page = alloc_page_inner(&allocation);
  if (page == nullptr) {
    return nullptr;
  }

  alloc_page_age_update(&allocation, page, age);

  MultiNUMATracker::install_tracker(&allocation, page);

  // Update allocation statistics. Exclude gc relocations to avoid
  // artificial inflation of the allocation rate during relocation.
  if (!flags.gc_relocation() && is_init_completed()) {
    // Note that there are two allocation rate counters, which have
    // different purposes and are sampled at different frequencies.
    ZStatInc(ZCounterMutatorAllocationRate, size);
    ZStatMutatorAllocRate::sample_allocation(size);
  }

  // Send event
  ZMemoryAllocation* const memory_allocation = allocation.memory_allocation();
  event.commit((u8)type, size, memory_allocation->harvested(), memory_allocation->committed(),
               (unsigned int)count_segments_physical(page->virtual_memory()), flags.non_blocking());

  return page;
}

void ZPageAllocator::safe_destroy_page(ZPage* page) {
  // Destroy page safely
  _safe_destroy.schedule_delete(page);

}

void ZPageAllocator::satisfy_stalled() {
  for (;;) {
    ZPageAllocation* const allocation = _stalled.first();
    if (allocation == nullptr) {
      // Allocation queue is empty
      return;
    }

    if (!claim_physical_round_robin(allocation)) {
      // Allocation could not be satisfied, give up
      return;
    }

    // Allocation succeeded, dequeue and satisfy allocation request.
    // Note that we must dequeue the allocation request first, since
    // it will immediately be deallocated once it has been satisfied.
    _stalled.remove(allocation);
    allocation->satisfy(true);
  }
}

void ZPageAllocator::prepare_memory_for_free(ZPage* page, ZArray<ZMemoryRange>* entries, bool allow_defragment) {
  // Extract memory and destroy page
  const ZMemoryRange vmem = page->virtual_memory();
  const ZPageType page_type = page->type();
  safe_destroy_page(page);

  // Perhaps remap mapping
  if (page_type == ZPageType::large && allow_defragment) {
    remap_and_defragment_mapping(vmem, entries);
  } else {
    entries->append(vmem);
  }
}

void ZPageAllocator::free_page_multi_numa(ZPage* page) {
  assert(page->is_multi_numa(), "only used for multi numa pages");
  MultiNUMATracker::free_and_destroy(this, page);
}

void ZPageAllocator::free_page(ZPage* page, bool allow_defragment) {
  if (page->is_multi_numa()) {
    // Multi numa is handled separately, multi numa allocations are always
    // effectively defragmented
    free_page_multi_numa(page);

    return;
  }

  ZArray<ZMemoryRange> to_cache;

  ZGenerationId id = page->generation_id();
  ZCacheState& state = state_from_vmem(page->virtual_memory());
  prepare_memory_for_free(page, &to_cache, allow_defragment);

  ZLocker<ZLock> locker(&_lock);

  // Update used statistics and cache memory
  ZArrayIterator<ZMemoryRange> iter(&to_cache);
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    const size_t size = vmem.size();

    // Reinsert mappings
    state.cache()->insert(vmem);

    // Update accounting
    state.decrease_used(size);
    state.decrease_used_generation(id, size);
  }

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::free_pages(const ZArray<ZPage*>* pages) {
  ZArray<ZMemoryRange> to_cache;

  // All pages belong to the same generation, so either only young or old.
  const ZGenerationId id = pages->first()->generation_id();

  // Prepare memory from pages to be cached before taking the lock
  ZArrayIterator<ZPage*> pages_iter(pages);
  for (ZPage* page; pages_iter.next(&page);) {
    if (page->is_multi_numa()) {
      // Multi numa is handled separately
      free_page_multi_numa(page);

      continue;
    }
    prepare_memory_for_free(page, &to_cache, true /* allow_defragment */);
  }

  ZLocker<ZLock> locker(&_lock);

    // Update used statistics and cache memory
  ZArrayIterator<ZMemoryRange> iter(&to_cache);
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    ZCacheState& state = state_from_vmem(vmem);
    const size_t size = vmem.size();

    // Reinsert mappings
    state.cache()->insert(vmem);

    // Update accounting
    state.decrease_used(size);
    state.decrease_used_generation(id, size);
  }

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::free_memory_alloc_failed_multi_numa(ZPageAllocation* allocation) {
  ZArrayMutableIterator<ZMemoryAllocation> iter(allocation->multi_numa_allocations());
  for (ZMemoryAllocation* partial_allocation; iter.next_addr(&partial_allocation);) {
    free_memory_alloc_failed(partial_allocation);
  }
}

ZPage* ZPageAllocator::free_memory_alloc_failed_and_retry(ZPageAllocation* allocation) {
  {
    ZLocker<ZLock> locker(&_lock);

    if (allocation->is_multi_numa_allocation()) {
      // Free each partial allocation
      free_memory_alloc_failed_multi_numa(allocation);
    } else {
      free_memory_alloc_failed(allocation->memory_allocation());
    }

    // Try satisfy stalled allocations
    satisfy_stalled();
  }

  assert(!allocation->retry_with_stall(), "should not have reached here");

  if (allocation->is_retrying()) {
    // The retry has failed
    if (allocation->is_stalling()) {
      // A stalling allocation which has been retried after the stall, OOM
      return nullptr;
    }

    // Reset the allocation for a retry with stall
    allocation->reset_for_retry_with_stall();
  } else {
    // Reset the allocation for a retry
    allocation->reset_for_retry();
  }

  // Retry the allocation
  return alloc_page_inner(allocation);
}

void ZPageAllocator::free_memory_alloc_failed(ZMemoryAllocation* allocation) {
  ZCacheState& state = _states.get(allocation->numa_id());

  // Only decrease the overall used and not the generation used,
  // since the allocation failed and generation used wasn't bumped.
  state.decrease_used(allocation->size());

  size_t freed = 0;

  // Free mapped memory
  ZArrayIterator<ZMemoryRange> iter(allocation->claimed_mappings());
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    freed += vmem.size();
    state._cache.insert(vmem);
  }
  assert(allocation->harvested() + allocation->committed() == freed, "must have freed all");

  // Adjust capacity to reflect the failed capacity increase
  const size_t remaining = allocation->size() - freed;
  if (remaining > 0) {
    state.decrease_capacity(remaining);
  }
}

bool ZCacheState::may_uncommit(size_t total_memory, size_t used_memory) const {
  const uint64_t delay = ZAdaptiveHeap::uncommit_delay(used_memory, total_memory);
  const uint64_t expires = Atomic::load(&_last_commit) + delay;
  const uint64_t now = os::elapsedTime();

  return now >= expires;
}

void ZPageAllocator::adjust_capacity(size_t used_soon) {
  const size_t total_memory = os::physical_memory();
  const size_t used_memory = os::used_memory();
  const bool force_uncommit = double(used_memory) > double(total_memory) * (1.0 - ZMemoryCriticalThreshold);

  ZPerNUMAIterator<ZCacheState> iter = state_iterator();
  for (ZCacheState* state; iter.next(&state);) {
    if (force_uncommit || state->may_uncommit(total_memory, used_memory)) {
      // Uncommit is urgent, or uncommit delay has changed
      ZUncommitter& uncommitter = state->uncommitter();
      uncommitter.wake_up();
      continue;
    }

    ZCommitter& committer = state->committer();
    if (used_soon > committer.target_capacity()) {
      const int numa_id = state->numa_id();
      const size_t used_soon_share = ZNUMA::calculate_share(numa_id, used_soon);
      committer.set_target_capacity(used_soon_share);
    }
  }
}

size_t ZPageAllocator::commit(uint32_t numa_id, size_t size) {
  ZCacheState& state = _states.get(numa_id);
  ZArray<ZMemoryRange> claimed_mappings(1);
  ZMemoryAllocation allocation(&claimed_mappings, size);

  {
    ZLocker<ZLock> locker(&_lock);
    if (!state.claim_committed(&allocation)) {
      return 0;
    }
  }

  const size_t mapped = _virtual.alloc_low_address_many_at_most(size, numa_id, &claimed_mappings);

  bool commit_failed = false;
  size_t total_committed = 0;
  ZArrayIterator<ZMemoryRange> iter(&claimed_mappings);
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    if (commit_failed) {
      // A commit has failed, free the remaining vmems
      _virtual.free(vmem);
      continue;
    }

    // Allocate and commit
    state.alloc_physical(vmem);
    const size_t committed = state.commit_physical(vmem);

    if (committed != vmem.size()) {
      // Failed to commit all memory
      commit_failed = true;
      const size_t not_committed = vmem.size() - committed;
      const ZMemoryRange to_free = vmem.split_from_back(not_committed);
      state.free_physical(to_free);
      _virtual.free(to_free);
    }

    // Map memory
    map_virtual_to_physical(vmem, numa_id);
    total_committed += committed;
  }

  {
    ZLocker<ZLock> locker(&_lock);
    if (total_committed != size) {
      // Decrease capacity for memory we failed to commit
      const size_t not_committed = size - total_committed;
      state.decrease_capacity(not_committed);
    }

    // Insert the mappings into to cache
    ZArrayIterator<ZMemoryRange> iter(&claimed_mappings);
    for (ZMemoryRange vmem; iter.next(&vmem);) {
      state.cache()->insert(vmem);
    }

    // Update used
    state.decrease_used(size);
  }

  return size;
}

size_t ZPageAllocator::uncommit(uint32_t numa_id, size_t limit) {
  ZCacheState& state = _states.get(numa_id);
  return state.uncommit(this, limit, [&](){ return true; });
}

size_t ZPageAllocator::uncommit(uint32_t numa_id, size_t limit, uint64_t* timeout) {
  const size_t total_memory = os::physical_memory();
  const size_t used_memory = os::used_memory();
  const uint64_t delay = ZAdaptiveHeap::uncommit_delay(used_memory, total_memory);

  ZCacheState& state = _states.get(numa_id);
  const auto decider = [&]() {
    const double now = os::elapsedTime();
    const double time_since_last_commit = std::floor(now - state._last_commit);
    const double time_since_last_uncommit = std::floor(now - state._last_uncommit);

    if (time_since_last_commit < double(delay)) {
      // We have committed within the delay, stop uncommitting.
      *timeout = uint64_t(double(delay) - time_since_last_commit);
      return false;
    }

    if (time_since_last_uncommit < double(delay)) {
      // We are in the uncommit phase
      const size_t num_uncommits_left = state._to_uncommit / limit;
      const double time_left = double(delay) - time_since_last_uncommit;
      if (time_left < *timeout * num_uncommits_left) {
        // Running out of time, speed up.
        uint64_t new_timeout = uint64_t(std::floor(time_left / double(num_uncommits_left + 1)));
        *timeout = new_timeout;
      }
    } else {
      // We are about to start uncommitting
      state._to_uncommit = state._cache.reset_min();
      state._last_uncommit = now;

      const size_t split = state._to_uncommit / limit + 1;
      uint64_t new_timeout = delay / split;
      *timeout = new_timeout;
    }
    return true;
  };

  return state.uncommit(this, limit, decider);
}

void ZPageAllocator::enable_safe_destroy() const {
  _safe_destroy.enable_deferred_delete();
}

void ZPageAllocator::disable_safe_destroy() const {
  _safe_destroy.disable_deferred_delete();
}

static bool has_alloc_seen_young(const ZPageAllocation* allocation) {
  return allocation->young_seqnum() != ZGeneration::young()->seqnum();
}

static bool has_alloc_seen_old(const ZPageAllocation* allocation) {
  return allocation->old_seqnum() != ZGeneration::old()->seqnum();
}

bool ZPageAllocator::is_alloc_stalling() const {
  ZLocker<ZLock> locker(&_lock);
  return _stalled.first() != nullptr;
}

bool ZPageAllocator::is_alloc_stalling_for_old() const {
  ZLocker<ZLock> locker(&_lock);

  ZPageAllocation* const allocation = _stalled.first();
  if (allocation == nullptr) {
    // No stalled allocations
    return false;
  }

  return has_alloc_seen_young(allocation) && !has_alloc_seen_old(allocation);
}

void ZPageAllocator::notify_out_of_memory() {
  // Fail allocation requests that were enqueued before the last major GC started
  for (ZPageAllocation* allocation = _stalled.first(); allocation != nullptr; allocation = _stalled.first()) {
    if (!has_alloc_seen_old(allocation)) {
      // Not out of memory, keep remaining allocation requests enqueued
      return;
    }

    // Out of memory, dequeue and fail allocation request
    _stalled.remove(allocation);
    allocation->satisfy(false);
  }
}

void ZPageAllocator::restart_gc() const {
  ZPageAllocation* const allocation = _stalled.first();
  if (allocation == nullptr) {
    // No stalled allocations
    return;
  }

  if (!has_alloc_seen_young(allocation)) {
    // Start asynchronous minor GC, keep allocation requests enqueued
    const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, 0);
    ZDriver::minor()->collect(request);
  } else {
    // Start asynchronous major GC, keep allocation requests enqueued
    const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, ZOldGCThreads);
    ZDriver::major()->collect(request);
  }
}

void ZPageAllocator::handle_alloc_stalling_for_young() {
  ZLocker<ZLock> locker(&_lock);
  restart_gc();
}

void ZPageAllocator::handle_alloc_stalling_for_old(bool cleared_all_soft_refs) {
  ZLocker<ZLock> locker(&_lock);
  if (cleared_all_soft_refs) {
    notify_out_of_memory();
  }
  restart_gc();
}

void ZPageAllocator::threads_do(ThreadClosure* tc) const {
  ZPerNUMAConstIterator<ZCacheState> iter = state_iterator();
  for (const ZCacheState* state; iter.next(&state);) {
    state->threads_do(tc);
  }
}

ZPerNUMAConstIterator<ZCacheState> ZPageAllocator::state_iterator() const {
  return ZPerNUMAConstIterator<ZCacheState>(&_states);
}

ZPerNUMAIterator<ZCacheState> ZPageAllocator::state_iterator() {
  return ZPerNUMAIterator<ZCacheState>(&_states);
}
