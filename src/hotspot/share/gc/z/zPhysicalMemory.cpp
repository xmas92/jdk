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
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zNMT.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/init.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"

ZPhysicalMemoryManager::ZPhysicalMemoryManager(size_t max_capacity)
  : _backing(max_capacity) {
  // Make the whole range free
  const size_t numa_nodes = ZNUMA::count();
  const size_t capacity_per_manager = max_capacity / numa_nodes;

  for (int numa_id = 0; numa_id < (int)numa_nodes; numa_id++) {
    ZMemoryManager& manager = _managers.get(numa_id);
    manager.free(zoffset(capacity_per_manager * numa_id), capacity_per_manager);
  }
}

bool ZPhysicalMemoryManager::is_initialized() const {
  return _backing.is_initialized();
}

void ZPhysicalMemoryManager::warn_commit_limits(size_t max_capacity) const {
  _backing.warn_commit_limits(max_capacity);
}

void ZPhysicalMemoryManager::try_enable_uncommit(size_t min_capacity, size_t max_capacity) {
  assert(!is_init_completed(), "Invalid state");

  // If uncommit is not explicitly disabled, max capacity is greater than
  // min capacity, and uncommit is supported by the platform, then uncommit
  // will be enabled.
  if (!ZUncommit) {
    log_info_p(gc, init)("Uncommit: Disabled");
    return;
  }

  if (max_capacity == min_capacity) {
    log_info_p(gc, init)("Uncommit: Implicitly Disabled (-Xms equals -Xmx)");
    FLAG_SET_ERGO(ZUncommit, false);
    return;
  }

  // Test if uncommit is supported by the operating system by committing
  // and then uncommitting a granule.
  const zoffset offset{};
  if (!commit(&offset, ZGranuleSize) || !uncommit(&offset, ZGranuleSize)) {
    log_info_p(gc, init)("Uncommit: Implicitly Disabled (Not supported by operating system)");
    FLAG_SET_ERGO(ZUncommit, false);
    return;
  }

  log_info_p(gc, init)("Uncommit: Enabled");
  log_info_p(gc, init)("Uncommit Delay: %zus", ZUncommitDelay);
}

void ZPhysicalMemoryManager::alloc(zoffset* pmem, size_t size, int numa_id) {
  assert(is_aligned(size, ZGranuleSize), "Invalid size");

  // Allocate segments
  while (size > 0) {
    size_t allocated = 0;
    const zoffset start = _managers.get(numa_id).alloc_low_address_at_most(size, &allocated);
    assert(start != zoffset(UINTPTR_MAX), "Allocation should never fail");
    size -= allocated;
    for (zoffset offset = start; allocated != 0; offset += ZGranuleSize, allocated -= ZGranuleSize, pmem++) {
      *pmem = offset;
    }
  }
}

template<typename ReturnType>
struct IterateInvoker {
  template<typename Function>
  bool operator()(Function function, zoffset segment_start, size_t segment_size) const {
    return function(segment_start, segment_size);
  }
};

template<>
struct IterateInvoker<void> {
  template<typename Function>
  bool operator()(Function function, zoffset segment_start, size_t segment_size) const {
    function(segment_start, segment_size);
    return true;
  }
};

template<typename Function>
bool for_each_segment_apply(const zoffset* pmem, size_t num_granules, Function function) {
  IterateInvoker<decltype(function(zoffset{}, size_t{}))> invoker;
  for (size_t i = 0; i < num_granules; i++) {
    const size_t start_i = i;
    const zoffset start = pmem[i];
    while (i + 1 < num_granules && pmem[i] + ZGranuleSize == pmem[i + 1]) {
      i++;
    }
    if (!invoker(function, start, (i - start_i + 1) * ZGranuleSize)) {
      return false;
    }
  }
  return true;
}

void ZPhysicalMemoryManager::free(const zoffset* pmem, size_t size, int numa_id) {
  // Free segments
  for_each_segment_apply(pmem, size >> ZGranuleSizeShift, [&](zoffset segment_start, size_t segment_size) {
    _managers.get(numa_id).free(segment_start, segment_size);
  });
}

size_t ZPhysicalMemoryManager::commit(const zoffset* pmem, size_t size, int numa_id) {
  size_t total_committed = 0;
  // Commit segments
  for_each_segment_apply(pmem, size >> ZGranuleSizeShift, [&](zoffset segment_start, size_t segment_size) {
    // Commit segment
#ifdef LINUX
    const size_t committed = _backing.commit(segment_start, segment_size, numa_id);
#else
    const size_t committed = _backing.commit(segment_start, segment_size);
#endif

    total_committed += committed;
    // Register with NMT
    if (committed > 0) {
      ZNMT::commit(segment_start, committed);
    }

    return segment_size == committed;
  });
  // Success
  return total_committed;
}

size_t ZPhysicalMemoryManager::uncommit(const zoffset* pmem, size_t size) {
  size_t total_uncommitted = 0;
  // Uncommit segments
  for_each_segment_apply(pmem, size >> ZGranuleSizeShift, [&](zoffset segment_start, size_t segment_size) {
    // Uncommit segment
    const size_t uncommitted = _backing.uncommit(segment_start, segment_size);
    total_uncommitted += uncommitted;
    // Unregister with NMT
    if (uncommitted > 0) {
      ZNMT::uncommit(segment_start, uncommitted);
    }

    return segment_size == uncommitted;
  });

  // Success
  return total_uncommitted;
}

// Map virtual memory to physical memory
void ZPhysicalMemoryManager::map(zoffset offset, const zoffset* pmem, size_t size) const {
  const zaddress_unsafe addr = ZOffset::address_unsafe(offset);

  size_t mapped = 0;

  for_each_segment_apply(pmem, size >> ZGranuleSizeShift, [&](zoffset segment_start, size_t segment_size) {
    _backing.map(addr + mapped, segment_size, segment_start);
    mapped += segment_size;
  });
  postcond(mapped == size);

  // Setup NUMA interleaving for large pages
  if (ZNUMA::is_enabled() && ZLargePages::is_explicit()) {
    // To get granule-level NUMA interleaving when using large pages,
    // we simply let the kernel interleave the memory for us at page
    // fault time.
    os::numa_make_global((char*)addr, size);
  }
}

// Unmap virtual memory from physical memory
void ZPhysicalMemoryManager::unmap(zoffset offset, const zoffset* /* ignored until anon memory support */, size_t size) const {
  const zaddress_unsafe addr = ZOffset::address_unsafe(offset);

  _backing.unmap(addr, size);
}

size_t ZPhysicalMemoryManager::count_segments(const zoffset* pmem, size_t size) {
  size_t count = 0;

  for_each_segment_apply(pmem, size >> ZGranuleSizeShift, [&](zoffset, size_t) {
    count++;
  });

  return count;
}
