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

#ifndef SHARE_GC_Z_ZRANGE_INLINE_HPP
#define SHARE_GC_Z_ZRANGE_INLINE_HPP

#include "gc/z/zRange.hpp"

#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size>::ZRange(End start, Size size, End end)
  : _start(start),
    _size(size) {
  postcond(this->end() == end);
}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size>::ZRange()
  : _start(End::invalid),
    _size{} {}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size>::ZRange(Start start, Size size)
  : _start(to_end_type(start, {})),
    _size(size) {}

template <typename Start, typename End, typename Size>
inline bool ZRange<Start, End, Size>::is_null() const {
  return _start == End::invalid;
}

template <typename Start, typename End, typename Size>
inline Start ZRange<Start, End, Size>::start() const {
  return to_start_type(_start);
}

template <typename Start, typename End, typename Size>
inline End ZRange<Start, End, Size>::end() const {
  return _start + _size;
}

template <typename Start, typename End, typename Size>
inline Size ZRange<Start, End, Size>::size() const {
  return _size;
}

template <typename Start, typename End, typename Size>
inline bool ZRange<Start, End, Size>::operator==(const ZRange& other) const {
  precond(!is_null());
  precond(!other.is_null());

  return _start == other._start && _size == other._size;
}

template <typename Start, typename End, typename Size>
inline bool ZRange<Start, End, Size>::operator!=(const ZRange& other) const {
  return !operator==(other);
}

template <typename Start, typename End, typename Size>
inline bool ZRange<Start, End, Size>::contains(const ZRange& other) const {
  precond(!is_null());
  precond(!other.is_null());

  return _start <= other._start && other.end() <= end();
}

template <typename Start, typename End, typename Size>
inline void ZRange<Start, End, Size>::grow_from_front(Size size) {
  precond(Size(start()) >= size);

  _start -= size;
  _size  += size;
}

template <typename Start, typename End, typename Size>
inline void ZRange<Start, End, Size>::grow_from_back(Size size) {
  _size += size;
}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size> ZRange<Start, End, Size>::shrink_from_front(Size size) {
  precond(this->size() >= size);

  _start += size;
  _size  -= size;

  return ZRange(_start - size, size, _start);
}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size> ZRange<Start, End, Size>::shrink_from_back(Size size) {
  precond(this->size() >= size);

  _size -= size;

  return ZRange(end(), size, end() + size);
}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size> ZRange<Start, End, Size>::partition(Size offset, Size partition_size) const {
  precond(size() - offset >= partition_size);

  return ZRange(_start + offset, partition_size, _start + offset + partition_size);
}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size> ZRange<Start, End, Size>::first_part(Size split_offset) const {
  return partition({}, split_offset);
}

template <typename Start, typename End, typename Size>
inline ZRange<Start, End, Size> ZRange<Start, End, Size>::last_part(Size split_offset) const {
  return partition(split_offset, size() - split_offset);
}

template <typename Start, typename End, typename Size>
inline bool ZRange<Start, End, Size>::adjacent_to(const ZRange<Start, End, Size>& other) const {
  return end() == other.start() || other.end() == start();
}

#endif // SHARE_GC_Z_ZRANGE_INLINE_HPP
