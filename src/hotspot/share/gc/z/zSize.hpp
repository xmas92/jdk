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

#ifndef SHARE_GC_Z_ZSIZE_HPP
#define SHARE_GC_Z_ZSIZE_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

enum class zbytes : size_t {};
enum class zwords : size_t {};

class ZBytes : public AllStatic {
public:
  // Alignment
  template <typename A>
  static constexpr bool is_aligned(zbytes bytes, A alignment);
  template <typename A>
  static constexpr zbytes align_up(zbytes bytes, A alignment);
  template <typename A>
  static constexpr zbytes align_down(zbytes bytes, A alignment);

  // Conversion
  static constexpr zwords to_words(zbytes bytes);
  static constexpr zwords to_words_round_up(zbytes bytes);
  static constexpr zwords to_words_round_down(zbytes bytes);

  static constexpr zbytes from_words(size_t size_in_words);
};

class ZWords : public AllStatic {
public:
  // Alignment
  template <typename A>
  static constexpr bool is_aligned(zwords bytes, A alignment);
  template <typename A>
  static constexpr zwords align_up(zwords bytes, A alignment);
  template <typename A>
  static constexpr zwords align_down(zwords bytes, A alignment);

  // Conversion
  static constexpr zbytes to_bytes(zwords words);

  static constexpr zwords from_bytes(size_t size_in_bytes);
};

#endif // SHARE_GC_Z_ZSIZE_HPP
