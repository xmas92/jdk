/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "logging/log.hpp"
#include "gc/z/zMappedMemory.hpp"
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "gc/z/zUtils.inline.hpp"

void ZMappedPhysicalMemory::append(const ZPhysicalMemorySegment& segment) {
  _segments.append(segment);
}

ZMappedPhysicalMemory::ZMappedPhysicalMemory()
  : _segments() {}

ZMappedPhysicalMemory::ZMappedPhysicalMemory(const ZPhysicalMemory& pmem)
  : _segments(pmem.nsegments()) {
  _segments.appendAll(&pmem._segments);
}

ZMappedPhysicalMemory::ZMappedPhysicalMemory(const ZMappedPhysicalMemory& other)
  : _segments(other.nsegments()) {
  _segments.appendAll(&other._segments);
}

const ZMappedPhysicalMemory& ZMappedPhysicalMemory::operator=(const ZMappedPhysicalMemory& other) {
  // Check for self-assignment
  if (this == &other) {
    return *this;
  }

  // Free and copy segments
  _segments.clear_and_deallocate();
  _segments.reserve(other.nsegments());
  _segments.appendAll(&other._segments);

  return *this;
}

bool ZMappedPhysicalMemory::is_null() const {
  return _segments.is_empty();
}

size_t ZMappedPhysicalMemory::size() const {
  size_t size = 0;

  for (int i = 0; i < _segments.length(); i++) {
    size += _segments.at(i).size();
  }

  return size;
}

int ZMappedPhysicalMemory::nsegments() const {
  return _segments.length();
}

void ZMappedPhysicalMemory::combine(const ZMappedPhysicalMemory& mpmem) {
  _segments.appendAll(&mpmem._segments);
}

ZMappedPhysicalMemory ZMappedPhysicalMemory::split(size_t size) {
  ZMappedPhysicalMemory mpmem;
  int nsegments = 0;

  for (int i = 0; i < _segments.length(); i++) {
    const ZPhysicalMemorySegment& segment = _segments.at(i);
    if (mpmem.size() < size) {
      if (mpmem.size() + segment.size() <= size) {
        // Transfer segment
        mpmem.append(segment);
      } else {
        // Split segment
        const size_t split_size = size - mpmem.size();
        mpmem.append(ZPhysicalMemorySegment(segment.start(), split_size, segment.is_committed()));
        _segments.at_put(nsegments++, ZPhysicalMemorySegment(segment.start() + split_size, segment.size() - split_size, segment.is_committed()));
      }
    } else {
      // Keep segment
      _segments.at_put(nsegments++, segment);
    }
  }

  _segments.trunc_to(nsegments);

  return mpmem;
}

ZPhysicalMemory ZMappedPhysicalMemory::sorted_physical() const {
  ZPhysicalMemory pmem;

  for (int i = 0; i < nsegments(); i++) {
    pmem.combine_and_sort_segment(_segments.at(i));
  }

  return pmem;
}

ZMappedMemory::ZMappedMemory(const ZVirtualMemory &vmem, const ZMappedPhysicalMemory& mpmem)
  : _vmem(vmem),
    _mpmem(mpmem) {
}

ZMappedMemory::ZMappedMemory()
  : _vmem(),
    _mpmem() {}

ZMappedMemory::ZMappedMemory(const ZVirtualMemory &vmem, const ZPhysicalMemory& pmem)
  : _vmem(vmem),
    _mpmem(pmem) {
  assert(vmem.size() == pmem.size(), "Virtual/Physical size mismatch");
}

ZMappedMemory::ZMappedMemory(const ZMappedMemory& other) 
  : _vmem(other._vmem),
    _mpmem(other._mpmem) {}

const ZMappedMemory& ZMappedMemory::operator=(const ZMappedMemory& other) {
  _vmem = other._vmem;
  _mpmem = other._mpmem;
  return *this;
}

bool ZMappedMemory::is_null() const {
  return _vmem.is_null();
}

zoffset ZMappedMemory::start() const {
  return _vmem.start();
}

zoffset_end ZMappedMemory::end() const {
  return _vmem.end();
}

size_t ZMappedMemory::size() const {
  return _vmem.size();
}

int ZMappedMemory::nsegments() const {
  return _mpmem.nsegments();
}

ZMappedMemory ZMappedMemory::split(size_t size) {
  return ZMappedMemory(_vmem.split(size), _mpmem.split(size));
}

bool ZMappedMemory::virtually_adjacent_to(const ZMappedMemory& other) const {
  return zoffset(_vmem.end()) == other._vmem.start() ||
         zoffset(other._vmem.end()) == _vmem.start();
}

void ZMappedMemory::extend_mapping(const ZMappedMemory& right) {
  // Increase virtual memory size by right's size.
  _vmem = ZVirtualMemory(_vmem.start(), _vmem.size() + right.size());

  // Combine physical memory segments in the order they appear
  _mpmem.combine(right._mpmem);
}

const ZVirtualMemory& ZMappedMemory::virtual_memory() const {
  return _vmem;
}

ZPhysicalMemory ZMappedMemory::physical_memory() const {
  return _mpmem.sorted_physical();
}
