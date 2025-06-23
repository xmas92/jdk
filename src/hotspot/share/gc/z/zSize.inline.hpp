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

#ifndef SHARE_GC_Z_ZSIZE_INLINE_HPP
#define SHARE_GC_Z_ZSIZE_INLINE_HPP

#include "gc/z/zSize.hpp"

#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"

// Alignment

#define ZSIZE_FIRST_(a, ...) a
#define ZSIZE_SECOND_(a, b, ...) b

#define ZSIZE_FIRST(...) ZSIZE_FIRST_(__VA_ARGS__, )
#define ZSIZE_SECOND(...) ZSIZE_SECOND_(__VA_ARGS__, )

#define ZSIZE_EMPTY()

#define ZSIZE_EVAL1(...) __VA_ARGS__
#define ZSIZE_EVAL2(...) ZSIZE_EVAL1(ZSIZE_EVAL1(__VA_ARGS__))
#define ZSIZE_EVAL(...) ZSIZE_EVAL2(__VA_ARGS__)

#define ZSIZE_DEFER2(deferred_macro) deferred_macro ZSIZE_EMPTY ZSIZE_EMPTY()()

#define ZSIZE_IS_PROBE(...) ZSIZE_SECOND(__VA_ARGS__, 0)
#define ZSIZE_PROBE() ~, 1

#define ZSIZE_CAT(a, b) a##b

#define ZSIZE_NOT(x) ZSIZE_IS_PROBE(ZSIZE_CAT(ZSIZE_NOT_, x))
#define ZSIZE_NOT_0 ZSIZE_PROBE()

#define ZSIZE_BOOL(x) ZSIZE_NOT(ZSIZE_NOT(x))

#define ZSIZE_IF_ELSE(condition) ZSIZE_IF_ELSE_(ZSIZE_BOOL(condition))
#define ZSIZE_IF_ELSE_(condition) ZSIZE_CAT(ZSIZE_IF_, condition)

#define ZSIZE_IF_1(...) __VA_ARGS__ ZSIZE_IF_1_ELSE
#define ZSIZE_IF_0(...) ZSIZE_IF_0_ELSE

#define ZSIZE_IF_1_ELSE(...)
#define ZSIZE_IF_0_ELSE(...) __VA_ARGS__

#define ZSIZE_HAS_ARGS(...)                                                    \
  ZSIZE_BOOL(ZSIZE_FIRST(ZSIZE_END_OF_ARGUMENTS __VA_ARGS__)())
#define ZSIZE_END_OF_ARGUMENTS() 0

#define ZSIZE_MAP2(m, sep, first, ...)                                         \
  sep(m(first)) ZSIZE_IF_ELSE(ZSIZE_HAS_ARGS(__VA_ARGS__))(                    \
      ZSIZE_DEFER2(ZSIZE_MAP_)()(m, sep, __VA_ARGS__))()

#define ZSIZE_MAP1(m, sep, first, ...)                                         \
  m(first) ZSIZE_IF_ELSE(ZSIZE_HAS_ARGS(__VA_ARGS__))(                         \
      ZSIZE_DEFER2(ZSIZE_MAP_)()(m, sep, __VA_ARGS__))()
#define ZSIZE_MAP_() ZSIZE_MAP2

#define ZSIZE_MAP_SEP(f, sep, ...) ZSIZE_EVAL(ZSIZE_MAP1(f, sep, __VA_ARGS__))
#define ZSIZE_SEP(x) , x
#define ZSIZE_UNTYPE(x) untype(x)
#define ZSIZE_ZBYTES(x) zbytes x
#define ZSIZE_ZWORDS(x) zwords x

#define MAP_UNTYPE_COMMA_LIST(...)                                             \
  ZSIZE_MAP_SEP(ZSIZE_UNTYPE, ZSIZE_SEP, __VA_ARGS__)
#define MAP_ZBYTES_COMMA_LIST(...)                                             \
  ZSIZE_MAP_SEP(ZSIZE_ZBYTES, ZSIZE_SEP, __VA_ARGS__)
#define MAP_ZWORDS_COMMA_LIST(...)                                             \
  ZSIZE_MAP_SEP(ZSIZE_ZWORDS, ZSIZE_SEP, __VA_ARGS__)

#define ZBYTES_DISPATCH_TO_GLOBAL_SHARED(name, cexpr, ret, ...)             \
  inline cexpr ret ZBytes::name(MAP_ZBYTES_COMMA_LIST(__VA_ARGS__)) {      \
    return static_cast<ret>(::name(MAP_UNTYPE_COMMA_LIST(__VA_ARGS__)));       \
  }
#define ZBYTES_DISPATCH_TO_GLOBAL(name, ret, ...)                              \
  ZBYTES_DISPATCH_TO_GLOBAL_SHARED(name, , ret, __VA_ARGS__)
#define ZBYTES_DISPATCH_TO_GLOBAL_CONSTEXPR(name, ret, ...)                              \
  ZBYTES_DISPATCH_TO_GLOBAL_SHARED(name, constexpr , ret, __VA_ARGS__)

#define ZWORDS_DISPATCH_TO_GLOBAL_SHARED(name, cexpr, ret, ...)             \
  inline cexpr ret ZWords::name(MAP_ZWORDS_COMMA_LIST(__VA_ARGS__)) {      \
    return static_cast<ret>(::name(MAP_UNTYPE_COMMA_LIST(__VA_ARGS__)));       \
  }
