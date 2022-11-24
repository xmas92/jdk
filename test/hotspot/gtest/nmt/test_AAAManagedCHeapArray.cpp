/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "precompiled.hpp"
#include "memory/allocationManaged.hpp"
#include "memory/allocationManagedUniquePtr.hpp"
#include "unittest.hpp"
#include "utilities/debug.hpp"
#include "gtest/gtest.h"

TEST_VM(ManagedCHeapArray, construction_destruction_side_effects) {
  static int i = 0;
  struct T {
    T() {
      ++i;
    }
    ~T() {
      --i;
    }
  };
  auto o1 = make_managed_c_heap_array_default_init<T>(10, mtTest);
  EXPECT_EQ(i, 10);
  {
    auto o2 = make_managed_c_heap_array_default_init<T>(5, mtTest);
    EXPECT_EQ(i, 15);
    o1.swap(o2);
    EXPECT_EQ(i, 15);
  }
  EXPECT_EQ(i, 5);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST(ManagedCHeapArray, size_of) {
  struct NonTrivialDestructorT {
    ~NonTrivialDestructorT() {}
  };
  struct TrivialDestructorT {};
  using PointerType = NonTrivialDestructorT*;
  using PrimativeType = int;
  STATIC_ASSERT(sizeof(ManagedCHeapArray<NonTrivialDestructorT>) == sizeof(ManagedCHeapArray<NonTrivialDestructorT>::pointer_type) + sizeof(size_t));
  STATIC_ASSERT(sizeof(ManagedCHeapArray<TrivialDestructorT>) == sizeof(ManagedCHeapArray<TrivialDestructorT>::pointer_type));
  STATIC_ASSERT(sizeof(ManagedCHeapArray<PointerType>) == sizeof(ManagedCHeapArray<PointerType>::pointer_type));
  STATIC_ASSERT(sizeof(ManagedCHeapArray<PrimativeType>) == sizeof(ManagedCHeapArray<PrimativeType>::pointer_type));
}

TEST_VM(ManagedCHeapObject, construction_destruction_side_effects_anyobj) {
  static int i = 0;
  struct T : AnyObj {
    T() {
      ++i;
    }
    ~T() {
      --i;
    }
  };
  auto o1 = make_managed_c_heap_object_default_init<T>(mtTest);
  EXPECT_EQ(i, 1);
  {
    auto o2 = make_managed_c_heap_object_default_init<T>(mtTest);
    EXPECT_EQ(i, 2);
    o1.swap(o2);
    EXPECT_EQ(i, 2);
  }
  EXPECT_EQ(i, 1);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST_VM(ManagedCHeapObject, construction_destruction_side_effects) {
  static int i = 0;
  struct T {
    T() {
      ++i;
    }
    ~T() {
      --i;
    }
  };
  auto o1 = make_managed_c_heap_object_default_init<T>(mtTest);
  EXPECT_EQ(i, 1);
  {
    auto o2 = make_managed_c_heap_object_default_init<T>(mtTest);
    EXPECT_EQ(i, 2);
    o1.swap(o2);
    EXPECT_EQ(i, 2);
  }
  EXPECT_EQ(i, 1);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST_VM(ManagedCHeapObject, value_init) {
  static int i = 0;
  struct T {
    int _a;
    T(int a) : _a(a) {
      i += _a;
    }
    ~T() {
      i -= _a;
    }
  };
  auto o1 = make_managed_c_heap_object_value_init<T>(mtTest, 6);
  EXPECT_EQ(i, 6);
  {
    auto o2 = make_managed_c_heap_object_value_init<T>(mtTest, 9);
    EXPECT_EQ(i, 15);
    o1.swap(o2);
    EXPECT_EQ(i, 15);
  }
  EXPECT_EQ(i, 9);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST_VM(ManagedCHeapObject, value_init_anyobj) {
  static int i = 0;
  struct T : AnyObj {
    int _a;
    T(int a) : _a(a) {
      i += _a;
    }
    ~T() {
      i -= _a;
    }
  };
  auto o1 = make_managed_c_heap_object_value_init<T>(mtTest, 6);
  EXPECT_EQ(i, 6);
  {
    auto o2 = make_managed_c_heap_object_value_init<T>(mtTest, 9);
    EXPECT_EQ(i, 15);
    o1.swap(o2);
    EXPECT_EQ(i, 15);
  }
  EXPECT_EQ(i, 9);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST_VM(ManagedCHeapObj, construction_destruction_side_effects) {
  static int i = 0;
  struct T : CHeapObj<mtTest> {
    T() {
      ++i;
    }
    ~T() {
      --i;
    }
  };
  //auto o1 = make_managed_c_heap_obj<T, mtTest>();
  T* t1 = new T;
  EXPECT_EQ(i, 1);
  {
    //auto o2 = make_managed_c_heap_obj<T, mtTest>();
    T* t2 = new T;
    EXPECT_EQ(i, 2);
    //o1.swap(o2);
    EXPECT_EQ(i, 2);
    delete t2;
  }
  EXPECT_EQ(i, 1);
    delete t1;
  //o1.reset();
  EXPECT_EQ(i, 0);
}

TEST_VM(ManagedCHeapObj, construction_destruction_side_effects_array) {
  static int i = 0;
  struct T : CHeapObj<mtTest> {
    T() {
      ++i;
    }
    ~T() {
      --i;
    }
  };
  auto o1 = make_managed_c_heap_obj_default_init<T[]>(10);
  EXPECT_EQ(i, 10);
  {
    auto o2 = make_managed_c_heap_obj_default_init<T[]>(5);
    EXPECT_EQ(i, 15);
    o1.swap(o2);
    EXPECT_EQ(i, 15);
  }
  EXPECT_EQ(i, 5);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST_VM(ManagedCHeapObj, value_init) {
  static int i = 0;
  struct T : CHeapObj<mtTest> {
    int _a;
    T(int a) : _a(a) {
      i += _a;
    }
    ~T() {
      i -= _a;
    }
  };
  auto o1 = make_managed_c_heap_obj_value_init<T>(6);
  EXPECT_EQ(i, 6);
  {
    auto o2 = make_managed_c_heap_obj_value_init<T>(9);
    EXPECT_EQ(i, 15);
    o1.swap(o2);
    EXPECT_EQ(i, 15);
  }
  EXPECT_EQ(i, 9);
  o1.reset();
  EXPECT_EQ(i, 0);
}

TEST(ManagedCHeapObj, base_of) {
  struct T1 : CHeapObj<mtGC> {};
  struct T2 : CHeapObj<mtNone> {};
  struct T3 : AnyObj {};
  struct T4 {};
  struct T5 : T1 {};

  STATIC_ASSERT(IsDerivedFromCHeapObj<T1>::value);
  STATIC_ASSERT(IsDerivedFromCHeapObj<T2>::value);
  STATIC_ASSERT(!IsDerivedFromCHeapObj<T3>::value);
  STATIC_ASSERT(!IsDerivedFromCHeapObj<T4>::value);
  STATIC_ASSERT(IsDerivedFromCHeapObj<T5>::value);
}

TEST(UniquePtr, test) {
  struct T1 : CHeapObj<mtGC> {};
  struct T2 : CHeapObj<mtNone> {
    ~T2() {}
  };
  struct T3 : AnyObj {};
  struct T4 {};
  struct T5 : T1 {};
  static int i = 6;
  struct T6 {
    ~T6() {}
  };

  STATIC_ASSERT(sizeof(UniquePtr<T1>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T2>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T3>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T4>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T5>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T6>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T1[]>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T2[]>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T3[]>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T4[]>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T5[]>) == sizeof(intptr_t));
  STATIC_ASSERT(sizeof(UniquePtr<T6[]>) == sizeof(intptr_t) + sizeof(size_t));

  UniquePtr<T6[]> test{nullptr, UniquePtrDeleter<T6[]>(0)};
  test = UniquePtr<T6[]>(NEW_C_HEAP_ARRAY(T6, 10, mtTest), UniquePtrDeleter<T6[]>(10));
  //UniquePtr<T6[]> test2 = nullptr;
}
