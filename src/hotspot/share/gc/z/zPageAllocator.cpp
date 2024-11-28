/*
 * Copyright (c) 2015, 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
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
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageAllocator.inline.hpp"
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "gc/z/zSafeDelete.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncommitter.hpp"
#include "gc/z/zUnmapper.hpp"
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
  ZArray<ZVirtualMemory>     _mappings;
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
      _mappings(1),
      _node(),
      _stall_result() {}

  void reset_for_retry() {
    _harvested = 0;
    _committed = 0;
    _mappings.clear();
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

  bool wait() {
    return _stall_result.get();
  }

  ZArray<ZVirtualMemory>* mappings() {
    return &_mappings;
  }

  void satisfy(bool result) {
    _stall_result.set(result);
  }

  bool gc_relocation() const {
    return _flags.gc_relocation();
  }
};

ZPageAllocator::ZPageAllocator(size_t min_capacity,
                               size_t initial_capacity,
                               size_t soft_max_capacity,
                               size_t max_capacity)
  : _lock(),
    _mapped_cache(),
    _virtual(max_capacity),
    _physical(max_capacity),
    _mappings(ZAddressOffsetMax),
    _min_capacity(min_capacity),
    _initial_capacity(initial_capacity),
    _max_capacity(max_capacity),
    _current_max_capacity(max_capacity),
    _capacity(0),
    _claimed(0),
    _used(0),
    _used_generations{0, 0},
    _collection_stats{{0, 0}, {0, 0}},
    _stalled(),
    _unmapper(new ZUnmapper(this)),
    _uncommitter(new ZUncommitter(this)),
    _safe_destroy(),
    _initialized(false) {

  if (!_virtual.is_initialized() || !_physical.is_initialized()) {
    return;
  }

  log_info_p(gc, init)("Min Capacity: " SIZE_FORMAT "M", min_capacity / M);
  log_info_p(gc, init)("Initial Capacity: " SIZE_FORMAT "M", initial_capacity / M);
  log_info_p(gc, init)("Max Capacity: " SIZE_FORMAT "M", max_capacity / M);
  log_info_p(gc, init)("Soft Max Capacity: " SIZE_FORMAT "M", soft_max_capacity / M);
  if (ZPageSizeMedium > 0) {
    log_info_p(gc, init)("Medium Page Size: " SIZE_FORMAT "M", ZPageSizeMedium / M);
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
  ZVirtualMemory vmem = _virtual.alloc(size, true /* force_low_address */);
  // Increase capacity, allocate and commit physical memory
  increase_capacity(size);
  _physical.alloc(_mappings.get_addr(vmem.start()), size);
  if (!commit_physical(&vmem)) {
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
  _mapped_cache.insert_mapping(vmem);

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
  // Note that SoftMaxHeapSize is a manageable flag
  const size_t soft_max_capacity = Atomic::load(&SoftMaxHeapSize);
  const size_t current_max_capacity = Atomic::load(&_current_max_capacity);
  return MIN2(soft_max_capacity, current_max_capacity);
}

size_t ZPageAllocator::capacity() const {
  return Atomic::load(&_capacity);
}

size_t ZPageAllocator::used() const {
  return Atomic::load(&_used);
}

size_t ZPageAllocator::used_generation(ZGenerationId id) const {
  return Atomic::load(&_used_generations[(int)id]);
}

size_t ZPageAllocator::unused() const {
  const ssize_t capacity = (ssize_t)Atomic::load(&_capacity);
  const ssize_t used = (ssize_t)Atomic::load(&_used);
  const ssize_t claimed = (ssize_t)Atomic::load(&_claimed);
  const ssize_t unused = capacity - used - claimed;
  return unused > 0 ? (size_t)unused : 0;
}

ZPageAllocatorStats ZPageAllocator::stats(ZGeneration* generation) const {
  ZLocker<ZLock> locker(&_lock);
  return ZPageAllocatorStats(_min_capacity,
                             _max_capacity,
                             soft_max_capacity(),
                             _capacity,
                             _used,
                             _collection_stats[(int)generation->id()]._used_high,
                             _collection_stats[(int)generation->id()]._used_low,
                             used_generation(generation->id()),
                             generation->freed(),
                             generation->promoted(),
                             generation->compacted(),
                             _stalled.size());
}

void ZPageAllocator::reset_statistics(ZGenerationId id) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");
  _collection_stats[(int)id]._used_high = _used;
  _collection_stats[(int)id]._used_low = _used;
}

size_t ZPageAllocator::increase_capacity(size_t size) {
  const size_t increased = MIN2(size, _current_max_capacity - _capacity);

  if (increased > 0) {
    // Update atomically since we have concurrent readers
    Atomic::add(&_capacity, increased);
  }

  return increased;
}

