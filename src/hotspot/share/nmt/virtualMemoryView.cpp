/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "precompiled.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/metaspaceStats.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "nmt/memTracker.hpp"
#include "nmt/threadStackTracker.hpp"
#include "nmt/virtualMemoryTracker.hpp"
#include "nmt/virtualMemoryView.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/threadCritical.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"

uint32_t VirtualMemoryView::PhysicalMemorySpace::unique_id = 0;
VirtualMemoryView::RegionStorage* VirtualMemoryView::_reserved_regions = nullptr;
GrowableArrayCHeap<const char*, mtNMT>* VirtualMemoryView::_names = nullptr;
GrowableArrayCHeap<VirtualMemoryView::OffsetRegionStorage, mtNMT>* VirtualMemoryView::_mapped_regions = nullptr;
GrowableArrayCHeap<VirtualMemoryView::RegionStorage, mtNMT>* VirtualMemoryView::_committed_regions = nullptr;
NativeCallStackStorage<VirtualMemoryView::IndexIterator>* VirtualMemoryView::_stack_storage = nullptr;
bool VirtualMemoryView::_is_detailed_mode = false;


void VirtualMemoryView::report(outputStream* output, size_t scale, bool skip_stack) {
  ResourceMark rm;

  const auto print_reserved_memory = [&](TrackedRange& reserved_range) {
    const NativeCallStack& stack = _stack_storage->get(reserved_range.stack_idx);
    const char* scale_name = NMTUtil::scale_name(scale);
    output->print("+ [" PTR_FORMAT " - " PTR_FORMAT "]" " reserved " SIZE_FORMAT "%s for %s",
                  p2i(reserved_range.start), p2i(reserved_range.end()),
                  NMTUtil::amount_in_scale(reserved_range.size, scale), scale_name,
                  NMTUtil::flag_to_name(reserved_range.flag));
    if (skip_stack || stack.is_empty()) {
      output->print_cr("");
    } else {
      output->print_cr(" from");
      stack.print_on(output, "|", 3);
    }
  };
  const auto print_mapped_virtual_memory_region = [&](TrackedOffsetRange& mapped_range) -> void {
    const NativeCallStack& stack = _stack_storage->get(mapped_range.stack_idx);
    const char* scale_name = NMTUtil::scale_name(scale);
    output->print("+-+-- [" PTR_FORMAT " - " PTR_FORMAT "]" " of size " SIZE_FORMAT "%s",
                  p2i(mapped_range.start), p2i(mapped_range.end()),
                  NMTUtil::amount_in_scale(mapped_range.size, scale), scale_name);
    if (mapped_range.start != mapped_range.physical_address) {
      output->print(" mapped to [" PTR_FORMAT " - " PTR_FORMAT "]", p2i(mapped_range.physical_address), p2i(mapped_range.physical_end()));
    } else {
      // Do nothing
    }
    if (skip_stack || stack.is_empty()) {
      output->print_cr(" ");
    } else {
      output->print_cr(" from");
      stack.print_on(output, "| |", 7);
    }
  };
  const auto print_committed_memory = [&](TrackedRange& committed_range, Range mapped_committed) {
    const NativeCallStack& stack = _stack_storage->get(committed_range.stack_idx);
    const char* scale_name = NMTUtil::scale_name(scale);
    output->print("| +---- [" PTR_FORMAT " - " PTR_FORMAT "]" " committed " SIZE_FORMAT "%s",
                  p2i(mapped_committed.start), p2i(mapped_committed.end()),
                  NMTUtil::amount_in_scale(mapped_committed.size, scale), scale_name);
    if (skip_stack || stack.is_empty()) {
      output->print_cr(" ");
    } else {
      output->print_cr(" from");
      stack.print_on(output, "| |", 9);
    }
  };
  for (Id space_id = 0; space_id < PhysicalMemorySpace::unique_id; space_id++) {
    RegionStorage& reserved_ranges = *_reserved_regions;
    OffsetRegionStorage& mapped_ranges = _mapped_regions->at(space_id);
    RegionStorage& committed_ranges = _committed_regions->at(space_id);
    GrowableArray<Range>* mapped_committed_per_range = NEW_RESOURCE_ARRAY(GrowableArray<Range>, committed_ranges.length());
    for (int i = 0; i < committed_ranges.length(); i++) {
      ::new(&mapped_committed_per_range[i]) GrowableArray<Range>();
    }

    VirtualMemoryView::sort_regions(mapped_ranges);
    VirtualMemoryView::merge_mapped(mapped_ranges);

    auto range_sort_fn = [](Range* r1, Range* r2) {
      return r1->start < r2->start ? -1 : 1;
    };

    struct MergedMapped {
      Range virt;
      GrowableArray<Range> phys;
    };

    GrowableArray<MergedMapped> merged_maps;
    for (int m = 0; m < mapped_ranges.length(); m++) {
      TrackedOffsetRange& mapped_range = mapped_ranges.at(m);

      // Merge into maps
      if (merged_maps.is_empty() || disjoint(mapped_range, merged_maps.at(merged_maps.length() - 1).virt)) {
        merged_maps.push({mapped_range, {}});
      } else {
        Range& last_virt = merged_maps.at(merged_maps.length() - 1).virt;
        last_virt = union_of(last_virt, mapped_range);
      }
      merged_maps.at(merged_maps.length() - 1).phys.push({mapped_range.physical_address, mapped_range.size});

      // Setup mapped_committed_per_range
      for (int c = 0; c < committed_ranges.length(); c++) {
        TrackedRange& committed_range = committed_ranges.at(c);
        const Range commited(committed_range.start, committed_range.size);
        const Range mapped_to(mapped_range.physical_address, mapped_range.size);
        if (overlaps(commited, mapped_to)) {
          const Range mapped_committed = overlap_of(commited, mapped_to);
          mapped_committed_per_range[c].push(mapped_committed);
        }
      }
    }

    GrowableArray<Range> reusable_growable_array;

    // Merge physical merged_maps
    for (int i = 0; i < merged_maps.length(); i++) {
      MergedMapped& merged_map = merged_maps.at(i);
      merged_map.phys.sort(range_sort_fn);
      GrowableArray<Range>& merged_phys = reusable_growable_array;
      merged_phys.clear();

      for (int j = 0; j < merged_map.phys.length(); j++) {
        Range phys_range = merged_map.phys.at(j);
        if (merged_phys.is_empty() || disjoint(merged_phys.last(), phys_range)) {
          merged_phys.push(phys_range);
        } else {
          Range& last_phys = merged_phys.at(merged_phys.length() - 1);
          last_phys = union_of(last_phys, phys_range);
        }
      }

      merged_map.phys.swap(&merged_phys);
    }


    output->print_cr("%s:", _names->at(space_id));
    for (int reserved_range_idx = 0; reserved_range_idx < reserved_ranges.length(); reserved_range_idx++) {
      TrackedRange& reserved_range = reserved_ranges.at(reserved_range_idx);
      print_reserved_memory(reserved_range);
      if (UseNewCode) {
        // Version 2: Prints the merged mappings, maybe multi mapped.
        //            Information is invalid if mapped range is not a sub range of reserved.
        for (int i = 0; i < merged_maps.length(); i++) {
          const MergedMapped& merged_map = merged_maps.at(i);
          const Range mapped_range = merged_map.virt;
          if (overlaps(reserved_range, mapped_range)) {
            const char* scale_name = NMTUtil::scale_name(scale);
            output->print_cr("+-+- [" PTR_FORMAT " - " PTR_FORMAT "]" " of size " SIZE_FORMAT "%s",
                          p2i(mapped_range.start), p2i(mapped_range.end()),
                          NMTUtil::amount_in_scale(mapped_range.size, scale), scale_name);
            for (int j = 0; j < merged_map.phys.length(); j++) {
              const Range mapped_to = merged_map.phys.at(j);
              output->print("| +-+- [" PTR_FORMAT " - " PTR_FORMAT "]" " of size " SIZE_FORMAT "%s",
                               p2i(mapped_to.start), p2i(mapped_to.end()),
                               NMTUtil::amount_in_scale(mapped_to.size, scale), scale_name);
              bool first_commit = true;
              for (int c = 0; c < committed_ranges.length(); c++) {
                TrackedRange& committed_range = committed_ranges.at(c);
                const Range commited(committed_range.start, committed_range.size);
                if (overlaps(commited, mapped_to)) {
                  const Range mapped_committed = overlap_of(commited, mapped_to);
                  if (first_commit) {
                    first_commit = false;
                    if (is_same(mapped_committed, mapped_to)) {
                      output->print_cr(" mapped and commited");
                      break;
                    } else {
                      output->print_cr(" mapped to");
                    }
                  }
                  const Range mapped_not_commited_pre(mapped_to.start, pointer_delta(mapped_to.start, mapped_to.start, 1));
                  const Range mapped_not_commited_post(mapped_committed.end(), pointer_delta(mapped_to.end(), mapped_committed.end(), 1));
                  if (mapped_not_commited_pre.size > 0) {
                    output->print_cr("| | +--- [" PTR_FORMAT " - " PTR_FORMAT "]" " not committed " SIZE_FORMAT "%s",
                                  p2i(mapped_not_commited_pre.start), p2i(mapped_not_commited_pre.end()),
                                  NMTUtil::amount_in_scale(mapped_not_commited_pre.size, scale), scale_name);
                  }
                  output->print_cr("| | +--- [" PTR_FORMAT " - " PTR_FORMAT "]" " committed " SIZE_FORMAT "%s",
                                p2i(mapped_committed.start), p2i(mapped_committed.end()),
                                NMTUtil::amount_in_scale(mapped_committed.size, scale), scale_name);
                  if (mapped_not_commited_post.size > 0) {
                    output->print_cr("| | +--- [" PTR_FORMAT " - " PTR_FORMAT "]" " not committed " SIZE_FORMAT "%s",
                                  p2i(mapped_not_commited_post.start), p2i(mapped_not_commited_post.end()),
                                  NMTUtil::amount_in_scale(mapped_not_commited_post.size, scale), scale_name);
                  }
                }
              }
              if (first_commit) {
                output->print_cr(" mapped and not commited");
              }
            }
            output->print_cr("|");
          }
        }
      } else {
        // Version 1: Prints the true mappings.
        for (int mapped_range_idx = 0; mapped_range_idx < mapped_ranges.length(); mapped_range_idx++) {
          TrackedOffsetRange& mapped_range = mapped_ranges.at(mapped_range_idx);
          if (overlaps(reserved_range, mapped_range)) {
            print_mapped_virtual_memory_region(mapped_range);
            for (int committed_range_idx = 0; committed_range_idx < committed_ranges.length();
                committed_range_idx++) {
              TrackedRange& committed_range = committed_ranges.at(committed_range_idx);
              const Range commited(committed_range.start, committed_range.size);
              const Range mapped_to(mapped_range.physical_address, mapped_range.size);
              if (overlaps(commited, mapped_to)) {
                const Range mapped_committed = overlap_of(commited, mapped_to);
                print_committed_memory(committed_range, mapped_committed);
              }
            }
            output->print_cr("|");
          }
        }
      }
      output->print_cr("");
    }

    for (int i = 0; i < committed_ranges.length(); i++) {
      TrackedRange& committed_range = committed_ranges.at(i);
      GrowableArray<Range>& mapped_committed_ranges = mapped_committed_per_range[i];
      mapped_committed_ranges.sort(range_sort_fn);
      GrowableArray<Range>& multi_mapped_ranges = reusable_growable_array;
      multi_mapped_ranges.clear();
      for (int j = 0; j < mapped_committed_ranges.length(); j++) {
        for (int k = j + 1; k < mapped_committed_ranges.length(); k++) {
          if (overlaps(mapped_committed_ranges.at(j), mapped_committed_ranges.at(k))) {
            const Range multi_mapped_range = overlap_of(mapped_committed_ranges.at(j), mapped_committed_ranges.at(k));
            if (multi_mapped_ranges.is_empty() || !overlaps(multi_mapped_ranges.last(), multi_mapped_range)) {
              multi_mapped_ranges.push(multi_mapped_range);
            } else {
              multi_mapped_ranges.push(union_of(multi_mapped_ranges.pop(), multi_mapped_range));
            }
          } else {
            break;
          }
        }
        multi_mapped_ranges.sort(range_sort_fn);
      }

      if (multi_mapped_ranges.is_nonempty()) {
        output->print_cr("+-+-- MULTI-MAPPED in [" PTR_FORMAT " - " PTR_FORMAT "]", p2i(committed_range.start), p2i(committed_range.end()));
      }

      for (int j = 0; j < multi_mapped_ranges.length(); j++) {
         print_committed_memory(committed_range, multi_mapped_ranges.at(j));
      }

      GrowableArray<Range>& merged_mapped_committed_ranges = reusable_growable_array;
      merged_mapped_committed_ranges.clear();

      for (int j = 0; j < mapped_committed_ranges.length(); j++) {
        const Range mapped_range = mapped_committed_ranges.at(j);
        if (merged_mapped_committed_ranges.is_empty() || disjoint(merged_mapped_committed_ranges.last(), mapped_range)) {
          merged_mapped_committed_ranges.push(mapped_range);
        } else {
          Range& last = merged_mapped_committed_ranges.at(merged_mapped_committed_ranges.length() - 1);
          last = union_of(last, mapped_range);
        }
      }

      bool printed_header = false;
      const Range commited(committed_range.start, committed_range.size);
      for (int j = 0; j <= merged_mapped_committed_ranges.length(); j++) {
        const address start = j == 0 ? committed_range.start : merged_mapped_committed_ranges.at(j - 1).end();
        const size_t size = j == merged_mapped_committed_ranges.length()
                          ? pointer_delta(commited.end() , start, 1)
                          : merged_mapped_committed_ranges.at(j).start < start
                            ? 0
                            : pointer_delta(merged_mapped_committed_ranges.at(j).start, start, 1);
        const Range unmapped_range(start, size);
        if (overlaps(unmapped_range, commited)) {
          if (!printed_header) {
            output->print_cr("+-+-- UNMAPPED in [" PTR_FORMAT " - " PTR_FORMAT "]", p2i(committed_range.start), p2i(committed_range.end()));
            printed_header = true;
          }
         print_committed_memory(committed_range, unmapped_range);
        }
      }
    }
    output->print_cr("");
  }
}

