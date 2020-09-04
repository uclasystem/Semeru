/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1_SEMERU_COMPACTIONPOINT_HPP
#define SHARE_GC_G1_G1_SEMERU_COMPACTIONPOINT_HPP

#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/growableArray.hpp"

class SemeruHeapRegion;

class G1SemeruCompactionPoint : public CHeapObj<mtGC> {
  SemeruHeapRegion* _current_region;      // Current destination Region.
  HeapWord*   _threshold;
  HeapWord*   _compaction_top;      // the top, when this Region is used as compaction destination Region.
  GrowableArray<SemeruHeapRegion*>* _compaction_regions;    // The destination Region candidates. The enqueued source Region.
  GrowableArrayIterator<SemeruHeapRegion*> _compaction_region_iterator; // points to the _compaction_regions[]

  bool object_will_fit(size_t size);
  void initialize_values(bool init_threshold);
  void switch_region();
  SemeruHeapRegion* next_region();

public:
  G1SemeruCompactionPoint();
  ~G1SemeruCompactionPoint();

  bool has_regions();
  bool is_initialized();
  void initialize(SemeruHeapRegion* hr, bool init_threshold);
  void update();
  void forward(oop object, size_t size);
  void add(SemeruHeapRegion* hr);
  void merge(G1SemeruCompactionPoint* other);

  SemeruHeapRegion* remove_last();
  SemeruHeapRegion* current_region();

  GrowableArray<SemeruHeapRegion*>* regions();

  // Semeru support

  // Semeru MS need to reset the CP information at the end of compaction task.
  void reset_compactionPoint();

};

#endif // SHARE_GC_G1_G1_SEMERU_COMPACTIONPOINT_HPP