void ZPageAllocator::decrease_capacity(size_t size, bool set_max_capacity) {
  // Update atomically since we have concurrent readers
  Atomic::sub(&_capacity, size);

  if (set_max_capacity) {
    // Adjust current max capacity to avoid further attempts to increase capacity
    log_error_p(gc)("Forced to lower max Java heap size from "
                    SIZE_FORMAT "M(%.0f%%) to " SIZE_FORMAT "M(%.0f%%)",
                    _current_max_capacity / M, percent_of(_current_max_capacity, _max_capacity),
                    _capacity / M, percent_of(_capacity, _max_capacity));

    // Update atomically since we have concurrent readers
    Atomic::store(&_current_max_capacity, _capacity);
  }
}

void ZPageAllocator::increase_used(size_t size) {
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

void ZPageAllocator::decrease_used(size_t size) {
  // Update atomically since we have concurrent readers
  const size_t used = Atomic::sub(&_used, size);

  // Update used low
  for (auto& stats : _collection_stats) {
    if (used < stats._used_low) {
      stats._used_low = used;
    }
  }
}

void ZPageAllocator::increase_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::add(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZPageAllocator::decrease_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::sub(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZPageAllocator::promote_used(size_t size) {
  decrease_used_generation(ZGenerationId::young, size);
  increase_used_generation(ZGenerationId::old, size);
}

void ZPageAllocator::free_physical(const ZVirtualMemory& vmem) {
  // Free physical memory
  _physical.free(_mappings.get_addr(vmem.start()), vmem.size());
}

bool ZPageAllocator::commit_physical(ZVirtualMemory* vmem) {
  // Commit physical memory
  const size_t committed = _physical.commit(_mappings.get_addr(vmem->start()), vmem->size());
  const size_t not_commited = vmem->size() - committed;

  if (not_commited > 0) {
    // Free the uncommitted memory and update vmem with the committed memory
    ZVirtualMemory not_commited_vmem = *vmem;
    *vmem = not_commited_vmem.split(committed);
    free_physical(not_commited_vmem);
    free_virtual(not_commited_vmem);
    return false;
  }

  return true;
}

void ZPageAllocator::uncommit_physical(const ZVirtualMemory& vmem) {
  precond(ZUncommit);

  // Uncommit physical memory
  _physical.uncommit(_mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::map_virtual_to_physical(const ZVirtualMemory& vmem) {
  // Map virtual memory to physical memory
  _physical.map(vmem.start(), _mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::unmap_virtual(const ZVirtualMemory& vmem) {
  // Unmap virtual memory from physical memory
  _physical.unmap(vmem.start(), _mappings.get_addr(vmem.start()), vmem.size());
}

void ZPageAllocator::free_virtual(const ZVirtualMemory& vmem) {
  // Free virtual memory
  _virtual.free(vmem);
}

bool ZPageAllocator::should_defragment(const ZVirtualMemory& vmem) const {
  assert(vmem.size() <= MAX2(ZPageSizeSmall, ZPageSizeMedium), "Should not be called for large pages");

  // Small and medium pages are allocated at a low address. They may end up at a
  // high address (second half of the address space) if we have a constrained
  // address space. To combat address space fragmentation we want to attemp to
  // remap such memory to a lower address.
  return vmem.start() >= to_zoffset(_virtual.reserved() / 2) &&
         vmem.start() >= _virtual.lowest_available_address();
}

ZVirtualMemory ZPageAllocator::remap_mapping(const ZVirtualMemory& vmem, bool force_low_address) {
  // Allocate new virtual memory
  const ZVirtualMemory new_vmem = _virtual.alloc(vmem.size(), force_low_address);

  if (new_vmem.is_null()) {
    // Failed to allocate new virtual address space, do nothing
    return vmem;
  }

  // Copy the physical mappings
  const size_t num_granules = vmem.size() >> ZGranuleSizeShift;
  memcpy(_mappings.get_addr(new_vmem.start()), _mappings.get_addr(vmem.start()), sizeof(zoffset) * num_granules);

  // Unmap the previous mapping asynchronously
  _unmapper->unmap_virtual(vmem);

  // Map the copied physical segments.
  map_virtual_to_physical(new_vmem);


  return new_vmem;
}

bool ZPageAllocator::is_alloc_allowed(size_t size) const {
  const size_t available = _current_max_capacity - _used - _claimed;
  return available >= size;
}

void ZPageAllocator::claim_mapped_or_increase_capacity(ZPageType type, size_t size, ZArray<ZVirtualMemory>* mappings) {
  // Try to allocate a contiguous mapping when not allocating a large page.
  if (type != ZPageType::large) {
    ZVirtualMemory mapped_vmem;
    if (_mapped_cache.remove_mapping_contiguous(&mapped_vmem, size)) {
      mappings->append(mapped_vmem);
      return;
    }
  }

  // If we've failed to allocate a contiguous range from the mapped cache,
  // there is still a possibility that the cache holds enough memory for the
  // allocation dispersed over more than one mapping if the capacity cannot be
  // increased to satisfy the allocation.

  // Try increase capacity
  const size_t increased = increase_capacity(size);
  if (increased < size) {
    // Could not increase capacity enough to satisfy the allocation completely.
    // Try removing multiple mappings from the mapped cache.
    const size_t remaining = size - increased;
    _mapped_cache.remove_mappings(mappings, remaining);
  }
}

bool ZPageAllocator::claim_physical(ZPageAllocation* allocation) {
  const ZPageType type = allocation->type();
  const size_t size = allocation->size();
  ZArray<ZVirtualMemory>* const mappings = allocation->mappings();

  if (!is_alloc_allowed(size)) {
    // Out of memory
    return false;
  }

  // Try to claim physical memory
  claim_mapped_or_increase_capacity(type, size, mappings);

  // Updated used statistics
  increase_used(size);

  // Success
  return true;
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

    if (claim_physical(allocation)) {
      // Success
      return true;
    }

    // Failed
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

  ZArrayIterator<ZVirtualMemory> iter(allocation->mappings());
  for (ZVirtualMemory vmem; iter.next(&vmem);) {
    // Copy the physical mappings
    const size_t num_granules = vmem.size() >> ZGranuleSizeShift;
    memcpy(_mappings.get_addr(new_vmem.start() + harvested), _mappings.get_addr(vmem.start()), sizeof(zoffset) * num_granules);

    harvested += vmem.size();

    _unmapper->unmap_virtual(vmem);
  }

  // Clear the array of stored mappings
  allocation->mappings()->clear();

  if (harvested > 0) {
    allocation->set_harvested(harvested);
    log_debug(gc, heap)("Mapped Cache Harvest: " SIZE_FORMAT "M", harvested / M);
  }
}

bool ZPageAllocator::is_alloc_satisfied(ZPageAllocation* allocation) const {
  // The allocation is immediately satisfied if the list of mappings contains
  // exactly one mapping and is of the correct size.
  if (allocation->mappings()->length() != 1) {
    // Not a contiguous mapping
    return false;
  }

  const ZVirtualMemory& vmem = allocation->mappings()->first();
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
  commit_physical(&to_be_committed_vmem);
  committed_vmem.extend(to_be_committed_vmem.size());

  // Sort the backing memory before mapping
  qsort(_mappings.get_addr(committed_vmem.start()), committed_vmem.size() >> ZGranuleSizeShift, sizeof(zoffset),
        [](const void* a, const void* b) -> int {
          return *static_cast<const zoffset*>(a) < *static_cast<const zoffset*>(b) ? -1 : 1;
        });
  map_virtual_to_physical(committed_vmem);
  allocation->mappings()->append(committed_vmem);

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

  // If the claimed physical memory holds a large enough contiguous virtual
  // address range, we're done.
  if (is_alloc_satisfied(allocation)) {
    ZVirtualMemory mapped_vmem = allocation->mappings()->pop();

    if (allocation->type() == ZPageType::large) {
      // Large pages are placed in high address space, the memory returned from
      // the mapped cache is at low address space, need to remap.
      mapped_vmem = remap_mapping(mapped_vmem, false /* force_low_address */);
    }

    return new ZPage(allocation->type(), mapped_vmem);
  }

  // We need to allocate a new virtual address range and make sure the claimed
  // physical memory is committed and mapped to the same virtual address range.
  ZVirtualMemory vmem = _virtual.alloc(allocation->size(), false /* force_low_address */);
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
    _physical.alloc(_mappings.get_addr(vmem.start() + allocation->harvested()), remaining_physical);
  }

  if (!commit_and_map_memory(allocation, vmem, allocation->harvested())) {
    free_memory_alloc_failed(allocation);
    goto retry;
  }

  return new ZPage(allocation->type(), vmem);
}

void ZPageAllocator::alloc_page_age_update(ZPage* page, size_t size, ZPageAge age) {
  // The generation's used is tracked here when the page is handed out
  // to the allocating thread. The overall heap "used" is tracked in
  // the lower-level allocation code.
  const ZGenerationId id = age == ZPageAge::old ? ZGenerationId::old : ZGenerationId::young;
  increase_used_generation(id, size);

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

  alloc_page_age_update(page, size, age);

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
               page->virtual_memory().size() / ZGranuleSize, flags.non_blocking());

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

    if (!claim_physical(allocation)) {
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
  const ZGenerationId generation_id = page->generation_id();

  // Extract memory and destroy page
  ZVirtualMemory vmem = page->virtual_memory();
  ZPageType page_type = page->type();
  safe_destroy_page(page);

  // Perhaps remap mapping
  if (page_type == ZPageType::large ||
     (allow_defragment && should_defragment(vmem))) {
    ZStatInc(ZCounterDefragment);
    vmem = remap_mapping(vmem, true /* force_low_address */);
  }

  ZLocker<ZLock> locker(&_lock);

  // Update used statistics
  decrease_used(vmem.size());
  decrease_used_generation(generation_id, vmem.size());

  // Cache the memory from the page
  _mapped_cache.insert_mapping(vmem);

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::free_pages(const ZArray<ZPage*>* pages) {
  ZArray<ZVirtualMemory> to_cache;

  size_t young_size = 0;
  size_t old_size = 0;

  // Prepare memory from pages to be cached before taking the lock
  ZArrayIterator<ZPage*> pages_iter(pages);
  for (ZPage* page; pages_iter.next(&page);) {
    if (page->is_young()) {
      young_size += page->size();
    } else {
      old_size += page->size();
    }

    // Extract mapped memory and destroy page
    ZVirtualMemory vmem = page->virtual_memory();
    ZPageType page_type = page->type();
    safe_destroy_page(page);

    // Perhaps remap mapping
    if (page_type == ZPageType::large || should_defragment(vmem)) {
      ZStatInc(ZCounterDefragment);
      vmem = remap_mapping(vmem, true /* force_low_address */);
    }

    to_cache.append(vmem);
  }

  ZLocker<ZLock> locker(&_lock);

  // Update used statistics
  decrease_used(young_size + old_size);
  decrease_used_generation(ZGenerationId::young, young_size);
  decrease_used_generation(ZGenerationId::old, old_size);

  // Insert mappings to the cache
  ZArrayIterator<ZVirtualMemory> iter(&to_cache);
  for (ZVirtualMemory vmem; iter.next(&vmem);) {
    _mapped_cache.insert_mapping(vmem);
  }

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::free_memory_alloc_failed(ZPageAllocation* allocation) {
  ZLocker<ZLock> locker(&_lock);

  // Only decrease the overall used and not the generation used,
  // since the allocation failed and generation used wasn't bumped.
  decrease_used(allocation->size());

  size_t freed = 0;

  // Free mapped memory
  ZArrayIterator<ZVirtualMemory> iter(allocation->mappings());
  for (ZVirtualMemory vmem; iter.next(&vmem);) {
    freed += vmem.size();
    _mapped_cache.insert_mapping(vmem);
  }

  // Adjust capacity to reflect the failed capacity increase
  const size_t remaining = allocation->size() - freed;
  if (remaining > 0) {
    decrease_capacity(remaining, true /* set_max_capacity */);
  }

  // Reset allocation for a potential retry
  allocation->reset_for_retry();

  // Try satisfy stalled allocations
  satisfy_stalled();
}

size_t ZPageAllocator::uncommit(uint64_t* timeout) {
  // TODO: Do nothing until implemented correctly.
  return 0;

  // We need to join the suspendible thread set while manipulating capacity and
  // used, to make sure GC safepoints will have a consistent view.
  ZArray<ZVirtualMemory> flushed_mappings;
  size_t flushed = 0;

  {
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&_lock);

    // Never uncommit below min capacity. We flush out and uncommit chunks at
    // a time (~0.8% of the max capacity, but at least one granule and at most
    // 256M), in case demand for memory increases while we are uncommitting.
    const size_t retain = MAX2(_used, _min_capacity);
    const size_t release = _capacity - retain;
    const size_t limit = MIN2(align_up(_current_max_capacity >> 7, ZGranuleSize), 256 * M);
    const size_t flush = MIN2(release, limit);

    // Flush memory from the mapped cache to uncommit
    _mapped_cache.remove_mappings(&flushed_mappings, flush);

    if (flushed == 0) {
      // Nothing flushed
      return 0;
    }

    // Record flushed memory as claimed
    Atomic::add(&_claimed, flushed);
  }

  // Unmap and uncommit flushed memory
  ZArrayIterator<ZVirtualMemory> it(&flushed_mappings);
  for (ZVirtualMemory vmem; it.next(&vmem);) {
    unmap_virtual(vmem);
    uncommit_physical(vmem);
    free_physical(vmem);
    free_virtual(vmem);
  }

  {
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&_lock);

    // Adjust claimed and capacity to reflect the uncommit
    Atomic::sub(&_claimed, flushed);
    decrease_capacity(flushed, false /* set_max_capacity */);
  }

  return flushed;
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
