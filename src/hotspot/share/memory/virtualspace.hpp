/*
 * Copyright (c) 1997, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_MEMORY_VIRTUALSPACE_HPP
#define SHARE_MEMORY_VIRTUALSPACE_HPP

#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

// ReservedSpace is a data structure for reserving a contiguous address range.

class ReservedSpaceView {
 protected:
  char*    _base;
  size_t   _size;
  size_t   _page_size;
  size_t   _alignment;
  bool     _special;
  bool     _executable;
  MEMFLAGS _flag;

  ReservedSpaceView() {}

private:
  ReservedSpaceView(char *base, size_t size, size_t alignment, size_t page_size,
                    bool special, bool executable, MEMFLAGS flag)
      : _base(base), _size(size), _page_size(page_size), _alignment(alignment),
        _special(special), _executable(executable), _flag(flag) {};

public:
  // Splitting
  // This splits the space into two spaces, the first part of which will be returned.
  ReservedSpaceView first_part(size_t partition_size, size_t alignment) const;
  ReservedSpaceView last_part (size_t partition_size, size_t alignment) const;
  ReservedSpaceView partition (size_t offset, size_t partition_size, size_t alignment) const;

  // These simply call the above using the default alignment.
  inline ReservedSpaceView first_part(size_t partition_size) const;
  inline ReservedSpaceView last_part (size_t partition_size) const;
  inline ReservedSpaceView partition (size_t offset, size_t partition_size) const;


public:
  // Accessors
  char*    base()        const { return _base;            }
  size_t   size()        const { return _size;            }
  char*    end()         const { return _base + _size;    }
  size_t   alignment()   const { return _alignment;       }
  size_t   page_size()   const { return _page_size;       }
  bool     special()     const { return _special;         }
  bool     executable()  const { return _executable;      }
  MEMFLAGS nmt_flag()    const { return _flag;            }
  bool     is_reserved() const { return _base != nullptr; }
};

ReservedSpaceView ReservedSpaceView::first_part(size_t partition_size) const
{
  return first_part(partition_size, alignment());
}

ReservedSpaceView ReservedSpaceView::last_part(size_t partition_size) const
{
  return last_part(partition_size, alignment());
}

ReservedSpaceView ReservedSpaceView::partition(size_t offset, size_t partition_size) const
{
  return partition(offset, partition_size, alignment());
}

class ReservedSpace : public ReservedSpaceView {
  friend class VMStructs;
protected:
  // ReservedSpace
  ReservedSpace(char* base, size_t size, size_t alignment,
                size_t page_size, bool special, bool executable, MEMFLAGS flag);

  // Helpers to clear and set members during initialization. These members
  // require special treatment:
  //  * _flag            - Used for NMT memory type. Once set in ctor,
  //                       it should not change after.
  //  * _alignment       - Not to be changed after initialization
  //  * _executable      - Not to be changed after initialization
  void clear_members();
  void initialize_members(char* base, size_t size, size_t alignment,
                          size_t page_size, bool special, bool executable);

  void initialize(size_t size, size_t alignment, size_t page_size,
                  char* requested_address, bool executable);

  void reserve(size_t size, size_t alignment, size_t page_size,
               char* requested_address, bool executable);
  void release_internal(char* base, size_t size);
 public:
  // Constructor
  ReservedSpace();
  // Initialize the reserved space with the given size. Depending on the size
  // a suitable page size and alignment will be used.
  ReservedSpace(size_t size, MEMFLAGS flag);
  // Initialize the reserved space with the given size. The preferred_page_size
  // is used as the minimum page size/alignment. This may waste some space if
  // the given size is not aligned to that value, as the reservation will be
  // aligned up to the final alignment in this case.
  ReservedSpace(size_t size, size_t preferred_page_size, MEMFLAGS flag);
  ReservedSpace(size_t size, size_t alignment, size_t page_size, MEMFLAGS flag,
                char* requested_address = nullptr);

  void release();

  // Alignment
  static size_t page_align_size_up(size_t size);
  static size_t page_align_size_down(size_t size);
  static size_t allocation_align_size_up(size_t size);
  bool contains(const void* p) const {
    return (base() <= ((char*)p)) && (((char*)p) < (base() + size()));
  }

  // Put a ReservedSpace over an existing range
  static ReservedSpace space_for_range(char* base, size_t size, size_t alignment,
                                       size_t page_size, bool special, bool executable, MEMFLAGS flag);
};

// Class encapsulating behavior specific of memory space reserved for Java heap.
class ReservedHeapSpace : private ReservedSpace {
  size_t   _noaccess_prefix;
  int      _fd_for_heap;
 private:
  void reserve(size_t size, size_t alignment, size_t page_size,
               char* requested_address, bool executable);
  void try_reserve_heap(size_t size, size_t alignment, size_t page_size,
                        char *requested_address);
  void try_reserve_range(char *highest_start, char *lowest_start,
                         size_t attach_point_alignment, char *aligned_HBMA,
                         char *upper_bound, size_t size, size_t alignment, size_t page_size);
  void initialize_compressed_heap(const size_t size, size_t alignment, size_t page_size);
  // Create protection page at the beginning of the space.
  void establish_noaccess_prefix();
 public:
  // Constructor. Tries to find a heap that is good for compressed oops.
  // heap_allocation_directory is the path to the backing memory for Java heap. When set, Java heap will be allocated
  // on the device which is managed by the file system where the directory resides.
  ReservedHeapSpace(size_t size, size_t forced_base_alignment, size_t page_size, const char* heap_allocation_directory = nullptr);
  // Returns the base to be used for compression, i.e. so that null can be
  // encoded safely and implicit null checks can work.
  char *compressed_oop_base() const { return _base - _noaccess_prefix; }
  MemRegion region() const;

  size_t noaccess_prefix() const { return _noaccess_prefix;   }
  void release();

  ReservedSpaceView& view() { return *this; }

  using ReservedSpace::base;
  using ReservedSpace::size;
  using ReservedSpace::end;
  using ReservedSpace::alignment;
  using ReservedSpace::page_size;
  using ReservedSpace::special;
  using ReservedSpace::executable;
  using ReservedSpace::is_reserved;
};

// Class encapsulating behavior specific memory space for Code
class ReservedCodeSpace : public ReservedSpace {
 public:
  // Constructor
  ReservedCodeSpace(size_t r_size, size_t rs_align, size_t page_size);
};

// VirtualSpace is data structure for committing a previously reserved address range in smaller chunks.

class VirtualSpace {
  friend class VMStructs;
 private:
  // Reserved area
  char* _low_boundary;
  char* _high_boundary;

  // Committed area
  char* _low;
  char* _high;

  // The entire space has been committed and pinned in memory, no
  // os::commit_memory() or os::uncommit_memory().
  bool _special;

  // Need to know if commit should be executable.
  bool   _executable;

  MEMFLAGS _flag;

  // MPSS Support
  // Each virtualspace region has a lower, middle, and upper region.
  // Each region has an end boundary and a high pointer which is the
  // high water mark for the last allocated byte.
  // The lower and upper unaligned to LargePageSizeInBytes uses default page.
  // size.  The middle region uses large page size.
  char* _lower_high;
  char* _middle_high;
  char* _upper_high;

  char* _lower_high_boundary;
  char* _middle_high_boundary;
  char* _upper_high_boundary;

  size_t _lower_alignment;
  size_t _middle_alignment;
  size_t _upper_alignment;

  // MPSS Accessors
  char* lower_high() const { return _lower_high; }
  char* middle_high() const { return _middle_high; }
  char* upper_high() const { return _upper_high; }

  char* lower_high_boundary() const { return _lower_high_boundary; }
  char* middle_high_boundary() const { return _middle_high_boundary; }
  char* upper_high_boundary() const { return _upper_high_boundary; }

  size_t lower_alignment() const { return _lower_alignment; }
  size_t middle_alignment() const { return _middle_alignment; }
  size_t upper_alignment() const { return _upper_alignment; }

 public:
  // Committed area
  char* low()  const { return _low; }
  char* high() const { return _high; }

  // Reserved area
  char* low_boundary()  const { return _low_boundary; }
  char* high_boundary() const { return _high_boundary; }

  bool special() const { return _special; }

 public:
  // Initialization
  VirtualSpace();
  bool initialize_with_granularity(const ReservedSpaceView& rs, size_t committed_byte_size, size_t max_commit_ganularity);
  bool initialize(const ReservedSpaceView& rs, size_t committed_byte_size);

  // Destruction
  ~VirtualSpace();

  // Reserved memory
  size_t reserved_size() const;
  // Actually committed OS memory
  size_t actual_committed_size() const;
  // Memory used/expanded in this virtual space
  size_t committed_size() const;
  // Memory left to use/expand in this virtual space
  size_t uncommitted_size() const;

  bool   contains(const void* p) const;

  // Operations
  // returns true on success, false otherwise
  bool expand_by(size_t bytes, bool pre_touch = false);
  void shrink_by(size_t bytes);
  void release();

  void check_for_contiguity() PRODUCT_RETURN;

  // Debugging
  void print_on(outputStream* out) const PRODUCT_RETURN;
  void print() const;
};

#endif // SHARE_MEMORY_VIRTUALSPACE_HPP
