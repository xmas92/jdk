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

#ifdef _WINDOWS

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zMemory.inline.hpp"
#include "gc/z/zSyscall_windows.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zVirtualMemory.hpp"
#include "runtime/os.hpp"
#include "unittest.hpp"

using namespace testing;

#define EXPECT_ALLOC_OK(range) EXPECT_FALSE(range.is_null())

class ZMapperTest : public Test {
private:
  static constexpr size_t ZMapperTestReservationSize = 32 * M;

  static bool                      _initialized;
  static ZPerNUMA<ZMemoryManager>* _vas;
  static ZMemoryManager*           _va;

  ZVirtualMemoryManager* _vmm;

public:
  bool reserve_for_test() {
    // Initialize platform specific parts before reserving address space
    _vmm->pd_initialize_before_reserve();

    // Reserve address space
    if (!_vmm->pd_reserve(ZOffset::address_unsafe(zoffset(0)), ZMapperTestReservationSize)) {
      return false;
    }

    // Make the address range free before setting up callbacks below
    _va->free(zoffset(0), ZMapperTestReservationSize);

    // Initialize platform specific parts after reserving address space
    _vmm->pd_initialize_after_reserve();

    return true;
  }

  virtual void SetUp() {
    // Only run test on supported Windows versions
    if (!ZSyscall::is_supported()) {
      GTEST_SKIP() << "Requires Windows version 1803 or later";
      return;
    }

    ZSyscall::initialize();
    ZGlobalsPointers::initialize();

    // Fake a ZVirtualMemoryManager
    _vmm = (ZVirtualMemoryManager*)os::malloc(sizeof(ZVirtualMemoryManager), mtTest);
    _vas = ::new (&_vmm->_managers) ZPerNUMA<ZMemoryManager>();
    _va = _vmm->_managers.addr(0);

    // Reserve address space for the test
    if (!reserve_for_test()) {
      GTEST_SKIP() << "Failed to reserve address space";
      return;
    }

    _initialized = true;
  }

  virtual void TearDown() {
    if (!ZSyscall::is_supported()) {
      // Test skipped, nothing to cleanup
      return;
    }

    if (_initialized) {
      _vmm->pd_unreserve(ZOffset::address_unsafe(zoffset(0)), 0);
    }

    _vas->~ZPerNUMA<ZMemoryManager>();
    os::free(_vmm);
  }

  static void test_alloc_low_address() {
    // Verify that we get placeholder for first granule
    ZMemoryRange bottom = _va->alloc_low_address(ZGranuleSize);
    EXPECT_ALLOC_OK(bottom);

    _va->free(bottom);

    // Alloc something larger than a granule and free it
    bottom = _va->alloc_low_address(ZGranuleSize * 3);
    EXPECT_ALLOC_OK(bottom);

    _va->free(bottom);

    // Free with more memory allocated
    bottom = _va->alloc_low_address(ZGranuleSize);
    EXPECT_ALLOC_OK(bottom);

    ZMemoryRange next = _va->alloc_low_address(ZGranuleSize);
    EXPECT_ALLOC_OK(next);

    _va->free(bottom);
    _va->free(next);
  }

  static void test_alloc_high_address() {
    // Verify that we get placeholder for last granule
    ZMemoryRange high = _va->alloc_high_address(ZGranuleSize);
    EXPECT_ALLOC_OK(high);

    ZMemoryRange prev = _va->alloc_high_address(ZGranuleSize);
    EXPECT_ALLOC_OK(prev);

    _va->free(high);
    _va->free(prev);

    // Alloc something larger than a granule and return it
    high = _va->alloc_high_address(ZGranuleSize * 2);
    EXPECT_ALLOC_OK(high);

    _va->free(high);
  }

  static void test_alloc_whole_area() {
    // Alloc the whole reservation
    ZMemoryRange bottom = _va->alloc_low_address(ZMapperTestReservationSize);
    EXPECT_ALLOC_OK(bottom);

    // Free two chunks and then allocate them again
    _va->free(bottom.start(), ZGranuleSize * 4);
    _va->free(bottom.start() + ZGranuleSize * 6, ZGranuleSize * 6);

    ZMemoryRange range = _va->alloc_low_address(ZGranuleSize * 4);
    EXPECT_ALLOC_OK(range);

    range = _va->alloc_low_address(ZGranuleSize * 6);
    EXPECT_ALLOC_OK(range);

    // Now free it all, and verify it can be re-allocated
    _va->free(bottom.start(), ZMapperTestReservationSize);

    bottom = _va->alloc_low_address(ZMapperTestReservationSize);
    EXPECT_ALLOC_OK(bottom);

    _va->free(bottom.start(), ZMapperTestReservationSize);
  }
};

bool ZMapperTest::_initialized              = false;
ZPerNUMA<ZMemoryManager>* ZMapperTest::_vas = nullptr;
ZMemoryManager* ZMapperTest::_va            = nullptr;

TEST_VM_F(ZMapperTest, test_alloc_low_address) {
  test_alloc_low_address();
}

TEST_VM_F(ZMapperTest, test_alloc_high_address) {
  test_alloc_high_address();
}

TEST_VM_F(ZMapperTest, test_alloc_whole_area) {
  test_alloc_whole_area();
}

#endif // _WINDOWS
