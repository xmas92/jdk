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
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zIntrusiveRBTree.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "utilities/align.hpp"
#include <cstdint>

class ZMappedCacheEntry {
private:
  zoffset _start;
  ZIntrusiveRBTreeNode _node;
public:
  ZMappedCacheEntry(zoffset start) : _start(start), _node() {}

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
};

ZMappedCacheEntry* ZMappedCacheEntry::cast_to_entry(ZIntrusiveRBTreeNode* node) {
  return (ZMappedCacheEntry*)((uintptr_t)node - offset_of(ZMappedCacheEntry, _node));
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

void ZMappedCache::insert(const Tree::FindCursor& cursor, const ZVirtualMemory& vmem) {
  // Create new entry
  ZMappedCacheEntry* entry = create_entry(vmem);

  assert(entry->start() == vmem.start(), "must be");
  assert(entry->end() == vmem.end(), "must be");

  // And insert it
  _tree.insert(entry->node_addr(), cursor);
}

void ZMappedCache::remove(const Tree::FindCursor& cursor, const ZVirtualMemory& vmem) {
  ZIntrusiveRBTreeNode* const node = cursor.node();
  ZMappedCacheEntry* entry = ZMappedCacheEntry::cast_to_entry(node);

  assert(entry->start() == vmem.start(), "must be");
  assert(entry->end() == vmem.end(), "must be");

  // Remove from tree
  _tree.remove(cursor);
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
  // Destroy old entry
  old_entry->~ZMappedCacheEntry();
}

void ZMappedCache::update(ZMappedCacheEntry* entry, const ZVirtualMemory& vmem) {
  assert(entry->end() == vmem.end(), "must be");
  entry->update_start(vmem.start());
}

ZMappedCache::ZMappedCache()
  : _tree() {}

void ZMappedCache::insert_mapping(const ZVirtualMemory& vmem) {
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

  // Find what mappings to remove
  ZIntrusiveRBTreeNode* node = _tree.first();
  while (node != nullptr) {
    ZIntrusiveRBTreeNode* next_node = node->next();
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
        break;
      }
    } else {
      const size_t uneeded = after_remove - size;
      const size_t needed =  mapped_vmem.size() - uneeded;
      const ZVirtualMemory used = mapped_vmem.split(needed);
      update(entry, mapped_vmem);
      mappings->append(used);
      removed = size;
      break;
    }
    node = next_node;
  }

  return removed;
}

bool ZMappedCache::remove_mapping_contiguous(ZVirtualMemory* mapping, size_t size) {
  ZIntrusiveRBTreeNode* node = _tree.first();
  while (node != nullptr) {
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
    node = node->next();
  }
  return false;
}
