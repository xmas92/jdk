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

#ifndef SHARE_GC_Z_ZUTILS_INLINE_HPP
#define SHARE_GC_Z_ZUTILS_INLINE_HPP

#include "gc/z/zUtils.hpp"

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zSize.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/copy.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

inline uintptr_t ZUtils::alloc_aligned_unfreeable(zbytes alignment, zbytes size) {
  const zbytes padded_size = size + (alignment - 1_zb);
  void* const addr = os::malloc(untype(padded_size), mtGC);
  void* const aligned_addr = align_up(addr, alignment);

  memset(aligned_addr, 0, untype(size));

  // Since free expects pointers returned by malloc, aligned_addr cannot be
  // freed since it is most likely not the same as addr after alignment.
  return (uintptr_t)aligned_addr;
}

inline zbytes ZUtils::object_size(oop obj) {
  return ZBytes::from_words(obj->size());
}

inline zbytes ZUtils::object_size(zaddress addr) {
  return object_size(to_oop(addr));
}

inline void ZUtils::object_copy_disjoint(zaddress from, zaddress to, zbytes size) {
  Copy::aligned_disjoint_words((HeapWord*)untype(from), (HeapWord*)untype(to), untype(ZBytes::to_words(size)));
}

inline void ZUtils::object_copy_conjoint(zaddress from, zaddress to, zbytes size) {
  if (from != to) {
    Copy::aligned_conjoint_words((HeapWord*)untype(from), (HeapWord*)untype(to), untype(ZBytes::to_words(size)));
  }
}

template <typename T>
inline void ZUtils::copy_disjoint(T* dest, const T* src, size_t count) {
  memcpy(dest, src, sizeof(T) * count);
}

template <typename T>
inline void ZUtils::copy_disjoint(T* dest, const T* src, int count) {
  assert(count >= 0, "must be positive %d", count);

  copy_disjoint(dest, src, static_cast<size_t>(count));
}

template <typename T, typename Comparator>
inline void ZUtils::sort(T* array, size_t count, Comparator comparator) {
  using SortType = int(const void*, const void*);
  using ComparatorType = int(const T*, const T*);

  ComparatorType* const comparator_fn_ptr = comparator;

  // We rely on ABI compatibility between ComparatorType and SortType
  qsort(array, count, sizeof(T), reinterpret_cast<SortType*>(comparator_fn_ptr));
}

template <typename T, typename Comparator>
inline void ZUtils::sort(T* array, int count, Comparator comparator) {
  assert(count >= 0, "must be positive %d", count);

  sort(array, static_cast<size_t>(count), comparator);
}

#endif // SHARE_GC_Z_ZUTILS_INLINE_HPP
