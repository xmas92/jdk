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

ZMappedMemory::ZMappedMemory()
  : _vmem(),
    _pmem() {}

ZMappedMemory::ZMappedMemory(const ZVirtualMemory &vmem, const ZPhysicalMemory& pmem)
  : _vmem(vmem),
    _pmem(pmem) {}

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

size_t ZMappedMemory::size() const {
  return _vmem.size();
}

ZMappedMemory ZMappedMemory::split(size_t size) {
  return ZMappedMemory(_vmem.split(size), _pmem.split_unsorted(size));
}

ZMappedMemory ZMappedMemory::split_committed() {
  const ZPhysicalMemory pmem = _pmem.split_committed();
  if (pmem.is_null()) {
    return ZMappedMemory();
  }

  assert(!_pmem.is_null(), "Should not be null");
  const ZVirtualMemory vmem = _vmem.split(pmem.size());

  assert(vmem.end() == _vmem.start(), "Should be consecutive");

  log_trace(gc, page)("Split memory [" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT "]",
      untype(vmem.start()),
      untype(vmem.end()),
      untype(_vmem.end()));

  return ZMappedMemory(vmem, pmem);
}

bool ZMappedMemory::virtually_adjacent_to(const ZMappedMemory& other) const {
  return zoffset(_vmem.end()) == other.virtual_memory().start() ||
         zoffset(other.virtual_memory().end()) == _vmem.start();
}

void ZMappedMemory::extend_mapping(const ZMappedMemory& right) {
  // Increase virtual memory size by right's size.
  _vmem = ZVirtualMemory(_vmem.start(), _vmem.size() + right.size());

  // Combine physical memory segments in the order the appear (i.e unsorted).
  _pmem.append_segments(right.physical_memory());
}

const ZVirtualMemory& ZMappedMemory::virtual_memory() const {
  return _vmem;
}

const ZPhysicalMemory& ZMappedMemory::physical_memory() const {
  return _pmem;
}

ZPhysicalMemory& ZMappedMemory::physical_memory() {
  return _pmem;
}
