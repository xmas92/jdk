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

#ifndef SHARE_RUNTIME_THREADIDENTIFIER_HPP
#define SHARE_RUNTIME_THREADIDENTIFIER_HPP

#include "jni_md.h"
#include "memory/allStatic.hpp"

#include <cstdint>
#include <limits>

/*
 * Provides unique monotonic identifiers for threads.
 *
 * Java uses unsafe to initialize the tid field for Thread and VirtualThread on construction.
 * JFR uses next() for a non-reusable id for non-java threads.
 */

enum class ThreadID : int64_t {
  ZERO_TID = 0,
  MIN_TID = 3,
  PRIMORDIAL_TID = MIN_TID,
  INITIAL_TID = PRIMORDIAL_TID + 1,
  MAX_TID = std::numeric_limits<jlong>::max(),
};

class ThreadIdentifier : AllStatic {
 public:
  static ThreadID next();
  static ThreadID current();
  static uintptr_t unsafe_offset();
};

#endif // SHARE_RUNTIME_THREADIDENTIFIER_HPP
