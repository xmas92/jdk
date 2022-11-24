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

#ifndef SHARE_MEMORY_ALLOCATIONMANAGED_HPP
#define SHARE_MEMORY_ALLOCATIONMANAGED_HPP

#include "memory/allStatic.hpp"
#include "memory/allocation.hpp"
#include "metaprogramming/enableIf.hpp"
#include "metaprogramming/integralConstant.hpp"
#include "metaprogramming/isDerivedFromCHeapObj.hpp"
#include "metaprogramming/isSame.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#include <cstddef>
#include <utility>
#include <type_traits>


template <typename E, ENABLE_IF(IsDerivedFromCHeapObj<typename std::remove_extent<E>::type>::value)>
class ManagedCHeapObj {
  using remove_extent_type = typename std::remove_extent<E>::type;
public:
  using pointer_type = remove_extent_type*;
  using element_type = remove_extent_type;
  using reference_type = remove_extent_type&;

  constexpr ManagedCHeapObj() = default;
  explicit constexpr ManagedCHeapObj(std::nullptr_t) {};

  ManagedCHeapObj(pointer_type ptr) : _data(ptr) {};
  ManagedCHeapObj(ManagedCHeapObj&& other) : _data(other._data) {
    other._data = nullptr;
  };

  ManagedCHeapObj& operator=(ManagedCHeapObj&& other) {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  ManagedCHeapObj& operator=(std::nullptr_t) {
    reset();
    return *this;
  }

  NONCOPYABLE(ManagedCHeapObj);

  ~ManagedCHeapObj() {
    destroy(_data);
  }

  template<typename T = E, ENABLE_IF(!std::is_array<T>::value)>
  reference_type operator*() const {
    return *_data;
  }

  template<typename T = E, ENABLE_IF(!std::is_array<T>::value)>
  pointer_type operator->() const {
    return get();
  }
  template<typename T = E, ENABLE_IF(std::is_array<T>::value)>
  reference_type operator[](size_t i) const {
    return _data[i];
  }

  pointer_type get() const {
    return _data;
  }

  pointer_type release() {
    pointer_type ptr = get();
    _data = nullptr;
    return ptr;
  }

  pointer_type leak() {
    return release();
  }

  void reset(pointer_type ptr = pointer_type()) {
    pointer_type old_ptr = get();
    _data = ptr;
    destroy(old_ptr);
  }

  void swap(ManagedCHeapObj& other) {
    ::swap(_data, other._data);
  }

private:
  template<typename T = E, ENABLE_IF(std::is_array<T>::value)>
  void destroy(pointer_type ptr) {
    if (ptr != nullptr) {
      delete[] ptr;
    }
  }
  template<typename T = E, ENABLE_IF(!std::is_array<T>::value)>
  void destroy(pointer_type ptr) {
    if (ptr != nullptr) {
      delete ptr;
    }
  }
  pointer_type _data = nullptr;
};


template<typename E>
class ManagedCHeapObject {
  STATIC_ASSERT(!IsDerivedFromCHeapObj<typename std::remove_extent<E>::type>::value);
  STATIC_ASSERT((!std::is_base_of<ResourceObj, E>::value));
  friend class VMStructs;
public:
  using pointer_type = E*;
  using element_type = E;
  using reference_type = E&;

  constexpr ManagedCHeapObject() = default;

  ManagedCHeapObject(pointer_type ptr) : _data(ptr) {};
  ManagedCHeapObject(ManagedCHeapObject&& other) : _data(other._data) {
    other._data = nullptr;
  };

