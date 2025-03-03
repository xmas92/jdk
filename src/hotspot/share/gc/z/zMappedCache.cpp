/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zIntrusiveRBTree.inline.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zMemory.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"

constexpr size_t ZMappedCache::SizeClasses[];

class ZMappedCacheEntry {
private:
  zoffset                          _start;
  ZIntrusiveRBTreeNode             _tree_node;
  ZMappedCache::ZSizeClassListNode _size_class_list_nodes[ARRAY_SIZE(ZMappedCache::SizeClasses)];

public:
  ZMappedCacheEntry(zoffset start)
    : _start(start),
      _tree_node(),
      _size_class_list_nodes{} {}

  static ZMappedCacheEntry* cast_to_entry(ZIntrusiveRBTreeNode* tree_node);
  static const ZMappedCacheEntry* cast_to_entry(const ZIntrusiveRBTreeNode* tree_node);
  static ZMappedCacheEntry* cast_to_entry(ZMappedCache::ZSizeClassListNode* list_node, size_t index);

  zoffset start() const {
    return _start;
  }

  zoffset_end end() const {
    const uintptr_t this_addr = reinterpret_cast<uintptr_t>(this);
    return zoffset_end(align_up(this_addr, ZGranuleSize) - ZAddressHeapBase);
  }

  ZMemoryRange vmem() const {
    return ZMemoryRange(start(), end() - start());
  }

  ZIntrusiveRBTreeNode* node_addr() {
    return &_tree_node;
  }

  void update_start(zoffset start) {
    _start = start;
  }

  ZMappedCache::ZSizeClassListNode* size_class_node(size_t index) {
    return &_size_class_list_nodes[index];
  }
};

ZMappedCacheEntry* ZMappedCacheEntry::cast_to_entry(ZIntrusiveRBTreeNode* tree_node) {
  return const_cast<ZMappedCacheEntry*>(ZMappedCacheEntry::cast_to_entry(const_cast<const ZIntrusiveRBTreeNode*>(tree_node)));
}

const ZMappedCacheEntry* ZMappedCacheEntry::cast_to_entry(const ZIntrusiveRBTreeNode* tree_node) {
  return (const ZMappedCacheEntry*)((uintptr_t)tree_node - offset_of(ZMappedCacheEntry, _tree_node));
}

ZMappedCacheEntry* ZMappedCacheEntry::cast_to_entry(ZMappedCache::ZSizeClassListNode* list_node, size_t index) {
  const size_t size_class_list_nodes_offset = offset_of(ZMappedCacheEntry, _size_class_list_nodes);
  const size_t element_offset_in_array = sizeof(ZMappedCache::ZSizeClassListNode) * index;
  return (ZMappedCacheEntry*)((uintptr_t)list_node - (size_class_list_nodes_offset + element_offset_in_array));
}

static void* entry_address_for_zoffset_end(zoffset_end offset) {
  STATIC_ASSERT(ZCacheLineSize % alignof(ZMappedCacheEntry) == 0);

  constexpr size_t cache_lines_per_z_granule = ZGranuleSize / ZCacheLineSize;
  constexpr size_t cache_lines_per_entry = sizeof(ZMappedCacheEntry) / ZCacheLineSize +
                                           static_cast<size_t>(sizeof(ZMappedCacheEntry) % ZCacheLineSize != 0);

  // Do not use the last location
  constexpr size_t number_of_locations = cache_lines_per_z_granule / cache_lines_per_entry - 1;
  const size_t index = (untype(offset) >> ZGranuleSizeShift) % number_of_locations;
  const uintptr_t end_addr = untype(offset) + ZAddressHeapBase;

  return reinterpret_cast<void*>(end_addr - (cache_lines_per_entry * ZCacheLineSize) * (index + 1));
}

