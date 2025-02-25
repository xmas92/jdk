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

template <typename Range>
class ZRangeNode : public CHeapObj<mtGC> {
  friend class ZList<ZRangeNode>;

private:
  using offset     = typename Range::offset;
  using offset_end = typename Range::offset_end;

  Range                 _range;
  ZListNode<ZRangeNode> _node;

public:
  ZRangeNode(offset start, size_t size)
    : _range(start, size),
      _node() {}

  Range* range() {
    return &_range;
  }

  offset start() const {
    return _range.start();
  }

  offset_end end() const {
    return _range.end();
  }

  size_t size() const {
    return _range.size();
  }
};

template <typename Range>
ZRangeNode<Range>* ZMemoryManagerImpl<Range>::create(offset start, size_t size) {
  ZMemory* const area = new ZMemory(start, size);
  if (_callbacks._create != nullptr) {
    _callbacks._create(*area->range());
  }
  return area;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::destroy(ZMemory* area) {
  if (_callbacks._destroy != nullptr) {
    _callbacks._destroy(*area->range());
  }
  delete area;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::shrink_from_front(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_front != nullptr) {
    _callbacks._shrink_from_front(*area->range(), size);
  }
  area->range()->shrink_from_front(size);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::shrink_from_back(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_back != nullptr) {
    _callbacks._shrink_from_back(*area->range(), size);
  }
  area->range()->shrink_from_back(size);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::grow_from_front(ZMemory* area, size_t size) {
  if (_callbacks._grow_from_front != nullptr) {
    _callbacks._grow_from_front(*area->range(), size);
  }
  area->range()->grow_from_front(size);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::grow_from_back(ZMemory* area, size_t size) {
  if (_callbacks._grow_from_back != nullptr) {
    _callbacks._grow_from_back(*area->range(), size);
  }
  area->range()->grow_from_back(size);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::split_from_front(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_front != nullptr) {
    _callbacks._shrink_from_front(*area->range(), size);
  }
  return area->range()->split_from_front(size);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::split_from_back(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_back != nullptr) {
    _callbacks._shrink_from_back(*area->range(), size);
  }
  return area->range()->split_from_back(size);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::alloc_low_address_inner(size_t size) {
  ZListIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() >= size) {
      if (area->size() == size) {
        // Exact match, remove area
        const Range range = *area->range();
        _freelist.remove(area);
        destroy(area);
        return range;
      } else {
        // Larger than requested, shrink area
        return split_from_front(area, size);
      }
    }
  }

  // Out of memory
  return Range();
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::alloc_low_address_at_most_inner(size_t size) {
  ZMemory* const area = _freelist.first();
  if (area != nullptr) {
    if (area->size() <= size) {
      // Smaller than or equal to requested, remove area
      const Range range = *area->range();
      _freelist.remove(area);
      destroy(area);
      return range;
    } else {
      // Larger than requested, shrink area
      return split_from_front(area, size);
    }
  }

  return Range();
}

template <typename Range>
void ZMemoryManagerImpl<Range>::free_inner(offset start, size_t size) {
  assert(start != offset::invalid, "Invalid address");
  const offset_end end = to_end_type(start, size);

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

template <typename Range>
size_t ZMemoryManagerImpl<Range>::alloc_low_address_many_at_most_inner(size_t size, ZArray<Range>* out) {
  size_t to_allocate = size;

  while (to_allocate > 0) {
    const Range range = alloc_low_address_at_most_inner(to_allocate);

    if (range.is_null()) {
      // Out of memory
      return size - to_allocate;
    }

    to_allocate -= range.size();
    out->append(range);
  }

  return size;
}

template <typename Range>
ZMemoryManagerImpl<Range>::Callbacks::Callbacks()
  : _create(nullptr),
    _destroy(nullptr),
    _shrink_from_front(nullptr),
    _shrink_from_back(nullptr),
    _grow_from_front(nullptr),
    _grow_from_back(nullptr) {}

template <typename Range>
ZMemoryManagerImpl<Range>::ZMemoryManagerImpl()
  : _freelist(),
    _callbacks() {}

template <typename Range>
bool ZMemoryManagerImpl<Range>::free_is_contiguous() const {
  return _freelist.size() == 1;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::register_callbacks(const Callbacks& callbacks) {
  _callbacks = callbacks;
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::total_range() const {
  ZLocker<ZLock> locker(&_lock);

  if (_freelist.is_empty()) {
    return Range();
  }

  const offset start = _freelist.first()->start();
  const size_t size = _freelist.last()->end() - start;

  return Range(start, size);
}

template <typename Range>
typename ZMemoryManagerImpl<Range>::offset ZMemoryManagerImpl<Range>::peek_low_address() const {
  ZLocker<ZLock> locker(&_lock);

  const ZMemory* const area = _freelist.first();
  if (area != nullptr) {
    return area->start();
  }

  // Out of memory
  return offset::invalid;
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::alloc_low_address(size_t size) {
  ZLocker<ZLock> locker(&_lock);
  Range range = alloc_low_address_inner(size);
  return range;
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::alloc_low_address_at_most(size_t size) {
  ZLocker<ZLock> lock(&_lock);
  Range range = alloc_low_address_at_most_inner(size);
  return range;
}

template <typename Range>
size_t ZMemoryManagerImpl<Range>::alloc_low_address_many_at_most(size_t size, ZArray<Range>* out) {
  ZLocker<ZLock> lock(&_lock);
  return alloc_low_address_many_at_most_inner(size, out);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::alloc_high_address(size_t size) {
  ZLocker<ZLock> locker(&_lock);

  ZListReverseIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() >= size) {
      if (area->size() == size) {
        // Exact match, remove area
        const Range range = *area->range();
        _freelist.remove(area);
        destroy(area);
        return range;
      } else {
        // Larger than requested, shrink area
        return split_from_back(area, size);
      }
    }
  }

  // Out of memory
  return Range();
}

template <typename Range>
void ZMemoryManagerImpl<Range>::transfer_low_address(ZMemoryManagerImpl* other, size_t size) {
  assert(other->_freelist.is_empty(), "Should only be used for initialization");

  ZLocker<ZLock> locker(&_lock);
  size_t to_move = size;

  ZListIterator<ZMemory> iter(&_freelist);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() <= to_move) {
      // Smaller than or equal to requested, remove from this freelist and
      // insert in other's freelist
      to_move -= area->size();
      _freelist.remove(area);
      other->_freelist.insert_last(area);
    } else {
      // Larger than requested, shrink area
      const offset start = area->start();
      shrink_from_front(area, to_move);
      other->free(start, to_move);
      to_move = 0;
    }

    if (to_move == 0) {
      break;
    }
  }
}

template <typename Range>
int ZMemoryManagerImpl<Range>::shuffle_memory_low_addresses(offset start, size_t size, ZArray<Range>* out) {
  ZLocker<ZLock> locker(&_lock);
  free_inner(start, size);
  const int length_before = out->length();
  const size_t shuffled = alloc_low_address_many_at_most_inner(size, out);
  assert(shuffled == size, "must succeed");
  return out->length() - length_before;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::shuffle_memory_low_addresses_contiguous(size_t size, ZArray<Range>* out) {
  ZLocker<ZLock> locker(&_lock);

  size_t freed = 0;

  // Free everything
  ZArrayIterator<Range> iter(out);
  for (Range mem; iter.next(&mem);) {
    free_inner(mem.start(), mem.size());
    freed += mem.size();
  }

  // Clear stored memory so that we can populate it below
  out->clear();

  // Try to allocate a contiguous chunk
  Range range = alloc_low_address_inner(size);
  if (!range.is_null()) {
    out->append(range);
    return;
  }

  // Failed to allocate a contiguous chunk, split it up into smaller chunks and
  // only allocate up to as much that has been freed
  alloc_low_address_many_at_most_inner(freed, out);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::free(offset start, size_t size) {
  ZLocker<ZLock> locker(&_lock);
  free_inner(start, size);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::free(const Range& range) {
  free(range.start(), range.size());
}

// Instantiate the concrete classes
template class ZMemoryManagerImpl<ZMemoryRange>;
template class ZMemoryManagerImpl<ZBackingIndexRange>;
