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

#ifndef SHARE_GC_Z_ZMAPPEDMEMORY_HPP
#define SHARE_GC_Z_ZMAPPEDMEMORY_HPP

#include "gc/z/zPhysicalMemory.hpp"
#include "gc/z/zVirtualMemory.hpp"

class ZMappedPhysicalMemory {
private:
  ZArray<ZPhysicalMemorySegment> _segments;

  void append(const ZPhysicalMemorySegment& segment);

public:
  ZMappedPhysicalMemory();
  ZMappedPhysicalMemory(const ZPhysicalMemory& pmem);
  const ZMappedPhysicalMemory& operator=(const ZMappedPhysicalMemory& other);
  ZMappedPhysicalMemory(const ZMappedPhysicalMemory& other);

  bool is_null() const;
  size_t size() const;

  int nsegments() const;

  void combine(const ZMappedPhysicalMemory& mpmem);
  ZMappedPhysicalMemory split(size_t size);

  ZPhysicalMemory sorted_physical() const;
};

class ZMappedMemory {
private:
  ZVirtualMemory  _vmem;
  ZMappedPhysicalMemory _mpmem;

  ZMappedMemory(const ZVirtualMemory& vmem, const ZMappedPhysicalMemory& mpmem);

public:
  ZMappedMemory();
  ZMappedMemory(const ZVirtualMemory& vmem, const ZPhysicalMemory& pmem);
  ZMappedMemory(const ZMappedMemory& other);
  const ZMappedMemory& operator=(const ZMappedMemory& other);

  bool is_null() const;
  zoffset start() const;
  zoffset_end end() const;
  size_t size() const;

  int nsegments() const;

  ZMappedMemory split(size_t size);
  bool virtually_adjacent_to(const ZMappedMemory& other) const;
  void extend_mapping(const ZMappedMemory& right);

  const ZVirtualMemory& virtual_memory() const;
  ZPhysicalMemory physical_memory() const;
};

#endif // SHARE_GC_Z_ZMAPPEDMEMORY_HPP
