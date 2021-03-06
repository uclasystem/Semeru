/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1RegionMarkStatsCache.inline.hpp"
#include "memory/allocation.inline.hpp"

G1RegionMarkStatsCache::G1RegionMarkStatsCache(G1RegionMarkStats* target, uint max_regions, uint num_cache_entries) :
  _target(target),    // the global, stats entry. Used as overflow/evict cache entries.
  _num_stats(max_regions),
  _cache(NULL),       // the content for <key, value> pair
  _num_cache_entries(num_cache_entries),   // 1 entry records, <region_id, live_words>
  _cache_hits(0),     // hash hit 
  _cache_misses(0),   // hash miss
  _num_cache_entries_mask(_num_cache_entries - 1) {

  guarantee(is_power_of_2(num_cache_entries),
            "Number of cache entries must be power of two, but is %u", num_cache_entries);
  _cache = NEW_C_HEAP_ARRAY(G1RegionMarkStatsCacheEntry, _num_cache_entries, mtGC);  // need to reset the cache entries.
}

G1RegionMarkStatsCache::~G1RegionMarkStatsCache() {
  FREE_C_HEAP_ARRAY(G1RegionMarkStatsCacheEntry, _cache);
}

// Evict all remaining statistics, returning cache hits and misses.
Pair<size_t, size_t> G1RegionMarkStatsCache::evict_all() {
  for (uint i = 0; i < _num_cache_entries; i++) {
    evict(i);
  }
  return Pair<size_t,size_t>(_cache_hits, _cache_misses);
}

/**
 * Semeru MS : Evic a specific Region to Global 
 * return the alive HeapWords for the entry.
 */
size_t G1RegionMarkStatsCache::evict_region(uint region_index) {
  G1RegionMarkStatsCacheEntry* cur;
  
  cur = find_for_add(region_index);  // Should find it, It's the last added region_index for current G1SemeruCMTask.
  if(cur->_region_idx == region_index &&  cur->_stats._live_words != 0 ){
    // Add into global
    Atomic::add(cur->_stats._live_words, &_target[cur->_region_idx]._live_words); // evict to global

    cur->clear(); // reset current local cache entry
  } 

  // calculate the alive ratio based on the stored value in global 
  return _target[region_index]._live_words;
}




// Reset all cache entries to their default values.
void G1RegionMarkStatsCache::reset() {
  _cache_hits = 0;
  _cache_misses = 0;

  for (uint i = 0; i < _num_cache_entries; i++) {
    _cache[i].clear();
  }
}
