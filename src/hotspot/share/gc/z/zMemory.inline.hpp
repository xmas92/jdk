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

#ifndef SHARE_GC_Z_ZMEMORY_INLINE_HPP
#define SHARE_GC_Z_ZMEMORY_INLINE_HPP

#include "gc/z/zMemory.hpp"

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zList.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"


template <typename Start, typename End>
inline ZRange<Start, End>::ZRange()
  : _start(Start::invalid),
    _end(End::invalid) {}

template <typename Start, typename End>
inline ZRange<Start, End>::ZRange(Start start, size_t size)
  : _start(start),
    _end(to_end_type(start, size)) {}

template <typename Start, typename End>
inline bool ZRange<Start, End>::is_null() const {
  return _start == Start::invalid;
}

template <typename Start, typename End>
inline Start ZRange<Start, End>::start() const {
  return _start;
}

template <typename Start, typename End>
inline End ZRange<Start, End>::end() const {
  return _end;
}

template <typename Start, typename End>
inline size_t ZRange<Start, End>::size() const {
  return end() - start();
}

template <typename Start, typename End>
inline void ZRange<Start, End>::shrink_from_front(size_t size) {
  assert(this->size() >= size, "Too small");
  _start += size;
}

template <typename Start, typename End>
inline void ZRange<Start, End>::shrink_from_back(size_t size) {
  assert(this->size() >= size, "Too small");
  _end -= size;
}

template <typename Start, typename End>
inline void ZRange<Start, End>::grow_from_front(size_t size) {
  assert(size_t(start()) >= size, "Too big");
  _start -= size;
}

template <typename Start, typename End>
inline void ZRange<Start, End>::grow_from_back(size_t size) {
  _end += size;
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::split_from_front(size_t size) {
  shrink_from_front(size);
  return ZRange(_start - size, size);
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::split_from_back(size_t size) {
  shrink_from_back(size);
  return ZRange(to_start_type(_end), size);
}

template <typename Start, typename End>
inline bool ZRange<Start, End>::adjacent_to(const ZRange<Start, End>& other) const {
  return end() == other.start() || other.end() == start();
}

inline ZMemoryRange::ZMemoryRange()
  : ZRange() {}

inline ZMemoryRange::ZMemoryRange(zoffset start, size_t size)
  : ZRange(start, size) {
  // ZMemoryRange is only used for ZGranuleSize multiple ranges
  assert(is_aligned(untype(start), ZGranuleSize), "must be multiple of ZGranuleSize");
  assert(is_aligned(size, ZGranuleSize), "must be multiple of ZGranuleSize");
}

inline ZMemoryRange::ZMemoryRange(const ZRange<zoffset, zoffset_end>& range)
  : ZMemoryRange(range.start(), range.size()) {}

inline size_t ZMemoryRange::size_in_granules() const {
  return size() >> ZGranuleSizeShift;
}

#endif // SHARE_GC_Z_ZMEMORY_INLINE_HPP
