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
#include "gc/z/zGranuleMap.inline.hpp"
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "gc/z/zSegmentTable.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "utilities/debug.hpp"

ZSegmentTable::ZSegmentTable()
  : _map(ZAddressOffsetMax) {}

void ZSegmentTable::insert(const ZVirtualMemory& vmem, const ZPhysicalMemory& pmem) {
  // Insert all physical memory segments to their corresponding virtual address
  // in the map.
  zoffset vmem_offset = vmem.start();
  for (int i = 0; i < pmem.nsegments(); i++) {

    ZPhysicalMemorySegment segment = pmem.segment(i);

    for (zoffset seg_offset = segment.start(); seg_offset < segment.end(); seg_offset += ZGranuleSize) {
      _map.put(vmem_offset, seg_offset);
      vmem_offset += ZGranuleSize;
    }
  }
}

ZPhysicalMemory ZSegmentTable::remove(const ZVirtualMemory& vmem) {
  ZPhysicalMemory pmem;

  for (zoffset current = vmem.start(); current < vmem.end(); current +=  ZGranuleSize) {
    ZPhysicalMemorySegment segment = ZPhysicalMemorySegment(_map.get(current), ZGranuleSize, true);
    pmem.combine_and_sort_segment(segment);
  }

  return pmem;
}
