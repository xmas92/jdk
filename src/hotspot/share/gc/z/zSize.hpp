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
#include "metaprogramming/enableIf.hpp"
#include "utilities/globalDefinitions.hpp"
#include <type_traits>

enum class zbytes : size_t {};
enum class zwords : size_t {};

class ZBytes : public AllStatic {
public:
  // Alignment
  static constexpr bool is_aligned(zbytes bytes, zbytes alignment);

  static constexpr zbytes align_up(zbytes bytes, zbytes alignment);
  static constexpr zbytes align_down(zbytes bytes, zbytes alignment);

  // Power of two
  static constexpr bool is_power_of_2(zbytes bytes);

  static int log2i_graceful(zbytes bytes);
  static int log2i_exact(zbytes bytes);
  static int log2i_ceil(zbytes bytes);

  static zbytes round_down_power_of_2(zbytes bytes);

  // Miscellaneous
  static double percent_of(zbytes numerator, zbytes denominator);

  // Conversion
  static constexpr zwords to_words(zbytes bytes);
  static constexpr zwords to_words_round_up(zbytes bytes);
  static constexpr zwords to_words_round_down(zbytes bytes);

  static constexpr zbytes from_words(size_t size_in_words);
};

class ZWords : public AllStatic {
public:
  // Alignment
  static constexpr bool is_aligned(zwords words, zwords alignment);
  static constexpr zwords align_up(zwords words, zwords alignment);
  static constexpr zwords align_down(zwords words, zwords alignment);

  // Conversion
  static constexpr zbytes to_bytes(zwords words);

  static constexpr zwords from_bytes(size_t size_in_bytes);
};

// Implementation provided in header file so it can be used in other headerfiles
// in constexpr contexts.

// Construction

constexpr zbytes to_zbytes(size_t byte_size) {
  return static_cast<zbytes>(byte_size);
}

constexpr zwords to_zwords(size_t word_size) {
  return static_cast<zwords>(word_size);
}

constexpr zbytes operator""_zb(unsigned long long int word_size) {
  return to_zbytes(word_size);
}

constexpr zwords operator""_zw(unsigned long long int word_size) {
  return to_zwords(word_size);
}

// Deconstruction

constexpr size_t untype(zbytes bytes) { return static_cast<size_t>(bytes); }

constexpr size_t untype(zwords words) { return static_cast<size_t>(words); }

// Arithmetic operators (ptr)

#define ZSIZE_POINTER_ARITH_OPERATOR(type, op, size)                           \
  template <typename T, ENABLE_IF(sizeof(T) == size)>                          \
  constexpr T *operator op(T *const &a, const type &b) {                       \
    return a op untype(b);                                                     \
  }

ZSIZE_POINTER_ARITH_OPERATOR(zbytes, +, 1)
ZSIZE_POINTER_ARITH_OPERATOR(zbytes, -, 1)

ZSIZE_POINTER_ARITH_OPERATOR(zwords, +, wordSize)
ZSIZE_POINTER_ARITH_OPERATOR(zwords, -, wordSize)

constexpr uintptr_t operator+(const uintptr_t& a, const zbytes& b) {
  return a + untype(b);
}

constexpr uintptr_t operator-(const uintptr_t& a, const zbytes& b) {
  return a - untype(b);
}

#undef ZSIZE_POINTER_ARITH_OPERATOR

#define ZSIZE_POINTER_ASSIGNMENT_OPERATOR(type, op, size)                      \
  template <typename T, ENABLE_IF(sizeof(T) == size)>                          \
  constexpr T *operator op##=(T *&a, const type & b) {                         \
    return a = (a op untype(b));                                               \
  }

ZSIZE_POINTER_ASSIGNMENT_OPERATOR(zbytes, +, 1)
ZSIZE_POINTER_ASSIGNMENT_OPERATOR(zbytes, -, 1)

ZSIZE_POINTER_ASSIGNMENT_OPERATOR(zwords, +, wordSize)
ZSIZE_POINTER_ASSIGNMENT_OPERATOR(zwords, -, wordSize)

#undef ZSIZE_POINTER_ASSIGNMENT_OPERATOR

// Arithmetic operators

#define ZSIZE_BINARY_ARITH_OPERATOR(type, op)                                  \
  constexpr type operator op(const type& a, const type& b) {                   \
    return to_##type(untype(a) op untype(b));                                  \
  }

ZSIZE_BINARY_ARITH_OPERATOR(zbytes, +)
ZSIZE_BINARY_ARITH_OPERATOR(zbytes, -)
ZSIZE_BINARY_ARITH_OPERATOR(zbytes, %)

ZSIZE_BINARY_ARITH_OPERATOR(zwords, +)
ZSIZE_BINARY_ARITH_OPERATOR(zwords, -)
ZSIZE_BINARY_ARITH_OPERATOR(zwords, %)

#undef ZSIZE_BINARY_ARITH_OPERATOR

#define ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(type, op)                         \
  template <typename T, ENABLE_IF(std::is_integral<T>::value)>                 \
  constexpr type operator op(const type& a, const T& b) {                      \
    return to_##type(untype(a) op b);                                          \
  }

#define ZSIZE_BINARY_FLOATING_ARITH_OPERATOR(type, op)                         \
  template <typename T, ENABLE_IF(std::is_floating_point<T>::value)>           \
  constexpr type operator op(const type& a, const T& b) {                      \
    return to_##type(size_t(double(untype(a)) op b));                          \
  }

