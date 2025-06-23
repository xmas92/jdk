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
 */

#include "gc/z/zTLABUsage.hpp"
#include "gc/z/zSize.inline.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"

ZTLABUsage::ZTLABUsage()
  : _used(0_zb),
    _used_history() {}

void ZTLABUsage::increase_used(zbytes size) {
  Atomic::add(reinterpret_cast<volatile size_t*>(&_used), untype(size), memory_order_relaxed);
}

void ZTLABUsage::decrease_used(zbytes size) {
  precond(size <= _used);

  Atomic::sub(reinterpret_cast<volatile size_t*>(&_used), untype(size), memory_order_relaxed);
}

void ZTLABUsage::reset() {
  const zbytes used = Atomic::xchg(&_used, 0_zb);

  // Avoid updates when nothing has been allocated since the last YC
  if (used == 0_zb) {
    return;
  }

  // Save the old values for logging
  const zbytes old_tlab_used = tlab_used();
  const zbytes old_tlab_capacity = tlab_capacity();

  // Update the usage history with the current value
  _used_history.add(untype(used));

  log_debug(gc, tlab)("TLAB usage update: used %zuM -> %zuM, capacity: %zuM -> %zuM",
                      old_tlab_used / M_zb,
                      tlab_used() / M_zb,
                      old_tlab_capacity / M_zb,
                      tlab_capacity() / M_zb);
  }

zbytes ZTLABUsage::tlab_used() const {
  return to_zbytes(_used_history.last());
}

zbytes ZTLABUsage::tlab_capacity() const {
  return to_zbytes(_used_history.davg());
}
