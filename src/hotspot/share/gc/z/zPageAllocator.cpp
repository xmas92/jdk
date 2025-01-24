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

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zFuture.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zNUMA.hpp"
#include "gc/z/zPage.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zPageAllocator.inline.hpp"
#include "gc/z/zSafeDelete.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncommitter.hpp"
#include "gc/z/zUnmapper.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "gc/z/zWorkers.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <cmath>

static const ZStatCounter       ZCounterMutatorAllocationRate("Memory", "Allocation Rate", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterDefragment("Memory", "Defragment", ZStatUnitOpsPerSecond);
static const ZStatCriticalPhase ZCriticalPhaseAllocationStall("Allocation Stall");

class ZPageAllocation : public StackObj {
  friend class ZList<ZPageAllocation>;

private:
  const ZPageType            _type;
  const size_t               _size;
  const ZAllocationFlags     _flags;
  const uint32_t             _young_seqnum;
  const uint32_t             _old_seqnum;
  size_t                     _harvested;
  size_t                     _committed;
  int                        _numa_id;
  ZArray<ZVirtualMemory>     _claimed_mappings;
  ZListNode<ZPageAllocation> _node;
  ZFuture<bool>              _stall_result;

public:
  ZPageAllocation(ZPageType type, size_t size, ZAllocationFlags flags)
    : _type(type),
      _size(size),
      _flags(flags),
      _young_seqnum(ZGeneration::young()->seqnum()),
      _old_seqnum(ZGeneration::old()->seqnum()),
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

  bool wait() {
    return _stall_result.get();
  }

  ZArray<ZVirtualMemory>* claimed_mappings() {
    return &_claimed_mappings;
  }

  void satisfy(bool result) {
    _stall_result.set(result);
  }

  bool gc_relocation() const {
    return _flags.gc_relocation();
  }
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

size_t ZCacheState::available_memory() const {
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

ZPageAllocator::ZPageAllocator(size_t min_capacity,
                               size_t initial_capacity,
                               size_t soft_max_capacity,
                               size_t max_capacity)
  : _lock(),
    _virtual(max_capacity),
    _physical(max_capacity),
    _physical_mappings(ZAddressOffsetMax),
    _min_capacity(min_capacity),
    _initial_capacity(initial_capacity),
    _max_capacity(max_capacity),
    _states(),
    _stalled(),
    _unmapper(new ZUnmapper(this)),
    _uncommitter(new ZUncommitter(this)),
    _safe_destroy(),
    _initialized(false) {

  if (!_virtual.is_initialized() || !_physical.is_initialized()) {
    return;
  }

  const int numa_nodes = ZNUMA::count();
  const size_t capacity_per_state = align_up(max_capacity / numa_nodes, ZGranuleSize);
  size_t capacity_left = max_capacity;

  for (int numa_id = 0; numa_id < numa_nodes; numa_id++) {
    const size_t capacity = MIN2(capacity_per_state, capacity_left);
    capacity_left -= capacity;

    ZCacheState& state = _states.get(numa_id);
    state.initialize(capacity);
    _physical.install_capacity(numa_id, zoffset(capacity_per_state * numa_id), capacity);
  }

  log_info_p(gc, init)("Min Capacity: %zuM", min_capacity / M);
  log_info_p(gc, init)("Initial Capacity: %zuM", initial_capacity / M);
  log_info_p(gc, init)("Max Capacity: %zuM", max_capacity / M);
  log_info_p(gc, init)("Soft Max Capacity: %zuM", soft_max_capacity / M);
  if (ZPageSizeMedium > 0) {
    log_info_p(gc, init)("Medium Page Size: %zuM", ZPageSizeMedium / M);
  } else {
    log_info_p(gc, init)("Medium Page Size: N/A");
  }
  log_info_p(gc, init)("Pre-touch: %s", AlwaysPreTouch ? "Enabled" : "Disabled");

  // Warn if system limits could stop us from reaching max capacity
  _physical.warn_commit_limits(max_capacity);

  // Check if uncommit should and can be enabled
  _physical.try_enable_uncommit(min_capacity, max_capacity);

  // Successfully initialized
  _initialized = true;
}

bool ZPageAllocator::is_initialized() const {
  return _initialized;
}

class ZPreTouchTask : public ZTask {
private:
  volatile uintptr_t _current;
  const uintptr_t    _end;

  static void pretouch(zaddress zaddr, size_t size) {
    const uintptr_t addr = untype(zaddr);
    const size_t page_size = ZLargePages::is_explicit() ? ZGranuleSize : os::vm_page_size();
    os::pretouch_memory((void*)addr, (void*)(addr + size), page_size);
  }

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
      const zaddress addr = ZOffset::address(offset);

      // Pre-touch the granule
      pretouch(addr, size);
    }
  }
};

bool ZPageAllocator::prime_cache(ZWorkers* workers, size_t size) {
  const int numa_nodes = ZNUMA::count();
  const size_t size_per_state = align_up(size / numa_nodes, ZGranuleSize);
  size_t size_left = size;

  for (int numa_id = 0; numa_id < numa_nodes; numa_id++) {
    const size_t to_prime = MIN2(size_left, size_per_state);
    size_left -= to_prime;

    if (to_prime == 0) {
      // Nothing more to prime, exit
      return true;
    }

    ZCacheState& state = _states.get(numa_id);
    ZVirtualMemory vmem = _virtual.alloc(to_prime, numa_id, true /* force_low_address */);

    // Increase capacity, allocate and commit physical memory
    state.increase_capacity(to_prime);
    _physical.alloc(_physical_mappings.get_addr(vmem.start()), to_prime, numa_id);
    if (!commit_physical(&vmem, numa_id)) {
      // This is a failure state. We do not cleanup the maybe partially committed memory.
      return false;
    }

    map_virtual_to_physical(vmem);

    if (AlwaysPreTouch) {
      // Pre-touch memory
      ZPreTouchTask task(vmem.start(), vmem.end());
      workers->run_all(&task);
    }

    // We don't have to take a lock here as no other threads will access the
    // mapped cache until we're finished.
    state._cache.insert_mapping(vmem);
  }

  return true;
}

size_t ZPageAllocator::initial_capacity() const {
  return _initial_capacity;
}

size_t ZPageAllocator::min_capacity() const {
  return _min_capacity;
}

size_t ZPageAllocator::max_capacity() const {
  return _max_capacity;
}

size_t ZPageAllocator::soft_max_capacity() const {
  size_t current_max_capacity = 0;
  ZPerNUMAConstIterator<ZCacheState> iter(&_states);
  for (const ZCacheState* state; iter.next(&state);) {
    current_max_capacity += Atomic::load(&state->_current_max_capacity);
  }

  const size_t soft_max_heapsize = Atomic::load(&SoftMaxHeapSize);
  return MIN2(soft_max_heapsize, current_max_capacity);
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
                            _max_capacity,
                            0,
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

ZCacheState& ZPageAllocator::state_from_vmem(const ZVirtualMemory& vmem) {
  return _states.get(_virtual.get_numa_id(vmem));
}

void ZPageAllocator::promote_used(const ZVirtualMemory& from, const ZVirtualMemory& to) {
  const size_t size = from.size();
  state_from_vmem(from).decrease_used_generation(ZGenerationId::young, size);
  state_from_vmem(to).increase_used_generation(ZGenerationId::old, size);
}

size_t ZPageAllocator::count_segments_physical(const ZVirtualMemory& vmem) {
  return _physical.count_segments(_physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::copy_physical(const ZVirtualMemory& from, zoffset to) {
  const size_t num_granules = from.size() >> ZGranuleSizeShift;
  memcpy(_physical_mappings.get_addr(to), _physical_mappings.get_addr(from.start()), sizeof(zoffset) * num_granules);
}

void ZPageAllocator::free_physical(const ZVirtualMemory& vmem, int numa_id) {
  // Free physical memory
  _physical.free(_physical_mappings.get_addr(vmem.start()), vmem.size(), numa_id);
}

bool ZPageAllocator::commit_physical(ZVirtualMemory* vmem, int numa_id) {
  // Commit physical memory
  const size_t committed = _physical.commit(_physical_mappings.get_addr(vmem->start()), vmem->size(), numa_id);
  const size_t not_commited = vmem->size() - committed;

  if (not_commited > 0) {
    // Free the uncommitted memory and update vmem with the committed memory
    ZVirtualMemory not_commited_vmem = *vmem;
    *vmem = not_commited_vmem.split(committed);
    free_physical(not_commited_vmem, numa_id);
    free_virtual(not_commited_vmem);
    return false;
  }

  return true;
}

void ZPageAllocator::uncommit_physical(const ZVirtualMemory& vmem) {
  precond(ZUncommit);

  // Uncommit physical memory
  _physical.uncommit(_physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::map_virtual_to_physical(const ZVirtualMemory& vmem) {
  // Map virtual memory to physical memory
  _physical.map(vmem.start(), _physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::unmap_virtual(const ZVirtualMemory& vmem) {
  // Unmap virtual memory from physical memory
  _physical.unmap(vmem.start(), _physical_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::free_virtual(const ZVirtualMemory& vmem) {
  // Free virtual memory
  _virtual.free(vmem);
}

bool ZPageAllocator::should_defragment(const ZVirtualMemory& vmem) const {
  // Get NUMA id associated with the vmem
  const int numa_id = _virtual.get_numa_id(vmem);

  // Small and medium pages are allocated at a low address. They may end up at a
  // high address (second half of the address space) if we have a constrained
  // address space. To combat address space fragmentation we want to attemp to
  // remap such memory to a lower address.
  return vmem.start() >= _virtual.half_available_space(numa_id) &&
         vmem.start() >= _virtual.lowest_available_address(numa_id);
}

ZVirtualMemory ZPageAllocator::remap_mapping(const ZVirtualMemory& vmem, bool force_low_address) {
  // Allocate new virtual memory
  const int numa_id = _virtual.get_numa_id(vmem);
  const ZVirtualMemory new_vmem = _virtual.alloc(vmem.size(), numa_id, force_low_address);

  if (new_vmem.is_null()) {
    // Failed to allocate new virtual address space, do nothing
    return vmem;
  }

  // Copy the physical mappings
  copy_physical(vmem, new_vmem.start());

  // Unmap the previous mapping asynchronously
  _unmapper->unmap_virtual(vmem);

  // Map the copied physical segments.
  map_virtual_to_physical(new_vmem);

  return new_vmem;
}

bool ZPageAllocator::claim_mapped_or_increase_capacity(ZCacheState& state, size_t size, ZArray<ZVirtualMemory>* mappings) {
  ZMappedCache& cache = state._cache;

  // Try to allocate a contiguous mapping.
  ZVirtualMemory mapping;
  if (cache.remove_mapping_contiguous(&mapping, size)) {
    mappings->append(mapping);
    return true;
  }

  // If we've failed to allocate a contiguous range from the mapped cache,
  // there is still a possibility that the cache holds enough memory for the
  // allocation dispersed over more than one mapping if the capacity cannot be
  // increased to satisfy the allocation.

  // Try increase capacity
  const size_t increased = state.increase_capacity(size);
  if (increased == size) {
    // Capacity increase covered the entire request, done.
    return true;
  }

  // Could not increase capacity enough to satisfy the allocation completely.
  // Try removing multiple mappings from the mapped cache. We only remove if
  // cache has enough remaining to cover the request.
  const size_t remaining = size - increased;
  if (cache.size() >= remaining) {
    const size_t removed = cache.remove_mappings(mappings, remaining);
    assert(removed == remaining, "must be %zu != %zu", removed, remaining);
    return true;
  }

  // Could not claim enough memory from the cache or increase capacity to
  // fulfill the request.
  return false;
}

bool ZPageAllocator::claim_physical(ZPageAllocation* allocation, ZCacheState& state) {
  const size_t size = allocation->size();
  ZArray<ZVirtualMemory>* const mappings = allocation->claimed_mappings();

  if (state.available_memory() < size) {
    // Out of memory
    return false;
  }

  // Try to claim physical memory
  if (!claim_mapped_or_increase_capacity(state, size, mappings)) {
    // Failed to claim enough memory or increase capacity
    return false;
  }

  // Updated used statistics
  state.increase_used(size);

  // Success
  return true;
}

bool ZPageAllocator::claim_physical_round_robin(ZPageAllocation* allocation) {
  const size_t numa_nodes = ZNUMA::count();
  const int start_node = allocation->numa_id();
  int current_node = start_node;

  do {
    ZCacheState& state = _states.get(current_node);

    if (claim_physical(allocation, state)) {
      // Success
      allocation->set_numa_id(current_node);
      return true;
    }

    // Could not claim physical memory on current node, potentially move on to
    // the next node
    current_node = (current_node + 1) % numa_nodes;
  } while(current_node != start_node);

  return false;
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

void ZPageAllocator::harvest_claimed_physical(const ZVirtualMemory& new_vmem, ZPageAllocation* allocation) {
  size_t harvested = 0;
  const int num_mappings_harvested = allocation->claimed_mappings()->length();

  ZArrayIterator<ZVirtualMemory> iter(allocation->claimed_mappings());
  for (ZVirtualMemory vmem; iter.next(&vmem);) {
    copy_physical(vmem, new_vmem.start() + harvested);
    harvested += vmem.size();
    _unmapper->unmap_virtual(vmem);
  }

  // Clear the array of stored mappings
  allocation->claimed_mappings()->clear();

  if (harvested > 0) {
    allocation->set_harvested(harvested);
    log_debug(gc, heap)("Mapped Cache Harvest: %zuM from %d mappings", harvested / M, num_mappings_harvested);
  }
}

bool ZPageAllocator::is_alloc_satisfied(ZPageAllocation* allocation) const {
  // The allocation is immediately satisfied if the list of mappings contains
  // exactly one mapping and is of the correct size.
  if (allocation->claimed_mappings()->length() != 1) {
    // Not a contiguous mapping
    return false;
  }

  const ZVirtualMemory& vmem = allocation->claimed_mappings()->first();
  if (vmem.size() != allocation->size()) {
    // Not correct sized mapping
    return false;
  }

  // Allocation immediately satisfied
  return true;
}

bool ZPageAllocator::commit_and_map_memory(ZPageAllocation* allocation, const ZVirtualMemory& vmem, size_t committed_size) {
  ZVirtualMemory to_be_committed_vmem = vmem;
  ZVirtualMemory committed_vmem = to_be_committed_vmem.split(committed_size);

  // Try to commit all physical memory, commit_physical frees both the virtual
  // and physical parts that correspond to the memory that failed to be committed.
  commit_physical(&to_be_committed_vmem, allocation->numa_id());
  committed_vmem.extend(to_be_committed_vmem.size());

  // We have not managed to get any committed memory at all, meaning this allocation
  // failed to commit memory on capacity increase alone and nothing harvested.
  if (committed_vmem.size() == 0)  {
    return false;
  }

  // Sort the backing memory before mapping
  qsort(_physical_mappings.get_addr(committed_vmem.start()), committed_vmem.size() >> ZGranuleSizeShift, sizeof(zoffset),
        [](const void* a, const void* b) -> int {
          return *static_cast<const zoffset*>(a) < *static_cast<const zoffset*>(b) ? -1 : 1;
        });
  map_virtual_to_physical(committed_vmem);
  allocation->claimed_mappings()->append(committed_vmem);

  if (committed_vmem.size() != vmem.size()) {
    log_trace(gc, page)("Split memory [" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT "]",
        untype(committed_vmem.start()),
        untype(committed_vmem.end()),
        untype(vmem.end()));
    return false;
  }

  log_trace(gc, heap)("Committed memory at 0x%lx (NUMA preferred=%d actual=%d)",
      untype(vmem.start()),
      _virtual.get_numa_id(vmem),
      ZNUMA::memory_id(untype(ZOffset::address(vmem.start()))));

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

  // If the claimed physical memory holds a large enough contiguous virtual
  // address range, we're done.
  if (is_alloc_satisfied(allocation)) {
    ZVirtualMemory vmem = allocation->claimed_mappings()->pop();
    return new ZPage(allocation->type(), vmem);
  }

  // We need to allocate a new virtual address range and make sure the claimed
  // physical memory is committed and mapped to the same virtual address range.
  ZVirtualMemory vmem = _virtual.alloc(allocation->size(), allocation->numa_id(), false /* force_low_address */);
  if (vmem.is_null()) {
    log_error(gc)("Out of address space");
    free_memory_alloc_failed(allocation);
    return nullptr;
  }

  harvest_claimed_physical(vmem, allocation);
  const size_t remaining_physical = allocation->size() - allocation->harvested();

  // Allocate any remaining physical memory. Capacity and used has already been
  // adjusted, we just need to fetch the memory, which is guaranteed to succeed.
  if (remaining_physical > 0) {
    allocation->set_committed(remaining_physical);
    zoffset* mapping_addr = _physical_mappings.get_addr(vmem.start() + allocation->harvested());
    _physical.alloc(mapping_addr, remaining_physical, allocation->numa_id());
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
  ZCacheState& state = _states.get(numa_id);
  state.increase_used_generation(id, size);

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

void ZPageAllocator::free_page(ZPage* page, bool allow_defragment) {
  // Extract memory and destroy page
  ZVirtualMemory vmem = page->virtual_memory();
  const ZGenerationId generation_id = page->generation_id();
  const ZPageType page_type = page->type();
  safe_destroy_page(page);

  // Perhaps remap mapping
  if (page_type == ZPageType::large ||
     (allow_defragment && should_defragment(vmem))) {
    ZStatInc(ZCounterDefragment);
    vmem = remap_mapping(vmem, true /* force_low_address */);
  }

  ZLocker<ZLock> locker(&_lock);
  ZCacheState& state = state_from_vmem(vmem);

  // Update used statistics and cache memory
  state.decrease_used(vmem.size());
  state.decrease_used_generation(generation_id, vmem.size());
  state._cache.insert_mapping(vmem);

  // Try satisfy stalled allocations
  satisfy_stalled();
}

struct ZToFreeEntry {
  ZVirtualMemory vmem;
  ZGenerationId generation_id;
};

void ZPageAllocator::free_pages(const ZArray<ZPage*>* pages) {
  ZArray<ZToFreeEntry> to_cache;

  // Prepare memory from pages to be cached before taking the lock
  ZArrayIterator<ZPage*> pages_iter(pages);
  for (ZPage* page; pages_iter.next(&page);) {
    // Extract mapped memory, store generation id and destroy page
    ZVirtualMemory vmem = page->virtual_memory();
    const ZGenerationId generation_id = page->generation_id();
    const ZPageType page_type = page->type();

    safe_destroy_page(page);

    // Perhaps remap mapping
    if (page_type == ZPageType::large || should_defragment(vmem)) {
      ZStatInc(ZCounterDefragment);
      vmem = remap_mapping(vmem, true /* force_low_address */);
    }

    to_cache.append({ vmem, generation_id });
  }

  ZLocker<ZLock> locker(&_lock);

  // Insert mappings to the cache
  ZArrayIterator<ZToFreeEntry> iter(&to_cache);
  for (ZToFreeEntry entry; iter.next(&entry);) {
    ZCacheState& state = state_from_vmem(entry.vmem);

    // Update used statistics and cache memory
    state.decrease_used(entry.vmem.size());
    state.decrease_used_generation(entry.generation_id, entry.vmem.size());
    state._cache.insert_mapping(entry.vmem);
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
  ZArrayIterator<ZVirtualMemory> iter(allocation->claimed_mappings());
  for (ZVirtualMemory vmem; iter.next(&vmem);) {
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

size_t ZPageAllocator::uncommit(uint64_t* timeout) {
  const int numa_nodes = ZNUMA::count();
  ZArray<ZVirtualMemory> flushed_mappings;
  ZPerNUMA<size_t> flushed_per_numa(0);
  uint64_t lowest_timeout = ZUncommitDelay;

  {
    // We need to join the suspendible thread set while manipulating capacity and
    // used, to make sure GC safepoints will have a consistent view.
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
  ZArrayIterator<ZVirtualMemory> it(&flushed_mappings);
  for (ZVirtualMemory vmem; it.next(&vmem);) {
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

      // Adjust claimed and capacity to reflect the uncommit
      ZCacheState& state = _states.get(numa_id);
      Atomic::sub(&state._claimed, flushed);
      state.decrease_capacity(flushed, false /* set_max_capacity */);
      total_flushed += flushed;
    }
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
  tc->do_thread(_unmapper);
  tc->do_thread(_uncommitter);
}