static ZMappedCacheEntry* create_entry(const ZMemoryRange& vmem) {
  precond(vmem.size() >= ZGranuleSize);

  void* placement_addr = entry_address_for_zoffset_end(vmem.end());
  ZMappedCacheEntry* entry = new (placement_addr) ZMappedCacheEntry(vmem.start());

  assert(entry->start() == vmem.start(), "must be");
  assert(entry->end() == vmem.end(), "must be");

  return entry;
}

int ZMappedCache::EntryCompare::operator()(ZIntrusiveRBTreeNode* a, ZIntrusiveRBTreeNode* b) {
  const ZMemoryRange vmem_a = ZMappedCacheEntry::cast_to_entry(a)->vmem();
  const ZMemoryRange vmem_b = ZMappedCacheEntry::cast_to_entry(b)->vmem();

  if (vmem_a.end() < vmem_b.start()) { return -1; }
  if (vmem_b.end() < vmem_a.start()) { return 1; }

  return 0; // Overlapping
}

int ZMappedCache::EntryCompare::operator()(zoffset key, ZIntrusiveRBTreeNode* node) {
  const ZMemoryRange vmem = ZMappedCacheEntry::cast_to_entry(node)->vmem();

  if (key < vmem.start()) { return -1; }
  if (key > vmem.end()) { return 1; }

  return 0; // Containing
}

size_t ZMappedCache::get_size_class(size_t index) {
  if (index == 0 && ZPageSizeMedium > ZPageSizeSmall) {
    return ZPageSizeMedium;
  }

  return SizeClasses[index];
}

void ZMappedCache::tree_insert(const Tree::FindCursor& cursor, const ZMemoryRange& vmem) {
  ZMappedCacheEntry* entry = create_entry(vmem);

  // Insert in tree
  _tree.insert(entry->node_addr(), cursor);

  // And in size class lists
  const size_t size = vmem.size();
  for (size_t i = 0; i < NumSizeClasses; i++) {
    const size_t size_class = get_size_class(i);
    if (size >= size_class) {
      _size_class_lists[i].insert_first(entry->size_class_node(i));
    }
  }
}

void ZMappedCache::tree_remove(const Tree::FindCursor& cursor, const ZMemoryRange& vmem) {
  ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(cursor.node());

  // Remove from tree
  _tree.remove(cursor);

  // And in size class lists
  const size_t size = vmem.size();
  for (size_t i = 0; i < NumSizeClasses; i++) {
    const size_t size_class = get_size_class(i);
    if (size >= size_class) {
      _size_class_lists[i].remove(entry->size_class_node(i));
    }
  }

  // Destroy entry
  entry->~ZMappedCacheEntry();
}

void ZMappedCache::tree_replace(const Tree::FindCursor& cursor, const ZMemoryRange& vmem) {
  ZMappedCacheEntry* entry = create_entry(vmem);

  ZIntrusiveRBTreeNode* const node = cursor.node();
  ZMappedCacheEntry* old_entry = ZMappedCacheEntry::cast_to_entry(node);
  assert(old_entry->end() != vmem.end(), "should not replace, use update");

  // Replace in tree
  _tree.replace(entry->node_addr(), cursor);

  // And in size class lists
  const size_t new_size = vmem.size();
  const size_t old_size = old_entry->vmem().size();
  for (size_t i = 0; i < NumSizeClasses; i++) {
    const size_t size_class = get_size_class(i);
    if (old_size >= size_class) {
      _size_class_lists[i].remove(old_entry->size_class_node(i));
    }
    if (new_size >= size_class) {
      _size_class_lists[i].insert_first(entry->size_class_node(i));
    }
  }

  // Destroy old entry
  old_entry->~ZMappedCacheEntry();
}

