/*
 * Copyright (c) 2022, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#ifndef SHARE_RUNTIME_LOCKSTACK_HPP
#define SHARE_RUNTIME_LOCKSTACK_HPP

#include "memory/allocation.hpp"
#include "oops/oopHandle.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/sizes.hpp"
#include <cstdint>

class JavaThread;
class OopClosure;
class outputStream;

class LockStack {
  friend class VMStructs;
  JVMCI_ONLY(friend class JVMCIVMStructs;)
public:
  enum class Index : uint32_t {
    empty_index = 0,
    first_index = BytesPerWord ,
  };

  static const int CAPACITY = 8;
private:
  static const uint32_t INITIAL_CAPACITY = 1;
  static const int lock_stack_offset;
  static const int lock_stack_top_offset;
  static const int lock_stack_base_offset;

  // The offset of the next element, in bytes, relative to the JavaThread structure.
  // We do this instead of a simple index into the array because this allows for
  // efficient addressing in generated code.
  uint32_t _top;
  const intptr_t _bad_oop_sentinel = badOopVal;
  oop _base[CAPACITY];

  // one-indexed.
  Index _next_index;
  // last index, == empty_index if
  Index _last_index;

  struct LockStackStorage {
    using ElementT = oop;

    const intptr_t _bad_oop_sentinel = badOopVal;

    static size_t stack_size(uint32_t capacity) {
      return sizeof(ElementT) * capacity;
    }

    static size_t header_size() {
      // Required for _bad_oop_sentinel
      STATIC_ASSERT(sizeof(LockStackStorage) % alignof(ElementT) == 0);
      return sizeof(LockStackStorage);
    }

    static size_t total_size(uint32_t capacity) {
      return header_size() + stack_size(capacity);
    }

    static LockStackStorage* allocate(uint32_t capacity) {
      const size_t size = total_size(capacity);
      char* const addr = AllocateHeap(size, MEMFLAGS::mtSynchronizer);

      char* const stack_addr = addr + header_size();
      ElementT* const stack = ::new (stack_addr) ElementT[capacity];
      LockStackStorage* const storage = ::new (addr) LockStackStorage();

      postcond(storage->stack() == stack);
      return storage;
    }

    ElementT* stack() {
      char* const addr = reinterpret_cast<char*>(this);
      char* const stack_addr = addr + header_size();
      return reinterpret_cast<ElementT*>(stack_addr);
    }

    static LockStackStorage* create(uint32_t capacity) {
      return allocate(capacity);
    }

    static void clear(ElementT* stack, size_t capacity) {
      for (size_t i = 0; i < capacity; i++) {
        stack[i] = nullptr;
      }
    }

#ifdef CHECK_UNHANDLED_OOPS
    static void destruct_stack(ElementT* stack, size_t capacity) {
      for (size_t i = 0; i < capacity; i++) {
        stack[i].~oop();
      }
    }
#endif

    static void destroy(LockStackStorage* storage, size_t capacity) {
      CHECK_UNHANDLED_OOPS_ONLY(destruct_stack(storage->stack(), capacity);)
      storage->~LockStackStorage();
      FreeHeap(storage);
    }

    static void resize(LockStackStorage*& storage, uint32_t& capacity, uint32_t new_capacity) {
      LockStackStorage* const new_storage = allocate(new_capacity);
      ElementT* const stack = storage->stack();
      ElementT* const new_stack = new_storage->stack();
      DEBUG_ONLY(clear(new_stack, new_capacity);)
      for (size_t i = 0; i < capacity; i++) {
        new_stack[i] = stack[i];
      }
      destroy(storage, capacity);

      storage = new_storage;
      capacity = new_capacity;
    }
  };

  LockStackStorage* _storage;

  // Required for emitted code.
  STATIC_ASSERT(sizeof(LockStackStorage) == sizeof(LockStackStorage::ElementT));

  // Get the owning thread of this lock-stack.
  JavaThread* get_thread() const;

  inline static uint32_t to_array_index(Index index);
  inline static Index from_array_index(uint32_t index);

  inline uint32_t capacity() const;

  class Verifier;

  // Given an offset (in bytes) calculate the index into the lock-stack.
  static inline int to_index(uint32_t offset);
public:
  static ByteSize top_offset()  { return byte_offset_of(LockStack, _top); }
  static ByteSize base_offset() { return byte_offset_of(LockStack, _base); }
  static ByteSize next_index_offset()  { return byte_offset_of(LockStack, _next_index); }
  static ByteSize storage_addr_offset() { return byte_offset_of(LockStack, _storage); }
  static ByteSize last_index_offset() { return byte_offset_of(LockStack, _last_index); }

  inline Index top_index() const;
  Index next_index() const { return _next_index; }

  bool is_full() const {
    return _next_index > _last_index;
  }

  LockStack(JavaThread* jt);
  ~LockStack();

  inline oop* stack();
  inline const oop* stack() const;

  // The boundary indicies of the lock-stack.
  static uint32_t start_offset();
  static uint32_t end_offset();

  // Return true if we have room to push onto this lock-stack, false otherwise.
  inline bool can_push() const;

  // Pushes an oop on this lock-stack.
  inline void push(oop o);

  // Removes an oop from an arbitrary location of this lock-stack.
  inline void remove(oop o);

  // Pushes an oop on this lock-stack.
  inline Index enter(oop o);

  // Removes an oop from an arbitrary location of this lock-stack.
  // Precondition: This lock-stack must contain the oop.
  // Returns true if call exit was unstructured
  inline bool exit(oop o, Index at);
  inline bool exit(oop o);
  // Tests whether the oop is on this lock-stack.
  inline bool contains(oop o, Index at) const;
  inline bool contains(oop o) const;

  // GC support
  inline void oops_do(OopClosure* cl);

  // Printing
  void print_on(outputStream* st) const;

  void verify() const;
};

#endif // SHARE_RUNTIME_LOCKSTACK_HPP
