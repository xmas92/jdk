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

#include "memory/allocation.hpp"
#include "memory/arena.hpp"
#include "nmt/memTag.hpp"
#include "gc/z/zIntrusiveRBTree.hpp"
#include "unittest.hpp"
#include "utilities/globalDefinitions.hpp"
#include <ostream>

//#ifndef PRODUCT

struct ZTestEntryCompare {
  int operator()(ZIntrusiveRBTreeNode* a, ZIntrusiveRBTreeNode* b);
  int operator()(int key, ZIntrusiveRBTreeNode* entry);
};

class ZTestEntry : public ArenaObj {
  friend class ZIntrusiveRBTree<int, ZTestEntryCompare>;

public:
  using ZTree = ZIntrusiveRBTree<int, ZTestEntryCompare>;
private:
  const int   _id;
  ZIntrusiveRBTreeNode _node;

public:
  ZTestEntry(int id)
    : _id(id),
      _node() {}

  int id() const {
    return _id;
  }

  static ZIntrusiveRBTreeNode* cast_to_inner(ZTestEntry* element) {
    return &element->_node;
  }
  static  ZTestEntry* cast_to_outer(ZIntrusiveRBTreeNode* node) {
    return (ZTestEntry*)((uintptr_t)node - offset_of(ZTestEntry, _node));
  }

};

int ZTestEntryCompare::operator()(ZIntrusiveRBTreeNode* a, ZIntrusiveRBTreeNode* b) {
  return ZTestEntry::cast_to_outer(a)->id() - ZTestEntry::cast_to_outer(b)->id();
}
int ZTestEntryCompare::operator()(int key, ZIntrusiveRBTreeNode* entry) {
  return key - ZTestEntry::cast_to_outer(entry)->id();
}

class ZTreeTest : public ::testing::Test {
};

class ResettableArena : public Arena {
public:
  using Arena::Arena;

  void reset_arena() {
    if (_chunk != _first) {
      set_size_in_bytes(_chunk->length());
      Chunk::next_chop(_first);
    }
    _chunk = _first;
    _hwm = _chunk->bottom();
    _max = _chunk->top();
  }
};

TEST_F(ZTreeTest, test_insert) {
  constexpr size_t sizes[] = {1, 2, 4, 8, 16, 1024 NOT_DEBUG(COMMA 1024 * 1024)};
  constexpr size_t num_sizes = ARRAY_SIZE(sizes);
  constexpr size_t iterations_multiplier = 4;
  constexpr size_t max_allocation_size = sizes[num_sizes - 1] * iterations_multiplier * sizeof(ZTestEntry);
  ResettableArena arena{MemTag::mtTest, Arena::Tag::tag_other, max_allocation_size};
  for (size_t s : sizes) {
    ZTestEntry::ZTree tree;
    const size_t num_iterations = s * iterations_multiplier;
    std::cout << "Runing a total of " << num_iterations <<
                 " iteration on set [0, " << s << "[" << std::endl;
    for (size_t i = 0; i < num_iterations; i++) {
      int id = rand() % s;
      auto cursor = tree.find(id);
      if (cursor.found()) {
        // Replace or Remove
        if (i % 2 == 0) {
          // Replace
          if (i % 4 == 0) {
            // Replace with new
            tree.replace(ZTestEntry::cast_to_inner(new (&arena) ZTestEntry(id)), cursor);
          } else {
            // Replace with same
            tree.replace(cursor.node(), cursor);
          }
        } else {
          // Remove
          tree.remove(cursor);
        }
      } else {
        // Insert
        tree.insert(ZTestEntry::cast_to_inner(new (&arena) ZTestEntry(id)), cursor);
      }
    }
    arena.reset_arena();
  }
}

TEST_F(ZTreeTest, test_remove) {
}

//#endif // PRODUCT
