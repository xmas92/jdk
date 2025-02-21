/*
 * Copyright (c) 2019, 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZNUMA_INLINE_HPP
#define SHARE_GC_Z_ZNUMA_INLINE_HPP

#include "gc/z/zNUMA.hpp"

#include "gc/z/zGlobals.hpp"
#include "utilities/align.hpp"

inline bool ZNUMA::is_enabled() {
  return _enabled;
}

inline size_t ZNUMA::calculate_share(uint32_t numa_id, size_t total) {
  const uint32_t num_nodes = count();
  const size_t base_share = align_down(total / num_nodes, ZGranuleSize);

  const size_t extra_share_nodes = (total - base_share * num_nodes) / ZGranuleSize;
  if (numa_id < extra_share_nodes) {
    return base_share + ZGranuleSize;
  }

  return base_share;
}

template <typename Function>
inline void ZNUMA::divide_resource(size_t total, Function function) {
  for (uint32_t numa_id = 0; numa_id < count(); numa_id++) {
    function(numa_id, calculate_share(numa_id, total));
  }
}

#endif // SHARE_GC_Z_ZNUMA_INLINE_HPP