void VirtualMemoryView::unregister_memory(RegionStorage& storage, address base_addr, size_t size) {
  Range range_to_remove{base_addr, size};
  TrackedOffsetRange out[2];
  int len;
  int orig_len = storage.length();
  for (int i = 0; i < orig_len; i++) {
    TrackedRange& crng = storage.at(i);
    OverlappingResult o_r = overlap_of(TrackedOffsetRange{crng}, range_to_remove, out, &len);
    if (o_r == OverlappingResult::NoOverlap) {
      continue;
    }
    int stack_idx = crng.stack_idx;
    // Delete old region
    storage.delete_at(i);
    // delete_at replaces the ith elt with the last one, so we need to rewind
    // otherwise we'll skip the previously last element.
    i--;
    // We have 1 fewer element to look at
    orig_len--;
    // And push on the new ones.
    for (int j = 0; j < len; j++) {
      storage.push(out[j]);
      _stack_storage->increment(stack_idx);
    }
    _stack_storage->decrement(stack_idx);
    // We're not breaking, no guarantee that there's exactly 1 region that matches
  }

  sort_regions(storage);
  merge_committed(storage);
}

void VirtualMemoryView::release_memory(address base_addr, size_t size) {
  unregister_memory(*_reserved_regions, base_addr, size);
}

