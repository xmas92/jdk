/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_PREGCINITALLOCATIONEXPANSIONLOCK_INLINE_HPP
#define SHARE_GC_SHARED_PREGCINITALLOCATIONEXPANSIONLOCK_INLINE_HPP

#include "gc/shared/preGCInitAllocationExpansionLock.hpp"
#include "runtime/init.hpp"
#include "utilities/debug.hpp"

inline PreGCInitAllocationExpansionLock::AllocationLocker::AllocationLocker()
    DEBUG_ONLY( : _locked(false)) {
  if (is_init_completed()) {
    return;
  }
  DEBUG_ONLY(_locked = true;)

  PreGCInitAllocationExpansionLock::lock_for_allocation();
}

inline PreGCInitAllocationExpansionLock::AllocationLocker::~AllocationLocker() {
  if (is_init_completed()) {
    precond(!_locked);
    return;
  }
  precond(_locked);

  PreGCInitAllocationExpansionLock::unlock_for_allocation();
}

inline PreGCInitAllocationExpansionLock::ExpansionLocker::ExpansionLocker()
    DEBUG_ONLY( : _locked(false)) {
  if (is_init_completed()) {
    return;
  }
  DEBUG_ONLY(_locked = true;)

  PreGCInitAllocationExpansionLock::lock_for_expansion();
}

inline PreGCInitAllocationExpansionLock::ExpansionLocker::~ExpansionLocker() {
  if (is_init_completed()) {
    precond(!_locked);
    return;
  }
  precond(_locked);

  PreGCInitAllocationExpansionLock::unlock_for_expansion();
}

#endif // SHARE_GC_SHARED_PREGCINITALLOCATIONEXPANSIONLOCK_INLINE_HPP