  ManagedCHeapObject& operator=(ManagedCHeapObject&& other) {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  ManagedCHeapObject& operator=(std::nullptr_t) {
    reset();
    return *this;
  }

  NONCOPYABLE(ManagedCHeapObject);

  ~ManagedCHeapObject() {
    destroy(_data);
  }

  reference_type operator*() const {
    return *_data;
  }

  pointer_type operator->() const {
    return get();
  }

  pointer_type get() const {
    return _data;
  }

  pointer_type release() {
    pointer_type ptr = get();
    _data = nullptr;
    return ptr;
  }

  pointer_type leak() {
    return release();
  }

  void reset(pointer_type ptr = pointer_type()) {
    pointer_type old_ptr = get();
    _data = ptr;
    destroy(old_ptr);
  }

  void swap(ManagedCHeapObject& other) {
    ::swap(_data, other._data);
  }

private:
  template<typename T = element_type, ENABLE_IF(std::is_base_of<AnyObj, T>::value)>
  void destroy(pointer_type ptr) {
    if (ptr != nullptr) {
      delete ptr;
    }
  }
  template<typename T = element_type, ENABLE_IF(!std::is_base_of<AnyObj, T>::value)>
  void destroy(pointer_type ptr) {
    if (ptr != nullptr) {
      ptr->~element_type();
      FreeHeap((void*)ptr);
    }
  }
  pointer_type _data = nullptr;
};

template <typename E, bool = (sizeof(E) != 0 && std::is_trivially_destructible<E>::value)>
class ManagedCHeapArray {
  // STATIC_ASSERT(!IsDerivedFromCHeapObj<E>::value);
public:
  using pointer_type = E*;
  using element_type = E;
  using reference_type = E&;

  constexpr ManagedCHeapArray() = default;
  explicit constexpr ManagedCHeapArray(std::nullptr_t) {};

  ManagedCHeapArray(pointer_type array_data, size_t) : _data(array_data) {};
  ManagedCHeapArray(ManagedCHeapArray&& other) : _data(other._data) {
    other._data = nullptr;
  };
  template<typename T,
          ENABLE_IF(std::is_convertible<typename ManagedCHeapArray<T>::pointer_type,
                                        pointer_type>::value)>
  ManagedCHeapArray(ManagedCHeapArray<T>&& other) {
    reset(other.release(), size_t{});
  };

  ManagedCHeapArray& operator=(ManagedCHeapArray&& other) {
    if (this != &other) {
      reset(other.release(), size_t{});
    }
    return *this;
  }
  ManagedCHeapArray& operator=(std::nullptr_t) {
    reset();
    return *this;
  }
  template<typename T,
          ENABLE_IF(std::is_convertible<typename ManagedCHeapArray<T>::pointer_type,
                                        pointer_type>::value)>
  ManagedCHeapArray& operator=(ManagedCHeapArray<T>&& other) {
    reset(other.release(), size_t{});
    return *this;
  }

  NONCOPYABLE(ManagedCHeapArray);

  ~ManagedCHeapArray() {
    destroy(_data);
  }

  reference_type operator[](std::size_t i) const {
    return *(_data + i);
  }

  pointer_type get() const {
    return _data;
  }

  pointer_type release() {
    pointer_type ptr = get();
    _data = nullptr;
    return ptr;
  }

  pointer_type leak() {
    return release();
  }

  void reset() {
    reset(nullptr, 0);
  }

  void reset(pointer_type ptr, size_t) {
    pointer_type old_ptr = get();
    _data = ptr;
    destroy(old_ptr);
  }

  void swap(ManagedCHeapArray& other) {
    ::swap(_data, other._data);
  }

private:
  void destroy(pointer_type ptr) {
    if (ptr != nullptr) {
      FreeHeap((void*)ptr);
    }
  }
  pointer_type _data = nullptr;
};

template <typename E>
class ManagedCHeapArray<E, false> {
public:
  using pointer_type = E*;
  using element_type = E;
  using reference_type = E&;

  constexpr ManagedCHeapArray() = default;
  explicit constexpr ManagedCHeapArray(std::nullptr_t) {};

  ManagedCHeapArray(pointer_type array_data, size_t size) : _data(array_data), _size(size) {};
  ManagedCHeapArray(ManagedCHeapArray&& other) : _data(other._data), _size(other._size) {
    other._data = nullptr;
    other._size = 0;
  };
  template<typename T,
          ENABLE_IF(std::is_convertible<typename ManagedCHeapArray<T>::pointer_type,
                                        pointer_type>::value)>
  ManagedCHeapArray(ManagedCHeapArray<T>&& other) {
      const size_t size = other.size();
      reset(other.release(), size);
  };