void VirtualMemoryView::uncommit_memory_into_space(const PhysicalMemorySpace& space,
                                                         address offset, size_t size) {
  RegionStorage& committed_ranges = _committed_regions->at(space.id);
  unregister_memory(committed_ranges, offset, size);

}

void VirtualMemoryView::register_memory(RegionStorage& storage, address base_addr, size_t size, MEMFLAGS flag, const NativeCallStack& stack) {
  // Small optimization: Is the next commit overlapping with the last one? Then we don't need to push.
  // TODO: Merge more, may be adjacent or overlap multiple TrackedRanges. Removed for now to make the
  //       printing logic easier. Not sure if the sorted and disjoint properties should be invariant.
  //       But it is very nice if they are. If they are supposed to be invariant this is a bug.
  // if (storage.length() > 0) {
  //   TrackedRange& range = storage.at(storage.length() - 1);
  //   if ((overlaps(range, Range{base_addr, size})
  //        || adjacent(range, Range{base_addr, size}))
  //       && _stack_storage->get(range.stack_idx).equals(stack)
  //       && range.flag == flag) {
  //     range.start = MIN2(base_addr, range.start);
  //     range.size = MAX2(base_addr+size, range.end()) - range.start;
  //     return;
  //   }
  // }
  int idx = _stack_storage->push(stack);
  storage.push(TrackedRange{base_addr, size, idx, flag});

  sort_regions(storage);
  merge_committed(storage);
}

