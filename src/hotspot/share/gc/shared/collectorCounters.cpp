/*
 * Copyright (c) 2002, 2019, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/collectorCounters.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/allocationManaged.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/os.hpp"

CollectorCounters::CollectorCounters(const char* name, int ordinal) {

  if (UsePerfData) {
    EXCEPTION_MARK;
    ResourceMark rm;

    const char* cns = PerfDataManager::name_space("collector", ordinal);

    // candidate: c-d
    const size_t len = strlen(cns) + 1;
    _name_space = make_managed_c_heap_array_default_init<char>(len, mtGC);
    strncpy(_name_space.get(), cns, len);
    _name_space[len-1] = '\0';

    char* cname = PerfDataManager::counter_name(_name_space.get(), "name");
    PerfDataManager::create_string_constant(SUN_GC, cname, name, CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "invocations");
    _invocations = PerfDataManager::create_counter(SUN_GC, cname,
                                                   PerfData::U_Events, CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "time");
    _time = PerfDataManager::create_counter(SUN_GC, cname, PerfData::U_Ticks,
                                            CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "lastEntryTime");
    _last_entry_time = PerfDataManager::create_variable(SUN_GC, cname,
                                                        PerfData::U_Ticks,
                                                        CHECK);

    cname = PerfDataManager::counter_name(_name_space.get(), "lastExitTime");
    _last_exit_time = PerfDataManager::create_variable(SUN_GC, cname,
                                                       PerfData::U_Ticks,
                                                       CHECK);
  }
}

TraceCollectorStats::TraceCollectorStats(CollectorCounters* c) :
    PerfTraceTimedEvent(c->time_counter(), c->invocation_counter()),
    _c(c) {

  if (UsePerfData) {
     _c->last_entry_counter()->set_value(os::elapsed_counter());
  }
}

TraceCollectorStats::~TraceCollectorStats() {
  if (UsePerfData) {
    _c->last_exit_counter()->set_value(os::elapsed_counter());
  }
}
