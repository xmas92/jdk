/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_CLDOOPHANDLE_HPP
#define SHARE_OOPS_CLDOOPHANDLE_HPP

#include "oops/oopHandle.hpp"

// Simple classes for wrapping an OopHandle stored in the ClassLoaderData handles area.
// This class helps with NativeAccess loads and stores with the appropriate barriers.

class CLDOopHandle {
  friend class VMStructs;
private:
  OopHandle _handle;

  // Special accessor for CDS
  friend class Modules;
  OopHandle handle() const { return _handle; }

public:
  CLDOopHandle() : _handle(nullptr) {}
  explicit CLDOopHandle(oop* w) : _handle(w) {}

  CLDOopHandle(const CLDOopHandle& copy) : _handle(copy._handle) {}

  CLDOopHandle& operator=(const CLDOopHandle& copy) {
    _handle = copy._handle;
    return *this;
  }

  void swap(CLDOopHandle& copy) {
    _handle.swap(copy._handle);
  }

  inline oop resolve() const;
  inline oop peek() const;

  bool is_empty() const { return _handle.is_empty(); }

  inline void replace(oop obj);

  inline oop xchg(oop new_value);

  oop* ptr_raw() const { return _handle.ptr_raw(); }
};

#endif // SHARE_OOPS_CLDOOPHANDLE_HPP
