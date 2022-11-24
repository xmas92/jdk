/*
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
 *
 */

#ifndef SHARE_MEMORY_ALLOCATIONMANAGEDUNIQUEPTR_HPP
#define SHARE_MEMORY_ALLOCATIONMANAGEDUNIQUEPTR_HPP

#include "memory/allStatic.hpp"
#include "memory/allocation.hpp"
#include "metaprogramming/enableIf.hpp"
#include "metaprogramming/isDerivedFromCHeapObj.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>


template<typename T, bool = !std::is_trivially_destructible<typename std::remove_extent<T>::type>::value
                         && !IsDerivedFromCHeapObj<typename std::remove_extent<T>::type>::value
                         && !std::is_base_of<AnyObj, typename std::remove_extent<T>::type>::value>
struct UniquePtrDeleter {
  constexpr UniquePtrDeleter() noexcept = default;

  template <class U, ENABLE_IF(std::is_convertible<U*, T*>::value)>
  constexpr UniquePtrDeleter(const UniquePtrDeleter<U>& d) noexcept {}

  template <typename E = T, ENABLE_IF(IsDerivedFromCHeapObj<E>::value), ENABLE_IF(std::is_base_of<AnyObj, E>::value)>
  constexpr void operator()(T* ptr) const noexcept {
      ShouldNotReachHere();
  }

  template <typename E = T, ENABLE_IF(IsDerivedFromCHeapObj<E>::value), ENABLE_IF(!std::is_base_of<AnyObj, E>::value)>
  constexpr void operator()(T* ptr) const noexcept {
      STATIC_ASSERT(sizeof(T) != 0);
      delete ptr;
  }

  template <typename E = T, ENABLE_IF(!IsDerivedFromCHeapObj<E>::value), ENABLE_IF(std::is_base_of<AnyObj, E>::value)>
  constexpr void operator()(T* ptr) const noexcept {
      STATIC_ASSERT(sizeof(T) != 0);
      delete ptr;
  }

  template <typename E = T, ENABLE_IF(!IsDerivedFromCHeapObj<E>::value), ENABLE_IF(!std::is_base_of<AnyObj, E>::value)>
  constexpr void operator()(T* ptr) const noexcept {
      STATIC_ASSERT(sizeof(T) != 0);
      if (ptr != nullptr) {
        ptr->~T();
        FreeHeap((void*)ptr);
      }
  }
};

template<typename T>
struct UniquePtrDeleter<T[], false> {
  constexpr UniquePtrDeleter() noexcept = default;
  template<class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value)>
  constexpr UniquePtrDeleter(const UniquePtrDeleter<U[]>& d) noexcept {}


  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(IsDerivedFromCHeapObj<U>::value), ENABLE_IF(std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      ShouldNotReachHere();
  }

  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(IsDerivedFromCHeapObj<U>::value), ENABLE_IF(!std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      STATIC_ASSERT(sizeof(U) != 0);
      delete[] ptr;
  }

  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(!IsDerivedFromCHeapObj<U>::value), ENABLE_IF(std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      ShouldNotReachHere();
  }

  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(!IsDerivedFromCHeapObj<U>::value), ENABLE_IF(!std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      STATIC_ASSERT(sizeof(U) != 0);
    if (ptr != nullptr) {
      FreeHeap((void*)ptr);
    }
  }
};

template<typename T>
struct UniquePtrDeleter<T[], true> {
  constexpr UniquePtrDeleter() noexcept = delete;
  constexpr UniquePtrDeleter(size_t size) : _size(size) {};
  template<class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value)>
  constexpr UniquePtrDeleter( const UniquePtrDeleter<U[]>& d ) noexcept {}


  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(IsDerivedFromCHeapObj<U>::value), ENABLE_IF(std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      ShouldNotReachHere();
  }

  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(IsDerivedFromCHeapObj<U>::value), ENABLE_IF(!std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      STATIC_ASSERT(sizeof(U) != 0);
      delete[] ptr;
  }

  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(!IsDerivedFromCHeapObj<U>::value), ENABLE_IF(std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
      ShouldNotReachHere();
  }

  template <class U, ENABLE_IF(std::is_convertible<const volatile U(*)[], const volatile T(*)[]>::value),
            ENABLE_IF(!IsDerivedFromCHeapObj<U>::value), ENABLE_IF(!std::is_base_of<AnyObj, U>::value)>
  constexpr void operator()(U* ptr) const noexcept {
    STATIC_ASSERT(sizeof(U) != 0);
    T* t_ptr = ptr;
    if (t_ptr != nullptr) {
      for (size_t i = _size; i != 0; --i) {
        (t_ptr + (i - 1))->~T();
      }
      FreeHeap((void*)t_ptr);
    }
  }
private:
  size_t _size = 0;
};

template<typename T>
using UniquePtr = std::unique_ptr<T, UniquePtrDeleter<T>>;


#endif // SHARE_MEMORY_ALLOCATIONMANAGEDUNIQUEPTR_HPP
