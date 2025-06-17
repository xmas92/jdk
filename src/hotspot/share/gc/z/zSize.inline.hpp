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

template <typename T, ENABLE_IF(sizeof(T) == 1)>
constexpr T *operator+(T *const& a, const zbytes& bytes) {
  return a + untype(bytes);
}

template <typename T, ENABLE_IF(sizeof(T) == wordSize)>
constexpr T *operator+(T *const& a, const zwords& bytes) {
  return a + untype(bytes);
}

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

constexpr zbytes operator*(const zbytes& a, size_t b) {
  return to_zbytes(untype(a) * b);
}

constexpr zbytes operator*(const zbytes& a, double b) {
  return to_zbytes(size_t(double(untype(a)) * b));
}

constexpr zwords operator*(const zwords& a, const size_t& b) {
  return to_zwords(untype(a) * b);
}

constexpr zwords operator*(const zwords& a, const double& b) {
  return to_zwords(size_t(double(untype(a)) * b));
}

constexpr size_t operator/(const zbytes& a, const zbytes& b) {
  return untype(a) / untype(b);
}

constexpr zbytes operator/(const zbytes& a, const size_t& b) {
  return to_zbytes(untype(a) / b);
}

constexpr size_t operator/(const zwords& a, const zwords& b) {
  return untype(a) / untype(b);
}

constexpr zwords operator/(const zwords& a, const size_t& b) {
  return to_zwords(untype(a) / b);
}

constexpr zbytes operator%(const zbytes& a, const size_t& b) {
  return to_zbytes(untype(a) % b);
}

constexpr zwords operator%(const zwords& a, const size_t& b) {
  return to_zwords(untype(a) % b);
}

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
    a = (a op b);                                                              \
    return a;                                                                  \
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

// Alignment

template <typename A>
constexpr bool ZBytes::is_aligned(zbytes bytes, A alignment) {
  const size_t value = untype(bytes);
  return ::is_aligned(value, alignment);
}

template <typename A>
constexpr zbytes ZBytes::align_up(zbytes bytes, A alignment) {
  const size_t value = untype(bytes);
  return to_zbytes(::align_up(value, alignment));
}

template <typename A>
constexpr zbytes ZBytes::align_down(zbytes bytes, A alignment) {
  const size_t value = untype(bytes);
  return to_zbytes(::align_down(value, alignment));
}

template <typename A>
constexpr bool ZWords::is_aligned(zwords bytes, A alignment) {
  const size_t value = untype(bytes);
  return ::is_aligned(value, alignment);
}

template <typename A>
constexpr zwords ZWords::align_up(zwords bytes, A alignment) {
  const size_t value = untype(bytes);
  return to_zwords(::align_up(value, alignment));
}

template <typename A>
constexpr zwords ZWords::align_down(zwords bytes, A alignment) {
  const size_t value = untype(bytes);
  return to_zwords(::align_down(value, alignment));
}

// Conversion

constexpr zwords ZBytes::to_words(zbytes bytes) {
  const size_t value = untype(bytes);
  assert(::is_aligned(value, BytesPerWord), "Must be aligned: %zu", value);
  return to_zwords(value >> LogBytesPerWord);
}

constexpr zwords ZBytes::to_words_round_up(zbytes bytes) {
  return to_words(align_up(bytes, BytesPerWord));
}

constexpr zwords ZBytes::to_words_round_down(zbytes bytes) {
  return to_words(align_down(bytes, BytesPerWord));
}

constexpr zbytes ZBytes::from_words(size_t size_in_words) {
  const zwords words = to_zwords(size_in_words);
  return ZWords::to_bytes(words);
}

constexpr zbytes ZWords::to_bytes(zwords words) {
  const size_t value = untype(words);
  assert(value <= (value << LogBytesPerWord), "Value overflow: %zu", value);
  return to_zbytes(value << LogBytesPerWord);
}

constexpr zwords ZWords::from_bytes(size_t size_in_bytes) {
  const zbytes bytes = to_zbytes(size_in_bytes);
  return ZBytes::to_words(bytes);
}

#endif // SHARE_GC_Z_ZSIZE_INLINE_HPP
