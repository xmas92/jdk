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

#ifndef SHARE_RUNTIME_LOCKSTACK_INLINE_HPP
#define SHARE_RUNTIME_LOCKSTACK_INLINE_HPP

#include "runtime/lockStack.hpp"

#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/globals.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/stackWatermarkSet.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"
#include "runtime/vm_version.hpp"
#include <cstdint>

inline oop* LockStack::stack() {
  if (LSRecursiveFixedSize) {
    return _base;
  }

  if (_storage == nullptr) {
    return nullptr;
  }

  return _storage->stack();
}

inline const oop* LockStack::stack() const {
  if (LSRecursiveFixedSize) {
    return _base;
  }

  if (_storage == nullptr) {
    return nullptr;
  }

  return _storage->stack();
}

inline int LockStack::to_index(uint32_t offset) {
  return (offset - lock_stack_base_offset) / oopSize;
}

inline bool LockStack::can_push() const {
  assert(!VM_Version::supports_recursive_lightweight_locking(), "does not use");
  return to_index(_top) < CAPACITY;
}

inline void LockStack::push(oop o) {
  assert(!VM_Version::supports_recursive_lightweight_locking(), "does not use");
  assert(oopDesc::is_oop(o), "must be");
  assert(!contains(o), "entries must be unique");
  assert(can_push(), "must have room");
  assert(_base[to_index(_top)] == nullptr, "expect zapped entry");
  _base[to_index(_top)] = o;
  _top += oopSize;
}

inline void LockStack::remove(oop o) {
  assert(!VM_Version::supports_recursive_lightweight_locking(), "does not use");
  assert(contains(o), "entry must be present: " PTR_FORMAT, p2i(o));
  int end = to_index(_top);
  for (int i = 0; i < end; i++) {
    if (_base[i] == o) {
      int last = end - 1;
      for (; i < last; i++) {
        _base[i] = _base[i + 1];
      }
      _top -= oopSize;
#ifdef ASSERT
      _base[to_index(_top)] = nullptr;
#endif
      break;
    }
  }
  assert(!contains(o), "entries must be unique: " PTR_FORMAT, p2i(o));
}

static const uint32_t index_shift = LogBytesPerWord;

inline uint32_t LockStack::to_array_index(Index index) {
  precond(index >= Index::first_index);
  return (static_cast<uint32_t>(index) >> index_shift) - 1;
}

inline LockStack::Index LockStack::from_array_index(uint32_t index) {
  const Index ret = Index((index + 1) << index_shift);
  postcond(ret >= Index::first_index);
  postcond(to_array_index(ret) == index);
  return ret;
}

class LockStack::Verifier : StackObj {
private:
  const LockStack& _ls;
  const char* const _prefix;
  const char* _at;
  const bool _relaxed_oop;

  void log_lock_stack(bool invariant) {
    if (!invariant) {
      LogTarget(Info, fastlock) lt;
      if (lt.is_enabled()) {
        ResourceMark rm;
        LogStream ls(lt);
        _ls.print_on(&ls);
      }

    }
  }

  void invariant(bool invariant, const char* msg) {
    log_lock_stack(invariant);
    assert(invariant, "%s(%s) %s", _prefix, _at, msg);
  }

  void invariant(bool invariant, const char* msg, uint32_t index) {
    log_lock_stack(invariant);
    assert(invariant, "%s(%s) %s [%u]", _prefix, _at, msg, index);
  }

  void invariant(bool invariant, const char* msg, Index index) {
    this->invariant(invariant, msg, static_cast<uint32_t>(index));
  }

  void invariant(bool invariant, const char* msg, intptr_t pointer) {
    log_lock_stack(invariant);
    assert(invariant, "%s(%s) %s [" PTR_FORMAT "]", _prefix, _at, msg, pointer);
  }

  void invariant(bool invariant, const char* msg, void* pointer) {
    this->invariant(invariant, msg, p2i(pointer));
  }

  void invariant(bool invariant, const char* msg, uint32_t index, intptr_t pointer) {
    log_lock_stack(invariant);
    assert(invariant, "%s(%s) %s [%u]: [" PTR_FORMAT "]", _prefix, _at, msg, index, pointer);
  }

  void invariant(bool invariant, const char* msg, uint32_t index, void* pointer) {
    this->invariant(invariant, msg, index, p2i(pointer));
  }

