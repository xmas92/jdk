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

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zHeuristics.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zObjectAllocator.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageTable.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"

static const ZStatCounter ZCounterUndoObjectAllocationSucceeded("Memory", "Undo Object Allocation Succeeded", ZStatUnitOpsPerSecond);
static const ZStatCounter ZCounterUndoObjectAllocationFailed("Memory", "Undo Object Allocation Failed", ZStatUnitOpsPerSecond);

template <size_t N>
ZObjectAllocator::ZPageState<N>::ZPageState()
  : _shared_page(nullptr),
    _extra_pages() {}

template <size_t N>
ZPage* volatile* ZObjectAllocator::ZPageState<N>::shared_page_addr() {
  return &_shared_page;
}

template <size_t N>
ZPage* volatile const* ZObjectAllocator::ZPageState<N>::shared_page_addr() const {
  return &_shared_page;
}

template <size_t N>
void ZObjectAllocator::ZPageState<N>::insert_replaced_page(ZPage* page) {
  ZPage* replaced_page = page;
  for (ZPage* volatile& extra_page : _extra_pages) {
    if (replaced_page == nullptr || replaced_page->remaining() == 0) {
      break;
    }

    if (extra_page == nullptr || extra_page->size() < replaced_page->size()) {
      replaced_page = Atomic::xchg(&extra_page, replaced_page, memory_order_relaxed);
    }
  }
}

template <size_t N>
zaddress ZObjectAllocator::ZPageState<N>::alloc_object(size_t size) {
  ZPage* shared_page = Atomic::load_acquire(&_shared_page);
  zaddress addr = zaddress::null;

  for (ZPage* volatile& extra_page : _extra_pages) {
    if (extra_page == nullptr) {
      continue;
    }

    addr = extra_page->alloc_object_atomic(size);
    if (!is_null(addr)) {
      return addr;
    }
  }

  if (shared_page != nullptr) {
    addr = shared_page->alloc_object_atomic(size);
  }

  return addr;
}

ZObjectAllocator::ZObjectAllocator(ZPageAge age)
  : _age(age),
    _use_per_cpu_shared_small_pages(ZHeuristics::use_per_cpu_shared_small_pages()),
    _shared_small_page_state(),
    _shared_medium_page(nullptr),
    _medium_page_alloc_lock() {}

ZObjectAllocator::ZSmallPageState* ZObjectAllocator::shared_small_state() {
  return _use_per_cpu_shared_small_pages ? _shared_small_page_state.addr() : _shared_small_page_state.addr(0);
}

const ZObjectAllocator::ZSmallPageState* ZObjectAllocator::shared_small_state() const {
  return _use_per_cpu_shared_small_pages ? _shared_small_page_state.addr() : _shared_small_page_state.addr(0);
}

ZPage* ZObjectAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags) {
  return ZHeap::heap()->alloc_page(type, size, flags, _age);
}

ZPage* ZObjectAllocator::alloc_page_for_relocation(ZPageType type, size_t size, ZAllocationFlags flags) {
  return ZHeap::heap()->alloc_page(type, size, flags, _age);
}

void ZObjectAllocator::undo_alloc_page(ZPage* page) {
  ZHeap::heap()->undo_alloc_page(page);
}

zaddress ZObjectAllocator::alloc_object_in_shared_page(ZPage* volatile* shared_page,
                                                       ZPageType page_type,
                                                       size_t page_size,
                                                       size_t size,
                                                       ZAllocationFlags flags) {
  ZPage* unused;
  return alloc_object_in_shared_page(shared_page, page_type, page_size, size, flags, &unused);
}

