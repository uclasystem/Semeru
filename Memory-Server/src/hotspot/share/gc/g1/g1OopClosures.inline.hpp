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

#ifndef SHARE_VM_GC_G1_G1OOPCLOSURES_INLINE_HPP
#define SHARE_VM_GC_G1_G1OOPCLOSURES_INLINE_HPP

#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1OopClosures.hpp"
#include "gc/g1/g1ParScanThreadState.inline.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"


// Semeru
#include "gc/g1/g1SemeruCollectedHeap.hpp"
#include "gc/g1/g1SemeruConcurrentMark.inline.hpp"


template <class T>
inline void G1ScanClosureBase::prefetch_and_push(T* p, const oop obj) {
	// We're not going to even bother checking whether the object is
	// already forwarded or not, as this usually causes an immediate
	// stall. We'll try to prefetch the object (for write, given that
	// we might need to install the forwarding reference) and we'll
	// get back to it when pop it from the queue
	Prefetch::write(obj->mark_addr_raw(), 0);
	Prefetch::read(obj->mark_addr_raw(), (HeapWordSize*2));

	// slightly paranoid test; I'm trying to catch potential
	// problems before we go into push_on_queue to know where the
	// problem is coming from
	assert((obj == RawAccess<>::oop_load(p)) ||
				 (obj->is_forwarded() &&
				 obj->forwardee() == RawAccess<>::oop_load(p)),
				 "p should still be pointing to obj or to its forwardee");

	_par_scan_state->push_on_queue(p);
}


/**
 * Tag : For a cross-region reference, the target obejct is NOT in CSet.
 * 
 *  [?] The target object isn't in CSet, but it can be in a Tracked Old Region. How to hanele it ?
 * 
 * [?] Meaning of optional_region ? 
 * 
 */
template <class T>
inline void G1ScanClosureBase::handle_non_cset_obj_common(InCSetState const state, T* p, oop const obj) {
	if (state.is_humongous()) {
		_g1h->set_humongous_is_live(obj);
	} else if (state.is_optional()) {			// [?] optional region should be in CSet ???
		_par_scan_state->remember_reference_into_optional_region(p);  // push this field into corresponding optional_region's queue
	}   
}

inline void G1ScanClosureBase::trim_queue_partially() {
	_par_scan_state->trim_queue_partially();
}


/**
 * Tag : Closure scan the fields of an evacuated objects during Young GC.
 * 
 * Parameter
 * 		Class T : CompressOop* or NormalOop* ?
 * 		T* p 		: usualy the oop* : address of the filed,  &(filed)
 * 
 * More Explanation
 * 
 * The Mark logic :
 * if the target oop in CSet
 * 		Push the &(field) into  scan queue
 * else 	// target oop isn't in CSet
 * 		if target oop is  Humongous Obj
 * 				Mark it alive & Handle it specially
 * 		else if : this is a cross-region ref && source (p) is in Old
 * 				Enqueue the card of &(field) to a G1ThreadLocalData->DirtyCardQueue
 * 
 * 
 * [x] How to handle the Young --> Old(Not in CSet) reference ?
 * 		Use _scanning_in_young to filter all the Young to Other reference.
 * 		RemSet is only used for Young STW GC, we don't need the Young to Other references.
 * 
 * [x] After the finish of a Concurrent Marking, Remark and Rebuild phase will also stop running before each mixed Young GC.
 * 			The Young GC relys on 
 * 	1) Write Barrier (Concurrent Refine + Update RemSet)   
 * 	2) Young GC itsefl to maintain the RemSet of Tracked Regions. (G1ScanEvacuatedObjClosure)
 * 			
 * 
 */
