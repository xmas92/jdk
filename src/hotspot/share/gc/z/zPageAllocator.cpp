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
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zCommitter.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zFuture.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zMemory.inline.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageAllocator.inline.hpp"
#include "gc/z/zSafeDelete.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncommitter.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zWorkers.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <cmath>

static const ZStatCounter       ZCounterMutatorAllocationRate("Memory", "Allocation Rate", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterDefragment("Memory", "Defragment", ZStatUnitOpsPerSecond);
static const ZStatCriticalPhase ZCriticalPhaseAllocationStall("Allocation Stall");

static void sort_zoffset_ptrs(void* at, size_t size) {
  qsort(at, size, sizeof(zoffset),
    [](const void* a, const void* b) -> int {
      return *static_cast<const zoffset*>(a) < *static_cast<const zoffset*>(b) ? -1 : 1;
    });
}

class ZSegmentStash {
private:
  ZGranuleMap<zoffset>* _physical_mappings;
  ZArray<zoffset>       _stash;

  void sort_stashed_segments() {
    sort_zoffset_ptrs(_stash.adr_at(0), (size_t)_stash.length());
  }

public:
  ZSegmentStash(ZGranuleMap<zoffset>* physical_mappings, int num_granules)
    : _physical_mappings(physical_mappings),
      _stash(num_granules, num_granules, zoffset(0)) {}

  void stash(const ZMemoryRange& vmem) {
    memcpy(_stash.adr_at(0), _physical_mappings->get_addr(vmem.start()), sizeof(zoffset) * (int)vmem.size_in_granules());
    sort_stashed_segments();
  }

  void stash(ZArray<ZMemoryRange>* mappings) {
    int stash_index = 0;
    ZArrayIterator<ZMemoryRange> iter(mappings);
    for (ZMemoryRange vmem; iter.next(&vmem);) {
      const size_t num_granules = vmem.size_in_granules();
      memcpy(_stash.adr_at(stash_index), _physical_mappings->get_addr(vmem.start()), sizeof(zoffset) * num_granules);
      stash_index += (int)num_granules;
    }
    sort_stashed_segments();
  }

  void pop(ZArray<ZMemoryRange>* mappings, size_t num_mappings) {
    int stash_index = 0;
    for (int idx = mappings->length() - (int)num_mappings; idx < mappings->length(); idx++) {
      const ZMemoryRange& vmem = mappings->at(idx);
      const size_t num_granules = vmem.size_in_granules();
      const size_t granules_left = _stash.length() - stash_index;

      // If we run out of segments in the stash, we finish early
      if (num_granules >= granules_left) {
        memcpy(_physical_mappings->get_addr(vmem.start()), _stash.adr_at(stash_index), sizeof(zoffset) * granules_left);
        return;
      }

      memcpy(_physical_mappings->get_addr(vmem.start()), _stash.adr_at(stash_index), sizeof(zoffset) * num_granules);
      stash_index += (int)num_granules;
    }
  };
};

class ZPageAllocation : public StackObj {
  friend class ZList<ZPageAllocation>;

private:
  const ZPageType            _type;
  const size_t               _size;
  const ZAllocationFlags     _flags;
  const uint32_t             _young_seqnum;
  const uint32_t             _old_seqnum;
  const size_t               _current_max_capacity;
  size_t                     _harvested;
  size_t                     _committed;
  int                        _numa_id;
  ZArray<ZMemoryRange>     _claimed_mappings;
  ZListNode<ZPageAllocation> _node;
  ZFuture<bool>              _stall_result;

public:
  ZPageAllocation(ZPageType type, size_t size, ZAllocationFlags flags, size_t current_max_capacity)
    : _type(type),
      _size(size),
      _flags(flags),
      _young_seqnum(ZGeneration::young()->seqnum()),
      _old_seqnum(ZGeneration::old()->seqnum()),
      _current_max_capacity(current_max_capacity),
      _harvested(0),
      _committed(0),
      _numa_id(-1),
      _claimed_mappings(1),
      _node(),
      _stall_result() {}

  void reset_for_retry() {
    _harvested = 0;
    _committed = 0;
    _claimed_mappings.clear();
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

  void set_numa_id(int id) {
    _numa_id = id;
  }

  size_t current_max_capacity() const {
    return _current_max_capacity;
  }

  bool wait() {
    return _stall_result.get();
  }

  ZArray<ZMemoryRange>* claimed_mappings() {
    return &_claimed_mappings;
  }

  void satisfy(bool result) {
    _stall_result.set(result);
  }

  bool gc_relocation() const {
    return _flags.gc_relocation();
  }
};

class ZCacheState {
  friend class ZPageAllocator;

private:
  ZMappedCache               _cache;
  volatile size_t            _current_max_capacity;
  volatile size_t            _capacity;
  volatile size_t            _claimed;
  volatile size_t            _used;
  size_t                     _used_generations[2];
  struct {
    size_t                   _used_high;
    size_t                   _used_low;
  } _collection_stats[2];
  double                     _last_commit;
  double                     _last_uncommit;
  size_t                     _to_uncommit;

public:
  void initialize(size_t max_capacity);

  size_t available_capacity() const;
  size_t soft_max_capacity() const;

  size_t increase_capacity(size_t size);
  void decrease_capacity(size_t size, bool set_max_capacity);

  void increase_used(size_t size);
  void decrease_used(size_t size);

  void increase_used_generation(ZGenerationId id, size_t size);
  void decrease_used_generation(ZGenerationId id, size_t size);

  void reset_statistics(ZGenerationId id);

  bool claim_mapped_or_increase_capacity(ZPageAllocation* allocation);
  bool claim_physical(ZPageAllocation* allocation);
};

void ZCacheState::initialize(size_t max_capacity) {
  _current_max_capacity = max_capacity;
  _capacity = 0;
  _claimed = 0;
  _used = 0;

  for (int i = 0; i < 2; i++) {
    _used_generations[i] = 0;
    _collection_stats[i] = {0, 0};
  }

  _last_commit = 0.0;
  _last_uncommit = 0.0;
  _to_uncommit = 0;
}

size_t ZCacheState::available_capacity() const {
  return _current_max_capacity - _used - _claimed;
}

size_t ZCacheState::increase_capacity(size_t size) {
  const size_t increased = MIN2(size, _current_max_capacity - _capacity);

  if (increased > 0) {
    // Update atomically since we have concurrent readers
    Atomic::add(&_capacity, increased);

    _last_commit = os::elapsedTime();
    _last_uncommit = 0;
    _cache.reset_min();
  }

  return increased;
}

void ZCacheState::decrease_capacity(size_t size, bool set_max_capacity) {
  // Update state atomically since we have concurrent readers
  Atomic::sub(&_capacity, size);

  // Adjust current max capacity to avoid further attempts to increase capacity
  if (set_max_capacity) {
    Atomic::store(&_current_max_capacity, _capacity);
  }
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

bool ZCacheState::claim_mapped_or_increase_capacity(ZPageAllocation* allocation) {
  const size_t size = allocation->size();
  ZArray<ZMemoryRange>* mappings = allocation->claimed_mappings();

  // Try to allocate a contiguous mapping.
  ZMemoryRange mapping;
  if (_cache.remove_mapping_contiguous(&mapping, size)) {
    mappings->append(mapping);
    return true;
  }

  // If we've failed to allocate a contiguous range from the mapped cache,
  // there is still a possibility that the cache holds enough memory for the
  // allocation dispersed over more than one mapping if the capacity cannot be
  // increased to satisfy the allocation.

  // Try increase capacity
  const size_t increased = increase_capacity(size);
  if (increased == size) {
    // Capacity increase covered the entire request, done.
    return true;
  }

  // Could not increase capacity enough to satisfy the allocation completely.
  // Try removing multiple mappings from the mapped cache. We only remove if
  // cache has enough remaining to cover the request.
  const size_t remaining = size - increased;
  if (_cache.size() >= remaining) {
    const size_t removed = _cache.remove_mappings(mappings, remaining);
    allocation->set_harvested(removed);
    assert(removed == remaining, "must be %zu != %zu", removed, remaining);
    return true;
  }

  // Could not claim enough memory from the cache or increase capacity to
  // fulfill the request.
  return false;
}

bool ZCacheState::claim_physical(ZPageAllocation* allocation) {
  const size_t size = allocation->size();

  if (available_capacity() < size) {
    // Out of memory
    return false;
  }

  if (!claim_mapped_or_increase_capacity(allocation)) {
    // Failed to claim enough memory or increase capacity
    return false;
  }

  // Updated used statistics
  increase_used(size);

  // Success
  return true;
}

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
    _states(),
    _stalled(),
    _committer(new ZCommitter(this)),
    _uncommitter(new ZUncommitter(this)),
    _safe_destroy(),
    _initialized(false) {

  if (!_virtual.is_initialized() || !_physical.is_initialized()) {
    return;
  }

  ZNUMA::divide_resource(static_max_capacity, [&](uint32_t numa_id, size_t capacity) {
    _states.get(numa_id).initialize(capacity);
  });

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

  ZMemoryRange vmem = _virtual.alloc(to_prime, numa_id, true /* force_low_address */);
  ZCacheState& state = _states.get(numa_id);

  // Increase capacity, allocate and commit physical memory
  state.increase_capacity(to_prime);
  _physical.alloc(_physical_mappings.get_addr(vmem.start()), to_prime, numa_id);
  if (!commit_physical(&vmem, numa_id)) {
    // This is a failure state. We do not cleanup the maybe partially committed memory.
    return false;
  }

  map_virtual_to_physical(vmem, numa_id);

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

  // We don't have to take a lock here as no other threads will access the
  // mapped cache until we're finished.
  state._cache.insert_mapping(vmem);

  return true;
}

bool ZPageAllocator::prime_cache(ZWorkers* workers, size_t size) {
  for (uint32_t numa_id = 0; numa_id < ZNUMA::count(); numa_id++) {
    const size_t to_prime = ZNUMA::calculate_share(numa_id, size);
    if (!prime_state_cache(workers, numa_id, to_prime)) {
      return false;
    }
  }

  return true;
}

bool ZPageAllocator::prime_alloc_page(size_t size) {
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
  if (ZAdaptiveHeap::explicit_max_capacity()) {
    return _static_max_capacity;
  }

  size_t max = align_down(size_t(os::physical_memory() * (1.0 - ZMemoryCriticalThreshold)), ZGranuleSize);
  return MAX2(max, _min_capacity);
}

size_t ZPageAllocator::current_max_capacity() const {
  if (ZAdaptiveHeap::explicit_max_capacity()) {
    return _static_max_capacity;
  }

  const size_t capacity = Atomic::load(&_capacity);

  // Calculate current max capacity based on machine usage
  return ZAdaptiveHeap::current_max_capacity(capacity, dynamic_max_capacity());
}

size_t ZPageAllocator::heuristic_max_capacity() const {
  // Note that SoftMaxHeapSize is a manageable flag
  const size_t soft_max_capacity = Atomic::load(&SoftMaxHeapSize);
  const size_t heuristic_max_capacity = Atomic::load(&_heuristic_max_capacity);
  const size_t lowest_soft_capacity = soft_max_capacity == 0 ? heuristic_max_capacity
                                                             : MIN2(soft_max_capacity, heuristic_max_capacity);
  const size_t curr_max_capacity = current_max_capacity();
  return MIN2(lowest_soft_capacity, curr_max_capacity);
}

void ZPageAllocator::adapt_heuristic_max_capacity(ZGenerationId generation) {
  const size_t soft_max_capacity = Atomic::load(&SoftMaxHeapSize);
  const size_t heuristic_max_capacity = Atomic::load(&_heuristic_max_capacity);
  const size_t min_capacity = _min_capacity;
  const size_t used = Atomic::load(&_used);
  const size_t capacity = MAX2(Atomic::load(&_capacity), used);
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

  Atomic::store(&_heuristic_max_capacity, selected_capacity);

  // Complain about misconfigurations
  _physical.warn_commit_limits(selected_capacity, dynamic_max_capacity());

  _committer->heap_resized(capacity, selected_capacity);
}

size_t ZPageAllocator::capacity() const {
  size_t capacity = 0;
  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    capacity += Atomic::load(&state->_capacity);
  }
  return capacity;
}

size_t ZPageAllocator::used() const {
  size_t used = 0;
  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    used += Atomic::load(&state->_used);
  }
  return used;
}

size_t ZPageAllocator::used_generation(ZGenerationId id) const {
  size_t used_generation = 0;
  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    used_generation += Atomic::load(&state->_used_generations[(int)id]);
  }
  return used_generation;
}