zaddress ZObjectAllocator::alloc_object_in_shared_page(ZPage* volatile* shared_page,
                                                       ZPageType page_type,
                                                       size_t page_size,
                                                       size_t size,
                                                       ZAllocationFlags flags,
                                                       ZPage** replaced_page) {
  zaddress addr = zaddress::null;
  ZPage* page = Atomic::load_acquire(shared_page);

  *replaced_page = nullptr;

  if (page != nullptr) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // Allocate new page
    ZPage* const new_page = alloc_page(page_type, page_size, flags);
    if (new_page != nullptr) {
      // Allocate object before installing the new page
      addr = new_page->alloc_object(size);

    retry:
      // Install new page
      ZPage* const prev_page = Atomic::cmpxchg(shared_page, page, new_page);
      if (prev_page != page) {
        if (prev_page == nullptr) {
          // Previous page was retired, retry installing the new page
          page = prev_page;
          goto retry;
        }

        // Another page already installed, try allocation there first
        const zaddress prev_addr = prev_page->alloc_object_atomic(size);
        if (is_null(prev_addr)) {
          // Allocation failed, retry installing the new page
          page = prev_page;
          goto retry;
        }

        // Allocation succeeded in already installed page
        addr = prev_addr;

        // Undo new page allocation
        undo_alloc_page(new_page);
      } else {
        *replaced_page = page;
      }
    }
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_object_in_medium_page(size_t size,
                                                       ZAllocationFlags flags) {
  zaddress addr = zaddress::null;
  ZPage* volatile* shared_medium_page = _shared_medium_page.addr();
  ZPage* page = Atomic::load_acquire(shared_medium_page);

  if (page != nullptr) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // When a new medium page is required, we synchronize the allocation of the
    // new page using a lock. This is to avoid having multiple threads allocate
    // medium pages when we know only one of them will succeed in installing
    // the page at this layer.
    ZLocker<ZLock> locker(&_medium_page_alloc_lock);

    // When holding the lock we can't allow the page allocator to stall,
    // which in the common case it won't. The page allocation is thus done
    // in a non-blocking fashion and only if this fails we below (while not
    // holding the lock) do the blocking page allocation.
    ZAllocationFlags non_blocking_flags = flags;
    non_blocking_flags.set_non_blocking();

    if (ZPageSizeMediumMin != ZPageSizeMediumMax) {
      assert(ZPageSizeMediumEnabled, "must be enabled");
      // We attempt a fast medium allocations first. Which will only succeed
      // if a page in the range [ZPageSizeMediumMin, ZPageSizeMediumMax] can
      // be allocated without any expensive syscalls, directly from the cache.
      ZAllocationFlags fast_medium_flags = non_blocking_flags;
      fast_medium_flags.set_fast_medium();
      addr = alloc_object_in_shared_page(shared_medium_page, ZPageType::medium, ZPageSizeMediumMax, size, fast_medium_flags);
    }

    if (is_null(addr)) {
      addr = alloc_object_in_shared_page(shared_medium_page, ZPageType::medium, ZPageSizeMediumMax, size, non_blocking_flags);
    }

  }

  if (is_null(addr) && !flags.non_blocking()) {
    // The above allocation attempts failed and this allocation should stall
    // until memory is available. Redo the allocation with blocking enabled.
    addr = alloc_object_in_shared_page(shared_medium_page, ZPageType::medium, ZPageSizeMediumMax, size, flags);
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_large_object(size_t size, ZAllocationFlags flags) {
  zaddress addr = zaddress::null;

  // Allocate new large page
  const size_t page_size = align_up(size, ZGranuleSize);
  ZPage* const page = alloc_page(ZPageType::large, page_size, flags);
  if (page != nullptr) {
    // Allocate the object
    addr = page->alloc_object(size);
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_medium_object(size_t size, ZAllocationFlags flags) {
  return alloc_object_in_medium_page(size, flags);
}

zaddress ZObjectAllocator::alloc_small_object(size_t size, ZAllocationFlags flags) {
  ZSmallPageState* page_state = shared_small_state();
  zaddress addr = zaddress::null;

  addr = page_state->alloc_object(size);
  if (!is_null(addr)) {
    return addr;
  }

  ZPage* replaced_page = nullptr;
  addr = alloc_object_in_shared_page(page_state->shared_page_addr(), ZPageType::small, ZPageSizeSmall, size, flags, &replaced_page);

  page_state->insert_replaced_page(replaced_page);

  return addr;
}

zaddress ZObjectAllocator::alloc_object(size_t size, ZAllocationFlags flags) {
  if (size <= ZObjectSizeLimitSmall) {
    // Small
    return alloc_small_object(size, flags);
  } else if (size <= ZObjectSizeLimitMedium) {
    // Medium
    return alloc_medium_object(size, flags);
  } else {
    // Large
    return alloc_large_object(size, flags);
  }
}

zaddress ZObjectAllocator::alloc_object(size_t size) {
  const ZAllocationFlags flags;
  return alloc_object(size, flags);
}

zaddress ZObjectAllocator::alloc_object_for_relocation(size_t size) {
  ZAllocationFlags flags;
  flags.set_non_blocking();

  return alloc_object(size, flags);
}

void ZObjectAllocator::undo_alloc_object_for_relocation(zaddress addr, size_t size) {
  ZPage* const page = ZHeap::heap()->page(addr);

  if (page->is_large()) {
    undo_alloc_page(page);
    ZStatInc(ZCounterUndoObjectAllocationSucceeded);
  } else {
    if (page->undo_alloc_object_atomic(addr, size)) {
      ZStatInc(ZCounterUndoObjectAllocationSucceeded);
    } else {
      ZStatInc(ZCounterUndoObjectAllocationFailed);
    }
  }
}

ZPageAge ZObjectAllocator::age() const {
  return _age;
}

size_t ZObjectAllocator::remaining() const {
  assert(Thread::current()->is_Java_thread(), "Should be a Java thread");

  const ZPage* const page = Atomic::load_acquire(shared_small_state()->shared_page_addr());
  if (page != nullptr) {
    return page->remaining();
  }

  return 0;
}

void ZObjectAllocator::retire_pages() {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");

  // Reset allocation pages
  _shared_medium_page.set(nullptr);
  _shared_small_page_state.set_all({});
}