template <class T>
inline void G1ScanEvacuatedObjClosure::do_oop_work(T* p) {
	T heap_oop = RawAccess<>::oop_load(p);

	if (CompressedOops::is_null(heap_oop)) {
		return;
	}
	oop obj = CompressedOops::decode_not_null(heap_oop);  // The target oop
	const InCSetState state = _g1h->in_cset_state(obj);

	if (state.is_in_cset()) {
		// 1) Target oop is in CSet, push &(field) into parallel scan & evacuate queue		
		prefetch_and_push(p, obj);
	} else if (!HeapRegion::is_in_same_region(p, obj)) {
		// 2) Target oop is NOT in CSet. (It's no Old space or survivor region)
		handle_non_cset_obj_common(state, p, obj);
		assert(_scanning_in_young != Uninitialized, "Scan location has not been initialized.");

		if (_scanning_in_young == True) {    //f evacuated obj is in Young, skip Dirty cards update. Skip Young -> other referenes.
			return;
		}

		// 2) Enqueue a dirty card:    
		// 		a. target oop isn't in CSet and && 
		// 		b. p->oop is a cross-region reference  &&
		//		c. p is in Old space ()   // Should be in Optional Region, CSet
		//
		_par_scan_state->enqueue_card_if_tracked(p, obj);
	}
}

template <class T>
inline void G1CMOopClosure::do_oop_work(T* p) {
	_task->deal_with_reference(p);
}

/**
 * Semeru Memory Server concurrent marking. 
 * 
 */
template <class T>
inline void G1SemeruCMOopClosure::do_oop_work(T* p) {
	_semeru_task->deal_with_reference(p);
}


#ifdef ASSERT
// This verification is applied to all visited oops.
// The closures can turn is off by overriding should_verify_oops().
template <typename T>
void G1SemeruCMOopClosure::verify(T* p) {
	if (should_verify_oops()) {
		T heap_oop = RawAccess<>::oop_load(p);
		if (!CompressedOops::is_null(heap_oop)) {
			oop o = CompressedOops::decode_not_null(heap_oop);
			assert(Universe::semeru_heap()->semeru_is_in_closed_subset(o),
						 "should be in closed *p " PTR_FORMAT " " PTR_FORMAT, p2i(p), p2i(o));
		}
	}
}
#endif




/**
 * Tag : Closure of scanning Root Regions for CM.
 * 	Mark the object alive in next_bitmap directly, without pushing any fields/objects into task_queue.
 * 	One concurrent thread per Region.
 */
template <class T>
inline void G1RootRegionScanClosure::do_oop_work(T* p) {
	T heap_oop = RawAccess<MO_VOLATILE>::oop_load(p);
	if (CompressedOops::is_null(heap_oop)) {
		return;
	}
	oop obj = CompressedOops::decode_not_null(heap_oop);
	_cm->mark_in_next_bitmap(_worker_id, obj);
}

template <class T>
inline static void check_obj_during_refinement(T* p, oop const obj) {
#ifdef ASSERT
	G1CollectedHeap* g1h = G1CollectedHeap::heap();
	// can't do because of races
	// assert(oopDesc::is_oop_or_null(obj), "expected an oop");
	assert(check_obj_alignment(obj), "not oop aligned");
	assert(g1h->is_in_reserved(obj), "must be in heap");

	HeapRegion* from = g1h->heap_region_containing(p);

	assert(from != NULL, "from region must be non-NULL");
	assert(from->is_in_reserved(p) ||
				 (from->is_humongous() &&
					g1h->heap_region_containing(p)->is_humongous() &&
					from->humongous_start_region() == g1h->heap_region_containing(p)->humongous_start_region()),
				 "p " PTR_FORMAT " is not in the same region %u or part of the correct humongous object starting at region %u.",
				 p2i(p), from->hrm_index(), from->humongous_start_region()->hrm_index());
#endif // ASSERT
}


/** 
 * Tag : Transfer Mutator :G1BarrierSet->dirtyCard => HeapRegion->RemSet
 * This is the procedure of tansfering Mutator dirty cards to HeapRegion RemSet.
 * 
 * 	if NOT a cross-region reference
 * 		skip
 * 	else (cross-region)
 * 		Add this dirty card into target oop's HeapRegion->RemSet.
 * 
 */
