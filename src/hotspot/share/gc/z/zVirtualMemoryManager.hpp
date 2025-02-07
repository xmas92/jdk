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

#ifndef SHARE_GC_Z_ZVIRTUALMEMORY_HPP
#define SHARE_GC_Z_ZVIRTUALMEMORY_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zMemory.hpp"
#include "gc/z/zValue.hpp"

class ZVirtualMemoryManager {
  friend class ZMapperTest;

private:
  static size_t calculate_min_range(size_t size);

  ZPerNUMA<ZMemoryManager> _managers;
  ZPerNUMA<ZMemoryRange> _vmem_ranges;
  bool                     _initialized;

  // Platform specific implementation
  void pd_initialize_before_reserve();
  void pd_initialize_after_reserve();
  bool pd_reserve(zaddress_unsafe addr, size_t size);
  void pd_unreserve(zaddress_unsafe addr, size_t size);

  bool reserve_contiguous(zoffset start, size_t size);
  bool reserve_contiguous(size_t size);
  size_t reserve_discontiguous(zoffset start, size_t size, size_t min_range);
  size_t reserve_discontiguous(size_t size);
  bool reserve(size_t max_capacity);

  void set_vmem_range_for_manager(int numa_id);
  void initialize_managers(size_t size);

  DEBUG_ONLY(size_t force_reserve_discontiguous(size_t size);)

public:
  ZVirtualMemoryManager(size_t max_capacity);

  bool is_initialized() const;

  int shuffle_vmem_to_low_addresses(const ZMemoryRange& vmem, ZArray<ZMemoryRange>* out);
  void shuffle_vmem_to_low_addresses_contiguous(size_t size, ZArray<ZMemoryRange>* mappings);

  ZMemoryRange alloc(size_t size, int numa_id, bool force_low_address);
  void free(const ZMemoryRange& vmem);

  int get_numa_id(const ZMemoryRange& vmem) const;
  zoffset lowest_available_address(int numa_id) const;
};

#endif // SHARE_GC_Z_ZVIRTUALMEMORY_HPP
