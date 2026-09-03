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

#ifndef SHARE_VM_OOPS_OOPUTILITY_INLINE_HPP
#define SHARE_VM_OOPS_OOPUTILITY_INLINE_HPP

#include "oops/oopUtility.hpp"

#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/layoutKind.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/valuePayload.inline.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

template <typename T>
inline T OopUtility::load_field(address field_addr) {
  const T* const addr = reinterpret_cast<T*>(field_addr);
  return RawAccess<>::load(addr);
}

inline oop OopUtility::load_oop_field(address field_addr) {
  if (UseCompressedOops) {
    return HeapAccess<>::oop_load(reinterpret_cast<narrowOop*>(field_addr));
  }

  return HeapAccess<>::oop_load(reinterpret_cast<oop*>(field_addr));
}

inline bool OopUtility::field_equals_operator(const fieldDescriptor& field,
                                              const ValuePayload& a,
                                              const ValuePayload& b) {
  precond(a.klass() == b.klass());

  const int field_offset_in_payload = field.offset() - a.klass()->payload_offset();
  const address a_field_addr = a.addr() + field_offset_in_payload;
  const address b_field_addr = b.addr() + field_offset_in_payload;

  if (field.is_flat()) {
    const InlineLayoutInfo layout_info = field.field_holder()->inline_layout_info(field.index());
    InlineKlass* const field_klass = layout_info.klass();
    const LayoutKind layout_kind = layout_info.kind();

    const ValuePayload a_field_payload = ValuePayload::construct_from_parts(a_field_addr, field_klass, layout_kind);
    const ValuePayload b_field_payload = ValuePayload::construct_from_parts(b_field_addr, field_klass, layout_kind);

    // Recursively check flattened field
    return is_substitutable_internal(a_field_payload, b_field_payload);
  }

  // Switch on the field signatures first character
  switch (field.signature()->char_at(0)) {
  case JVM_SIGNATURE_BYTE:
    return load_field<jbyte>(a_field_addr) == load_field<jbyte>(b_field_addr);
  case JVM_SIGNATURE_CHAR:
    return load_field<jchar>(a_field_addr) == load_field<jchar>(b_field_addr);
  case JVM_SIGNATURE_FLOAT:
    return load_field<juint>(a_field_addr) == load_field<juint>(b_field_addr);
  case JVM_SIGNATURE_DOUBLE:
    return load_field<julong>(a_field_addr) == load_field<julong>(b_field_addr);
  case JVM_SIGNATURE_INT:
    return load_field<jint>(a_field_addr) == load_field<jint>(b_field_addr);
  case JVM_SIGNATURE_LONG:
    return load_field<jlong>(a_field_addr) == load_field<jlong>(b_field_addr);
  case JVM_SIGNATURE_SHORT:
    return load_field<jshort>(a_field_addr) == load_field<jshort>(b_field_addr);
  case JVM_SIGNATURE_BOOLEAN:
    return load_field<jboolean>(a_field_addr) == load_field<jboolean>(b_field_addr);
  }

  // Field must be a reference field, perform a recursive java equals operator.
  assert(field.signature()->char_at(0) == JVM_SIGNATURE_ARRAY ||
         field.signature()->char_at(0) == JVM_SIGNATURE_CLASS,
         "Unexpected field signature encountered: %.*s",
         field.signature()->utf8_length(), field.signature()->base());

  oop a_field_obj = load_oop_field(a_field_addr);
  oop b_field_obj = load_oop_field(b_field_addr);

  return java_equals_operator(a_field_obj, b_field_obj);
}

inline bool OopUtility::is_substitutable_internal(const ValuePayload& a, const ValuePayload& b) {
  precond(a.klass() == b.klass());

  // [TODO]: Unclear if we still have buffered visible objects with an
  //         apparent null payload. We treat them as if they are non-null.
  const bool a_is_null = a.layout_kind() != LayoutKind::BUFFERED && a.is_payload_null();
  const bool b_is_null = b.layout_kind() != LayoutKind::BUFFERED && b.is_payload_null();

  if (a_is_null && b_is_null) {
    // Both payloads are null payloads
    return true;
  }

  if (a_is_null || b_is_null) {
    // One of the objects is null, never equal
    return false;
  }

  for (HierarchicalFieldStream<JavaFieldStream> field_stream(a.klass()); !field_stream.done(); field_stream.next()) {
    if (field_stream.access_flags().is_static()) {
      // Skip static fields
      continue;
    }

    if (!field_equals_operator(field_stream.field_descriptor(), a, b)) {
      // Field is not substitutable
      return false;
    }
  }

  // All fields are substitutable
  return true;
}

template <typename OopT>
inline bool OopUtility::java_equals_operator_t(OopT a, OopT b) {
  if (a == b) {
    // Same objects are always equal
    return true;
  }

  if (CompressedOops::is_null(a) || CompressedOops::is_null(b)) {
    // One of the objects is null, never equal
    return false;
  }

  oop a_obj = CompressedOops::decode(a);
  oop b_obj = CompressedOops::decode(b);

  if (a_obj->has_identity() || b_obj->has_identity()) {
    // Classes with identity must have reference equality
    return false;
  }

  // Neither has identity, so both must be value objects
  const BufferedValuePayload a_payload{(inlineOop)a_obj};
  const BufferedValuePayload b_payload{(inlineOop)b_obj};

  // Value class uses substitutable checks
  return is_substitutable(a_payload, b_payload);
}

inline bool OopUtility::java_equals_operator(oop a, oop b) {
  return java_equals_operator_t(a, b);
}

inline bool OopUtility::java_equals_operator(oop a, narrowOop b) {
  return java_equals_operator_t(a, CompressedOops::decode(b));
}

inline bool OopUtility::java_equals_operator(narrowOop a, oop b) {
  return java_equals_operator_t(CompressedOops::decode(a), b);
}

inline bool OopUtility::java_equals_operator(narrowOop a, narrowOop b) {
  return java_equals_operator_t(a, b);
}

inline bool OopUtility::is_substitutable(const ValuePayload& a, const ValuePayload& b) {
  if (a.klass() != b.klass()) {
    // Different klasses cannot be substitutable
    return false;
  }

  return is_substitutable_internal(a, b);
}

#endif // SHARE_VM_OOPS_OOPUTILITY_INLINE_HPP
