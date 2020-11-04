/*
 * Copyright (c) 2017, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_G1_G1FULLGCOOPCLOSURES_INLINE_HPP
#define SHARE_VM_GC_G1_G1FULLGCOOPCLOSURES_INLINE_HPP

#include "gc/g1/g1Allocator.inline.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1FullGCMarker.inline.hpp"
#include "gc/g1/g1FullGCOopClosures.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"

template <typename T>
inline void G1MarkAndPushClosure::do_oop_work(T* p) {
  _marker->mark_and_push(p);
}

inline void G1MarkAndPushClosure::do_oop(oop* p) {
  do_oop_work(p);
}

inline void G1MarkAndPushClosure::do_oop(narrowOop* p) {
  do_oop_work(p);
}

inline bool G1MarkAndPushClosure::do_metadata() {
  return true;
}

inline void G1MarkAndPushClosure::do_klass(Klass* k) {
  _marker->follow_klass(k);
}

inline void G1MarkAndPushClosure::do_cld(ClassLoaderData* cld) {
  _marker->follow_cld(cld);
}

template <class T> inline void G1AdjustClosureNew::adjust_pointer(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);
  if (CompressedOops::is_null(heap_oop)) {
    return;
  }

  oop obj = CompressedOops::decode_not_null(heap_oop);
  assert(Universe::heap()->is_in(obj), "should be in heap");
  if (G1ArchiveAllocator::is_archived_object(obj)) {
    // We never forward archive objects.
    return;
  }

  oop forwardee = obj->forwardee();
  if (forwardee == NULL) {
    // Not forwarded, return current reference.
    assert(obj->mark_raw() == markOopDesc::prototype_for_object(obj) || // Correct mark
           obj->mark_raw()->must_be_preserved(obj) || // Will be restored by PreservedMarksSet
           (UseBiasedLocking && obj->has_bias_pattern_raw()), // Will be restored by BiasedLocking
           "Must have correct prototype or be preserved, obj: " PTR_FORMAT ", mark: " PTR_FORMAT ", prototype: " PTR_FORMAT,
           p2i(obj), p2i(obj->mark_raw()), p2i(markOopDesc::prototype_for_object(obj)));
    return;
  }

  // Forwarded, just update.
  assert(Universe::heap()->is_in_reserved(forwardee), "should be in object space");
  RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);

  // HeapRegion* from = _g1h->heap_region_containing(p);
  // if (!from->is_young())
  G1CollectedHeap* g1h = G1CollectedHeap::heap();

  // size_t card_index = g1h->card_table()->index_for(p);
  // // If the card hasn't been added to the buffer, do it.
  // if (g1h->card_table()->mark_card_deferred(card_index)) {
  //   g1h->dirty_card_queue_set().enqueue((jbyte*)g1h->card_table()->byte_for_index(card_index));
  // }  
  // enqueue_card_if_tracked(p, forwardee);
  
  
  
  oop containing_forwardee = _containing_obj;
  if(_containing_obj->forwardee() != NULL)
    containing_forwardee = _containing_obj->forwardee();
  
  size_t offset_byte = (size_t)(HeapWord*)p - (size_t)(HeapWord*)_containing_obj;
  T* new_p = (T*)(((size_t)(HeapWord*)containing_forwardee) + offset_byte);



  if (HeapRegion::is_in_same_region(new_p, forwardee)) {
    //tty->print("In the same region!");
    return;
  }






  HeapRegionRemSet* to_rem_set = G1CollectedHeap::heap()->heap_region_containing(forwardee)->rem_set();

  if(!g1h->is_in_g1_reserved((const void*) new_p)) {
    tty->print("containing obj: 0x%lx, forwardee:0x%lx\n", (size_t)(HeapWord*)_containing_obj, (size_t)(HeapWord*)forwardee);
    tty->print("containing forwardee: 0x%lx\n", (size_t)(HeapWord*)containing_forwardee);
    tty->print("offset_byte: 0x%lx\n, p: 0x%lx, new_p: 0x%lx\n", offset_byte, (size_t)p, (size_t)new_p);
    tty->print("Region #%u\n", G1CollectedHeap::heap()->heap_region_containing(forwardee)->hrm_index());
  }
  assert(g1h->is_in_g1_reserved((const void*) new_p),
         "Address " PTR_FORMAT " is outside of the heap ranging from [" PTR_FORMAT " to " PTR_FORMAT ")",
         p2i((void*)new_p), p2i(g1h->g1_reserved().start()), p2i(g1h->g1_reserved().end()));


  
  assert(to_rem_set != NULL, "Need per-region 'into' remsets.");
  //if (to_rem_set->is_tracked()) {
  //tty->print("Start!");
  if(!to_rem_set->is_tracked()) {
    to_rem_set->set_state_complete();
  }
  to_rem_set->add_reference(new_p, _worker_id);
  //tty->print("Success!\n");
  //}

}

inline void G1AdjustClosureNew::do_oop(oop* p)       { do_oop_work(p); }
inline void G1AdjustClosureNew::do_oop(narrowOop* p) { do_oop_work(p); }

template <class T> inline void G1AdjustClosure::adjust_pointer(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);
  if (CompressedOops::is_null(heap_oop)) {
    return;
  }

  oop obj = CompressedOops::decode_not_null(heap_oop);
  assert(Universe::heap()->is_in(obj), "should be in heap");
  if (G1ArchiveAllocator::is_archived_object(obj)) {
    // We never forward archive objects.
    return;
  }

  oop forwardee = obj->forwardee();
  if (forwardee == NULL) {
    // Not forwarded, return current reference.
    assert(obj->mark_raw() == markOopDesc::prototype_for_object(obj) || // Correct mark
           obj->mark_raw()->must_be_preserved(obj) || // Will be restored by PreservedMarksSet
           (UseBiasedLocking && obj->has_bias_pattern_raw()), // Will be restored by BiasedLocking
           "Must have correct prototype or be preserved, obj: " PTR_FORMAT ", mark: " PTR_FORMAT ", prototype: " PTR_FORMAT,
           p2i(obj), p2i(obj->mark_raw()), p2i(markOopDesc::prototype_for_object(obj)));
    return;
  }

  // Forwarded, just update.
  assert(Universe::heap()->is_in_reserved(forwardee), "should be in object space");
  RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);

  // // HeapRegion* from = _g1h->heap_region_containing(p);
  // // if (!from->is_young())
  // G1CollectedHeap* g1h = G1CollectedHeap::heap();

  // // size_t card_index = g1h->card_table()->index_for(p);
  // // // If the card hasn't been added to the buffer, do it.
  // // if (g1h->card_table()->mark_card_deferred(card_index)) {
  // //   g1h->dirty_card_queue_set().enqueue((jbyte*)g1h->card_table()->byte_for_index(card_index));
  // // }  
  // // enqueue_card_if_tracked(p, forwardee);
  
  // tty->print("containing obj: 0x%lx, forwardee:0x%lx\n", (size_t)(HeapWord*)_containing_obj, (size_t)(HeapWord*)forwardee);
  // oop containing_forwardee = _containing_obj->forwardee();
  // tty->print("containing forwardee: 0x%lx\n", (size_t)(HeapWord*)containing_forwardee);
  // size_t offset_byte = (size_t)(HeapWord*)p - (size_t)(HeapWord*)_containing_obj;
  // T* new_p = (T*)(((size_t)(HeapWord*)containing_forwardee) + offset_byte);
  // tty->print("offset_byte: 0x%lx\n, p: 0x%lx, new_p: 0x%lx\n", offset_byte, (size_t)p, (size_t)new_p);


  // if (HeapRegion::is_in_same_region(new_p, forwardee)) {
  //   tty->print("In the same region!");
  //   return;
  // }
  // tty->print("Region #%u\n", G1CollectedHeap::heap()->heap_region_containing(forwardee)->hrm_index());
  // HeapRegionRemSet* to_rem_set = G1CollectedHeap::heap()->heap_region_containing(forwardee)->rem_set();
  
  // assert(to_rem_set != NULL, "Need per-region 'into' remsets.");
  // //if (to_rem_set->is_tracked()) {
  // tty->print("Start!");
  // to_rem_set->add_reference(new_p, _worker_id);
  // tty->print("Success!\n");
  // //}

}

inline void G1AdjustClosure::do_oop(oop* p)       { do_oop_work(p); }
inline void G1AdjustClosure::do_oop(narrowOop* p) { do_oop_work(p); }

inline bool G1IsAliveClosure::do_object_b(oop p) {
  return _bitmap->is_marked(p) || G1ArchiveAllocator::is_closed_archive_object(p);
}

template<typename T>
inline void G1FullKeepAliveClosure::do_oop_work(T* p) {
  _marker->mark_and_push(p);
}

#endif