#define ZWORDS_DISPATCH_TO_GLOBAL(name, ret, ...)                              \
  ZWORDS_DISPATCH_TO_GLOBAL_SHARED(name, , ret __VA_ARGS__)
#define ZWORDS_DISPATCH_TO_GLOBAL_CONSTEXPR(name, ret, ...)                              \
  ZWORDS_DISPATCH_TO_GLOBAL_SHARED(name, constexpr , ret, __VA_ARGS__)

ZBYTES_DISPATCH_TO_GLOBAL_CONSTEXPR(is_aligned, bool, bytes, alignment)

ZBYTES_DISPATCH_TO_GLOBAL_CONSTEXPR(align_up, zbytes, bytes, alignment)
ZBYTES_DISPATCH_TO_GLOBAL_CONSTEXPR(align_down, zbytes, bytes, alignment)

ZWORDS_DISPATCH_TO_GLOBAL_CONSTEXPR(is_aligned, bool, bytes, alignment)

ZWORDS_DISPATCH_TO_GLOBAL_CONSTEXPR(align_up, zwords, bytes, alignment)
ZWORDS_DISPATCH_TO_GLOBAL_CONSTEXPR(align_down, zwords, bytes, alignment)

// Power of two

ZBYTES_DISPATCH_TO_GLOBAL_CONSTEXPR(is_power_of_2, bool, bytes)

ZBYTES_DISPATCH_TO_GLOBAL(log2i_graceful, int, bytes)
ZBYTES_DISPATCH_TO_GLOBAL(log2i_exact, int, bytes)
ZBYTES_DISPATCH_TO_GLOBAL(log2i_ceil, int, bytes)

ZBYTES_DISPATCH_TO_GLOBAL(round_down_power_of_2, zbytes, bytes)

// Miscellaneous

ZBYTES_DISPATCH_TO_GLOBAL(percent_of, double, numerator, denominator)

#undef ZSIZE_FIRST_
#undef ZSIZE_SECOND_
#undef ZSIZE_FIRST
#undef ZSIZE_SECOND
#undef ZSIZE_EMPTY
#undef ZSIZE_EVAL1
#undef ZSIZE_EVAL2
#undef ZSIZE_EVAL
#undef ZSIZE_DEFER2
#undef ZSIZE_IS_PROBE
#undef ZSIZE_PROBE
#undef ZSIZE_CAT
#undef ZSIZE_NOT
#undef ZSIZE_NOT_0
#undef ZSIZE_BOOL
#undef ZSIZE_IF_ELSE
#undef ZSIZE_IF_ELSE_
#undef ZSIZE_IF_1
#undef ZSIZE_IF_0
#undef ZSIZE_IF_1_ELSE
#undef ZSIZE_IF_0_ELSE
#undef ZSIZE_HAS_ARGS
#undef ZSIZE_END_OF_ARGUMENTS
#undef ZSIZE_MAP2
#undef ZSIZE_MAP1
#undef ZSIZE_MAP_
#undef ZSIZE_MAP_SEP
#undef ZSIZE_SEP
#undef ZSIZE_UNTYPE
#undef ZSIZE_ZBYTES
#undef ZSIZE_ZWORDS
#undef MAP_UNTYPE_COMMA_LIST
#undef MAP_ZBYTES_COMMA_LIST
#undef MAP_ZWORDS_COMMA_LIST
#undef ZBYTES_DISPATCH_TO_GLOBAL_CONSTEXPR
#undef ZBYTES_DISPATCH_TO_GLOBAL
#undef ZBYTES_DISPATCH_TO_GLOBAL_SHARED
#undef ZWORDS_DISPATCH_TO_GLOBAL_CONSTEXPR
#undef ZWORDS_DISPATCH_TO_GLOBAL
#undef ZWORDS_DISPATCH_TO_GLOBAL_SHARED

// Conversion

inline constexpr zwords ZBytes::to_words(zbytes bytes) {
  const size_t value = untype(bytes);
  assert(::is_aligned(value, BytesPerWord), "Must be aligned: %zu", value);
  return to_zwords(value >> LogBytesPerWord);
}

inline constexpr zwords ZBytes::to_words_round_up(zbytes bytes) {
  return to_words(align_up(bytes, to_zbytes(BytesPerWord)));
}

inline constexpr zwords ZBytes::to_words_round_down(zbytes bytes) {
  return to_words(align_down(bytes, to_zbytes(BytesPerWord)));
}

inline constexpr zbytes ZBytes::from_words(size_t size_in_words) {
  const zwords words = to_zwords(size_in_words);
  return ZWords::to_bytes(words);
}

inline constexpr zbytes ZWords::to_bytes(zwords words) {
  const size_t value = untype(words);
  assert(value <= (value << LogBytesPerWord), "Value overflow: %zu", value);
  return to_zbytes(value << LogBytesPerWord);
}

inline constexpr zwords ZWords::from_bytes(size_t size_in_bytes) {
  const zbytes bytes = to_zbytes(size_in_bytes);
  return ZBytes::to_words(bytes);
}

#endif // SHARE_GC_Z_ZSIZE_INLINE_HPP
