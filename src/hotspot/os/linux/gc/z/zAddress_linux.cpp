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

#include "gc/shared/gcLogPrecious.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zErrno.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"

#include <sys/mman.h>

// Maximum value where probing starts
static constexpr int MAXIMUM_MAX_HEAP_BASE_SHIFT = MIN2(ZAddressHeapBaseMaxShift, 47);
// Minimum value returned, if probing fail
static constexpr int MINIMUM_MAX_HEAP_BASE_SHIFT = ZAddressHeapBaseMinShift;

static int probe_valid_max_heap_base_shift() {
  const size_t page_size = os::vm_page_size();
  int max_heap_base_shift = 0;

  for (int i = MAXIMUM_MAX_HEAP_BASE_SHIFT; i > MINIMUM_MAX_HEAP_BASE_SHIFT; --i) {
    const uintptr_t base_addr = ((uintptr_t) 1U) << i;
    if (msync((void*)base_addr, page_size, MS_ASYNC) == 0) {
      // msync succeeded, the address is valid, and maybe even already mapped.
      max_heap_base_shift = i;
      break;
    }

    if (errno != ENOMEM) {
      ZErrno err;
      // Some error occurred. This should never happen, but msync
      // has some undefined behavior, hence ignore this bit.
      DEBUG_ONLY(fatal("Received '%s' while probing the address space for the highest valid bit", err.to_string());)
      log_warning_p(gc)("Received '%s' while probing the address space for the highest valid bit", err.to_string());
      continue;
    }

    // Since msync failed with ENOMEM, the page might not be mapped.
    // Try to map it, to see if the address is valid.
    void* const result_addr = mmap((void*) base_addr, page_size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (result_addr != MAP_FAILED) {
      munmap(result_addr, page_size);
    }

    if ((uintptr_t) result_addr == base_addr) {
      // Address is valid
      max_heap_base_shift = i;
      break;
    }
  }

  if (max_heap_base_shift == 0) {
    // Probing failed, allocate a very high page and take that bit as the maximum
    const uintptr_t high_addr = ((uintptr_t) 1U) << MAXIMUM_MAX_HEAP_BASE_SHIFT;
    void* const result_addr = mmap((void*) high_addr, page_size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);

    if (result_addr != MAP_FAILED) {
      max_heap_base_shift = BitsPerSize_t - (int)count_leading_zeros((size_t) result_addr) - 1;
      munmap(result_addr, page_size);
    }
  }

  log_debug_p(gc, init)("Probing address space for the highest valid bit: %d", max_heap_base_shift);

  return MAX2(max_heap_base_shift, MINIMUM_MAX_HEAP_BASE_SHIFT);
}

int ZGlobalsPointers::pd_max_heap_base_shift() {
  return probe_valid_max_heap_base_shift();
}
