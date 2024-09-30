/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP
#define SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP

#include "gc/z/zPageAllocator.hpp"

inline ZNUMALocalStats::ZNUMALocalStats()
  : _current_max_capacity(0),
    _capacity(0),
    _used(0),
    _used_high(0),
    _used_low(0),
    _used_generation(0) {}

inline ZNUMALocalStats::ZNUMALocalStats(size_t current_max_capacity,
                                        size_t capacity,
                                        size_t used,
                                        size_t used_high,
                                        size_t used_low,
                                        size_t used_generation)
  : _current_max_capacity(current_max_capacity),
    _capacity(capacity),
    _used(used),
    _used_high(used_high),
    _used_low(used_low),
    _used_generation(used_generation) {}

inline size_t ZNUMALocalStats::current_max_capacity() const {
  return _current_max_capacity;
}

inline void ZNUMALocalStats::aggregate(const ZNUMALocalStats& stats) {
  _current_max_capacity += stats._current_max_capacity;
  _capacity += stats._capacity;
  _used += stats._used;
  _used_high += stats._used_high;
  _used_low += stats._used_low;
  _used_generation += stats._used_generation;
}

inline ZPageAllocatorStats::ZPageAllocatorStats(size_t min_capacity,
                                                size_t max_capacity,
                                                size_t soft_max_capacity,
                                                size_t freed,
                                                size_t promoted,
                                                size_t compacted,
                                                size_t allocation_stalls,
                                                const ZNUMALocalStats& local_stats)
  : _min_capacity(min_capacity),
    _max_capacity(max_capacity),
    _soft_max_capacity(soft_max_capacity),
    _capacity(local_stats._capacity),
    _used(local_stats._used),
    _used_high(local_stats._used_high),
    _used_low(local_stats._used_low),
    _used_generation(local_stats._used_generation),
    _freed(freed),
    _promoted(promoted),
    _compacted(compacted),
    _allocation_stalls(allocation_stalls) {}

inline size_t ZPageAllocatorStats::min_capacity() const {
  return _min_capacity;
}

inline size_t ZPageAllocatorStats::max_capacity() const {
  return _max_capacity;
}

inline size_t ZPageAllocatorStats::soft_max_capacity() const {
  return _soft_max_capacity;
}

inline size_t ZPageAllocatorStats::capacity() const {
  return _capacity;
}

inline size_t ZPageAllocatorStats::used() const {
  return _used;
}

inline size_t ZPageAllocatorStats::used_high() const {
  return _used_high;
}

inline size_t ZPageAllocatorStats::used_low() const {
  return _used_low;
}

inline size_t ZPageAllocatorStats::used_generation() const {
  return _used_generation;
}

inline size_t ZPageAllocatorStats::freed() const {
  return _freed;
}

inline size_t ZPageAllocatorStats::promoted() const {
  return _promoted;
}

inline size_t ZPageAllocatorStats::compacted() const {
  return _compacted;
}

inline size_t ZPageAllocatorStats::allocation_stalls() const {
  return _allocation_stalls;
}

#endif // SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP
