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
#include "gc/shared/markBitMap.inline.hpp"
#include "memory/virtualspace.hpp"

void MarkBitMap::print_on_error(outputStream* st, const char* prefix) const {
  _bm.print_on_error(st, prefix);
}

// Object allocation is HeapWord, 8 bytes, alignment.
// So, 1 bit for each 8 bytes.
// When makring 1 bit dirty, means an object is alive, whose start addr is the marked HeapWord.
// Heap Size : bitmap = 64 : 1. 
size_t MarkBitMap::compute_size(size_t heap_size) {
  return ReservedSpace::allocation_align_size_up(heap_size / mark_distance());
}

// Calculate the range can be represented by 1 bit.
size_t MarkBitMap::mark_distance() {
  return MinObjAlignmentInBytes * BitsPerByte;   // 8 bytes alignment * 8 bits per byte.
}

/**
 * Allocate bitmap space from storage to cover the heap.
 *  
 * storage : the MemRegion stored the bitmap, 
 * heap    : the MemRegion to be covered by this bitmap.
 * 
 * the size of the bitmap is specified by storage.start() + _covered region size >> _shifter.
 * 
 */
void MarkBitMap::initialize(MemRegion heap, MemRegion storage) {
  _covered = heap;

  // MarkBitMap->_bm is the real content of bitmap.
  // The storage is alreay reserved in C-Heap.
  _bm = BitMapView((BitMap::bm_word_t*) storage.start(), _covered.word_size() >> _shifter);
}

void MarkBitMap::do_clear(MemRegion mr, bool large) {
  MemRegion intersection = mr.intersection(_covered);
  assert(!intersection.is_empty(),
         "Given range from " PTR_FORMAT " to " PTR_FORMAT " is completely outside the heap",
         p2i(mr.start()), p2i(mr.end()));
  // convert address range into offset range
  size_t beg = addr_to_offset(intersection.start());
  size_t end = addr_to_offset(intersection.end());
  if (large) {
    _bm.clear_large_range(beg, end);
  } else {
    _bm.clear_range(beg, end);
  }
}

#ifdef ASSERT
void MarkBitMap::check_mark(HeapWord* addr) {
  assert(Universe::heap()->is_in_reserved(addr),
         "Trying to access bitmap " PTR_FORMAT " for address " PTR_FORMAT " not in the heap.",
         p2i(this), p2i(addr));
}
#endif