void ZMappedCache::tree_update(ZMappedCacheEntry* entry, const ZMemoryRange& vmem) {
  assert(entry->end() == vmem.end(), "must be");

  // Remove or add to lists if required
  const size_t new_size = vmem.size();
  const size_t old_size = entry->vmem().size();
  for (size_t i = 0; i < NumSizeClasses; i++) {
    const size_t size_class = get_size_class(i);
    const bool old_size_in_size_class = old_size >= size_class;
    const bool new_size_in_size_class = new_size >= size_class;
    if (old_size_in_size_class != new_size_in_size_class) {
      // Need to update a list
      if (old_size_in_size_class) {
        // Removing
        _size_class_lists[i].remove(entry->size_class_node(i));
      } else {
        // Adding
        assert(new_size_in_size_class, "must be");
        _size_class_lists[i].insert_first(entry->size_class_node(i));
      }
    }
  }

  // And update entry
  entry->update_start(vmem.start());
}

template <typename SelectFunction>
ZMemoryRange ZMappedCache::remove_mapping(ZMappedCacheEntry* const entry, size_t min_size, SelectFunction select) {
  ZMemoryRange vmem = entry->vmem();
  const size_t size = vmem.size();

  if (size < min_size) {
    // Do not select this, smaller than min_size
    return ZMemoryRange();
  }

  // Query how much to remove
  const size_t to_remove = select(size);
  assert(to_remove <= size, "must not remove more than size");

  if (to_remove == 0) {
    // Nothing to remove
    return ZMemoryRange();
  }

  if (to_remove != size) {
    // Partial removal
    const size_t unused_size = size - to_remove;
    const ZMemoryRange unused_vmem = vmem.split_from_back(unused_size);
    tree_update(entry, unused_vmem);
  } else {
    // Whole removal
    auto cursor = _tree.get_cursor(entry->node_addr());
    assert(cursor.is_valid(), "must be");
    tree_remove(cursor, vmem);
  }

  // Update statistics
  _size -= to_remove;
  _min = MIN2(_size, _min);

  postcond(to_remove == vmem.size());
  return vmem;
}

template <typename SelectFunction, typename ConsumeFunction>
void ZMappedCache::scan_remove_mapping(size_t min_size, SelectFunction select, ConsumeFunction consume) {
  // TODO: Maybe start on best fit size class first, and only then go from large to small.

  // Scan size classes
  for (size_t index_plus_one = NumSizeClasses; index_plus_one > 0; index_plus_one--) {
    const size_t index = index_plus_one - 1;
    const size_t size_class = get_size_class(index);

    // Scan the list
    ZListIterator<ZSizeClassListNode> iter(&_size_class_lists[index]);
    for (ZSizeClassListNode* list_node; iter.next(&list_node);) {
      ZMappedCacheEntry* const entry = ZMappedCacheEntry::cast_to_entry(list_node, index);
      const ZMemoryRange vmem = remove_mapping(entry, min_size, select);
      if (!vmem.is_null() && consume(vmem)) {
        // Found a mapping and consume is satisfied.
        return;
      }
    }

    if (min_size > size_class) {
      // No use walking any more, other lists and the tree will only contain smaller nodes.
      return;
    }
  }

  // Scan whole tree
  for (ZIntrusiveRBTreeNode* node = _tree.first(); node != nullptr; node = node->next()) {
    ZMappedCacheEntry* const entry = ZMappedCacheEntry::cast_to_entry(node);
    const ZMemoryRange vmem = remove_mapping(entry, min_size, select);
    if (!vmem.is_null() && consume(vmem)) {
      // Found a mapping and consume is satisfied.
      return;
    }
  }
}

template <typename SelectFunction, typename ConsumeFunction>
void ZMappedCache::scan_remove_mapping(SelectFunction select, ConsumeFunction consume) {
  // Scan without a min_size
  scan_remove_mapping(0, select, consume);
}

ZMappedCache::ZMappedCache()
  : _tree(),
    _size_class_lists{},
    _size(0),
    _min(_size) {}

