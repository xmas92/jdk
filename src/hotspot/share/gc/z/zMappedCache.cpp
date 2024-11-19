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
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"

ZMappedCache::ZMappedCache()
  : _tree() {}

void ZMappedCache::insert_mapping(const ZVirtualMemory& vmem) {
  bool merged_left = false;

  // Check left node.
  ZMappedTreeNode* lnode = _tree.closest_leq(vmem.start());
  if (lnode != nullptr) {
    ZVirtualMemory& left_vmem = lnode->val();

    if (left_vmem.adjacent_to(vmem)) {
      left_vmem.extend(vmem.size());
      merged_left = true;
    }
  }

  // Check right node.
  ZMappedTreeNode* rnode = _tree.closest_leq(zoffset(vmem.end()));
  if (rnode != lnode) {
    // If there exists a node that is LEQ than mapping.end() which is not the
    // left_node, it is adjacent.
    ZVirtualMemory& right_vmem = rnode->val();

    if (merged_left) {
      // Extend the left mapping, again
      lnode->val().extend(right_vmem.size());
      _tree.remove(rnode->key());
    } else {
      rnode->key() = vmem.start();
      right_vmem = ZVirtualMemory(vmem.start(), vmem.size() + right_vmem.size());
    }

    return;
  }

  if (!merged_left) {
    _tree.upsert(vmem.start(), vmem);
  }
}

size_t ZMappedCache::remove_mappings(ZArray<ZVirtualMemory>* mappings, size_t size) {
  ZArray<ZVirtualMemory> to_remove;
  size_t removed = 0;

  // Find what mappings to remove
  ZMappedTree::InReverseOrderIterator iterator(&_tree);
  for (ZMappedTreeNode* node; iterator.next(&node);) {
    ZVirtualMemory& mapped_vmem = node->val();
    size_t after_remove = removed + mapped_vmem.size();

    if (after_remove <= size) {
      to_remove.append(mapped_vmem);
      removed = after_remove;

      if (removed == size) {
        break;
      }
    } else if (after_remove > size) {
      ZVirtualMemory initial_chunk = mapped_vmem.split(after_remove - size);
      to_remove.append(mapped_vmem);
      mapped_vmem = initial_chunk;
      removed = size;
      break;
    }
  }

  // Remove all mappings marked for removal from the tree
  for (int i = 0; i < to_remove.length(); i++) {
    _tree.remove(to_remove.at(i).start());
  }

  to_remove.swap(mappings);
  return removed;
}

bool ZMappedCache::remove_mapping_contiguous_granule(ZVirtualMemory* mapping, size_t size) {
  // Since the smallest possible size of a VMA that can be stored in the tree is
  // granule-sized, we can optimize by always removing a mapping from the left-most
  // node in the tree.
  ZMappedTreeNode* node = _tree.leftmost_node();
  if (node == nullptr) {
    return false;
  }

  ZVirtualMemory& node_mapping = node->val();

  if (node_mapping.size() > size) {
  // Larger than necessary
    *mapping = node_mapping.split(size);
    node->key() = node_mapping.start();
    return true;
  }

  // Perfect match (i.e node is granule-sized)
  *mapping = node_mapping;
  _tree.remove(node->key());
  return true;
}

bool ZMappedCache::remove_mapping_contiguous(ZVirtualMemory* mapping, size_t size) {
  if (size == ZPageSizeSmall) {
    return remove_mapping_contiguous_granule(mapping, size);
  }

  ZMappedTree::InOrderIterator iterator(&_tree);
  for (ZMappedTreeNode* node; iterator.next(&node);) {
    ZVirtualMemory& node_mapping = node->val();

    if (node_mapping.size() == size) {
      // Perfect match
      *mapping = node_mapping;
      _tree.remove(node->key());
      return true;
    } else if (node_mapping.size() > size) {
      // Larger than necessary
      *mapping = node_mapping.split(size);
      node->key() = node_mapping.start();
      return true;
    }
  }

  return false;
}
