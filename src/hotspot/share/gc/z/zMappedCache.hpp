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
#include "gc/z/zIntrusiveRBTree.hpp"
#include "gc/z/zMemory.hpp"
#include "utilities/globalDefinitions.hpp"

class ZMappedCacheEntry;
class ZMappedCache {
  friend class ZMappedCacheTest;
  friend class ZMappedCacheEntry;

private:
  struct EntryCompare {
    int operator()(ZIntrusiveRBTreeNode* a, ZIntrusiveRBTreeNode* b);
    int operator()(zoffset key, ZIntrusiveRBTreeNode* node);
  };

  using Tree = ZIntrusiveRBTree<zoffset, EntryCompare>;
  using Node = ZIntrusiveRBTreeNode;

  struct ZSizeClassListNode {
    ZListNode<ZSizeClassListNode> _node;
  };
  static constexpr size_t SizeClasses[] = {32 * M, 512 * M};
  static constexpr size_t NumSizeClasses = ARRAY_SIZE(SizeClasses);

  Tree _tree;
  ZList<ZSizeClassListNode> _size_class_lists[NumSizeClasses];
  size_t _size;
  size_t _min;

  static size_t get_size_class(size_t index);

  void insert(const Tree::FindCursor& cursor, const ZMemoryRange& vmem);
  void remove(const Tree::FindCursor& cursor, const ZMemoryRange& vmem);
  void replace(const Tree::FindCursor& cursor, const ZMemoryRange& vmem);
  void update(ZMappedCacheEntry* entry, const ZMemoryRange& vmem);

public:
  ZMappedCache();

  void insert_mapping(const ZMemoryRange& vmem);

  size_t remove_mappings(ZArray<ZMemoryRange>* mappings, size_t size);

  bool remove_mapping_contiguous(ZMemoryRange* mapping, size_t size);

  size_t min() const;
  size_t reset_min();
  size_t remove_from_min(ZArray<ZMemoryRange>* mappings, size_t max_size);

  size_t size() const;
};

#endif // SHARE_GC_Z_ZMAPPEDCACHE_HPP
