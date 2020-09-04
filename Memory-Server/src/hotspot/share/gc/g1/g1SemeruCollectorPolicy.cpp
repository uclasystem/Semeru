/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1Analytics.hpp"
//#include "gc/g1/g1CollectorPolicy.hpp"
#include "gc/g1/g1YoungGenSizer.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/shared/gcPolicyCounters.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"

// Semeru
#include "gc/g1/g1SemeruCollectorPolicy.hpp"
#include "gc/g1/SemeruHeapRegion.hpp"

/**
 * [?]We need to rewrite these  control parameters for Semeru
 *  
 */
G1SemeruCollectorPolicy::G1SemeruCollectorPolicy():
_semeru_max_heap_byte_size(SemeruMemPoolMaxSize),
_semeru_initial_heap_byte_size(SemeruMemPoolInitialSize),
_semeru_heap_alignment(0) {    // Initialize the alignment size in initialize_alignments() 

  // Set up the region size and associated fields. Given that the
  // policy is created before the heap, we have to set this up here,
  // so it's done as soon as possible.

  // It would have been natural to pass initial_heap_byte_size() and
  // max_heap_byte_size() to setup_heap_region_size() but those have
  // not been set up at this point since they should be aligned with
  // the region size. So, there is a circular dependency here. We base
  // the region size on the heap size, but the heap size should be
  // aligned with the region size. To get around this we use the
  // unaligned values for the heap.
  SemeruHeapRegion::setup_semeru_heap_region_size(SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);
  HeapRegionRemSet::setup_semeru_remset_size();

  // Semeru
  reset_the_abandoned_super_class_fields();

}

void G1SemeruCollectorPolicy::initialize_alignments() {

  // Some parameters for original heap  control.
  // If hese parameters are not static, can we just rewrite their value to Semeru paratmers. 
  _space_alignment = SemeruHeapRegion::SemeruGrainBytes;
  size_t card_table_alignment = CardTableRS::ct_max_alignment_constraint();
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  
  // abandoned, use _semeru_heap_alignment
  //_heap_alignment = MAX3(card_table_alignment, _space_alignment, page_size);

  // Semeru patarmeters 
  _semeru_heap_alignment = MAX3(card_table_alignment, SemeruHeapRegion::SemeruGrainBytes, page_size);
  log_info(heap)("%s, _semeru_heap_alignment is 0x%llx ", __func__, 
                              (unsigned long long)_semeru_heap_alignment);

  // initialized in G1SemeruCollectorPolicy::initialize_flags()
  //_min_heap_byte_size = _semeru_heap_alignment;

}