void VirtualMemoryView::reserve_memory(address base_addr, size_t size, MEMFLAGS flag, const NativeCallStack& stack) {
  register_memory(*_reserved_regions, base_addr, size, flag, stack);
}

void VirtualMemoryView::commit_memory_into_space(const PhysicalMemorySpace& space,
                                                       address offset, size_t size,
                                                       const NativeCallStack& stack) {
  RegionStorage& crngs = _committed_regions->at(space.id);
  register_memory(crngs, offset, size, mtNone, stack);
}

void VirtualMemoryView::remove_view_into_space(const PhysicalMemorySpace& space,
                                                     address base_addr, size_t size) {
  Range range_to_remove{base_addr, size};
  OffsetRegionStorage& range_array = _mapped_regions->at(space.id);
  TrackedOffsetRange out[2];
  int len;
  for (int i = 0; i < range_array.length(); i++) {
    TrackedOffsetRange& range = range_array.at(i);
    bool has_overlap =
        OverlappingResult::NoOverlap != overlap_of(range, range_to_remove, out, &len);
    if (has_overlap) {
      int stack_idx = range.stack_idx;
      // Delete old region.
      // TODO: Bug skips an entry. Or just start at range_array.length() - 1 and decrement.
      //       As the pushed values should never overlap again.
      range_array.delete_at(i--);
      // Replace with the remaining ones
      for (int j = 0; j < len; j++) {
        _stack_storage->increment(stack_idx);
        range_array.push(out[j]);
      }
      // Decrement after incrementing to avoid triggering resizing
      _stack_storage->decrement(stack_idx);
    }
  }
}

