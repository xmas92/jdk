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

#ifndef SHARE_GC_Z_ZMEMORY_HPP
#define SHARE_GC_Z_ZMEMORY_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "memory/allocation.hpp"

template <typename Range>
class ZRangeNode;

template <typename Start, typename End>
class ZRange {
  friend class VMStructs;

public:
  using offset     = Start;
  using offset_end = End;

private:
  Start _start;
  End   _end;

public:
  ZRange();
  ZRange(Start start, size_t size);

  bool is_null() const;

  Start start() const;
  End end() const;

  size_t size() const;

  void shrink_from_front(size_t size);
  void shrink_from_back(size_t size);
  void grow_from_front(size_t size);
  void grow_from_back(size_t size);

  ZRange split_from_front(size_t size);
  ZRange split_from_back(size_t size);

  bool adjacent_to(const ZRange& other) const;
};

class ZMemoryRange : public ZRange<zoffset, zoffset_end> {
public:
  ZMemoryRange();
  ZMemoryRange(zoffset start, size_t size);
  ZMemoryRange(const ZRange<zoffset, zoffset_end>& range);

  size_t size_in_granules() const;
};

using ZBackingIndexRange = ZRange<zbacking_index, zbacking_index_end>;

template <typename Range>
class ZMemoryManagerImpl {
private:
  using ZMemory = ZRangeNode<Range>;

public:
  using offset     = typename Range::offset;
  using offset_end = typename Range::offset_end;
  typedef void (*CreateDestroyCallback)(const Range& range);
  typedef void (*ResizeCallback)(const Range& range, size_t size);

  struct Callbacks {
    CreateDestroyCallback _create;
    CreateDestroyCallback _destroy;
    ResizeCallback        _shrink_from_front;
    ResizeCallback        _shrink_from_back;
    ResizeCallback        _grow_from_front;
    ResizeCallback        _grow_from_back;

    Callbacks();
  };

private:
  mutable ZLock  _lock;
  ZList<ZMemory> _freelist;
  Callbacks      _callbacks;

  ZMemory* create(offset start, size_t size);
  void destroy(ZMemory* area);
  void shrink_from_front(ZMemory* area, size_t size);
  void shrink_from_back(ZMemory* area, size_t size);
  void grow_from_front(ZMemory* area, size_t size);
  void grow_from_back(ZMemory* area, size_t size);
  Range split_from_front(ZMemory* area, size_t size);
  Range split_from_back(ZMemory* area, size_t size);

  Range alloc_low_address_inner(size_t size);
  Range alloc_low_address_at_most_inner(size_t size);
  void free_inner(offset start, size_t size);

  int alloc_low_address_many_at_most_inner(size_t size, ZArray<Range>* out);

public:
  ZMemoryManagerImpl();

  bool free_is_contiguous() const;

  void register_callbacks(const Callbacks& callbacks);

  Range total_range() const;

  offset peek_low_address() const;
  Range alloc_low_address(size_t size);
  Range alloc_low_address_at_most(size_t size);
  Range alloc_high_address(size_t size);

  void transfer_low_address(ZMemoryManagerImpl* other, size_t size);
  int shuffle_memory_low_addresses(offset start, size_t size, ZArray<Range>* out);
  void shuffle_memory_low_addresses_contiguous(size_t size, ZArray<Range>* out);

  void free(offset start, size_t size);
  void free(const Range& range);
};

#endif // SHARE_GC_Z_ZMEMORY_HPP