  ManagedCHeapArray& operator=(ManagedCHeapArray&& other) {
    if (this != &other) {
      const size_t size = other.size();
      reset(other.release(), size);
    }
    return *this;
  }
  ManagedCHeapArray& operator=(std::nullptr_t) {
    reset();
    return *this;
  }
  template<typename T,
          ENABLE_IF(std::is_convertible<typename ManagedCHeapArray<T, false>::pointer_type,
                                        pointer_type>::value)>
  ManagedCHeapArray& operator=(ManagedCHeapArray<T, false>&& other) {
    const size_t size = other.size();
    reset(other.release(), size);
    return *this;
  }

  NONCOPYABLE(ManagedCHeapArray);

  ~ManagedCHeapArray() {
    destroy(_data, _size);
  }

  reference_type operator[](std::size_t i) const {
    return *(_data + i);
  }

  pointer_type get() const {
    return _data;
  }

  pointer_type release() {
    pointer_type ptr = get();
    _data = nullptr;
    _size = 0;
    return ptr;
  }

  pointer_type leak() {
    return release();
  }

  void reset() {
    reset(nullptr, 0);
  }

  void reset(pointer_type ptr, size_t size) {
    pointer_type old_ptr = get();
    size_t old_size = _size;
    _data = ptr;
    _size = size;
    destroy(old_ptr, old_size);
  }

  void swap(ManagedCHeapArray& other) {
    ::swap(_data, other._data);
    ::swap(_size, other._size);
  }