// TODO: Maybe sorting the regions makes this easier to understand?
void VirtualMemoryView::add_view_into_space(const PhysicalMemorySpace& space,
                                                  address base_addr, size_t size, address offset,
                                                  MEMFLAGS flag, const NativeCallStack& stack) {
  // This method is a bit tricky because we need to care about preserving the offsets of any already existing view
  // that overlaps with the view being added.
  int stack_idx = _stack_storage->push(stack);
  OffsetRegionStorage& rngs = _mapped_regions->at(space.id);
  // We need to find overlapping regions and split on them, because the offsets may differ.
  for (int i = 0; i < rngs.length(); i++) {
    TrackedOffsetRange& rng = rngs.at(i);
    TrackedOffsetRange out[2];
    int len;
    OverlappingResult res = overlap_of(rng, Range{base_addr, size}, out, &len);
    if (res == OverlappingResult::NoOverlap) {
      // Do nothing
    } else if (res == OverlappingResult::EntirelyEnclosed
               || res == OverlappingResult::SplitInMiddle) {
      // We replace it.
      rngs.at_put(i, TrackedOffsetRange{base_addr, size, offset, stack_idx, flag});
      // And put the out with the physical offsets of the original
      for (int i = 0; i < len; i++) {
        rngs.push(out[i]);
      }
      // There can be no more, so we're done
      return;
    } else if (res == OverlappingResult::ShortenedFromLeft || res == OverlappingResult::ShortenedFromRight) {
      assert(len == 1, "must be");
      // We replace the old one with the shortened one
      rngs.at_put(i, out[0]);
      // But we can't quit, just gotta continue iterating
    }
  }
  // If we reach this then either there has only been ShortenedFromRight/ShortenedFromLeft
  // or no overlap. Then we must add the original region
  rngs.push(TrackedOffsetRange{base_addr, size, offset, stack_idx, flag});
  // And now we're done.
}

