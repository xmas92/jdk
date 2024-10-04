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
#include "gc/z/zList.inline.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"

ZMappedCache::ZMappedCache()
  : _tree(),
    _last_commit(0) {}

size_t ZMappedCache::remove_mapped(ZArray<ZMappedMemory>* mappings, size_t size) {
  ZArray<ZMappedMemory> to_remove;
  size_t removed = 0;

  // Find what mappings to remove
  ZMappedTreap::InReverseOrderIterator iterator(&_tree);
  for (ZMappedTreapNode* node; iterator.next(&node);) {
    ZMappedMemory& mapped = node->val();
    size_t after_remove = removed + mapped.size();

    if (after_remove <= size) {
      to_remove.append(mapped);
      removed = after_remove;

      if (removed == size) {
        break;
      }
    } else if (after_remove > size) {
      ZMappedMemory initial_chunk = mapped.split(after_remove - size);
      to_remove.append(mapped);
      _tree.upsert(node->key(), initial_chunk);
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

ZMappedMemory ZMappedCache::remove_mapped_contiguous(size_t size) {
  ZMappedTreap::InOrderIterator iterator(&_tree);
  for (ZMappedTreapNode* node; iterator.next(&node);) {
    ZMappedMemory mapped = node->val();

    if (mapped.size() == size) {
      // Perfect match
      _tree.remove(node->key());
      return mapped;
    } else if (mapped.size() > size) {
      // Larger than necessary
      ZMappedMemory initial_chunk = mapped.split(size);
      _tree.remove(node->key());
      _tree.upsert(mapped.start(), mapped);
      return initial_chunk;
    }
  }

  return ZMappedMemory();
}

size_t ZMappedCache::flush(ZArray<ZMappedMemory>* mappings, size_t size, uint64_t* timeout) {
  const uint64_t now = (uint64_t)os::elapsedTime();
  const uint64_t expires = _last_commit + ZUncommitDelay;
  if (expires > now) {
    // Delay uncommit, set next timeout
    *timeout = expires - now;
    return 0;
  }

  return remove_mapped(mappings, size);
}

void ZMappedCache::set_last_commit() {
  _last_commit = (uint64_t)ceil(os::elapsedTime());
}

void ZMappedCache::free_mapped(ZPage* page) {
  free_mapped(ZMappedMemory(page->virtual_memory(), page->physical_memory()));
}

void ZMappedCache::free_mapped(ZMappedMemory mapping) {
  bool merged_left = false;

  // Check left node.
  ZMappedTreapNode* lnode = _tree.closest_leq(mapping.start());
  if (lnode != nullptr) {
    ZMappedMemory left_mapped = lnode->val();

    if (mapping.virtually_adjacent_to(left_mapped)) {
      left_mapped.extend_mapping(mapping);
      _tree.upsert(lnode->key(), left_mapped);

      // Update mapped to the larger mapping
      mapping = left_mapped;
      merged_left = true;
    }
  }

  // Check right node.
  ZMappedTreapNode* rnode = _tree.closest_leq(zoffset(mapping.virtual_memory().end()));
  if (rnode != lnode) {
    // If there exists a node that is LEQ than vmem.end() which is not the
    // left_node, it is adjacent.
    mapping.extend_mapping(rnode->val());
    _tree.remove(rnode->key());
    _tree.upsert(mapping.start(), mapping);
    return;
  }

  if (!merged_left) {
    _tree.upsert(mapping.start(), mapping);
  }
}
