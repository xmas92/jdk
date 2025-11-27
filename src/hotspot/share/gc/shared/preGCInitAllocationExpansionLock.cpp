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

#include "gc/shared/preGCInitAllocationExpansionLock.inline.hpp"
#include "runtime/mutexLocker.hpp"

Atomic<ssize_t> PreGCInitAllocationExpansionLock::_count{0};

void PreGCInitAllocationExpansionLock::lock_for_allocation() {
  for (;;) {
    const ssize_t count = _count.load_acquire();
    if (count < 0) {
      // Expansion is pending. Wait for until finished.
      MonitorLocker locker(PreGCInitAllocationExpansion_lock);
      while (_count.load_acquire() < 0) {
        locker.wait();
      }
    } else if (count == _count.compare_exchange(count, count + 1)) {
      // Successfully acquired allocation side lock.
      return;
    }
  }
}

void PreGCInitAllocationExpansionLock::unlock_for_allocation() {
  for (;;) {
    const ssize_t count = _count.load_acquire();
    precond(count != 0);

    const ssize_t unlock_value = count < 0 ? count + 1 : count - 1;

    if (count == _count.compare_exchange(count, unlock_value)) {
      // Successfully unlocked allocation side.
      if (count == -2) {
        // Last allocation unlock with pending expansion.
        MonitorLocker locker(PreGCInitAllocationExpansion_lock);
        locker.notify_all();
      }
      return;
    }
  }
}

void PreGCInitAllocationExpansionLock::lock_for_expansion() {
  for (;;) {
    const ssize_t count = _count.load_acquire();
    if (count < 0) {
      // Already pending expansion. Wait for until finished.
      MonitorLocker locker(PreGCInitAllocationExpansion_lock);
      while (_count.load_acquire() < 0) {
        locker.wait();
      }
    } else if (count == _count.compare_exchange(count, -(count + 1))) {
      // Successfully signaled pending expansion.
      if (count != 0) {
        // We must wait for all pendiding allocations.
        MonitorLocker locker(PreGCInitAllocationExpansion_lock);
        while (_count.load_acquire() != -1) {
          locker.wait();
        }
      }
      return;
    }
  }
}

void PreGCInitAllocationExpansionLock::unlock_for_expansion() {
  precond(_count.load_relaxed() == -1);

  MonitorLocker locker(PreGCInitAllocationExpansion_lock);
  _count.release_store(0);
  locker.notify_all();
}