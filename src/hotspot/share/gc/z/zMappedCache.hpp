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

#ifndef SHARE_GC_Z_ZMAPPEDCACHE_HPP
#define SHARE_GC_Z_ZMAPPEDCACHE_HPP

#include "gc/z/zArray.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zMappedMemory.hpp"
#include "gc/z/zPage.hpp"

#include "nmt/nmtTreap.hpp"

class ZMappedCache {
  friend class ZMappedCacheTest;

private:
  class ZOffsetComparator {
  public:
    static int cmp(zoffset a, zoffset b) {
      if (a <  b) return -1;
      if (a == b) return  0;
      if (a >  b) return  1;
      ShouldNotReachHere();
    }
  };

  using ZMappedTreap = TreapCHeap<zoffset, ZMappedMemory, ZOffsetComparator>;
  using ZMappedTreapNode = ZMappedTreap::TreapNode;

  ZMappedTreap _tree;

public:
  ZMappedCache();

  size_t remove_mappings(ZArray<ZMappedMemory>* mappings, size_t size);
  ZMappedMemory remove_mapping_contiguous(size_t size);

  // TODO: Insert istället
  void free_mapping(ZMappedMemory mapping);

  size_t flush(ZArray<ZMappedMemory>* mappings, size_t size, uint64_t* timeout);
};

#endif // SHARE_GC_Z_ZMAPPEDCACHE_HPP
