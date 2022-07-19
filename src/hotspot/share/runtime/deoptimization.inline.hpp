/*
 * Copyright (c) 2022, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_DEOPTIMIZATION_INLINE_HPP
#define SHARE_RUNTIME_DEOPTIMIZATION_INLINE_HPP

#include "runtime/deoptimization.hpp"

#include "code/compiledMethod.hpp"
#include "runtime/safepointVerifiers.hpp"

// Standin for C++17 Fold-expressions
template <typename... Ints>
int mark_and_deoptimize_sum(Ints... ints) {
  using unused = int[];
  int result = 0;
  (void)unused{ 0, (result += ints, 0)... };
  return result;
}

template<typename... MarkerFn>
int Deoptimization::mark_and_deoptimize(MarkerFn... marker_fns) {
  auto mark_lambda = [](CompiledMethod* cm, bool inc_recompile_counts = true) {
    assert(cm != nullptr, "complied method must not be null");
    cm->mark_for_deoptimization(inc_recompile_counts);
  };
  ResourceMark rm;
  DeoptimizationMarker dm;
  int number_marked = 0;
  {
    NoSafepointVerifier nsv;
    assert_locked_or_safepoint(Compile_lock);
    number_marked = mark_and_deoptimize_sum(marker_fns(mark_lambda)...);
    deoptimize_all_marked();
  }
  run_deoptimize_closure();
  return number_marked;
}

template<typename... MarkerFn>
int Deoptimization::mark_and_forget(MarkerFn... marker_fns) {
  auto mark_lambda = [](CompiledMethod* cm, bool inc_recompile_counts = true) {
    assert(cm != nullptr, "complied method must not be null");
    cm->mark_for_deoptimization(inc_recompile_counts, false);
  };
  int number_marked = 0;
  {
    NoSafepointVerifier nsv;
    assert_locked_or_safepoint(Compile_lock);
    number_marked = mark_and_deoptimize_sum(marker_fns(mark_lambda)...);
  }
  return number_marked;
}

#endif // SHARE_RUNTIME_DEOPTIMIZATION_INLINE_HPP