  void verify() {
    invariant(LockingMode == LM_LIGHTWEIGHT, "LockStack used with wrong LockingMode");

    const bool lock_stack_is_stable = SafepointSynchronize::is_at_safepoint() || _ls.get_thread() == Thread::current();
    if (!lock_stack_is_stable) {
      return;
    }

    invariant(_ls._next_index == from_array_index(to_array_index(_ls._next_index)),
              "Bad [to|from]_array_index");

    if (_ls._storage == nullptr) {
      if (LSRecursiveFixedSize) {
        invariant(_ls._last_index == from_array_index(CAPACITY - 1),
                  "Bad _last_index", _ls._last_index);
        invariant(_ls.capacity() == CAPACITY, "Bad capacity", _ls.capacity());
      } else {
        // Empty lock stack.
        invariant(_ls._next_index == Index::first_index,
                  "Bad _next_index", _ls._next_index);
        invariant(_ls._last_index == Index::empty_index,
                  "Bad _last_index", _ls._last_index);
        invariant(_ls.capacity() == 0, "Bad capacity", _ls.capacity());
        return;
      }
    }

    invariant(_ls._last_index == from_array_index(to_array_index(_ls._last_index)),
              "Bad [to|from]_array_index");

    const oop* const stack = _ls.stack();
    if (LSRecursiveFixedSize) {
      invariant(Atomic::load(&_ls._bad_oop_sentinel) == badOopVal,
                "Bad _bad_oop_sentinel", Atomic::load(&_ls._bad_oop_sentinel));
    } else {
      invariant(Atomic::load(&_ls._storage->_bad_oop_sentinel) == badOopVal,
                "Bad _bad_oop_sentinel", Atomic::load(&_ls._storage->_bad_oop_sentinel));

    }
    invariant(_ls.capacity() > 0, "Bad capacity", _ls.capacity());
    invariant(to_array_index(_ls._next_index) <= _ls.capacity(),
              "Bad _next_index", _ls._next_index);
    invariant(to_array_index(_ls._last_index) == _ls.capacity() - 1,
              "Bad _last_index", _ls._last_index);

    for (uint32_t i = 0; i < _ls.capacity(); i++) {
      oop obj = stack[i];
      if (i < to_array_index(_ls._next_index)) {
        if (_relaxed_oop) {
          // We cannot look at the object, nor the object headers here
          continue;
        }
        invariant(oopDesc::is_oop_or_null(obj), "Must be oop or null @", i, obj);
        if (obj != nullptr) {
          invariant(obj->is_locked(), "Must be locked @", i);
          if (obj->mark_acquire().has_monitor()) {
            ObjectMonitor* monitor = obj->mark().monitor();
            invariant(monitor->is_owner_anonymous() ||
                                monitor->owner() == _ls.get_thread() ||
                                _ls.get_thread()->current_waiting_monitor() == monitor,
                      "Inflated with bad owner @", i , monitor->owner());
          }
        }
      } else {
        invariant(obj == nullptr, "Must be null @", i, obj);
      }
    }

  }

public:
  Verifier(const LockStack& ls, const char* prefix, bool relaxed_oop = false) :
      _ls(ls),
      _prefix(prefix),
      _at(nullptr),
      _relaxed_oop(relaxed_oop) {
    _at = "ctor";
    verify();
  }
  ~Verifier() {
    _at = "dtor";
    verify();
  }

};

inline uint32_t LockStack::capacity() const {
  precond(VM_Version::supports_recursive_lightweight_locking());
  if (_last_index == Index::empty_index) {
    return 0;
  }
  return to_array_index(_last_index) + 1;
}

