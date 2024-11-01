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

ZMappedMemory::ZMappedMemory()
  : _vmem(),
    _pmem() {}

ZMappedMemory::ZMappedMemory(const ZVirtualMemory &vmem, const ZPhysicalMemory& pmem)
  : _vmem(vmem),
    _pmem(pmem) {
  assert(vmem.size() == pmem.size(), "Virtual/Physical size mismatch");
}

ZMappedMemory::ZMappedMemory(const ZMappedMemory& other) 
  : _vmem(other._vmem),
    _pmem(other._pmem) {}

const ZMappedMemory& ZMappedMemory::operator=(const ZMappedMemory& other) {
  _vmem = other._vmem;
  _pmem = other._pmem;
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
  return _pmem.nsegments();
}

ZMappedMemory ZMappedMemory::split(size_t size) {
  return ZMappedMemory(_vmem.split(size), _pmem.split_unsorted(size));
}

bool ZMappedMemory::virtually_adjacent_to(const ZMappedMemory& other) const {
  return zoffset(_vmem.end()) == other._vmem.start() ||
         zoffset(other._vmem.end()) == _vmem.start();
}

void ZMappedMemory::extend_mapping(const ZMappedMemory& right) {
  // Increase virtual memory size by right's size.
  _vmem = ZVirtualMemory(_vmem.start(), _vmem.size() + right.size());

  // Combine physical memory segments in the order they appear (i.e unsorted).
  _pmem.append_segments(right.unsorted_physical_memory());
}

const ZVirtualMemory& ZMappedMemory::virtual_memory() const {
  return _vmem;
}

const ZPhysicalMemory& ZMappedMemory::unsorted_physical_memory() const {
  return _pmem;
}
