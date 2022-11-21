/*
 * Copyright (c) 2021, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1CARDSETALLOCOPTIONS_HPP
#define SHARE_GC_G1_G1CARDSETALLOCOPTIONS_HPP

#include "gc/g1/g1MonotonicArena.hpp"

// Collects G1CardSetAllocator options/heuristics. Called by G1CardSetAllocator
// to determine the next size of the allocated memory Segment.
class G1CardSetAllocOptions : public G1MonotonicArena::AllocOptions {
  static const uint MinimumNumSlots = 8;
  static const uint MaximumNumSlots = UINT_MAX / 2;

  uint exponential_expand(uint prev_num_slots) const {
    return clamp(prev_num_slots * 2, _initial_num_slots, _max_num_slots);
  }

public:
  static const uint SlotAlignment = 8;

  G1CardSetAllocOptions(uint slot_size, uint initial_num_slots = MinimumNumSlots, uint max_num_slots = MaximumNumSlots) :
    G1MonotonicArena::AllocOptions(mtGCCardSet, slot_size, initial_num_slots, max_num_slots, SlotAlignment) {
  }

  virtual uint next_num_slots(uint prev_num_slots) const override {
    return exponential_expand(prev_num_slots);
  }
};


#endif // SHARE_GC_G1_G1CARDSETALLOCOPTIONS_HPP
