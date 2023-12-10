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

#include "runtime/lockStack.hpp"
#include "precompiled.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/lockStack.inline.hpp"
#include "utilities/macros.hpp"
#include <cstdint>

const int LockStack::lock_stack_offset = in_bytes(JavaThread::lock_stack_offset());

LockStack::LockStack(JavaThread* jt) :
  _next_index(Index::first_index), _last_index(Index::empty_index), _storage(nullptr) {
}

LockStack::~LockStack() {
  if (_storage != nullptr) {
    LockStackStorage::destroy(_storage, to_array_index(_last_index) + 1);
  }
}

JavaThread* LockStack::get_thread() const {
  char* addr = reinterpret_cast<char*>(const_cast<LockStack*>(this));
  return reinterpret_cast<JavaThread*>(addr - lock_stack_offset);
}

void LockStack::print_on(outputStream* st) const {
  auto print_index = [&](const char* s, LockStack::Index index) {
    if (index >= LockStack::Index::first_index) {
      st->print_cr("%s: %u[%u]", s, static_cast<uint32_t>(index), LockStack::to_array_index(index));
    } else {
      st->print_cr("%s: %u", s, static_cast<uint32_t>(index));
    }
  };
  st->print_cr("_storage: " PTR_FORMAT, p2i(_storage));
  st->print_cr("capacity: %u", capacity());
  print_index("_next_index", _next_index);
  print_index("_last_index", _last_index);

  if (_storage == nullptr) {
    return;
  }

  uint32_t end = DEBUG_ONLY(to_array_index(_last_index) + 1)
                 NOT_DEBUG(to_array_index(_next_index));
  for (uint32_t i = end; i != 0; i--) {
    st->print("LockStack[%d]: ", i - 1);
    oop o = _storage->stack()[i - 1];
    if (oopDesc::is_oop(o)) {
      o->print_on(st);
    } else {
      st->print_cr("not an oop: " PTR_FORMAT, p2i(o));
    }
  }
}

void LockStack::verify() const {
  Verifier v(*this, "verify");
}
