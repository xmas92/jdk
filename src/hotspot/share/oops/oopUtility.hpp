/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_OOPS_OOPUTILITY_HPP
#define SHARE_VM_OOPS_OOPUTILITY_HPP

#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/globalDefinitions.hpp"

class ValuePayload;
class fieldDescriptor;

class OopUtility : AllStatic {
  template <typename T>
  inline static T load_field(address field_addr);
  inline static oop load_oop_field(address field_addr);
  inline static bool field_equals_operator(const fieldDescriptor& field,
                                           const ValuePayload& a,
                                           const ValuePayload& b);
  inline static bool is_substitutable_internal(const ValuePayload& a, const ValuePayload& b);

  template <typename OopT>
  inline static bool java_equals_operator_t(OopT a, OopT b);

public:
  inline static bool java_equals_operator(oop a, oop b);
  inline static bool java_equals_operator(oop a, narrowOop b);
  inline static bool java_equals_operator(narrowOop a, oop b);
  inline static bool java_equals_operator(narrowOop a, narrowOop b);

  inline static bool is_substitutable(const ValuePayload& a, const ValuePayload& b);
};

#endif // SHARE_VM_OOPS_OOPUTILITY_HPP