VirtualMemoryView::PhysicalMemorySpace VirtualMemoryView::register_space(const char* descriptive_name) {
  const PhysicalMemorySpace next_space = PhysicalMemorySpace{PhysicalMemorySpace::next_unique()};
  // These are allocated just to be copied for at_put_grow.
  OffsetRegionStorage to_copy_res{};
  RegionStorage to_copy_comm{};
  _mapped_regions->at_put_grow(next_space.id, to_copy_res);
  _committed_regions->at_put_grow(next_space.id, to_copy_comm);
  _names->at_put_grow(next_space.id, descriptive_name, "");
  return next_space;
}

void VirtualMemoryView::initialize(bool is_detailed_mode) {
  _reserved_regions = new RegionStorage{};
  _mapped_regions = new GrowableArrayCHeap<OffsetRegionStorage, mtNMT>{5};
  _committed_regions = new GrowableArrayCHeap<RegionStorage, mtNMT>{5};
  _stack_storage = new NativeCallStackStorage<IndexIterator>{is_detailed_mode ? NativeCallStackStorage<IndexIterator>::static_stack_size : 1, is_detailed_mode};
  _names = new GrowableArrayCHeap<const char*, mtNMT>{5};
  _is_detailed_mode = is_detailed_mode;
}

void VirtualMemoryView::merge_committed(RegionStorage& ranges) {
  // We displace into the array at rlen+j instead of
  // creating a new array and swapping it out at the end.
  // This is because of a limitation with GrowableArray
  int rlen = ranges.length();
  if (rlen <= 1) return;
  int j = 0;
  ranges.push(ranges.at(j));
  for (int i = 1; i < rlen; i++) {
    TrackedRange& merging_range = ranges.at(rlen+j);
    // Take an explicit copy, this is necessary because
    // the push might invalidate the reference and then SIGSEGV.
    const TrackedRange potential_range = ranges.at(i);
    if (merging_range.end() >=
            potential_range.start // There's overlap, known because of pre-condition
        // Ignoring the stack traces for now.
        // && _stack_storage->get(merging_range.stack_idx)
        //        .equals(_stack_storage->get(potential_range.stack_idx))
               ) {
      // Merge it
      merging_range.size = potential_range.end() - merging_range.start;
    } else {
      j++;
      ranges.push(potential_range);
    }
  }
  // Remove all the old elements, only keeping the merged ones.
  ranges.remove_till(rlen);
  return;
}

void VirtualMemoryView::merge_mapped(OffsetRegionStorage& ranges) {
  // TODO: Bug?, does not account for the FLAG. Should different flags (mismatch) crash?
  //       Maybe the flags are already guaranteed to be the same.

  // We displace into the array at rlen+j instead of
  // creating a new array and swapping it out at the end.
  // This is because of a limitation with GrowableArray
  int rlen = ranges.length();
  if (rlen <= 1) return;
  int j = 0;
  ranges.push(ranges.at(j));
  for (int i = 1; i < rlen; i++) {
    TrackedOffsetRange& merging_range = ranges.at(rlen+j);
    // Take an explicit copy, this is necessary because
    // the push might invalidate the reference and then SIGSEGV.
    const TrackedOffsetRange potential_range = ranges.at(i);
    if (merging_range.end() >=
        potential_range.start // There's overlap, known because of pre-condition
        // Ignoring the stack traces for now.
        // && _stack_storage->get(merging_range.stack_idx)
        // .equals(_stack_storage->get(potential_range.stack_idx))
        // TODO: Bug, the merge may mess up the physical mappings. Just because the
        //       virtual range overlap does not mean that the physical does. Only
        //       merge if equal for now. Can be made improved.
        && merging_range.physical_end() ==
        potential_range.physical_address) {
      // Merge it
      merging_range.size = potential_range.end() - merging_range.start;
    } else {
      j++;
      ranges.push(potential_range);
    }
  }
  // Remove all the old elements, only keeping the merged ones.
  ranges.remove_till(rlen);
  return;
}

