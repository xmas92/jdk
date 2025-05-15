/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/gc_globals.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zForwarding.inline.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zRelocationSetSelector.inline.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"
#include "utilities/powerOfTwo.hpp"

ZRelocationSetSelectorGroupStats::ZRelocationSetSelectorGroupStats()
  : _npages_candidates(0),
    _total(0),
    _live(0),
    _empty(0),
    _npages_selected(0),
    _relocate(0) {}

ZRelocationSetSelectorGroupLiveStats::ZRelocationSetSelectorGroupLiveStats(size_t live)
  : _live(live) {}

ZRelocationSetSelectorGroup::ZRelocationSetSelectorGroup(const char* name,
                                                         ZPageType page_type,
                                                         size_t page_size,
                                                         size_t object_size_limit,
                                                         ZGenerationId id)
  : _name(name),
    _id(id),
    _page_type(page_type),
    _page_size(page_size),
    _object_size_limit(object_size_limit),
    _fragmentation_limit(id == ZGenerationId::old ? ZFragmentationLimit : ZYoungCompactionLimit),
    _page_fragmentation_limit((size_t)(_page_size * (_fragmentation_limit / 100))),
    _live_pages(),
    _not_selected_pages(),
    _forwarding_entries(0),
    _stats() {}

bool ZRelocationSetSelectorGroup::is_disabled() const {
  // Medium pages are disabled when their page size is zero
  return _page_type == ZPageType::medium && _page_size == 0;
}

bool ZRelocationSetSelectorGroup::is_selectable() const {
  // Large pages are not selectable
  return _page_type != ZPageType::large;
}

bool ZRelocationSetSelectorGroup::is_young() const {
  return _id == ZGenerationId::young;
}

void ZRelocationSetSelectorGroup::semi_sort() {
  // Semi-sort live pages by number of live bytes in ascending order
  const size_t partition_size = _page_size >> NumPartitionsShift;
  const int partition_size_shift = log2i_exact(partition_size);

  // Partition ends/fingers
  int partition_end[NumPartitions] = { /* zero initialize */ };
  int partition_finger[NumPartitions] = { /* zero initialize */ };

  // Calculate partition sizes
  for (const ZPage* page : _live_pages) {
    const size_t index = page->live_bytes() >> partition_size_shift;
    partition_end[index]++;
  }

  // Calculate partition ends
  for (size_t i = 1; i < NumPartitions; i++) {
    partition_end[i] += partition_end[i - 1];
  }

  // Calculate partition fingers
  for (size_t i = 1; i < NumPartitions; i++) {
    partition_finger[i] = partition_end[i - 1];
  }

  // Sort pages into partitions
  for (size_t i = 0; i < NumPartitions; i++) {
    while (partition_finger[i] != partition_end[i]) {
      const int page_index = partition_finger[i];
      ZPage*& page = _live_pages.at(page_index);
      const size_t partition_index = (page->live_bytes() >> partition_size_shift);

      if (partition_index == i) {
        // Already correct
        partition_finger[i] += 1;
      } else {
        // Page belongs in another partition
        ::swap(page, _live_pages.at(partition_finger[partition_index]++));
      }
    }
  }
}