template <class T>
inline void G1ConcurrentRefineOopClosure::do_oop_work(T* p) {
	T o = RawAccess<MO_VOLATILE>::oop_load(p);
	if (CompressedOops::is_null(o)) {
		return;
	}
	oop obj = CompressedOops::decode_not_null(o);  // target oop

	check_obj_during_refinement(p, obj);

	if (HeapRegion::is_in_same_region(p, obj)) {
		// Normally this closure should only be called with cross-region references.
		// But since Java threads are manipulating the references concurrently and we
		// reload the values things may have changed.
		// Also this check lets slip through references from a humongous continues region
		// to its humongous start region, as they are in different regions, and adds a
		// remembered set entry. This is benign (apart from memory usage), as we never
		// try to either evacuate or eager reclaim humonguous arrays of j.l.O.
		return;
	}

	HeapRegionRemSet* to_rem_set = _g1h->heap_region_containing(obj)->rem_set(); // HeapRegion->RemSet of target oop

	assert(to_rem_set != NULL, "Need per-region 'into' remsets.");
	if (to_rem_set->is_tracked()) {
		to_rem_set->add_reference(p, _worker_i);
	}
}


/**
 * Used during the Update RS phase to refine remaining cards in the DCQ during garbage collection.]
 * 
 * Tag : The closure of Update RemSet. 
 * 			1) if target object is in CSet, 
 * 						enequeue the &field into G1ParScanThreadState->_refs (StarTask queue) 
 * 			2) if the target object is in Tracked Regions, transfer the card into G1ParScanThreadState->_dcqs,
 * 					which points to G1CollectedHeap->_dirty_card_queue_set.
 * 
 * 			Compared to Concurrent Refine,
 * 				if the target objects is in Tracked Region, transfer it into corresponding Regions's RemSet.
 * 
 * 
 * More Explanation
 * 		oop p, the field of the objects within the card.
 * 		
 */
template <class T>
inline void G1ScanObjsDuringUpdateRSClosure::do_oop_work(T* p) {
	T o = RawAccess<>::oop_load(p);
	if (CompressedOops::is_null(o)) {
		return;
	}
	oop obj = CompressedOops::decode_not_null(o);  // target oop

	check_obj_during_refinement(p, obj);

	assert(!_g1h->is_in_cset((HeapWord*)p), "Oop originates from " PTR_FORMAT " (region: %u) which is in the collection set.", p2i(p), _g1h->addr_to_region((HeapWord*)p));
	const InCSetState state = _g1h->in_cset_state(obj);
	if (state.is_in_cset()) {			// This test isn't equal to HeapRegion->RemSet->state ??
		// Since the source is always from outside the collection set, here we implicitly know
		// that this is a cross-region reference too.
		prefetch_and_push(p, obj);
	} else if (!HeapRegion::is_in_same_region(p, obj)) {
		handle_non_cset_obj_common(state, p, obj);
		_par_scan_state->enqueue_card_if_tracked(p, obj);		// [?] What's the meanning of Tracked, for a Region.
	}
}

/**
 * Tag : Closure of scanning the RemSet of a HeapRegion
 * 
 * [?] Scan RemSet is handling the Old to CSet reference.
 * 		if target object is in CSet
 * 			evacuate it.
 * 			[no need to update RemSet ?]
 * 		else if target object isn't in the same Region
 * 				if target object is humonguos object
 * 					mark it alive
 * 				else if target object is in Optional Region
 * 					[?]
 * 				else //[ target object is in Old region]
 * 					nothing.
 * 		
 */
template <class T>
inline void G1ScanObjsDuringScanRSClosure::do_oop_work(T* p) {
	T heap_oop = RawAccess<>::oop_load(p);
	if (CompressedOops::is_null(heap_oop)) {
		return;
	}
	oop obj = CompressedOops::decode_not_null(heap_oop);

	const InCSetState state = _g1h->in_cset_state(obj);
	if (state.is_in_cset()) {
		prefetch_and_push(p, obj);			// 1) In CSet. Push &field into G1ParScanThreadState->RefToScanQueue to be evacuated.
	} else if (!HeapRegion::is_in_same_region(p, obj)) {
		handle_non_cset_obj_common(state, p, obj);  // 2) Cross-Region, target object is Humonguous or in Optional_region.
	}
}

template <class T>
inline void G1ScanRSForOptionalClosure::do_oop_work(T* p) {
	_scan_cl->do_oop_work(p);
	_scan_cl->trim_queue_partially();
}

/**
 * Tag: after evacuating alive objects to Survivor Regions, need to update ClassLoder information ?
 *  
 */