inline LockStack::Index LockStack::enter(oop o) {
  precond(VM_Version::supports_recursive_lightweight_locking());
  precond(!contains(o));
  DEBUG_ONLY(Verifier v(*this, "enter");)

  uint32_t i = to_array_index(_next_index);

  // Reclaim unstructured exits
  while (i > 0 && stack()[i - 1] == nullptr) {
    i--;
  }

  // Allocate stack slot
  if (i == capacity()) {
    if (LSRecursiveFixedSize) {
      // We cannot enter. The lock stack is fixed and full.
      return Index::empty_index;
    }

    if (_storage == nullptr) {
      ResourceMark rm;
      log_debug(fastlock)("LS[" PTR_FORMAT "] Inital: " PTR_FORMAT " @ %u(%u) TN: %s", p2i(get_thread()),
                          p2i(o), static_cast<uint32_t>(from_array_index(i)), i, get_thread()->name());
      _storage = LockStackStorage::create(INITIAL_CAPACITY);
      _last_index = from_array_index(INITIAL_CAPACITY - 1);
    } else {
      // TODO[Axel]: Bound this resize.
      //             Cleanup resize interface.
      uint32_t capacity = this->capacity();
      log_debug(fastlock)("LS[" PTR_FORMAT "] Resize: " PTR_FORMAT " @ %u(%u) %u -> %u", p2i(get_thread()),
                          p2i(o), static_cast<uint32_t>(from_array_index(i)), i, capacity, capacity + 1);
      LockStackStorage::resize(_storage, capacity, capacity + 1);
      _last_index = from_array_index(capacity - 1);
      postcond(capacity == this->capacity());
    }
  }
  // Fill stack slot
  stack()[i] = o;
  _next_index = from_array_index(i + 1);
  log_trace(fastlock)("LS[" PTR_FORMAT "]  Enter: " PTR_FORMAT " @ %u(%u)", p2i(get_thread()),
                      p2i(o), static_cast<uint32_t>(from_array_index(i)), i);

  return from_array_index(i);
}

inline LockStack::Index LockStack::top_index() const {
  precond(VM_Version::supports_recursive_lightweight_locking());
  precond(_next_index != Index::first_index);
  return from_array_index(to_array_index(_next_index) - 1);
}

inline bool LockStack::exit(oop o, Index at) {
  precond(VM_Version::supports_recursive_lightweight_locking());
  precond(o == stack()[to_array_index(at)]);
  DEBUG_ONLY(Verifier v(*this, "exit");)

  if (at == top_index()) {
    // Top of stack.
    log_trace(fastlock)("LS[" PTR_FORMAT "]   Exit: " PTR_FORMAT " @ %u(%u)", p2i(get_thread()),
                       p2i(o), static_cast<uint32_t>(at), to_array_index(at));
    DEBUG_ONLY(stack()[to_array_index(at)] = nullptr;)
    _next_index = at;
    return false;
  } else {
    // Unstructured exit.
    log_trace(fastlock)("LS[" PTR_FORMAT "]  UExit: " PTR_FORMAT " @ %u(%u)", p2i(get_thread()),
                       p2i(o), static_cast<uint32_t>(at), to_array_index(at));
    stack()[to_array_index(at)] = nullptr;
    return true;
  }
}

inline bool LockStack::exit(oop o) {
  precond(contains(o));
  precond(VM_Version::supports_recursive_lightweight_locking());
  DEBUG_ONLY(Verifier v(*this, "exit");)

  const uint32_t next_index = to_array_index(_next_index);

  for (uint32_t i = next_index; i > 0; i--) {
    if (stack()[i - 1] == o) {
      return exit(o, from_array_index(i - 1));
    }
  }

  ShouldNotReachHere();
  return true;
}

inline bool LockStack::contains(oop o, Index at) const {
  assert(at < _next_index, "%u(%u)", static_cast<uint32_t>(at), to_array_index(at));
  precond(at < _next_index);
  precond(VM_Version::supports_recursive_lightweight_locking());
  DEBUG_ONLY(Verifier v(*this, "contains");)

  if (at == Index::empty_index) {
    return false;
  }
  return stack()[to_array_index(at)] == o;
}

inline bool LockStack::contains(oop o) const {
  // Can't poke around in thread oops without having started stack watermark processing.
  assert(StackWatermarkSet::processing_started(get_thread()), "Processing must have started!");
  DEBUG_ONLY(Verifier v(*this, "contains");)

  if (!VM_Version::supports_recursive_lightweight_locking()) {
    int end = to_index(_top);
    for (int i = end - 1; i >= 0; i--) {
      if (_base[i] == o) {
        return true;
      }
    }
    return false;
  }

  for (uint32_t i = to_array_index(_next_index); i > 0; i--) {
    if (stack()[i - 1] == o) {
      return true;
    }
  }
  return false;
}

inline void LockStack::oops_do(OopClosure* cl) {
  DEBUG_ONLY(Verifier v(*this, "oops_do", true);)

  if (!VM_Version::supports_recursive_lightweight_locking()) {
    int end = to_index(_top);
    for (int i = 0; i < end; i++) {
      cl->do_oop(&_base[i]);
    }
  }

  for (uint32_t i = to_array_index(_next_index); i > 0; i--) {
    cl->do_oop(&stack()[i - 1]);
  }
}

#endif // SHARE_RUNTIME_LOCKSTACK_INLINE_HPP