void ZRelocationSetSelectorGroup::select_inner() {
  // Calculate the number of pages to relocate by successively including pages in
  // a candidate relocation set and calculate the maximum space requirement for
  // their live objects.
  const int npages = _live_pages.length();
  int selected_from = 0;
  int selected_to = 0;
  size_t npages_selected[ZPageAgeMax + 1] = { 0 };
  size_t selected_live_bytes[ZPageAgeMax + 1] = { 0 };
  size_t selected_forwarding_entries = 0;

  size_t from_live_bytes = 0;
  size_t from_forwarding_entries = 0;

  semi_sort();

  for (int from = 1; from <= npages; from++) {
    // Add page to the candidate relocation set
    ZPage* const page = _live_pages.at(from - 1);
    const size_t page_live_bytes = page->live_bytes();
    from_live_bytes += page_live_bytes;
    from_forwarding_entries += ZForwarding::nentries(page);

    // Calculate the maximum number of pages needed by the candidate relocation set.
    // By subtracting the object size limit from the pages size we get the maximum
    // number of pages that the relocation set is guaranteed to fit in, regardless
    // of in which order the objects are relocated.
    const int to = (int)ceil(from_live_bytes / (double)(_page_size - _object_size_limit));

    // Calculate the relative difference in reclaimable space compared to our
    // currently selected final relocation set. If this number is larger than the
    // acceptable fragmentation limit, then the current candidate relocation set
    // becomes our new final relocation set.
    const int diff_from = from - selected_from;
    const int diff_to = to - selected_to;
    const double diff_reclaimable = 100 - percent_of(diff_to, diff_from);
    if (diff_reclaimable > _fragmentation_limit) {
      selected_from = from;
      selected_to = to;
      selected_live_bytes[static_cast<uint>(page->age())] += page_live_bytes;
      npages_selected[static_cast<uint>(page->age())] += 1;
      selected_forwarding_entries = from_forwarding_entries;
    }

    log_trace(gc, reloc)("Candidate Relocation Set (%s Pages): %d->%d, "
                         "%.1f%% relative defragmentation, %zu forwarding entries, %s, live %d",
                         _name, from, to, diff_reclaimable, from_forwarding_entries,
                         (selected_from == from) ? "Selected" : "Rejected",
                         int(page_live_bytes * 100 / page->size()));
  }

  // Finalize selection
  if (is_young()) {
    ZArraySlice<ZPage*> _not_selected = _live_pages.slice_back(selected_from);
    _not_selected_pages.appendAll(&_not_selected);
  }
  _live_pages.trunc_to(selected_from);
  _forwarding_entries = selected_forwarding_entries;

  // Update statistics
  for (uint i = 0; i <= ZPageAgeMax; ++i) {
    _stats[i]._relocate = selected_live_bytes[i];
    _stats[i]._npages_selected = npages_selected[i];
  }

  log_debug(gc, reloc)("Relocation Set (%s Pages): %d->%d, %d skipped, %zu forwarding entries",
                       _name, selected_from, selected_to, npages - selected_from, selected_forwarding_entries);
}

void ZRelocationSetSelectorGroup::select() {
  if (is_disabled()) {
    return;
  }

  EventZRelocationSetGroup event;

  if (is_selectable()) {
    select_inner();
  } else if (is_young()) {
    // Mark pages as not selected
    _not_selected_pages.appendAll(&_live_pages);
  }

  ZRelocationSetSelectorGroupStats s{};
  for (uint i = 0; i <= ZPageAgeMax; ++i) {
    s._npages_candidates += _stats[i].npages_candidates();
    s._total += _stats[i].total();
    s._empty += _stats[i].empty();
    s._npages_selected += _stats[i].npages_selected();
    s._relocate += _stats[i].relocate();
  }

  // Send event
  event.commit((u8)_page_type, s._npages_candidates, s._total, s._empty, s._npages_selected, s._relocate);
}

ZRelocationSetSelector::ZRelocationSetSelector(ZGenerationId id)
  : _small("Small", ZPageType::small, ZPageSizeSmall, ZObjectSizeLimitSmall, id),
    _medium("Medium", ZPageType::medium, ZPageSizeMedium, ZObjectSizeLimitMedium, id),
    _large("Large", ZPageType::large, 0 /* page_size */, 0 /* object_size_limit */, id),
    _empty_pages() {}

void ZRelocationSetSelector::select() {
  // Select pages to relocate. The resulting relocation set will be
  // sorted such that medium pages comes first, followed by small
  // pages. Pages within each page group will be semi-sorted by live
  // bytes in ascending order. Relocating pages in this order allows
  // us to start reclaiming memory more quickly.

  EventZRelocationSet event;

  // Select pages from each group
  _large.select();
  _medium.select();
  _small.select();

  // Send event
  event.commit(total(), empty(), relocate());
}

ZRelocationSetSelectorStats ZRelocationSetSelector::stats() const {
  ZRelocationSetSelectorStats stats;

  for (uint i = 0; i <= ZPageAgeMax; ++i) {
    const ZPageAge age = static_cast<ZPageAge>(i);
    stats._small[i] = _small.stats(age);
    stats._medium[i] = _medium.stats(age);
    stats._large[i] = _large.stats(age);
  }

  stats._has_relocatable_pages = total() > 0;

  return stats;
}

ZRelocationSetSelectorLiveStats ZRelocationSetSelector::live_stats() const {
  ZRelocationSetSelectorLiveStats stats;

  for (uint i = 0; i <= ZPageAgeMax; ++i) {
    const ZPageAge age = static_cast<ZPageAge>(i);
    stats._small[i] = _small.live_stats(age);
    stats._medium[i] = _medium.live_stats(age);
    stats._large[i] = _large.live_stats(age);
  }

  return stats;
}