void VirtualMemoryView::sort_regions(GrowableArrayCHeap<VirtualMemoryView::Range, mtNMT>& storage) {
  storage.sort([](Range* a, Range* b) -> int {
    return (a->start > b->start) - (a->start < b->start);
  });
}
void VirtualMemoryView::sort_regions(RegionStorage& storage) {
  storage.sort([](TrackedRange* a, TrackedRange* b) -> int {
    return (a->start > b->start) - (a->start < b->start);
  });
}

void VirtualMemoryView::sort_regions(OffsetRegionStorage& storage) {
  storage.sort([](TrackedOffsetRange* a, TrackedOffsetRange* b) -> int {
    return (a->start > b->start) - (a->start < b->start);
  });
}

bool VirtualMemoryView::overlaps(Range a, Range b) {
  return MAX2(b.start, a.start) < MIN2(b.end(), a.end());
}

bool VirtualMemoryView::disjoint(Range a, Range b) {
  return !(overlaps(a, b) || adjacent(a, b));
}

bool VirtualMemoryView::is_same(Range a, Range b) {
  return a.start == b.start && a.size == b.size;
}

VirtualMemoryView::Range VirtualMemoryView::overlap_of(Range a, Range b) {
  if (!overlaps(a, b)) {
    return {};
  }
  const address start = MAX2(b.start, a.start);
  const address end = MIN2(b.end(), a.end());
  return Range(start , pointer_delta(end, start, 1));
}

VirtualMemoryView::Range VirtualMemoryView::union_of(Range a, Range b) {
  precond(!disjoint(a, b));
  const address start = MIN2(b.start, a.start);
  const address end = MAX2(b.end(), a.end());
  return Range(start , pointer_delta(end, start, 1));
}

bool VirtualMemoryView::adjacent(Range a, Range b) {
  return (a.start == b.end() || b.start == a.end());
}

VirtualMemoryView::OverlappingResult
VirtualMemoryView::overlap_of(TrackedOffsetRange to_split, Range to_remove,
                                    TrackedOffsetRange* out, int* len) {
  const auto a = to_split.start;
  const auto b = to_split.end();
  const auto c = to_remove.start;
  const auto d = to_remove.end();
  /*
      to_split enclosed entirely by to_remove -- nothing is left
      Also handles the case where they are exactly the same, still has the same result.
         a  b
       | |  | | => None.
       c      d
     */
  if (a >= c && b <= d) {
    *len = 0;
    return OverlappingResult::EntirelyEnclosed;
  }
  // to_remove enclosed entirely by to_split -- we end up with two ranges and a hole in the middle
  /*
       a      b    a c   d b
       | |  | | => | | , | |
         c  d
     */
  if (c > a && d < b) {
    *len = 2;
    address left_start = a;
    size_t left_size = static_cast<size_t>(c - a);
    address left_offset = to_split.physical_address;
    out[0] = TrackedOffsetRange{left_start, left_size, to_split.physical_address,
                                to_split.stack_idx, to_split.flag};
    address right_start = d;
    size_t right_size = static_cast<size_t>((a + to_split.size) - right_start);
    address right_offset =
        to_split.physical_address +
        (right_start - left_start); // How far along have we traversed into our offset?
    out[1] = TrackedOffsetRange{right_start, right_size, right_offset, to_split.stack_idx,
                                to_split.flag};
    return OverlappingResult::SplitInMiddle;
  }
  // Overlap from the left -- We end up with one region on the right
  /*
        a    b    d  b
      | | |  | => |  |
      c   d
     */
  if (c <= a && d > a && d < b) {
    *len = 1;
    out[0] = TrackedOffsetRange{d, static_cast<size_t>(b - d), to_split.physical_address + (d - a),
                                to_split.stack_idx, to_split.flag};
    return OverlappingResult::ShortenedFromLeft;
  }
  // Overlap from the right
  /*
      a   b       a  c
      | | |  | => |  |
        c    d
     */
  if (a < c && c < b && b <= d) {
    *len = 1;
    out[0] = TrackedOffsetRange{a, static_cast<size_t>(c - a), to_split.physical_address,
                                to_split.stack_idx, to_split.flag};
    return OverlappingResult::ShortenedFromRight;
  }
  // No overlap at all
  *len = 0;
  return OverlappingResult::NoOverlap;
}
