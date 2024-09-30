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

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"

constexpr size_t ZMappedCache::SizeClasses[];

class ZMappedCacheEntry {
private:
  zoffset _start;
  ZIntrusiveRBTreeNode _node;
  ZMappedCache::ZSizeClassListNode _size_class_list_nodes[ARRAY_SIZE(ZMappedCache::SizeClasses)];
public:
  ZMappedCacheEntry(zoffset start) : _start(start), _node(), _size_class_list_nodes{} {}

  zoffset start() const { return _start; }
  zoffset_end end() const {
    const uintptr_t this_addr = reinterpret_cast<uintptr_t>(this);
    return zoffset_end(align_up(this_addr, ZGranuleSize) - ZAddressHeapBase);
  }
  ZVirtualMemory vmem() const {
    return ZVirtualMemory(start(), end() - start());
  }

  ZIntrusiveRBTreeNode* node_addr() { return &_node; }

  void update_start(zoffset start) { _start = start; }

  static ZMappedCacheEntry* cast_to_entry(ZIntrusiveRBTreeNode* node);
  static ZMappedCacheEntry* cast_to_entry(ZMappedCache::ZSizeClassListNode* node, size_t index);

  ZMappedCache::ZSizeClassListNode* size_class_node(size_t index) {
    return &_size_class_list_nodes[index];
  }
};

ZMappedCacheEntry* ZMappedCacheEntry::cast_to_entry(ZIntrusiveRBTreeNode* node) {
  return (ZMappedCacheEntry*)((uintptr_t)node - offset_of(ZMappedCacheEntry, _node));
}

ZMappedCacheEntry* ZMappedCacheEntry::cast_to_entry(ZMappedCache::ZSizeClassListNode* node, size_t index) {
  const size_t size_class_list_nodes_offset = offset_of(ZMappedCacheEntry, _size_class_list_nodes);
  const size_t element_offset_in_array = sizeof(ZMappedCache::ZSizeClassListNode) * index;
  return (ZMappedCacheEntry*)((uintptr_t)node - (size_class_list_nodes_offset + element_offset_in_array));
}

static void* entry_address_for_zoffset_end(zoffset_end offset) {
  STATIC_ASSERT(ZCacheLineSize % alignof(ZMappedCacheEntry) == 0);
  constexpr size_t cache_lines_per_z_granule = ZGranuleSize / ZCacheLineSize;
  constexpr size_t cache_lines_per_entry = sizeof(ZMappedCacheEntry) / ZCacheLineSize +
                                           static_cast<size_t>(sizeof(ZMappedCacheEntry) % ZCacheLineSize != 0);
  // Do not use the last location
  constexpr size_t number_of_locations = cache_lines_per_z_granule / cache_lines_per_entry - 1;
  const size_t index = untype(offset) % number_of_locations;
  const uintptr_t end_addr = untype(offset) + ZAddressHeapBase;
  return reinterpret_cast<void*>(end_addr - (cache_lines_per_entry * ZCacheLineSize) * (index + 1));
}

static ZMappedCacheEntry* create_entry(const ZVirtualMemory& vmem) {
  precond(vmem.size() >= ZGranuleSize);
  return new (entry_address_for_zoffset_end(vmem.end())) ZMappedCacheEntry(vmem.start());
}

