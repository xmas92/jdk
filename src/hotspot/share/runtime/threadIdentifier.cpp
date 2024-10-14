/*
 * Copyright (c) 2022, 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "runtime/atomic.hpp"
#include "runtime/threadIdentifier.hpp"
#include "utilities/debug.hpp"

#include <type_traits>

// starting at 3, excluding reserved values defined in ObjectMonitor.hpp
static volatile ThreadID next_thread_id = ThreadID::INITIAL_TID;

uintptr_t ThreadIdentifier::unsafe_offset() {
  return reinterpret_cast<uintptr_t>(&next_thread_id);
}

ThreadID ThreadIdentifier::current() {
  return Atomic::load(&next_thread_id);
}

ThreadID operator+(ThreadID a, ThreadID b) {
  return static_cast<ThreadID>(
    static_cast<std::underlying_type_t<ThreadID>>(a) +
    static_cast<std::underlying_type_t<ThreadID>>(b)
  );
}

ThreadID ThreadIdentifier::next() {
  ThreadID tid = static_cast<ThreadID>(
    Atomic::fetch_then_add(
      reinterpret_cast<volatile std::underlying_type_t<ThreadID>*>(&next_thread_id),
      static_cast<std::underlying_type_t<ThreadID>>(1),
      memory_order_relaxed));
  guarantee(tid < ThreadID::MAX_TID, "Running out of tids.");
  return tid;
}