void ZMappedCache::insert(const ZMemoryRange& vmem) {
  _size += vmem.size();

  Tree::FindCursor current_cursor = _tree.find(vmem.start());
  Tree::FindCursor next_cursor = _tree.next(current_cursor);

  const bool extends_left = current_cursor.found();
  const bool extends_right = next_cursor.is_valid() && next_cursor.found() &&
                             ZMappedCacheEntry::cast_to_entry(next_cursor.node())->start() == vmem.end();

  if (extends_left && extends_right) {
    ZMappedCacheEntry* next_entry = ZMappedCacheEntry::cast_to_entry(next_cursor.node());

    const ZMemoryRange left_vmem = ZMappedCacheEntry::cast_to_entry(current_cursor.node())->vmem();
    const ZMemoryRange right_vmem = next_entry->vmem();
    assert(left_vmem.adjacent_to(vmem), "must be");
    assert(vmem.adjacent_to(right_vmem), "must be");

    ZMemoryRange new_vmem = left_vmem;
    new_vmem.grow_from_back(vmem.size());
    new_vmem.grow_from_back(right_vmem.size());

    // Remove current (left vmem)
    tree_remove(current_cursor, left_vmem);

    // And update next's start
    tree_update(next_entry, new_vmem);

    return;
  }

  if (extends_left) {
    const ZMemoryRange left_vmem = ZMappedCacheEntry::cast_to_entry(current_cursor.node())->vmem();
    assert(left_vmem.adjacent_to(vmem), "must be");

    ZMemoryRange new_vmem = left_vmem;
    new_vmem.grow_from_back(vmem.size());

    tree_replace(current_cursor, new_vmem);

    return;
  }

  if (extends_right) {
    ZMappedCacheEntry* next_entry = ZMappedCacheEntry::cast_to_entry(next_cursor.node());

    const ZMemoryRange right_vmem = next_entry->vmem();
    assert(vmem.adjacent_to(right_vmem), "must be");

    ZMemoryRange new_vmem = vmem;
    new_vmem.grow_from_back(right_vmem.size());

    // Update next's start
    tree_update(next_entry, new_vmem);

    return;
  }

  tree_insert(current_cursor, vmem);
}

ZMemoryRange ZMappedCache::remove_contiguous(size_t size) {
  precond(size > 0);
  precond(size % ZGranuleSize == 0);

  ZMemoryRange result;

  const auto select_mapping = [&](size_t) {
    // We always select the size
    return size;
  };

  const auto consume_mapping = [&](ZMemoryRange vmem) {
    assert(result.is_null(), "only consume once");
    assert(vmem.size() == size, "wrong size consumed");
    result = vmem;

    // Only require one mapping
    return true;
  };

  scan_remove_mapping(size, select_mapping, consume_mapping);

  return result;
}

size_t ZMappedCache::remove_discontiguous(ZArray<ZMemoryRange>* mappings, size_t size) {
  precond(size > 0);
  precond(size % ZGranuleSize == 0);

  size_t remaining = size;
  const auto select_mapping = [&](size_t mapping_size) {
    // Select at most remaining
    return MIN2(remaining, mapping_size);
  };

  const auto consume_mapping = [&](ZMemoryRange vmem) {
    const size_t vmem_size = vmem.size();
    mappings->append(vmem);

    assert(vmem_size <= remaining, "consumed to much");

    // Track remaining, and stop when it reaches zero
    remaining -= vmem_size;
    return remaining == 0;
  };

  scan_remove_mapping(select_mapping, consume_mapping);

  return size - remaining;
}

size_t ZMappedCache::reset_min() {
  const size_t old_min = _min;
  _min = _size;

  return old_min;
}

size_t ZMappedCache::remove_from_min(ZArray<ZMemoryRange>* mappings, size_t max_size) {
  const size_t size = MIN2(_min, max_size);
  if (size == 0) {
    return 0;
  }

  return remove_discontiguous(mappings, size);
}

size_t ZMappedCache::size() const {
  return _size;
}
