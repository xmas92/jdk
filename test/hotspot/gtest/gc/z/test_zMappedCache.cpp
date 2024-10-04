/*
 * Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "unittest.hpp"

class ZMappedCacheTest : public ::testing::Test {
protected:
  static void test_free_left_merge() {
    ZMappedCache cache;
    ZVirtualMemory vmem1(zoffset(0), 100);
    ZVirtualMemory vmem2(zoffset(100), 100);
    ZPhysicalMemory pmem;

    cache.free_mapped(ZMappedMemory(vmem1, pmem));
    cache.free_mapped(ZMappedMemory(vmem2, pmem));

    EXPECT_EQ(cache._tree._node_count, 1);
    EXPECT_EQ(cache._tree._root->key(), zoffset(0));

    EXPECT_EQ(cache._tree._root->val().virtual_memory().start(), zoffset(0));
    EXPECT_EQ(cache._tree._root->val().virtual_memory().size(), (size_t)200);
  }

  static void test_free_right_merge() {
    ZMappedCache cache;
    ZVirtualMemory vmem1(zoffset(0), 100);
    ZVirtualMemory vmem2(zoffset(100), 100);
    ZPhysicalMemory pmem;

    cache.free_mapped(ZMappedMemory(vmem2, pmem));
    cache.free_mapped(ZMappedMemory(vmem1, pmem));

    EXPECT_EQ(cache._tree._node_count, 1);
    EXPECT_EQ(cache._tree._root->key(), zoffset(0));

    EXPECT_EQ(cache._tree._root->val().virtual_memory().start(), zoffset(0));
    EXPECT_EQ(cache._tree._root->val().virtual_memory().size(), (size_t)200);
  }

  static void test_free_both_merge() {
    ZMappedCache cache;
    ZVirtualMemory vmem1(zoffset(0), 100);
    ZVirtualMemory vmem2(zoffset(100), 100);
    ZVirtualMemory vmem3(zoffset(200), 100);

    ZPhysicalMemory pmem;

    cache.free_mapped(ZMappedMemory(vmem1, pmem));
    cache.free_mapped(ZMappedMemory(vmem3, pmem));
    cache.free_mapped(ZMappedMemory(vmem2, pmem));

    EXPECT_EQ(cache._tree._node_count, 1);
    EXPECT_EQ(cache._tree._root->key(), zoffset(0));

    EXPECT_EQ(cache._tree._root->val().virtual_memory().start(), zoffset(0));
    EXPECT_EQ(cache._tree._root->val().virtual_memory().size(), (size_t)300);
  }

  static void test_remove_mapped_contiguous() {
    ZMappedCache cache;

    ZPhysicalMemory pmem1;
    pmem1.add_segment(ZPhysicalMemorySegment(zoffset(0), 50, true));

    ZPhysicalMemory pmem2;
    ZPhysicalMemorySegment seg1(zoffset(25), 25, true);
    ZPhysicalMemorySegment seg2(zoffset(0), 25, true);
    pmem2.add_segment_unsorted(seg1);
    pmem2.add_segment_unsorted(seg2);

    ZVirtualMemory vmem1(zoffset(0), 50);
    ZVirtualMemory vmem2(zoffset(100), 50);
    ZVirtualMemory vmem3(zoffset(200), 50);

    cache.free_mapped(ZMappedMemory(vmem1, pmem1));
    cache.free_mapped(ZMappedMemory(vmem2, pmem2));
    cache.free_mapped(ZMappedMemory(vmem3, pmem1));

    ZMappedMemory chunk = cache.remove_mapped_contiguous(50);
    EXPECT_EQ(chunk.start(), zoffset(0));
    EXPECT_EQ(chunk.size(), (size_t)50);

    chunk = cache.remove_mapped_contiguous(25);
    EXPECT_EQ(chunk.start(), zoffset(100));
    EXPECT_EQ(chunk.size(), (size_t)25);
    EXPECT_EQ(chunk.physical_memory().nsegments(), 1);
    EXPECT_EQ(chunk.physical_memory().segment(0).start(), seg1.start());
    EXPECT_EQ(chunk.physical_memory().segment(0).size(), seg1.size());

    chunk = cache.remove_mapped_contiguous(100);
    EXPECT_TRUE(chunk.is_null());
  }

  static void test_remove_mapped() {
    ZMappedCache cache;

    ZPhysicalMemory pmem;

    ZVirtualMemory vmem1(zoffset(0), 100);
    ZVirtualMemory vmem2(zoffset(200), 100);
    ZVirtualMemory vmem3(zoffset(400), 100);
    ZVirtualMemory vmem4(zoffset(600), 100);

    cache.free_mapped(ZMappedMemory(vmem1, pmem));
    cache.free_mapped(ZMappedMemory(vmem2, pmem));
    cache.free_mapped(ZMappedMemory(vmem3, pmem));
    cache.free_mapped(ZMappedMemory(vmem4, pmem));

    ZArray<ZMappedMemory> mappings;
    cache.remove_mapped(&mappings, 150);

    EXPECT_EQ(mappings.length(), 2);
    EXPECT_EQ(mappings.at(0).start(), zoffset(600));
    EXPECT_EQ(mappings.at(0).size(), (size_t)100);

    EXPECT_EQ(mappings.at(1).start(), zoffset(450));
    EXPECT_EQ(mappings.at(1).size(), (size_t)50);
  }

};

TEST_F(ZMappedCacheTest, test_merge) {
  test_free_left_merge();
  test_free_right_merge();
  test_free_both_merge();
}

TEST_F(ZMappedCacheTest, test_remove_mapped) {
  test_remove_mapped_contiguous();
  test_remove_mapped();
}
