/*
 * Copyright (c) 2017, 2018 Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1SemeruCompactionPoint.hpp"
#include "gc/g1/SemeruHeapRegion.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/debug.hpp"

G1SemeruCompactionPoint::G1SemeruCompactionPoint() :
    _current_region(NULL),
    _threshold(NULL),
    _compaction_top(NULL) {
  _compaction_regions = new (ResourceObj::C_HEAP, mtGC) GrowableArray<SemeruHeapRegion*>(32, true, mtGC);
  _compaction_region_iterator = _compaction_regions->begin();     // Points to all the Source Region list.
}

G1SemeruCompactionPoint::~G1SemeruCompactionPoint() {
  delete _compaction_regions;
}

void G1SemeruCompactionPoint::update() {
  if (is_initialized()) {
    _current_region->set_compaction_top(_compaction_top);
  }
}

void G1SemeruCompactionPoint::initialize_values(bool init_threshold) {
  _compaction_top = _current_region->compaction_top();
  if (init_threshold) {
    _threshold = _current_region->initialize_threshold();
  }
}

bool G1SemeruCompactionPoint::has_regions() {
  return !_compaction_regions->is_empty();
}

bool G1SemeruCompactionPoint::is_initialized() {
  return _current_region != NULL;
}

void G1SemeruCompactionPoint::initialize(SemeruHeapRegion* hr, bool init_threshold) {
  _current_region = hr;
  initialize_values(init_threshold);
}

SemeruHeapRegion* G1SemeruCompactionPoint::current_region() {
  return *_compaction_region_iterator;
}

/**
 * Tag : Get next destination Region from G1SemeruCompactionPoint->_compaction_regions[]
 *    The content are the enqueued source Region. 
 * 
 */
SemeruHeapRegion* G1SemeruCompactionPoint::next_region() {
  SemeruHeapRegion* next = *(++_compaction_region_iterator);
  assert(next != NULL, "Must return valid region");
  return next;
}

GrowableArray<SemeruHeapRegion*>* G1SemeruCompactionPoint::regions() {
  return _compaction_regions;
}

bool G1SemeruCompactionPoint::object_will_fit(size_t size) {
  size_t space_left = pointer_delta(_current_region->end(), _compaction_top);
  return size <= space_left;
}

void G1SemeruCompactionPoint::switch_region() {
  // Save compaction top in the region.
  _current_region->set_compaction_top(_compaction_top);
  // Get the next region and re-initialize the values.
  _current_region = next_region();  // [?] how to determin the next Region ??
  initialize_values(true);
}


/**
 * Calculate the object, passed in parameter, 's destination address in current CompactionPoint/Region.
 *  
 */
void G1SemeruCompactionPoint::forward(oop object, size_t size) {
  assert(_current_region != NULL, "Must have been initialized");

  // Ensure the object fit in the current region.
  while (!object_will_fit(size)) {
    switch_region();  // Switch to a new compaction Region. No need to put any fake oop after the SemeruHeapRegion->_top
  }

  // Store a forwarding pointer if the object should be moved.
  if ((HeapWord*)object != _compaction_top) {
    object->forward_to(oop(_compaction_top));
  } else {
    if (object->forwardee() != NULL) {
      // Object should not move but mark-word is used so it looks like the
      // object is forwarded. Need to clear the mark and it's no problem
      // since it will be restored by preserved marks. There is an exception
      // with BiasedLocking, in this case forwardee() will return NULL
      // even if the mark-word is used. This is no problem since
      // forwardee() will return NULL in the compaction phase as well.
      object->init_mark_raw();  // [?] What cause this? This object should not be forwared, so just reset its markOop. 
    } else {
      // Make sure object has the correct mark-word set or that it will be
      // fixed when restoring the preserved marks.
      assert(object->mark_raw() == markOopDesc::prototype_for_object(object) || // Correct mark
             object->mark_raw()->must_be_preserved(object) || // Will be restored by PreservedMarksSet
             (UseBiasedLocking && object->has_bias_pattern_raw()), // Will be restored by BiasedLocking
             "should have correct prototype obj: " PTR_FORMAT " mark: " PTR_FORMAT " prototype: " PTR_FORMAT,
             p2i(object), p2i(object->mark_raw()), p2i(markOopDesc::prototype_for_object(object)));
    }
    assert(object->forwardee() == NULL, "should be forwarded to NULL");
  }

  // Update compaction values.
  _compaction_top += size;
  if (_compaction_top > _threshold) {
    _threshold = _current_region->cross_threshold(_compaction_top - size, _compaction_top);
  }
}

/**
 *  Tag : Add this source Region to current compaction_region's queue.
 *  
 *  [x] The newly added source Region are also the destination Region candidates.
 *      Which means that the full-gc compaction is procedure of compacting data into themselves.     
 * 
 *  [x] if can't fully compact this source Region into current compaction_region,
 *       Add a second destination Region into source Region->_next_compaction_space.
 * 
 */
void G1SemeruCompactionPoint::add(SemeruHeapRegion* hr) {
  _compaction_regions->append(hr);
}

void G1SemeruCompactionPoint::merge(G1SemeruCompactionPoint* other) {
   _compaction_regions->appendAll(other->regions());
}

SemeruHeapRegion* G1SemeruCompactionPoint::remove_last() {
  return _compaction_regions->pop();
}


/**
 * clear all the values 
 *  
 * SemeruHeapRegion* _current_region;      // Current destination Region.
 * HeapWord*   _threshold;
 * HeapWord*   _compaction_top;      // the top, when this Region is used as compaction destination Region.
 * GrowableArray<SemeruHeapRegion*>* _compaction_regions;    // The destination Region candidates. The enqueued source Region.
 * GrowableArrayIterator<SemeruHeapRegion*> _compaction_region_iterator; // points to the _compaction_regions[]
 *   
 *  
 */
void G1SemeruCompactionPoint::reset_compactionPoint(){

  _current_region = NULL;  // set flag non-initialized.
  _threshold      = NULL;
  _compaction_top = NULL;
  _compaction_regions->clear(); // set index, len to 0.
  _compaction_region_iterator = _compaction_regions->begin();   // point to _compaction_regions's first element.

}