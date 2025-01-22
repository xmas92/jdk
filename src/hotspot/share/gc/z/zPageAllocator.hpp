/*
 * Copyright (c) 2015, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZPAGEALLOCATOR_HPP
#define SHARE_GC_Z_ZPAGEALLOCATOR_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zAllocationFlags.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zGranuleMap.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zMappedCache.hpp"
#include "gc/z/zPage.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zPhysicalMemory.hpp"
#include "gc/z/zSafeDelete.hpp"
#include "gc/z/zValue.hpp"
#include "gc/z/zVirtualMemory.hpp"

class ThreadClosure;
class ZGeneration;
class ZNUMALocal;
class ZNUMALocalStats;
class ZPageAllocation;
class ZPageAllocator;
class ZPageAllocatorStats;
class ZUncommitter;
class ZUnmapper;
class ZWorkers;

class ZNUMALocal {
  friend class ZPageAllocator;

private:
  ZMappedCache               _cache;
  volatile size_t            _current_max_capacity;
  volatile size_t            _capacity;
  volatile size_t            _claimed;
  volatile size_t            _used;
  size_t                     _used_generations[2];
  struct {
    size_t                   _used_high;
    size_t                   _used_low;
  } _collection_stats[2];
  double                     _last_commit;
  double                     _last_uncommit;
  size_t                     _to_uncommit;

public:
  void initialize(size_t max_capacity);

  ZNUMALocalStats stats(ZGenerationId id) const;

  size_t available_memory() const;
  size_t soft_max_capacity() const;

  size_t increase_capacity(size_t size);
  void decrease_capacity(size_t size, bool set_max_capacity);

  void increase_used(size_t size);
  void decrease_used(size_t size);

  void increase_used_generation(ZGenerationId id, size_t size);
  void decrease_used_generation(ZGenerationId id, size_t size);

  void reset_statistics(ZGenerationId id);
};

class ZPageAllocator {
  friend class VMStructs;
  friend class ZUnmapper;
  friend class ZUncommitter;

private:
  mutable ZLock              _lock;

  ZVirtualMemoryManager      _virtual;
  ZPhysicalMemoryManager     _physical;
  ZGranuleMap<zoffset>       _physical_mappings;
  const size_t               _min_capacity;
  const size_t               _initial_capacity;
  const size_t               _max_capacity;
  ZPerNUMA<ZNUMALocal>       _states;
  ZList<ZPageAllocation>     _stalled;
  ZUnmapper*                 _unmapper;
  ZUncommitter*              _uncommitter;
  mutable ZSafeDelete<ZPage> _safe_destroy;
  bool                       _initialized;

  ZNUMALocal& state_from_vmem(const ZVirtualMemory& vmem);

  size_t count_segments_physical(const ZVirtualMemory& vmem);
  void copy_physical(const ZVirtualMemory& from, zoffset to);
  void free_physical(const ZVirtualMemory& vmem, int numa_id);
  bool commit_physical(ZVirtualMemory* vmem, int numa_id);
  void uncommit_physical(const ZVirtualMemory& vmem);

  void map_virtual_to_physical(const ZVirtualMemory& vmem);

  void unmap_virtual(const ZVirtualMemory& vmem);
  void free_virtual(const ZVirtualMemory& vmem);

  bool should_defragment(const ZVirtualMemory& vmem) const;
  ZVirtualMemory remap_mapping(const ZVirtualMemory& mapping, bool force_low_address);

  bool claim_mapped_or_increase_capacity(ZNUMALocal& state, size_t size, ZArray<ZVirtualMemory>* mappings);
  bool claim_physical(ZPageAllocation* allocation, ZNUMALocal& state);
  bool claim_physical_round_robin(ZPageAllocation* allocation);
  bool alloc_page_stall(ZPageAllocation* allocation);
  bool claim_physical_or_stall(ZPageAllocation* allocation);
  bool is_alloc_satisfied(ZPageAllocation* allocation) const;

  ZPage* alloc_page_inner(ZPageAllocation* allocation);
  void alloc_page_age_update(ZPage* page, size_t size, ZPageAge age, int numa_id);


  void harvest_claimed_physical(const ZVirtualMemory& new_vmem, ZPageAllocation* allocation);

  bool commit_and_map_memory(ZPageAllocation* allocation, const ZVirtualMemory& vmem, size_t committed_size);
  void free_memory_alloc_failed(ZPageAllocation* allocation);

  void satisfy_stalled();

  size_t uncommit(uint64_t* timeout);

  void notify_out_of_memory();
  void restart_gc() const;
  size_t calculate_soft_max_capacity(size_t current_max_capacity) const;

public:
  ZPageAllocator(size_t min_capacity,
                 size_t initial_capacity,
                 size_t soft_max_capacity,
                 size_t max_capacity);

  bool is_initialized() const;

  bool prime_cache(ZWorkers* workers, size_t size);

  size_t initial_capacity() const;
  size_t min_capacity() const;
  size_t max_capacity() const;
  size_t soft_max_capacity() const;
  size_t capacity() const;
  size_t used() const;
  size_t used_generation(ZGenerationId id) const;
  size_t unused() const;

  void promote_used(const ZVirtualMemory& from, const ZVirtualMemory& to);

  ZPageAllocatorStats stats(ZGeneration* generation) const;

  void reset_statistics(ZGenerationId id);

  ZPage* alloc_page(ZPageType type, size_t size, ZAllocationFlags flags, ZPageAge age);
  void safe_destroy_page(ZPage* page);
  void free_page(ZPage* page, bool allow_defragment);
  void free_pages(const ZArray<ZPage*>* pages);

  void enable_safe_destroy() const;
  void disable_safe_destroy() const;

  bool is_alloc_stalling() const;
  bool is_alloc_stalling_for_old() const;
  void handle_alloc_stalling_for_young();
  void handle_alloc_stalling_for_old(bool cleared_soft_refs);

  void threads_do(ThreadClosure* tc) const;
};

class ZNUMALocalStats {
  friend class ZPageAllocatorStats;

private:
  size_t _current_max_capacity;
  size_t _capacity;
  size_t _used;
  size_t _used_high;
  size_t _used_low;
  size_t _used_generation;

public:
  ZNUMALocalStats();

  ZNUMALocalStats(size_t current_max_capacity,
                  size_t capacity,
                  size_t used,
                  size_t used_high,
                  size_t used_low,
                  size_t used_generation);

  size_t current_max_capacity() const;

  void aggregate(const ZNUMALocalStats& stats);
};

class ZPageAllocatorStats {
private:
  size_t _min_capacity;
  size_t _max_capacity;
  size_t _soft_max_capacity;
  size_t _capacity;
  size_t _used;
  size_t _used_high;
  size_t _used_low;
  size_t _used_generation;
  size_t _freed;
  size_t _promoted;
  size_t _compacted;
  size_t _allocation_stalls;

public:
  ZPageAllocatorStats(size_t min_capacity,
                      size_t max_capacity,
                      size_t soft_max_capacity,
                      size_t freed,
                      size_t promoted,
                      size_t compacted,
                      size_t allocation_stalls,
                      const ZNUMALocalStats& local_stats);

  size_t min_capacity() const;
  size_t max_capacity() const;
  size_t soft_max_capacity() const;
  size_t capacity() const;
  size_t used() const;
  size_t used_high() const;
  size_t used_low() const;
  size_t used_generation() const;
  size_t freed() const;
  size_t promoted() const;
  size_t compacted() const;
  size_t allocation_stalls() const;
};

#endif // SHARE_GC_Z_ZPAGEALLOCATOR_HPP
