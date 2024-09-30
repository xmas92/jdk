/*
 * Copyright (c) 2015, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZPHYSICALMEMORY_HPP
#define SHARE_GC_Z_ZPHYSICALMEMORY_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zMemory.hpp"
#include "gc/z/zValue.hpp"
#include "memory/allocation.hpp"
#include OS_HEADER(gc/z/zPhysicalMemoryBacking)

class ZPhysicalMemoryManager {
private:
  ZPhysicalMemoryBacking   _backing;
  ZPerNUMA<ZMemoryManager> _managers;

public:
  ZPhysicalMemoryManager(size_t max_capacity);

  bool is_initialized() const;

  void warn_commit_limits(size_t max_capacity) const;
  void try_enable_uncommit(size_t min_capacity, size_t max_capacity);

  void alloc(zoffset* pmem, size_t size, int numa_id = -1);
  void free(const zoffset* pmem, size_t size, int numa_id);

  size_t commit(const zoffset* pmem, size_t size, int numa_id = -1);
  size_t uncommit(const zoffset* pmem, size_t size);

  void map(zoffset offset, const zoffset* pmem, size_t size) const;
  void unmap(zoffset offset, const zoffset* pmem, size_t size) const;

  size_t count_segments(const zoffset* pmem, size_t size);
};

#endif // SHARE_GC_Z_ZPHYSICALMEMORY_HPP
