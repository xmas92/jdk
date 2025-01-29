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
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMemory.inline.hpp"

ZMemory* ZMemoryManager::create(zoffset start, size_t size) {
  ZMemory* const area = new ZMemory(start, size);
  if (_callbacks._create != nullptr) {
    _callbacks._create(area);
  }
  return area;
}

void ZMemoryManager::destroy(ZMemory* area) {
  if (_callbacks._destroy != nullptr) {
    _callbacks._destroy(area);
  }
  delete area;
}

void ZMemoryManager::shrink_from_front(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_front != nullptr) {
    _callbacks._shrink_from_front(area, size);
  }
  area->shrink_from_front(size);
}

void ZMemoryManager::shrink_from_back(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_back != nullptr) {
    _callbacks._shrink_from_back(area, size);
  }
  area->shrink_from_back(size);
}

void ZMemoryManager::grow_from_front(ZMemory* area, size_t size) {
  if (_callbacks._grow_from_front != nullptr) {
    _callbacks._grow_from_front(area, size);
  }
  area->grow_from_front(size);
}

void ZMemoryManager::grow_from_back(ZMemory* area, size_t size) {
  if (_callbacks._grow_from_back != nullptr) {
    _callbacks._grow_from_back(area, size);
  }
  area->grow_from_back(size);
}

zoffset ZMemoryManager::alloc_low_address_no_lock(size_t size) {
  ZListIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() >= size) {
      if (area->size() == size) {
        // Exact match, remove area
        const zoffset start = area->start();
        _freelist.remove(area);
        destroy(area);
        return start;
      } else {
        // Larger than requested, shrink area
        const zoffset start = area->start();
        shrink_from_front(area, size);
        return start;
      }
    }
  }

  // Out of memory
  return zoffset(UINTPTR_MAX);
}

zoffset ZMemoryManager::alloc_low_address_at_most_no_lock(size_t size, size_t* allocated) {
  ZMemory* const area = _freelist.first();
  if (area != nullptr) {
    if (area->size() <= size) {
      // Smaller than or equal to requested, remove area
      const zoffset start = area->start();
      *allocated = area->size();
      _freelist.remove(area);
      destroy(area);
      return start;
    } else {
      // Larger than requested, shrink area
      const zoffset start = area->start();
      shrink_from_front(area, size);
      *allocated = size;
      return start;
    }
  }

  // Out of memory
  *allocated = 0;
  return zoffset(UINTPTR_MAX);
}


void ZMemoryManager::free_no_lock(zoffset start, size_t size) {
  assert(start != zoffset(UINTPTR_MAX), "Invalid address");
  const zoffset_end end = to_zoffset_end(start, size);

  ZListIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (start < area->start()) {
      ZMemory* const prev = _freelist.prev(area);
      if (prev != nullptr && start == prev->end()) {
        if (end == area->start()) {
          // Merge with prev and current area
          grow_from_back(prev, size + area->size());
          _freelist.remove(area);
          delete area;
        } else {
          // Merge with prev area
          grow_from_back(prev, size);
        }
      } else if (end == area->start()) {
        // Merge with current area
        grow_from_front(area, size);
      } else {
        // Insert new area before current area
        assert(end < area->start(), "Areas must not overlap");
        ZMemory* const new_area = create(start, size);
        _freelist.insert_before(area, new_area);
      }

      // Done
      return;
    }
  }

  // Insert last
  ZMemory* const last = _freelist.last();
  if (last != nullptr && start == last->end()) {
    // Merge with last area
    grow_from_back(last, size);
  } else {
    // Insert new area last
    ZMemory* const new_area = create(start, size);
    _freelist.insert_last(new_area);
  }
}

ZMemoryManager::Callbacks::Callbacks()
  : _create(nullptr),
    _destroy(nullptr),
    _shrink_from_front(nullptr),
    _shrink_from_back(nullptr),
    _grow_from_front(nullptr),
    _grow_from_back(nullptr) {}

ZMemoryManager::ZMemoryManager()
  : _freelist(),
    _callbacks() {}

bool ZMemoryManager::free_is_contiguous() const {
  return _freelist.size() == 1;
}