size_t ZPageAllocator::unused() const {
  ssize_t capacity = 0;
  ssize_t used = 0;
  ssize_t claimed = 0;

  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    capacity += (ssize_t)Atomic::load(&state->_capacity);
    used += (ssize_t)Atomic::load(&state->_used);
    claimed += (ssize_t)Atomic::load(&state->_claimed);
  }

  const ssize_t unused = capacity - used - claimed;
  return unused > 0 ? (size_t)unused : 0;
}

ZPageAllocatorStats ZPageAllocator::stats(ZGeneration* generation) const {
  ZLocker<ZLock> locker(&_lock);

  ZPageAllocatorStats stats(_min_capacity,
                            heuristic_max_capacity(),
                            generation->freed(),
                            generation->promoted(),
                            generation->compacted(),
                            _stalled.size());

  const int gen_id = (int)generation->id();
  size_t current_max_capacity = 0;
  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    current_max_capacity += state->_current_max_capacity;
    stats.increment_stats(state->_capacity,
                          state->_used,
                          state->_collection_stats[gen_id]._used_high,
                          state->_collection_stats[gen_id]._used_low,
                          state->_used_generations[gen_id]);
  }

  // We can only calculate the soft_max_capcity after finding the combined value
  // for the current_max_capacity, so we set it after constructing the stats object.
  const size_t soft_max_heapsize = Atomic::load(&SoftMaxHeapSize);
  if (current_max_capacity > soft_max_heapsize) {
    stats.set_soft_max_capacity(current_max_capacity);
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

ZCacheState& ZPageAllocator::state_from_vmem(const ZMemoryRange& vmem) {
  return _states.get(_virtual.get_numa_id(vmem));
}

void ZPageAllocator::promote_used(const ZMemoryRange& from, const ZMemoryRange& to) {
  const size_t size = from.size();
  state_from_vmem(from).decrease_used_generation(ZGenerationId::young, size);
  state_from_vmem(to).increase_used_generation(ZGenerationId::old, size);
}

size_t ZPageAllocator::count_segments_physical(const ZMemoryRange& vmem) {
  return _physical.count_segments(_physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::sort_segments_physical(const ZMemoryRange& vmem) {
  sort_zoffset_ptrs(_physical_mappings.get_addr(vmem.start()), vmem.size_in_granules());
}

void ZPageAllocator::alloc_physical(const ZMemoryRange& vmem, int numa_id) {
  _physical.alloc(_physical_mappings.get_addr(vmem.start()), vmem.size(), numa_id);
}

void ZPageAllocator::free_physical(const ZMemoryRange& vmem, int numa_id) {
  // Free physical memory
  _physical.free(_physical_mappings.get_addr(vmem.start()), vmem.size(), numa_id);
}

bool ZPageAllocator::commit_physical(ZMemoryRange* vmem, int numa_id) {
  // Commit physical memory
  const size_t committed = _physical.commit(_physical_mappings.get_addr(vmem->start()), vmem->size(), numa_id);
  const size_t not_commited = vmem->size() - committed;

  if (not_commited > 0) {
    // Free the uncommitted memory and update vmem with the committed memory
    ZMemoryRange not_commited_vmem = *vmem;
    *vmem = not_commited_vmem.split_from_front(committed);
    free_physical(not_commited_vmem, numa_id);
    free_virtual(not_commited_vmem);
    return false;
  }

  return true;
}

void ZPageAllocator::uncommit_physical(const ZMemoryRange& vmem) {
  assert(ZUncommit, "should not uncommit when uncommit is disabled");

  // Uncommit physical memory
  _physical.uncommit(_physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::heat_memory(zoffset start, size_t size) const {
  const zaddress addr = ZOffset::address(start);
  pretouch(addr, size);
  if (ZLargePages::is_collapse()) {
    _physical.collapse(start, size);
  }
}

void ZPageAllocator::map_virtual_to_physical(const ZMemoryRange& vmem, int numa_id) {
  // Map virtual memory to physical memory
  _physical.map(vmem.start(), _physical_mappings.get_addr(vmem.start()), vmem.size(), numa_id);
}

void ZPageAllocator::unmap_virtual(const ZMemoryRange& vmem) {
  // Make sure we don't try to pretouch unmapped pages
  _committer->remove_heating_request(page);

  // Unmap virtual memory from physical memory
  _physical.unmap(vmem.start(), _physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::free_virtual(const ZMemoryRange& vmem) {
  // Free virtual memory
  _virtual.free(vmem);
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
  const int num_ranges = _virtual.shuffle_vmem_to_low_addresses(vmem, entries);

  // Restore segments
  segments.pop(entries, num_ranges);

  // The entries array may contain entries from other defragmentations as well,
  // so we only operate on the last ranges that we have just inserted
  const int numa_id = _virtual.get_numa_id(vmem);
  for (int idx = entries->length() - num_ranges; idx < entries->length(); idx++) {
    const ZMemoryRange v = entries->at(idx);
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

bool ZPageAllocator::claim_physical_round_robin(ZPageAllocation* allocation) {
  const int start_node = allocation->numa_id();
  int current_node = start_node;

  do {
    ZCacheState& state = _states.get(current_node);
    if (state.claim_physical(allocation)) {
      // Record which state the allocation was made on
      allocation->set_numa_id(current_node);
      return true;
    }

    // Could not claim physical memory on current node, move on to next node
    current_node = (current_node + 1) % ZNUMA::count();
  } while(current_node != start_node);

  return false;
}

bool ZPageAllocator::claim_physical_or_stall(ZPageAllocation* allocation) {
  {
    ZLocker<ZLock> locker(&_lock);

    // Always start at the current thread's affinity for local allocation
    allocation->set_numa_id(ZNUMA::id());
    if (claim_physical_round_robin(allocation)) {
      return true;
    }

    // Failed to claim memory
    if (allocation->flags().non_blocking()) {
      // Don't stall
      return false;
    }

    // Enqueue allocation request
    _stalled.insert_last(allocation);
  }

  // Stall
  return alloc_page_stall(allocation);
}

void ZPageAllocator::harvest_claimed_physical(ZPageAllocation* allocation) {
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

void ZPageAllocator::request_alloc_heating(ZPage* page, ZPageAllocation* allocation) {
}

bool ZPageAllocator::claim_virtual_memory(ZPageAllocation* allocation) {
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

bool ZPageAllocator::commit_and_map_memory(ZPageAllocation* allocation, const ZMemoryRange& vmem, size_t committed_size) {
  ZMemoryRange to_be_committed_vmem = vmem;
  ZMemoryRange committed_vmem = to_be_committed_vmem.split_from_front(committed_size);

  // Try to commit all physical memory, commit_physical frees both the virtual
  // and physical parts that correspond to the memory that failed to be committed.
  commit_physical(&to_be_committed_vmem, allocation->numa_id());
  committed_vmem.grow_from_back(to_be_committed_vmem.size());

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
retry:
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
    const ZMemoryRange vmem = allocation->claimed_mappings()->pop();
    return new ZPage(allocation->type(), vmem);
  }

  // Claim virtual memory, either by harvesting or by allocating from the
  // virtual manager.
  if (!claim_virtual_memory(allocation)) {
    log_error(gc)("Out of address space");
    free_memory_alloc_failed(allocation);
    return nullptr;
  }

  const ZMemoryRange vmem = allocation->claimed_mappings()->pop();

  // Allocate any remaining physical memory. Capacity and used has already been
  // adjusted, we just need to fetch the memory, which is guaranteed to succeed.
  const size_t remaining_physical = allocation->size() - allocation->harvested();
  if (remaining_physical > 0) {
    allocation->set_committed(remaining_physical);
    const ZMemoryRange uncommitted_range = ZMemoryRange(vmem.start() + allocation->harvested(), remaining_physical);
    alloc_physical(uncommitted_range, allocation->numa_id());
  }

  if (!commit_and_map_memory(allocation, vmem, allocation->harvested())) {
    free_memory_alloc_failed(allocation);
    goto retry;
  }

  return new ZPage(allocation->type(), vmem);
}

void ZPageAllocator::alloc_page_age_update(ZPage* page, size_t size, ZPageAge age, int numa_id) {
  // The generation's used is tracked here when the page is handed out
  // to the allocating thread. The overall heap "used" is tracked in
  // the lower-level allocation code.
  const ZGenerationId id = age == ZPageAge::old ? ZGenerationId::old : ZGenerationId::young;
  _states.get(numa_id).increase_used_generation(id, size);

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

  alloc_page_age_update(page, size, age, allocation.numa_id());

  // Update allocation statistics. Exclude gc relocations to avoid
  // artificial inflation of the allocation rate during relocation.
  if (!flags.gc_relocation() && is_init_completed()) {
    // Note that there are two allocation rate counters, which have
    // different purposes and are sampled at different frequencies.
    ZStatInc(ZCounterMutatorAllocationRate, size);
    ZStatMutatorAllocRate::sample_allocation(size);
  }

  // Send event
  event.commit((u8)type, size, allocation.harvested(), allocation.committed(),
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

void ZPageAllocator::free_page(ZPage* page, bool allow_defragment) {
  ZArray<ZMemoryRange> to_cache;

  ZGenerationId id = page->generation_id();
  ZCacheState& state = state_from_vmem(page->virtual_memory());
  prepare_memory_for_free(page, &to_cache, allow_defragment);

  ZLocker<ZLock> locker(&_lock);

  ZArrayIterator<ZMemoryRange> iter(&to_cache);
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    // Update used statistics and cache memory
    state.decrease_used(vmem.size());
    state.decrease_used_generation(id, vmem.size());
    state._cache.insert_mapping(vmem);
  }

  // Try satisfy stalled allocations
  satisfy_stalled();
}

struct ZGenStats {
  size_t young_size;
  size_t old_size;
};

void ZPageAllocator::free_pages(const ZArray<ZPage*>* pages) {
  ZArray<ZMemoryRange> to_cache;

  // All pages belong to the same generation, so either only young or old.
  const ZGenerationId gen_id = pages->first()->generation_id();
  size_t gen_size = 0;

  // Prepare memory from pages to be cached before taking the lock
  ZArrayIterator<ZPage*> pages_iter(pages);
  for (ZPage* page; pages_iter.next(&page);) {
    gen_size += page->size();
    prepare_memory_for_free(page, &to_cache, true /* allow_defragment */);
  }

  ZLocker<ZLock> locker(&_lock);

  // Insert mappings to the cache
  ZArrayIterator<ZMemoryRange> iter(&to_cache);
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    ZCacheState& state = state_from_vmem(vmem);
    state.decrease_used(vmem.size());
    state._cache.insert_mapping(vmem);
  }

  for (int numa_id = 0; numa_id < (int)ZNUMA::count(); numa_id++) {
    ZCacheState& state = _states.get(numa_id);
    state.decrease_used_generation(gen_id, gen_size);
  }

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::free_memory_alloc_failed(ZPageAllocation* allocation) {
  ZLocker<ZLock> locker(&_lock);
  ZCacheState& state = _states.get(allocation->numa_id());

  // Only decrease the overall used and not the generation used,
  // since the allocation failed and generation used wasn't bumped.
  state.decrease_used(allocation->size());

  size_t freed = 0;

  // Free mapped memory
  ZArrayIterator<ZMemoryRange> iter(allocation->claimed_mappings());
  for (ZMemoryRange vmem; iter.next(&vmem);) {
    freed += vmem.size();
    state._cache.insert_mapping(vmem);
  }

  // Adjust capacity to reflect the failed capacity increase
  const size_t remaining = allocation->size() - freed;
  if (remaining > 0) {
    state.decrease_capacity(remaining, true /* set_max_capacity */);
    log_error_p(gc)("Forced to lower max Java heap size from "
                    "%zuM(%.0f%%) to %zuM(%.0f%%) (NUMA id %d)",
                    state._current_max_capacity / M, percent_of(state._current_max_capacity, _max_capacity),
                    state._capacity / M, percent_of(state._capacity, _max_capacity),
                    allocation->numa_id());
  }

  // Reset allocation for a potential retry
  allocation->reset_for_retry();

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::adjust_capacity(size_t used_soon) {
}
size_t ZPageAllocator::uncommit(uint64_t* timeout) {
  // We need to join the suspendible thread set while manipulating capacity and
  // used, to make sure GC safepoints will have a consistent view.
  ZList<ZPage> pages;
  size_t flushed;

  {
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&_lock);

    for (int numa_id = 0; numa_id < numa_nodes; numa_id++) {
      ZCacheState& state = _states.get(numa_id);

      const double now = os::elapsedTime();
      const double time_since_last_commit = std::floor(now - state._last_commit);
      const double time_since_last_uncommit = std::floor(now - state._last_uncommit);

      if (time_since_last_commit < double(ZUncommitDelay)) {
        // We have committed within the delay, stop uncommitting.
        lowest_timeout = MIN2(lowest_timeout, uint64_t(double(ZUncommitDelay) - time_since_last_commit));
        continue;
      }

      const size_t limit = MIN2(align_up(state._current_max_capacity >> 7, ZGranuleSize), 256 * M / ZNUMA::count());

      if (time_since_last_uncommit < double(ZUncommitDelay)) {
        // We are in the uncommit phase
        const size_t num_uncommits_left = state._to_uncommit / limit;
        const double time_left = double(ZUncommitDelay) - time_since_last_uncommit;
        if (time_left < *timeout * num_uncommits_left) {
          // Running out of time, speed up.
          uint64_t new_timeout = uint64_t(std::floor(time_left / double(num_uncommits_left + 1)));
          lowest_timeout = MIN2(lowest_timeout, new_timeout);
        }
      } else {
        // We are about to start uncommitting
        state._to_uncommit = state._cache.reset_min();
        state._last_uncommit = now;

        const size_t split = state._to_uncommit / limit + 1;
        uint64_t new_timeout = ZUncommitDelay / split;
        lowest_timeout = MIN2(lowest_timeout, new_timeout);
      }

      // Never uncommit below min capacity. We flush out and uncommit chunks at
      // a time (~0.8% of the max capacity, but at least one granule and at most
      // 256M), in case demand for memory increases while we are uncommitting.
      const size_t retain = MAX2(state._used, _min_capacity / ZNUMA::count());
      const size_t release = state._capacity - retain;
      const size_t flush = MIN3(release, limit, state._to_uncommit);

      if (flush == 0) {
      // Nothing to flush
        continue;
      }

      // Flush memory from the mapped cache to uncommit
      const size_t flushed = state._cache.remove_from_min(&flushed_mappings, flush);
      if (flushed == 0) {
        // Nothing flushed
        continue;
      }

      // Record flushed memory as claimed and how much we've flushed for this NUMA node
      Atomic::add(&state._claimed, flushed);
      state._to_uncommit -= flushed;
      flushed_per_numa.set(flushed, numa_id);
    }
  }

  *timeout = lowest_timeout;

  // Unmap and uncommit flushed memory
  ZArrayIterator<ZMemoryRange> it(&flushed_mappings);
  for (ZMemoryRange vmem; it.next(&vmem);) {
    const int numa_id = _virtual.get_numa_id(vmem);
    unmap_virtual(vmem);
    uncommit_physical(vmem);
    free_physical(vmem, numa_id);
    free_virtual(vmem);
  }

  size_t total_flushed = 0;

  {
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&_lock);

    for (int numa_id = 0; numa_id < numa_nodes; numa_id++) {
      const size_t flushed = flushed_per_numa.get(numa_id);
      if (flushed == 0) {
        continue;
      }

    for (int numa_id = 0; numa_id < numa_nodes; numa_id++) {
      const size_t flushed = flushed_per_numa.get(numa_id);
      if (flushed == 0) {
        continue;
      }

      // Adjust claimed and capacity to reflect the uncommit
      ZCacheState& state = _states.get(numa_id);
      Atomic::sub(&state._claimed, flushed);
      state.decrease_capacity(flushed, false /* set_max_capacity */);
      total_flushed += flushed;
  }

  return total_flushed;
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
  tc->do_thread(_committer);
  tc->do_thread(_uncommitter);
}
