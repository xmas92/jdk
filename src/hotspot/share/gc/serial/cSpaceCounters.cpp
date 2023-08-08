/*
 * Copyright (c) 2002, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/serial/cSpaceCounters.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/allocationManaged.hpp"
#include "memory/resourceArea.hpp"

CSpaceCounters::CSpaceCounters(const char* name, int ordinal, size_t max_size,
                               ContiguousSpace* s, GenerationCounters* gc)
  : _last_used_in_bytes(0), _space(s)
{
  if (UsePerfData) {
    EXCEPTION_MARK;
    ResourceMark rm;

    const char* cns = PerfDataManager::name_space(gc->name_space(), "space",
                                                  ordinal);
    // candidate: c-d
    const size_t len = strlen(cns) + 1;
    _name_space = make_managed_c_heap_array_default_init<char>(len, mtGC);
    strncpy(_name_space.get(), cns, len);
    _name_space[len-1] = '\0';

    const char* cname = PerfDataManager::counter_name(_name_space.get(), "name");
    PerfDataManager::create_string_constant(SUN_GC, cname, name, CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "maxCapacity");
    _max_capacity = PerfDataManager::create_variable(SUN_GC, cname,
                                                     PerfData::U_Bytes,
                                                     (jlong)max_size,
                                                     CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "capacity");
    _capacity = PerfDataManager::create_variable(SUN_GC, cname,
                                                 PerfData::U_Bytes,
                                                 _space->capacity(),
                                                 CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "used");
    _used = PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes,
                                             new UsedHelper(this),
                                             CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "initCapacity");
    PerfDataManager::create_constant(SUN_GC, cname, PerfData::U_Bytes,
                                     _space->capacity(), CHECK);
  }
}

void CSpaceCounters::update_capacity() {
  _capacity->set_value(_space->capacity());
}

void CSpaceCounters::update_used() {
  size_t new_used = _space->used();
  Atomic::store(&_last_used_in_bytes, new_used);
  _used->set_value(new_used);
}

void CSpaceCounters::update_all() {
  update_used();
  update_capacity();
}

jlong CSpaceCounters::UsedHelper::take_sample(){
  // Sampling may occur during GC, possibly while GC is updating the space.
  // The space can be in an inconsistent state during such an update.  We
  // don't want to block sampling for the duration of a GC.  Instead, skip
  // sampling in that case, using the last recorded value.
  assert(!Heap_lock->owned_by_self(), "precondition");
  if (Heap_lock->try_lock()) {
    size_t new_used = _counters->_space->used();
    Atomic::store(&_counters->_last_used_in_bytes, new_used);
    Heap_lock->unlock();
  }
  return Atomic::load(&_counters->_last_used_in_bytes);
}