int ZMappedCache::EntryCompare::operator()(ZIntrusiveRBTreeNode* a, ZIntrusiveRBTreeNode* b) {
  ZVirtualMemory vmem_a = ZMappedCacheEntry::cast_to_entry(a)->vmem();
  ZVirtualMemory vmem_b = ZMappedCacheEntry::cast_to_entry(b)->vmem();
  if (vmem_a.end() < vmem_b.start()) { return -1; }
  if (vmem_b.end() < vmem_a.start()) { return 1; }
  return 0; // Overlapping
}
int ZMappedCache::EntryCompare::operator()(zoffset key, ZIntrusiveRBTreeNode* node) {
  ZVirtualMemory vmem = ZMappedCacheEntry::cast_to_entry(node)->vmem();
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

void ZMappedCache::insert(const Tree::FindCursor& cursor, const ZVirtualMemory& vmem) {
  // Create new entry
  ZMappedCacheEntry* entry = create_entry(vmem);

  assert(entry->start() == vmem.start(), "must be");
  assert(entry->end() == vmem.end(), "must be");

  // And insert it in tree
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

void ZMappedCache::remove(const Tree::FindCursor& cursor, const ZVirtualMemory& vmem) {
  ZIntrusiveRBTreeNode* const node = cursor.node();
  ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(node);

  assert(entry->start() == vmem.start(), "must be");
  assert(entry->end() == vmem.end(), "must be");

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

void ZMappedCache::replace(const Tree::FindCursor& cursor, const ZVirtualMemory& vmem) {
  // Create new entry
  ZMappedCacheEntry* entry = create_entry(vmem);

  assert(entry->start() == vmem.start(), "must be");
  assert(entry->end() == vmem.end(), "must be");

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

void ZMappedCache::update(ZMappedCacheEntry* entry, const ZVirtualMemory& vmem) {
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

ZMappedCache::ZMappedCache()
  : _tree(), _size_class_lists{}, _size(0), _min(_size) {}

void ZMappedCache::insert_mapping(const ZVirtualMemory& vmem) {
  _size += vmem.size();
  auto current_cursor = _tree.find(vmem.start());
  auto next_cursor = _tree.next(current_cursor);
  const bool extends_left = current_cursor.found();
  const bool extends_right = next_cursor.is_valid() &&
                             ZMappedCacheEntry::cast_to_entry(next_cursor.node())->start() == vmem.end();
  if (extends_left && extends_right) {
    ZIntrusiveRBTreeNode* const next_node = next_cursor.node();
    const ZVirtualMemory left_vmem = ZMappedCacheEntry::cast_to_entry(current_cursor.node())->vmem();
    const ZVirtualMemory right_vmem = ZMappedCacheEntry::cast_to_entry(next_node)->vmem();
    assert(left_vmem.adjacent_to(vmem), "must be");
    assert(vmem.adjacent_to(right_vmem), "must be");
    ZVirtualMemory new_vmem = left_vmem;
    new_vmem.extend(vmem.size());
    new_vmem.extend(right_vmem.size());
    assert(new_vmem.end() == right_vmem.end(), "must be");
    assert(new_vmem.start() == left_vmem.start(), "must be");

    // Remove current (left vmem)
    remove(current_cursor, left_vmem);
    // And update next's start
    update(ZMappedCacheEntry::cast_to_entry(next_node), new_vmem);
    return;
  }

  if (extends_left) {
    const ZVirtualMemory left_vmem = ZMappedCacheEntry::cast_to_entry(current_cursor.node())->vmem();
    assert(left_vmem.adjacent_to(vmem), "must be");
    ZVirtualMemory new_vmem = left_vmem;
    new_vmem.extend(vmem.size());
    assert(new_vmem.end() == vmem.end(), "must be");
    assert(new_vmem.start() == left_vmem.start(), "must be");

    replace(current_cursor, new_vmem);
    return;
  }

  if (extends_right) {
    const ZVirtualMemory right_vmem = ZMappedCacheEntry::cast_to_entry(next_cursor.node())->vmem();
    assert(vmem.adjacent_to(right_vmem), "must be");
    ZVirtualMemory new_vmem = vmem;
    new_vmem.extend(right_vmem.size());
    assert(new_vmem.start() == vmem.start(), "must be");
    assert(new_vmem.end() == right_vmem.end(), "must be");
    // Update next's start
    update(ZMappedCacheEntry::cast_to_entry(next_cursor.node()), new_vmem);
    return;
  }

  assert(!extends_left && !extends_right, "must be");

  insert(current_cursor, vmem);
}

size_t ZMappedCache::remove_mappings(ZArray<ZVirtualMemory>* mappings, size_t size) {
  precond(size > 0);
  precond(size % ZGranuleSize == 0);
  size_t removed = 0;
  const auto remove_mapping = [&](ZIntrusiveRBTreeNode* node) {
    ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(node);
    ZVirtualMemory mapped_vmem = entry->vmem();
    size_t after_remove = removed + mapped_vmem.size();

    if (after_remove <= size) {
      auto cursor = _tree.get_cursor(node);
      assert(cursor.is_valid(), "must be");
      remove(cursor, mapped_vmem);
      removed = after_remove;
      mappings->append(mapped_vmem);

      if (removed == size) {
        return true;
      }
    } else {
      const size_t uneeded = after_remove - size;
      const size_t needed =  mapped_vmem.size() - uneeded;
      const ZVirtualMemory used = mapped_vmem.split(needed);
      update(entry, mapped_vmem);
      mappings->append(used);
      removed = size;
      return true;
    }
    return false;
  };

  // Scan size classes
  for (size_t index_plus_one = NumSizeClasses; index_plus_one > 0; index_plus_one--) {
    const size_t index = index_plus_one - 1;
    const size_t size_class = get_size_class(index);
    if (size >= size_class) {
      ZListIterator<ZSizeClassListNode> iter(&_size_class_lists[index]);
      ZSizeClassListNode* list_node;
      while (iter.next(&list_node)) {
        ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(list_node, index);
        if (remove_mapping(entry->node_addr())) {
          assert(removed == size, "must be");
          _size -= size;
          _min = MIN2(_size, _min);
          return size;
        }
      }
    }
  }

  // Find what mappings to remove
  ZIntrusiveRBTreeNode* node = _tree.first();
  while (node != nullptr) {
    ZIntrusiveRBTreeNode* next_node = node->next();
    if (remove_mapping(node)) {
      assert(removed == size, "must be");
      _size -= size;
      _min = MIN2(_size, _min);
      return size;
    }
    node = next_node;
  }

  _size -= removed;
  _min = MIN2(_size, _min);
  return removed;
}

bool ZMappedCache::remove_mapping_contiguous(ZVirtualMemory* mapping, size_t size) {
  const auto remove_mapping = [&](ZIntrusiveRBTreeNode* node) {
    ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(node);
    ZVirtualMemory mapped_vmem = entry->vmem();

    if (mapped_vmem.size() == size) {
      auto cursor = _tree.get_cursor(node);
      assert(cursor.is_valid(), "must be");
      remove(cursor, mapped_vmem);
      *mapping = mapped_vmem;
      return true;
    } else if (mapped_vmem.size() > size) {
      const ZVirtualMemory used = mapped_vmem.split(size);
      update(entry, mapped_vmem);
      *mapping = used;
      return true;
    }
    return false;
  };

  // Scan size classes
  for (size_t index_plus_one = NumSizeClasses; index_plus_one > 0; index_plus_one--) {
    const size_t index = index_plus_one - 1;
    const size_t size_class = get_size_class(index);
    if (size >= size_class) {
      ZListIterator<ZSizeClassListNode> iter(&_size_class_lists[index]);
      ZSizeClassListNode* list_node;
      while (iter.next(&list_node)) {
        ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(list_node, index);
        if (remove_mapping(entry->node_addr())) {
          _size -= size;
          _min = MIN2(_size, _min);
          return true;
        }
      }
      // No use walking any more, other lists and the tree will only contain smaller nodes.
      return false;
    }
  }

  // Scan whole tree
  ZIntrusiveRBTreeNode* node = _tree.first();
  while (node != nullptr) {
    if (remove_mapping(node)) {
      _size -= size;
      _min = MIN2(_size, _min);
      return true;
    }
    node = node->next();
  }
  return false;
}

size_t ZMappedCache::reset_min() {
  const size_t old_min = _min;
  _min = _size;
  return old_min;
}

size_t ZMappedCache::min() const {
  return _min;
}

size_t ZMappedCache::remove_from_min(ZArray<ZVirtualMemory>* mappings, size_t max_size) {
  const size_t size = MIN2(_min, max_size);
  if (size == 0) {
    return 0;
  }
  return remove_mappings(mappings, size);
}

size_t ZMappedCache::size() const {
  return _size;
}
