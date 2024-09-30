/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZUNMAPPER_HPP
#define SHARE_GC_Z_ZUNMAPPER_HPP

#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zThread.hpp"
#include "gc/z/zVirtualMemory.hpp"

class ZPage;
class ZPageAllocator;

class ZUnmapperEntry : public CHeapObj<mtGC> {
  friend class ZList<ZUnmapperEntry>;

private:
  ZVirtualMemory _vmem;
  ZListNode<ZUnmapperEntry> _node;

public:
  ZUnmapperEntry(const ZVirtualMemory& vmem)
    : _vmem(vmem),
      _node() {}

  const ZVirtualMemory& vmem() {
    return _vmem;
  }

  size_t size() {
    return _vmem.size();
  }
};

class ZUnmapper : public ZThread {
private:
  ZPageAllocator* const _page_allocator;
  ZConditionLock        _lock;
  ZList<ZUnmapperEntry> _queue;
  size_t                _enqueued_bytes;
  bool                  _warned_sync_unmapping;
  bool                  _stop;

  ZUnmapperEntry* dequeue();
  bool try_enqueue(const ZVirtualMemory& vmem);
  size_t queue_capacity() const;
  bool is_saturated() const;
  void do_unmap(const ZVirtualMemory& vmem) const;

protected:
  virtual void run_thread();
  virtual void terminate();

public:
  ZUnmapper(ZPageAllocator* page_allocator);

  void unmap_virtual(const ZVirtualMemory& vmem);
};

#endif // SHARE_GC_Z_ZUNMAPPER_HPP