#define ZSIZE_BINARY_COMMUTING_ARITH_OPERATOR(type, op)                        \
  template <typename T>                                                        \
  constexpr type operator op(const T& a, const type& b) {                      \
    return b op a;                                                             \
  }

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zbytes, *)
ZSIZE_BINARY_FLOATING_ARITH_OPERATOR(zbytes, *)
ZSIZE_BINARY_COMMUTING_ARITH_OPERATOR(zbytes, *)

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zwords, *)
ZSIZE_BINARY_FLOATING_ARITH_OPERATOR(zwords, *)
ZSIZE_BINARY_COMMUTING_ARITH_OPERATOR(zwords, *)

constexpr zwords operator*(const size_t& a, const zwords& b) {
  return b * a;
}

constexpr size_t operator/(const zbytes& a, const zbytes& b) {
  return untype(a) / untype(b);
}

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zbytes, /)
ZSIZE_BINARY_FLOATING_ARITH_OPERATOR(zbytes, /)

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zwords, /)
ZSIZE_BINARY_FLOATING_ARITH_OPERATOR(zwords, /)

constexpr size_t operator/(const zwords& a, const zwords& b) {
  return untype(a) / untype(b);
}

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zbytes, %)
ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zwords, %)

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zbytes, <<)
ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zbytes, >>)

ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zwords, <<)
ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR(zwords, >>)

#undef ZSIZE_BINARY_COMMUTING_ARITH_OPERATOR
#undef ZSIZE_BINARY_INTEGRAL_ARITH_OPERATOR

// Increment / Decrement operator

#define ZSIZE_INC_DEC_OPERATOR(type, op)                                       \
  constexpr type operator op##op(type& a) {                                    \
    a = a op to_##type(1);                                                     \
    return a;                                                                  \
  }                                                                            \
                                                                               \
  constexpr type operator op##op(type& a, int) {                               \
    const type b = a;                                                          \
    op##op a;                                                                  \
    return b;                                                                  \
  }

ZSIZE_INC_DEC_OPERATOR(zbytes, +);
ZSIZE_INC_DEC_OPERATOR(zbytes, -);

ZSIZE_INC_DEC_OPERATOR(zwords, +);
ZSIZE_INC_DEC_OPERATOR(zwords, -);

#undef ZSIZE_INC_DEC_OPERATOR

// Assignment operator

#define ZSIZE_ASSIGNMENT_OPERATOR(type, op)                                    \
  constexpr type operator op##=(type& a, const type& b) {                      \
    return a = (a op b);                                                       \
  }

ZSIZE_ASSIGNMENT_OPERATOR(zbytes, +);
ZSIZE_ASSIGNMENT_OPERATOR(zbytes, -);

ZSIZE_ASSIGNMENT_OPERATOR(zwords, +);
ZSIZE_ASSIGNMENT_OPERATOR(zwords, -);

#undef ZSIZE_ASSIGNMENT_OPERATOR

#define ZSIZE_ASSIGNMENT_OPERATOR(type, op)                                    \
  constexpr type operator op##=(type& a, const size_t& b) {                    \
    a = to_##type(untype(a) op b);                                             \
    return a;                                                                  \
  }

// ZSIZE_ASSIGNMENT_OPERATOR(zbytes, &)
// ZSIZE_ASSIGNMENT_OPERATOR(zbytes, |)
// ZSIZE_ASSIGNMENT_OPERATOR(zbytes, ^)

ZSIZE_ASSIGNMENT_OPERATOR(zbytes, >>)
ZSIZE_ASSIGNMENT_OPERATOR(zbytes, <<)

// ZSIZE_ASSIGNMENT_OPERATOR(zwords, &)
// ZSIZE_ASSIGNMENT_OPERATOR(zwords, |)
// ZSIZE_ASSIGNMENT_OPERATOR(zwords, ^)

ZSIZE_ASSIGNMENT_OPERATOR(zwords, >>)
ZSIZE_ASSIGNMENT_OPERATOR(zwords, <<)

#undef ZSIZE_ASSIGNMENT_OPERATOR

// Global Constants

constexpr zbytes K_zb = to_zbytes(K);
constexpr zbytes M_zb = to_zbytes(M);
constexpr zbytes G_zb = to_zbytes(G);
constexpr zbytes T_zb = to_zbytes(K * G);

constexpr zwords K_zw = to_zwords(K);
constexpr zwords M_zw = to_zwords(M);
constexpr zwords G_zw = to_zwords(G);
constexpr zwords T_zw = to_zwords(K * G);

constexpr zbytes MAX_zb = to_zbytes(SIZE_MAX);

// Global Definitions Print Macros

inline const char* proper_unit_for_byte_size(zbytes bytes) {
  return ::proper_unit_for_byte_size(untype(bytes));
}

inline size_t byte_size_in_proper_unit(zbytes bytes) {
  return ::byte_size_in_proper_unit(untype(bytes));
}

inline const char* exact_unit_for_byte_size(zbytes bytes) {
  return ::exact_unit_for_byte_size(untype(bytes));
}

inline size_t byte_size_in_exact_unit(zbytes bytes) {
  return ::byte_size_in_exact_unit(untype(bytes));
}

#endif // SHARE_GC_Z_ZSIZE_HPP