  size_t size() {
    return _size;
  }

private:
  void destroy(pointer_type ptr, size_t size) {
    if (ptr != nullptr) {
      for (size_t i = size; i != 0; --i) {
        (ptr + (i - 1))->~element_type();
      }
      FreeHeap((void*)ptr);
    }
  }
  pointer_type _data = nullptr;
  size_t _size = 0;
};


template<typename T>
struct ProclaimesRelocatable : FalseType {};
template<typename T>
struct ProclaimesRelocatable<ManagedCHeapArray<T>> : TrueType {};
template<typename T>
struct ProclaimesRelocatable<ManagedCHeapObject<T>> : TrueType {};
template<typename T>
struct ProclaimesRelocatable<ManagedCHeapObj<T>> : TrueType {};

template<typename E>
struct IsTriviallyRelocatable {
  static constexpr bool value = (
    std::is_scalar<E>::value ||
    std::is_pointer<E>::value ||
    ProclaimesRelocatable<E>::value
  );
};

template<typename T, ENABLE_IF(IsTriviallyRelocatable<T>::value)>
static ManagedCHeapArray<T> reallocate_managed_c_heap_array_default_init(
    ManagedCHeapArray<T>&& other, size_t old_size, size_t new_size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  char* const old_allocation =
      const_cast<char*>(reinterpret_cast<const volatile char*>(other.release()));
  const pointer_type allocation = reinterpret_cast<pointer_type>(
      ReallocateHeap(old_allocation, new_size * sizeof(T), flags));
  for (size_t i = old_size; i < new_size; ++i) {
    ::new (allocation + i) T;
  }
  return {allocation, new_size};
}

template<typename T, ENABLE_IF(IsTriviallyRelocatable<T>::value)>
static ManagedCHeapArray<T> reallocate_managed_c_heap_array_value_init(
    ManagedCHeapArray<T>&& other, size_t old_size, size_t new_size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  char* const old_allocation =
      const_cast<char*>(reinterpret_cast<const volatile char*>(other.release()));
  const pointer_type allocation = reinterpret_cast<pointer_type>(
      ReallocateHeap(old_allocation, new_size * sizeof(T), flags));
  for (size_t i = old_size; i < new_size; ++i) {
    ::new (allocation + i) T{};
  }
  return {allocation, new_size};
}

template<typename T, ENABLE_IF(!IsTriviallyRelocatable<T>::value &&
                               std::is_move_constructible<T>::value)>
static ManagedCHeapArray<T> reallocate_managed_c_heap_array_default_init(
    ManagedCHeapArray<T>&& other, size_t old_size, size_t new_size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(
      AllocateHeap(new_size * sizeof(T), flags));
  const size_t size_to_move = MIN2(new_size, old_size);
  for (size_t i = 0; i < size_to_move; ++i) {
    ::new (allocation + i) T{std::move(other[i])};
  }
  other.reset();
  for (size_t i = old_size; i < new_size; ++i) {
    ::new (allocation + i) T;
  }
  return {allocation, new_size};
}

template<typename T, ENABLE_IF(!IsTriviallyRelocatable<T>::value &&
                               std::is_move_constructible<T>::value)>
static ManagedCHeapArray<T> reallocate_managed_c_heap_array_value_init(
    ManagedCHeapArray<T>&& other, size_t old_size, size_t new_size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(
      AllocateHeap(new_size * sizeof(T), flags));
  const size_t size_to_move = MIN2(new_size, old_size);
  for (size_t i = 0; i < size_to_move; ++i) {
    ::new (allocation + i) T{std::move(other[i])};
  }
  other.reset();
  for (size_t i = old_size; i < new_size; ++i) {
    ::new (allocation + i) T{};
  }
  return {allocation, new_size};
}

template<typename T, ENABLE_IF(!IsTriviallyRelocatable<T>::value &&
                               !std::is_move_constructible<T>::value &&
                               std::is_copy_constructible<T>::value)>
static ManagedCHeapArray<T> reallocate_managed_c_heap_array_default_init(
    ManagedCHeapArray<T>&& other, size_t old_size, size_t new_size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(
      AllocateHeap(new_size * sizeof(T), flags));
  const size_t size_to_copy = MIN2(new_size, old_size);
  for (size_t i = 0; i < size_to_copy; ++i) {
    ::new (allocation + i) T{other[i]};
  }
  other.reset();
  for (size_t i = old_size; i < new_size; ++i) {
    ::new (allocation + i) T;
  }
  return {allocation, new_size};
}

template<typename T, ENABLE_IF(!IsTriviallyRelocatable<T>::value &&
                               !std::is_move_constructible<T>::value &&
                               std::is_copy_constructible<T>::value)>
static ManagedCHeapArray<T> reallocate_managed_c_heap_array_value_init(
    ManagedCHeapArray<T>&& other, size_t old_size, size_t new_size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(
      AllocateHeap(new_size * sizeof(T), flags));
  const size_t size_to_copy = MIN2(new_size, old_size);
  for (size_t i = 0; i < size_to_copy; ++i) {
    ::new (allocation + i) T{other[i]};
  }
  other.reset();
  for (size_t i = old_size; i < new_size; ++i) {
    ::new (allocation + i) T{};
  }
  return {allocation, new_size};
}

template<typename T>
static ManagedCHeapArray<T> make_managed_c_heap_array_default_init(size_t size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(AllocateHeap(size * sizeof(T), flags));
  for (size_t i = 0; i < size; ++i) {
    ::new (allocation + i) T;
  }
  return {allocation, size};
}

template<typename T>
static ManagedCHeapArray<T> make_managed_c_heap_array_value_init(size_t size, MEMFLAGS flags) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(AllocateHeap(size * sizeof(T), flags));
  for (size_t i = 0; i < size; ++i) {
    ::new (allocation + i) T{};
  }
  return {allocation, size};
}

template<typename T, typename Initilizer>
static ManagedCHeapArray<T> make_managed_c_heap_array_with_initilizer(size_t size, MEMFLAGS flags, Initilizer initilizer) {
  using pointer_type = typename ManagedCHeapArray<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(AllocateHeap(size * sizeof(T), flags));
  initilizer(reinterpret_cast<pointer_type>(allocation));
  return {allocation, size};
}


template<typename T, ENABLE_IF(std::is_base_of<AnyObj, T>::value)>
static ManagedCHeapObject<T> make_managed_c_heap_object_default_init(MEMFLAGS flags) {
  STATIC_ASSERT(!std::is_array<T>::value);
  using pointer_type = typename ManagedCHeapObject<T>::pointer_type;
  const pointer_type allocation = new (flags) T;
  return {allocation};
}

template<typename T, ENABLE_IF(!std::is_base_of<AnyObj, T>::value)>
static ManagedCHeapObject<T> make_managed_c_heap_object_default_init(MEMFLAGS flags) {
  STATIC_ASSERT(!std::is_array<T>::value);
  using pointer_type = typename ManagedCHeapObject<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(AllocateHeap(sizeof(T), flags));
  ::new (allocation) T;
  return {allocation};
}


template<typename T, ENABLE_IF(std::is_base_of<AnyObj, T>::value), typename... Args>
static ManagedCHeapObject<T> make_managed_c_heap_object_value_init(MEMFLAGS flags, Args&&... args) {
  STATIC_ASSERT(!std::is_array<T>::value);
  using pointer_type = typename ManagedCHeapObject<T>::pointer_type;
  const pointer_type allocation = new (flags) T{std::forward<Args>(args)...};
  return {allocation};
}

template<typename T, ENABLE_IF(!std::is_base_of<AnyObj, T>::value), typename... Args>
static ManagedCHeapObject<T> make_managed_c_heap_object_value_init(MEMFLAGS flags, Args&&... args) {
  STATIC_ASSERT(!std::is_array<T>::value);
  using pointer_type = typename ManagedCHeapObject<T>::pointer_type;
  const pointer_type allocation = reinterpret_cast<pointer_type>(AllocateHeap(sizeof(T), flags));
  ::new (allocation) T{std::forward<Args>(args)...};
  return {allocation};
}


template<typename T, typename Initilizer, ENABLE_IF(!std::is_base_of<AnyObj, T>::value)>
static ManagedCHeapObject<T> make_managed_c_heap_object_from_buffer(MEMFLAGS flags, size_t size_in_bytes, Initilizer initilizer) {
  STATIC_ASSERT(!std::is_array<T>::value);
  precond(size_in_bytes >= sizeof(T));
  using pointer_type = typename ManagedCHeapObject<T>::pointer_type;
  const address buffer = reinterpret_cast<address>(AllocateHeap(size_in_bytes, flags));
  const pointer_type allocation = initilizer(buffer);
  postcond(static_cast<void*>(buffer) == static_cast<void*>(allocation));
  return {allocation};
}

template <typename T,
          ENABLE_IF(IsDerivedFromCHeapObj<T>::value),
          ENABLE_IF(!std::is_array<T>::value)>
static ManagedCHeapObj<T> make_managed_c_heap_obj_default_init() {
  using pointer_type = typename ManagedCHeapObj<T>::pointer_type;
  using element_type = typename ManagedCHeapObj<T>::element_type;
  const pointer_type allocation = new element_type;
  return {allocation};
}

template <typename T,
          ENABLE_IF(IsDerivedFromCHeapObj<typename std::remove_extent<T>::type>::value),
          ENABLE_IF(std::is_array<T>::value)>
static ManagedCHeapObj<T> make_managed_c_heap_obj_default_init(size_t size) {
  STATIC_ASSERT(std::extent<T>::value == 0);
  using pointer_type = typename ManagedCHeapObj<T>::pointer_type;
  using element_type = typename ManagedCHeapObj<T>::element_type;
  const pointer_type allocation = new element_type[size];
  return {allocation};
}

template <typename T,
          ENABLE_IF(IsDerivedFromCHeapObj<T>::value),
          ENABLE_IF(!std::is_array<T>::value),
          typename... Args>
static ManagedCHeapObj<T> make_managed_c_heap_obj_value_init(Args&&... args) {
  using pointer_type = typename ManagedCHeapObj<T>::pointer_type;
  using element_type = typename ManagedCHeapObj<T>::element_type;
  const pointer_type allocation = new element_type{std::forward<Args>(args)...};
  return {allocation};
}

template <typename T,
          ENABLE_IF(IsDerivedFromCHeapObj<typename std::remove_extent<T>::type>::value),
          ENABLE_IF(std::is_array<T>::value)>
static ManagedCHeapObj<T> make_managed_c_heap_obj_value_init(size_t size) {
  STATIC_ASSERT(std::extent<T>::value == 0);
  using pointer_type = typename ManagedCHeapObj<T>::pointer_type;
  using element_type = typename ManagedCHeapObj<T>::element_type;
  const pointer_type allocation = new element_type[size]{};
  return {allocation};
}

template<typename T1, typename T2>
bool operator==(const ManagedCHeapArray<T1>& lhs, const ManagedCHeapArray<T2>& rhs) noexcept {
    return lhs.get() == rhs.get();
}
template<typename T1, typename T2>
bool operator!=(const ManagedCHeapArray<T1>& lhs, const ManagedCHeapArray<T2>& rhs) noexcept {
    return !(lhs == rhs);
}

template<typename T>
bool operator==(const ManagedCHeapArray<T>& lhs, std::nullptr_t) noexcept {
    return lhs.get() == nullptr;
}
template<typename T>
bool operator!=(const ManagedCHeapArray<T>& lhs, std::nullptr_t rhs) noexcept {
    return !(lhs == rhs);
}

template<typename T>
bool operator==(std::nullptr_t, const ManagedCHeapArray<T>& rhs) noexcept {
    return nullptr == rhs.get();
}
template<typename T>
bool operator!=(std::nullptr_t lhs, const ManagedCHeapArray<T>& rhs) noexcept {
    return !(lhs == rhs);
}


template<typename T1, typename T2>
bool operator==(const ManagedCHeapObject<T1>& lhs, const ManagedCHeapObject<T2>& rhs) noexcept {
    return lhs.get() == rhs.get();
}
template<typename T1, typename T2>
bool operator!=(const ManagedCHeapObject<T1>& lhs, const ManagedCHeapObject<T2>& rhs) noexcept {
    return !(lhs == rhs);
}

template<typename T>
bool operator==(const ManagedCHeapObject<T>& lhs, std::nullptr_t) noexcept {
    return lhs.get() == nullptr;
}
template<typename T>
bool operator!=(const ManagedCHeapObject<T>& lhs, std::nullptr_t rhs) noexcept {
    return !(lhs == rhs);
}

template<typename T>
bool operator==(std::nullptr_t, const ManagedCHeapObject<T>& rhs) noexcept {
    return nullptr == rhs.get();
}
template<typename T>
bool operator!=(std::nullptr_t lhs, const ManagedCHeapObject<T>& rhs) noexcept {
    return !(lhs == rhs);
}


template<typename T1, typename T2>
bool operator==(const ManagedCHeapObj<T1>& lhs, const ManagedCHeapObj<T2>& rhs) noexcept {
    return lhs.get() == rhs.get();
}
template<typename T1, typename T2>
bool operator!=(const ManagedCHeapObj<T1>& lhs, const ManagedCHeapObj<T2>& rhs) noexcept {
    return !(lhs == rhs);
}

template<typename T>
bool operator==(const ManagedCHeapObj<T>& lhs, std::nullptr_t) noexcept {
    return lhs.get() == nullptr;
}
template<typename T>
bool operator!=(const ManagedCHeapObj<T>& lhs, std::nullptr_t rhs) noexcept {
    return !(lhs == rhs);
}

template<typename T>
bool operator==(std::nullptr_t, const ManagedCHeapObj<T>& rhs) noexcept {
    return nullptr == rhs.get();
}
template<typename T>
bool operator!=(std::nullptr_t lhs, const ManagedCHeapObj<T>& rhs) noexcept {
    return !(lhs == rhs);
}

#endif // SHARE_MEMORY_ALLOCATIONMANAGED_HPP
