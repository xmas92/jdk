/*
 * Copyright (c) 2005, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "memory/resourceArea.inline.hpp"
#include "memory/universe.hpp"
#include "oops/arrayOop.hpp"
#include "oops/instanceOop.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/stackChunkOop.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/globalDefinitions.hpp"

#ifdef CHECK_UNHANDLED_OOPS

CheckOopFunctionPointer check_oop_function = nullptr;

void oop::register_oop() {
  assert (CheckUnhandledOops, "should only call when CheckUnhandledOops");
  // This gets expensive, which is why checking unhandled oops is on a switch.
  Thread* t = Thread::current_or_null();
  if (t != nullptr && t->is_Java_thread()) {
     t->unhandled_oops()->register_unhandled_oop(this);
  }
}

void oop::unregister_oop() {
  assert (CheckUnhandledOops, "should only call when CheckUnhandledOops");
  // This gets expensive, which is why checking unhandled oops is on a switch.
  Thread* t = Thread::current_or_null();
  if (t != nullptr && t->is_Java_thread()) {
    t->unhandled_oops()->unregister_unhandled_oop(this);
  }
}

#ifdef ASSERT

#define DEF_OOP_CHECK_TYPE_FN_IMPL(Type, OopType, TypeCheckFn)                 \
  void OopType::check_type() const {                                           \
    DescType* const o = *this;                                                 \
    if (o != nullptr && !Thread::current_disabled_oop_cast_checks() &&         \
        !o->TypeCheckFn()) {                                                   \
      ResourceMark rm;                                                         \
      fatal("must be type: " #Type " (%s)", o->print_string());                \
    }                                                                          \
  }

#define DEF_OOP_CHECK_TYPE_FN(type)                                            \
  DEF_OOP_CHECK_TYPE_FN_IMPL(type, type##Oop, is_##type)

DEF_OOP_CHECK_TYPE_FN(instance);
DEF_OOP_CHECK_TYPE_FN(stackChunk);
DEF_OOP_CHECK_TYPE_FN(array);
DEF_OOP_CHECK_TYPE_FN(objArray);
DEF_OOP_CHECK_TYPE_FN(typeArray);

#endif // ASSERT

#endif // CHECK_UNHANDLED_OOPS
