/*
 * Copyright (c) 2020, 2025, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zList.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zUnmapper.hpp"
#include "jfr/jfrEvents.hpp"
#include "runtime/globals.hpp"

ZUnmapper::ZUnmapper(ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _lock(),
    _queue(),
    _enqueued_bytes(0),
    _warned_sync_unmapping(false),
    _stop(false) {
  set_name("ZUnmapper");
  create_and_start();
}

ZUnmapperEntry* ZUnmapper::dequeue() {
  ZLocker<ZConditionLock> locker(&_lock);

  for (;;) {
    if (_stop) {
      return nullptr;
    }

    ZUnmapperEntry* const entry = _queue.remove_first();
    if (entry != nullptr) {
      _enqueued_bytes -= entry->size();
      return entry;
    }

    _lock.wait();
  }
}

bool ZUnmapper::try_enqueue(const ZVirtualMemory& vmem) {
  // Enqueue for asynchronous unmap and destroy
  ZLocker<ZConditionLock> locker(&_lock);
  if (is_saturated()) {
    // The unmapper thread is lagging behind and is unable to unmap memory fast enough
    if (!_warned_sync_unmapping) {
      _warned_sync_unmapping = true;
      log_warning_p(gc)("WARNING: Encountered synchronous unmapping because asynchronous unmapping could not keep up");
    }
    log_debug(gc, unmap)("Synchronous unmapping %zuM mapped memory", vmem.size() / M);
    return false;
  }

  log_trace(gc, unmap)("Asynchronous unmapping %zuM mapped memory (%zuM / %zuM enqueued)",
                       vmem.size() / M, _enqueued_bytes / M, queue_capacity() / M);

  ZUnmapperEntry* entry = new ZUnmapperEntry(vmem);
  _queue.insert_last(entry);
  _enqueued_bytes += vmem.size();
  _lock.notify_all();

  return true;
}

size_t ZUnmapper::queue_capacity() const {
  return align_up((size_t)(_page_allocator->max_capacity() * ZAsyncUnmappingLimit / 100.0), ZGranuleSize);
}

bool ZUnmapper::is_saturated() const {
  return _enqueued_bytes >= queue_capacity();
}

void ZUnmapper::do_unmap(const ZVirtualMemory& vmem) const {
  EventZUnmap event;
  const size_t unmapped = vmem.size();

  // Unmap and destroy
  _page_allocator->unmap_virtual(vmem);
  _page_allocator->free_virtual(vmem);

  // Send event
  event.commit(unmapped);
}

void ZUnmapper::unmap_virtual(const ZVirtualMemory& vmem) {
  if (!try_enqueue(vmem)) {
    // Synchronously unmap and destroy
    do_unmap(vmem);
  }
}

void ZUnmapper::run_thread() {
  for (;;) {
    ZUnmapperEntry* const entry = dequeue();
    if (entry == nullptr) {
      // Stop
      return;
    }

    do_unmap(entry->vmem());
    delete entry;
  }
}

void ZUnmapper::terminate() {
  ZLocker<ZConditionLock> locker(&_lock);
  _stop = true;
  _lock.notify_all();
}