void G1ParCopyHelper::do_cld_barrier(oop new_obj) {
	if (_g1h->heap_region_containing(new_obj)->is_young()) {
		_scanned_cld->record_modified_oops();
	}
}

/**
 * Tag : Mark the objects alive in ConcurrentMark->next_bitmap.
 * 	
 * This marking is in Young STW GC's Root Closure, G1ParCopyClosure.
 * If an objects is reached by the root (Java Thread, VM Thread, String variables), mark them alive in next_bitmap directly.			
 * 
 */
void G1ParCopyHelper::mark_object(oop obj) {
	assert(!_g1h->heap_region_containing(obj)->in_collection_set(), "should not mark objects in the CSet");

	// We know that the object is not moving so it's safe to read its size.
	_cm->mark_in_next_bitmap(_worker_id, obj);
}

void G1ParCopyHelper::trim_queue_partially() {
	_par_scan_state->trim_queue_partially();
}

/**
 * Tag : Closure of Scanning alive objects from Stack variable 
 * 	=> only the first level ?
 * 
 * 3 cases:
 * 	1) target object in Collection Set, do the tracing 
 *  2) target object isn't in CS 
 * 		2.1) target object is a Humongous object, mark it alive and done ??
 *  	2.2) target object in Optional Region, do the optional tracing 
 *    2.3) Intial Marking pahse, mark object alive in HeapRegion->_next_bitmap.
 * 
 * [x] Should update the RemSet here ?
 * 	=> No. Only mark the alive objects in HeapRegion->next_bitmap.
 * 
 */
template <G1Barrier barrier, G1Mark do_mark_object>
template <class T>
void G1ParCopyClosure<barrier, do_mark_object>::do_oop_work(T* p) {
	T heap_oop = RawAccess<>::oop_load(p);

	if (CompressedOops::is_null(heap_oop)) {
		return;
	}

	oop obj = CompressedOops::decode_not_null(heap_oop);  // get the instance address of the target obj

	assert(_worker_id == _par_scan_state->worker_id(), "sanity");

	const InCSetState state = _g1h->in_cset_state(obj);   // the address of the target obj, Young(CSet) or Old. 
	if (state.is_in_cset()) { 
		// a. Target object is in CSet.
		oop forwardee;
		markOop m = obj->mark_raw();
		if (m->is_marked()) {
			forwardee = (oop) m->decode_pointer();
		} else {
			forwardee = _par_scan_state->copy_to_survivor_space(state, obj, m);
		}
		assert(forwardee != NULL, "forwardee should not be NULL");
		RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);   // Update the field's reference to the new address of target object.

		if (barrier == G1BarrierCLD) {   // [?] What's this ?? 
			do_cld_barrier(forwardee);
		}
	} else {
		// b. Target object isn't in CSet.
		if (state.is_humongous()) {
			_g1h->set_humongous_is_live(obj); 		// Handle the humongous objects seperately 
		} else if (state.is_optional()) {
			_par_scan_state->remember_root_into_optional_region(p); //the target obj is in Old Region, CSet, push it into optional_region queue.
		}

		// b.3 target object isn't in CSet, nor humongous object, nor in optional region.
		//			Mark it in the region's next_bitmap.
		// The object is not in collection set. If we're a root scanning
		// closure during an initial mark pause then attempt to mark the object.
		if (do_mark_object == G1MarkFromRoot) { 		// [?] If this is an initial phase ??
			mark_object(obj);													//  Mark object in HeapRegion->next_bitmap 
		}
	}

	trim_queue_partially();  // [?] Process the StarTask queue partially, in case it's too long ?	
}

template <class T> void G1RebuildRemSetClosure::do_oop_work(T* p) {
	oop const obj = RawAccess<MO_VOLATILE>::oop_load(p);
	if (obj == NULL) {
		return;
	}

	if (HeapRegion::is_in_same_region(p, obj)) {
		return;
	}

	HeapRegion* to = _g1h->heap_region_containing(obj);
	HeapRegionRemSet* rem_set = to->rem_set();
	rem_set->add_reference(p, _worker_id);
}

#endif // SHARE_VM_GC_G1_G1OOPCLOSURES_INLINE_HPP