void G1SemeruCollectorPolicy::initialize_flags() {
  assert(_space_alignment != 0, "Space alignment not set up properly");
  assert(_semeru_heap_alignment != 0, " Semeru Heap alignment not set up properly");
  assert(_semeru_heap_alignment >= _space_alignment,
         "_semeru_heap_alignment: " SIZE_FORMAT " less than space_alignment: " SIZE_FORMAT,
         _semeru_heap_alignment, _space_alignment);
  assert(_semeru_heap_alignment % _space_alignment == 0,
         "_semeru_heap_alignment: " SIZE_FORMAT " not aligned by space_alignment: " SIZE_FORMAT,
         _semeru_heap_alignment, _space_alignment);

  if (FLAG_IS_CMDLINE(SemeruMemPoolMaxSize)) {
    if (FLAG_IS_CMDLINE(SemeruMemPoolInitialSize) && SemeruMemPoolInitialSize > SemeruMemPoolMaxSize) {
      vm_exit_during_initialization("Semeru Initial heap size set to a larger value than the Semeru maximum heap size");
    }
    if (_min_heap_byte_size != 0 && SemeruMemPoolMaxSize < _min_heap_byte_size) {
      vm_exit_during_initialization("Incompatible Semeru minimum and Semeru maximum heap sizes specified");
    }
  }

  // Check heap parameter properties
  if (SemeruMemPoolMaxSize < 2 * M) {
    vm_exit_during_initialization("Too small Semeru maximum heap");
  }
  if (SemeruMemPoolInitialSize < M) {
    vm_exit_during_initialization("Too small Semeru initial heap");
  }
  if (_min_heap_byte_size < M) {
    vm_exit_during_initialization("Too small Semeru minimum heap");
  }

  // User inputs from -Xmx and -Xms must be aligned
  _min_heap_byte_size = align_up(_min_heap_byte_size, _semeru_heap_alignment);
  size_t aligned_initial_heap_size = align_up(SemeruMemPoolInitialSize, _semeru_heap_alignment);
  size_t aligned_max_heap_size = align_up(SemeruMemPoolMaxSize, _semeru_heap_alignment);

  // Write back to flags if the values changed
  if (aligned_initial_heap_size != SemeruMemPoolInitialSize) {
    FLAG_SET_ERGO(size_t, SemeruMemPoolInitialSize, aligned_initial_heap_size);
  }

  if (aligned_max_heap_size != SemeruMemPoolMaxSize) {
    FLAG_SET_ERGO(size_t, SemeruMemPoolMaxSize, aligned_max_heap_size);
  }

  if (FLAG_IS_CMDLINE(SemeruMemPoolInitialSize) && _min_heap_byte_size != 0 &&
      SemeruMemPoolInitialSize < _min_heap_byte_size) {
    vm_exit_during_initialization("Incompatible minimum and initial heap sizes specified");
  }
  if (!FLAG_IS_DEFAULT(SemeruMemPoolInitialSize) && SemeruMemPoolInitialSize > SemeruMemPoolMaxSize) {
    FLAG_SET_ERGO(size_t, SemeruMemPoolMaxSize, SemeruMemPoolInitialSize);
  } else if (!FLAG_IS_DEFAULT(SemeruMemPoolMaxSize) && SemeruMemPoolInitialSize > SemeruMemPoolMaxSize) {
    FLAG_SET_ERGO(size_t, SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);
    if (SemeruMemPoolInitialSize < _min_heap_byte_size) {
      _min_heap_byte_size = SemeruMemPoolInitialSize;
    }
  }

  _semeru_initial_heap_byte_size = SemeruMemPoolInitialSize;
  _semeru_max_heap_byte_size = SemeruMemPoolMaxSize;

  // Semeru don't have such a glabal field, MinHeapDeltaBytes
  //FLAG_SET_ERGO(size_t, MinHeapDeltaBytes, align_up(MinHeapDeltaBytes, _space_alignment));

  DEBUG_ONLY(G1SemeruCollectorPolicy::assert_flags();)
}




// Handle the abandoned fields from super class, CollectorPolicy.
void G1SemeruCollectorPolicy::reset_the_abandoned_super_class_fields(){
  _initial_heap_byte_size = 0;
  _max_heap_byte_size     = 0;
  //size_t _min_heap_byte_size; // Still use this one.

  // size_t _space_alignment;   // Still use this one.
  _heap_alignment         = 0;       
}


void G1SemeruCollectorPolicy::initialize_size_info() {
  log_debug(gc, heap)("Semeru Minimum heap " SIZE_FORMAT "Semeru Initial heap " SIZE_FORMAT " Semeru Maximum heap " SIZE_FORMAT,
                      _min_heap_byte_size, SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);

  //DEBUG_ONLY(CollectorPolicy::assert_size_info();)
}


#ifdef ASSERT

void G1SemeruCollectorPolicy::assert_flags() {

  // abandoned super class, CollectorPolicy, flags.
  assert(_initial_heap_byte_size == 0, "_initial_heap_byte_size(0x%llx) muse be abandoned in Semeru.", 
                                                               (unsigned long long)_initial_heap_byte_size);
  assert(_max_heap_byte_size == 0, "_max_heap_byte_size(0x%llx)  muse be abandoned in Semeru.",
                                                               (unsigned long long)_max_heap_byte_size);
  assert(_heap_alignment == 0, "_heap_alignment(0x%llx) muse be abandoned in Semeru.",
                                                               (unsigned long long)_heap_alignment );

  // Semeru flags 
  assert(SemeruMemPoolInitialSize <= SemeruMemPoolMaxSize, "Ergonomics decided on incompatible SemeruMemPoolInitialSize and maximum heap sizes");
  assert(SemeruMemPoolInitialSize % _semeru_heap_alignment == 0, "SemeruMemPoolInitialSize alignment");
  assert(SemeruMemPoolMaxSize % _semeru_heap_alignment == 0, "SemeruMemPoolMaxSize alignment");
}

#endif