void ZMemoryManager::register_callbacks(const Callbacks& callbacks) {
  _callbacks = callbacks;
}

size_t ZMemoryManager::range_size() const {
  ZLocker<ZLock> locker(&_lock);

  if (_freelist.is_empty()) {
    return 0;
  }

  return _freelist.last()->end() - _freelist.first()->start();
}

zoffset ZMemoryManager::peek_low_address() const {
  ZLocker<ZLock> locker(&_lock);

  const ZMemory* const area = _freelist.first();
  if (area != nullptr) {
    return area->start();
  }

  // Out of memory
  return zoffset(UINTPTR_MAX);
}

zoffset ZMemoryManager::alloc_low_address(size_t size) {
  ZLocker<ZLock> locker(&_lock);
  return alloc_low_address_no_lock(size);
}

zoffset ZMemoryManager::alloc_low_address_at_most(size_t size, size_t* allocated) {
  ZLocker<ZLock> lock(&_lock);
  return alloc_low_address_at_most_no_lock(size, allocated);
}

zoffset ZMemoryManager::alloc_high_address(size_t size) {
  ZLocker<ZLock> locker(&_lock);

  ZListReverseIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() >= size) {
      if (area->size() == size) {
        // Exact match, remove area
        const zoffset start = area->start();
        _freelist.remove(area);
        destroy(area);
        return start;
      } else {
        // Larger than requested, shrink area
        shrink_from_back(area, size);
        return to_zoffset(area->end());
      }
    }
  }

  // Out of memory
  return zoffset(UINTPTR_MAX);
}

void ZMemoryManager::transfer_high_address(ZMemoryManager& other, size_t size) {
  assert(other._freelist.is_empty(), "Should only be used for initializatiion");

  ZLocker<ZLock> locker(&_lock);
  size_t to_move = size;

  ZListReverseIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() <= to_move) {
      // Smaller than or equal to requested, remove from this freelist and
      // insert in other's freelist
      to_move -= area->size();
      _freelist.remove(area);
      other._freelist.insert_first(area);
    } else {
      // Larger than requested, shrink area
      const zoffset_end end = area->end();
      shrink_from_back(area, to_move);
      other.free(zoffset(end - to_move), to_move);
      to_move = 0;
    }

    if (to_move == 0) {
      break;
    }
  }
}

size_t ZMemoryManager::shuffle_memory_low_addresses(zoffset start, size_t size, ZArray<ZNonDescriptMemory>* out) {
  ZLocker<ZLock> locker(&_lock);
  free_no_lock(start, size);

  size_t num_ranges = 0;

  size_t to_allocate = size;
  while (to_allocate > 0) {
    size_t allocated = 0;
    zoffset start = alloc_low_address_at_most_no_lock(to_allocate, &allocated);
    to_allocate -= allocated;

    out->append(ZNonDescriptMemory(start, to_zoffset_end(start, allocated)));
    num_ranges++;
  }

  return num_ranges;
}

void ZMemoryManager::shuffle_memory_low_addresses_contiguous(size_t size, ZArray<ZNonDescriptMemory>* out) {
  ZLocker<ZLock> locker(&_lock);

  // Free everything
  ZArrayIterator<ZNonDescriptMemory> iter(out);
  for (ZNonDescriptMemory mem; iter.next(&mem);) {
    free_no_lock(mem._start, mem._end - mem._start);
  }

  // Clear stored memory so that we can populate it below
  out->clear();

  // Try to allocate a contiguous chunk
  zoffset start = alloc_low_address_no_lock(size);
  if (start != zoffset(UINTPTR_MAX)) {
    out->append(ZNonDescriptMemory(start, to_zoffset_end(start, size)));
    return;
  }

  // Failed to allocate a contiguous chunk, split it up into smaller chunks
  size_t to_allocate = size;
  while (to_allocate > 0) {
    size_t allocated = 0;
    zoffset start = alloc_low_address_at_most_no_lock(to_allocate, &allocated);
    to_allocate -= allocated;

    out->append(ZNonDescriptMemory(start, to_zoffset_end(start, allocated)));
  }
}

void ZMemoryManager::free(zoffset start, size_t size) {
  ZLocker<ZLock> locker(&_lock);
  free_no_lock(start, size);
}
