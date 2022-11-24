/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_METAPROGRAMMING_ISDERIVEDFROMCHEAPOBJ_HPP
#define SHARE_METAPROGRAMMING_ISDERIVEDFROMCHEAPOBJ_HPP

#include "memory/allocation.hpp"

#include <type_traits>

template <typename E>
struct IsDerivedFromCHeapObj {
  static constexpr bool value = (
    false
#define check_type_is_base_of_c_cheap_obj(flag, desc) || \
  (std::is_base_of<CHeapObj<flag>, E>::value \
  && std::is_convertible<const volatile E*, const volatile CHeapObj<flag>*>::value)
    MEMORY_TYPES_DO(check_type_is_base_of_c_cheap_obj)
#undef check_type_is_base_of_c_cheap_obj
  );
};

#endif // SHARE_METAPROGRAMMING_ISDERIVEDFROMCHEAPOBJ_HPP
