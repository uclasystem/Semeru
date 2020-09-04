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
//#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
//#include "gc/g1/heapRegion.hpp"
//#include "gc/g1/heapRegionManager.inline.hpp"
//#include "gc/g1/heapRegionSet.inline.hpp"
#include "gc/g1/heterogeneousHeapRegionManager.hpp"   // useless ?
#include "gc/shared/collectorPolicy.hpp"
#include "memory/allocation.hpp"
#include "utilities/bitMap.inline.hpp"

// Semeru
#include "gc/g1/SemeruHeapRegion.hpp"
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"
#include "gc/g1/SemeruHeapRegionManager.inline.hpp"
#include "gc/g1/SemeruHeapRegionSet.inline.hpp"


/**
 * Create its own Semeru heap lock.
 *  
 * [?] Have nothing to do with the origial Heap_Lock ?
 * 			Change these conditions ?
 * 
 */
class SemeruMasterFreeRegionListChecker : public SemeruHeapRegionSetChecker {
public:
	void check_mt_safety() {
		// Master Free List MT safety protocol:
		// (a) If we're at a safepoint, operations on the master free list
		// should be invoked by either the VM thread (which will serialize
		// them) or by the GC workers while holding the
		// FreeList_lock.
		// (b) If we're not at a safepoint, operations on the master free
		// list should be invoked while holding the Memory_Pool_lock.

		if (SafepointSynchronize::is_at_safepoint()) {
			guarantee(Thread::current()->is_VM_thread() ||
								Semeru_FreeList_lock->owned_by_self(), "master free list MT safety protocol at a safepoint");
		} else {
			guarantee(Memory_Pool_lock->owned_by_self(), "master free list MT safety protocol outside a safepoint");
		}
	}
	bool is_correct_type(SemeruHeapRegion* hr) { return hr->is_free(); }
	const char* get_description() { return "Free Regions"; }
};



/**
 * Semeru
 * 
 * [x] Why does the initialization sequence matter ?
 * 	=> The fields initialization order must match their declaration order.
 * 
 */
SemeruHeapRegionManager::SemeruHeapRegionManager() :
	_bot_mapper(NULL),
	_cardtable_mapper(NULL),
	_card_counts_mapper(NULL),
	_available_map(mtGC),
	_num_committed(0),
	_allocated_heapregions_length(0),
	_regions(),
	_free_list("Semeru Free list", new SemeruMasterFreeRegionListChecker()),
	_heap_mapper(NULL)
{ }



/**
 * Semeru's SemeruHeapRegion manager  
 * 
 */
SemeruHeapRegionManager* SemeruHeapRegionManager::create_manager(G1SemeruCollectedHeap* heap, G1SemeruCollectorPolicy* policy) {
	if (policy->is_hetero_heap()) {
	//	return new HeterogeneousHeapRegionManager((uint)(policy->max_heap_byte_size() / SemeruHeapRegion::SemeruGrainBytes) /*heap size as num of regions*/);
		
		// Error
		printf("Error in %s, Semeru heap doesn't support heterogenous memory.\n", __func__);

		return NULL;
	}
	return new SemeruHeapRegionManager();
}




void SemeruHeapRegionManager::initialize(G1RegionToSpaceMapper* heap_storage,
															 G1RegionToSpaceMapper* bot,
															 G1RegionToSpaceMapper* cardtable,
															 G1RegionToSpaceMapper* card_counts) {
	_allocated_heapregions_length = 0;

	_heap_mapper = heap_storage;				// Java heap's Region->Page mapping  

	_bot_mapper = bot;									// Region -> G1BlockOffsetTable ->Page mapping 
	_cardtable_mapper = cardtable;

	_card_counts_mapper = card_counts;

	// Both _regions[] and _availale_map cover the whole reserved Java heap.
	MemRegion reserved = heap_storage->reserved();		// The Reserved space got from OS.
	_regions.initialize(reserved.start(), reserved.end(), SemeruHeapRegion::SemeruGrainBytes);  // Split the Java heap into Regions.

	_available_map.initialize(_regions.length());			// [?] Free bitmap for the Region list.
}




bool SemeruHeapRegionManager::is_available(uint region) const {
	return _available_map.at(region);
}

#ifdef ASSERT

/**
 * Tag : free means commited and then uncommited ? 
 * 		The difference between free and available ?
 * 		
 */
bool SemeruHeapRegionManager::is_free(SemeruHeapRegion* hr) const {
	return _free_list.contains(hr);
}
#endif

/**
 * Tag : Build SemeruHeapRegion management.
 *  
 */
SemeruHeapRegion* SemeruHeapRegionManager::new_heap_region(uint hrm_index) {
	G1SemeruCollectedHeap* g1h = G1SemeruCollectedHeap::heap();
	HeapWord* bottom = g1h->bottom_addr_for_region(hrm_index);
	MemRegion mr(bottom, bottom + SemeruHeapRegion::SemeruGrainWords);
	assert(reserved().contains(mr), "invariant");
	return g1h->new_heap_region(hrm_index, mr);
}

void SemeruHeapRegionManager::commit_regions(uint index, size_t num_regions, WorkGang* pretouch_gang) {
	guarantee(num_regions > 0, "Must commit more than zero regions");
	guarantee(_num_committed + num_regions <= max_length(), "Cannot commit more than the maximum amount of regions");

	_num_committed += (uint)num_regions;

	_heap_mapper->commit_regions(index, num_regions, pretouch_gang);		// Do initializaion on the corresponding pages of the Region.

	// Also commit auxiliary data
	//_prev_bitmap_mapper->commit_regions(index, num_regions, pretouch_gang);
	//_next_bitmap_mapper->commit_regions(index, num_regions, pretouch_gang);

	_bot_mapper->commit_regions(index, num_regions, pretouch_gang);			// Commit this Region's related BlockOffsetTable.
	_cardtable_mapper->commit_regions(index, num_regions, pretouch_gang);

	_card_counts_mapper->commit_regions(index, num_regions, pretouch_gang);
}




void SemeruHeapRegionManager::uncommit_regions(uint start, size_t num_regions) {
	guarantee(num_regions >= 1, "Need to specify at least one region to uncommit, tried to uncommit zero regions at %u", start);
	guarantee(_num_committed >= num_regions, "pre-condition");

	// Print before uncommitting.
	if (G1SemeruCollectedHeap::heap()->hr_printer()->is_active()) {
		for (uint i = start; i < start + num_regions; i++) {
			SemeruHeapRegion* hr = at(i);
			G1SemeruCollectedHeap::heap()->hr_printer()->uncommit(hr);
		}
	}

	_num_committed -= (uint)num_regions;

	_available_map.par_clear_range(start, start + num_regions, BitMap::unknown_range);
	_heap_mapper->uncommit_regions(start, num_regions);

	// Also uncommit auxiliary data
	//_prev_bitmap_mapper->uncommit_regions(start, num_regions);
	//_next_bitmap_mapper->uncommit_regions(start, num_regions);

	_bot_mapper->uncommit_regions(start, num_regions);
	_cardtable_mapper->uncommit_regions(start, num_regions);

	_card_counts_mapper->uncommit_regions(start, num_regions);
}


/**
 * Tag : Allocate SemeruHeapRegion management metadata.
 *  
 * [x] Where to allocate these data ? Normal C++ heap ? C_HEAP ? meta space ?
 * 			All the classes inherit from CHeapObj are allicated into native heap be overidden operator new.
 */
void SemeruHeapRegionManager::make_regions_available(uint start, uint num_regions, WorkGang* pretouch_gang) {
	guarantee(num_regions > 0, "No point in calling this for zero regions");
	
	// 1) Allocate && initialize the SemeruHeapRegion management meta data
	commit_regions(start, num_regions, pretouch_gang);
	for (uint i = start; i < start + num_regions; i++) {
		if (_regions.get_by_index(i) == NULL) {
			SemeruHeapRegion* new_hr = new_heap_region(i);			// 1) Invoke constructor. And then already invokes the SemeruHeapRegion::initialization ?
			log_debug(semeru,alloc)("%s, SemeruHeapRegion[0x%x] structur is allocated at 0x%lx \n", __func__, i, (size_t)new_hr );

			OrderAccess::storestore();
			_regions.set_by_index(i, new_hr);   //  G1HeapRegionTable _regions; <region_index, SemeruHeapRegion*>
			_allocated_heapregions_length = MAX2(_allocated_heapregions_length, i + 1);
		}
	}

	_available_map.par_set_range(start, start + num_regions, BitMap::unknown_range);

	// 2) Initialize the management for the space 
	for (uint i = start; i < start + num_regions; i++) {
		assert(is_available(i), "Just made region %u available but is apparently not.", i);
		SemeruHeapRegion* hr = at(i);		//  Retrieve the SemeruHeapRegion.
		if (G1SemeruCollectedHeap::heap()->hr_printer()->is_active()) {
			G1SemeruCollectedHeap::heap()->hr_printer()->commit(hr);
		}
		HeapWord* bottom = G1SemeruCollectedHeap::heap()->bottom_addr_for_region(i);	// start addr for the Region[i]
		MemRegion mr(bottom, bottom + SemeruHeapRegion::SemeruGrainWords);						// MR.

		hr->initialize(mr, (size_t)i);		// 2) initialzie more fileds for each SemeruHeapRegion.


		// RDMA : allocate cross region reference update queue
		//hr->allocate_init_cross_region_ref_update_queue(hr->hrm_index());


		// RDMA : Allocate the SemeruHeapRegion->_target_obj_q here.
		hr->allocate_init_target_oop_bitmap(hr->hrm_index());

		// Use SemeruHeapRegion->SyncBetweenMemoryAndCPU->_cross_region_ref_target_queue to initialize SemeruHeapRegion->_target_oop_bitmap
		// memory server bitmap#2, target_oop bitmap
  	// Temporary Design: Get its bitmap array from SemeruHeapRegion->_cross_region_ref_target_queue for now.
  	G1RegionToSpaceMapper* cur_region_target_oop_bitmap	= hr->create_target_oop_bitmap_storage(hr->hrm_index());
  	hr->_target_oop_bitmap.initialize(mr, cur_region_target_oop_bitmap );
		
		insert_into_free_list(at(i));
	}
}







// MemoryUsage SemeruHeapRegionManager::get_auxiliary_data_memory_usage() const {
// 	size_t used_sz =
// 		_prev_bitmap_mapper->committed_size() +
// 		_next_bitmap_mapper->committed_size() +
// 		_bot_mapper->committed_size() +
// 		_cardtable_mapper->committed_size() +
// 		_card_counts_mapper->committed_size();

// 	size_t committed_sz =
// 		_prev_bitmap_mapper->reserved_size() +
// 		_next_bitmap_mapper->reserved_size() +
// 		_bot_mapper->reserved_size() +
// 		_cardtable_mapper->reserved_size() +
// 		_card_counts_mapper->reserved_size();

// 	return MemoryUsage(0, used_sz, committed_sz, committed_sz);
// }


MemoryUsage SemeruHeapRegionManager::get_auxiliary_data_memory_usage() const {
	size_t used_sz =
		_bot_mapper->committed_size() +
		_cardtable_mapper->committed_size() +
		_card_counts_mapper->committed_size();

	size_t committed_sz =
		_bot_mapper->reserved_size() +
		_cardtable_mapper->reserved_size() +
		_card_counts_mapper->reserved_size();

	return MemoryUsage(0, used_sz, committed_sz, committed_sz);
}





/**
 * Tag : Expand from Region index 0, at num_regions.
 * 
 * 	[?] So, only invoke this function at Jave heap initialization ? 
 * 
 */
uint SemeruHeapRegionManager::expand_by(uint num_regions, WorkGang* pretouch_workers) {
	return expand_at(0, num_regions, pretouch_workers);
}


/**
 * Tag :  Find #N available Regions from index, start.
 * 		The Regions can be discontiguous. 
 * 		[x] Finnaly, the expanded Regions can be smaller than expectation, num_regions.
 */
uint SemeruHeapRegionManager::expand_at(uint start, uint num_regions, WorkGang* pretouch_workers) {
	if (num_regions == 0) {
		return 0;
	}

	uint cur = start;
	uint idx_last_found = 0;
	uint num_last_found = 0;

	uint expanded = 0;

	while (expanded < num_regions &&
				 (num_last_found = find_unavailable_from_idx(cur, &idx_last_found)) > 0) {
		uint to_expand = MIN2(num_regions - expanded, num_last_found);
		make_regions_available(idx_last_found, to_expand, pretouch_workers);   // Add the Regions into SemeruHeapRegionManager->_free_list.
		expanded += to_expand;
		cur = idx_last_found + num_last_found + 1;
	}

	// Debug - Padding for registering RDMA Buffer
	// 1) Padding the un-reserved Target_obj_queue
	// Commit the rest of TargetObjQueue space for RDMA buffer registration purpose.
	// 				The space needed to be padded is [ num_regions * TASKQUEUE_SIZE * , TARGET_OBJ_OFFSET + TARGET_OBJ_SIZE_BYTE]
	// Get the static pointer of the specific instantiation, CHeapRDMAObj<StarTask, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>
	// char* start_addr_to_padding	=	CHeapRDMAObj<StarTask, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>::_alloc_ptr ;
	// size_t size_to_be_padded	=	(size_t)(SEMERU_START_ADDR + TARGET_OBJ_OFFSET + TARGET_OBJ_SIZE_BYTE) -	(size_t)start_addr_to_padding;
	// G1SemeruCollectedHeap::heap()->_debug_rdma_padding_target_obj_queue = new(size_to_be_padded, start_addr_to_padding) rdma_padding();
	// tty->print("WARNING in %s, padding data in Meta Region[0x%lx,0x%lx) for target_obj_queue. \n",__func__,
	// 																																		(size_t)start_addr_to_padding,
	// 																																		(size_t)(start_addr_to_padding + size_to_be_padded));

	// 2) Padding for unused alive_bitmap
	//
	size_t per_region_bitmap_size = G1CMBitMap::compute_size(SemeruHeapRegion::SemeruGrainBytes);
	char* start_addr_to_padding = (char*)(SEMERU_START_ADDR + ALIVE_BITMAP_OFFSET +  num_regions*per_region_bitmap_size  );
	size_t size_to_be_padded	=	(size_t)(ALIVE_BITMAP_SIZE -  num_regions*per_region_bitmap_size);  // The size could be 0.
	if(size_to_be_padded>0){
		G1SemeruCollectedHeap::heap()->_debug_rdma_padding_alive_bitmap = new(size_to_be_padded, start_addr_to_padding) rdma_padding();
		tty->print("WARNING in %s, padding data in Meta Region[0x%lx,0x%lx) for alive_bitmap. \n",__func__,
																																			(size_t)start_addr_to_padding,
																																			(size_t)(start_addr_to_padding + size_to_be_padded));
	}

	// 3) Padding for the unused Cross Region reference update queue
	//		The BitQueue is defined in rdmaStructure.hpp as : CHeapRDMAObj<size_t, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>
	start_addr_to_padding	=	CHeapRDMAObj<size_t, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>::_alloc_ptr ;
	size_to_be_padded	=	(size_t)(SEMERU_START_ADDR + CROSS_REGION_REF_TARGET_Q_OFFSET + CROSS_REGION_REF_TARGET_Q_SIZE_LIMIT) -	(size_t)start_addr_to_padding;
	if(size_to_be_padded> 0){
		G1SemeruCollectedHeap::heap()->_debug_rdma_padding_cross_region_ref_update_queue = new(size_to_be_padded, start_addr_to_padding) rdma_padding();
		tty->print("WARNING in %s, padding data in Meta Region[0x%lx,0x%lx) for cross_region_ref_update_queue. \n",__func__,
																																			(size_t)start_addr_to_padding,
																																			(size_t)(start_addr_to_padding + size_to_be_padded));
	}

	verify_optional();
	return expanded;
}


/**
 * Tag : Find NUM contiguous empty Regions for allocation.
 * 
 * [x] Young/Old/Humonguous spaces share the same Region freelist, SemeruHeapRegionManager->_free_list.
 * 		After received the free Region, mark it with different tag : Eden, Survivor, Old, Humonguous etc.
 * 
 * [x] Scanning start from index, 0. 
 * 		Different scan the free_list from different direction:
 * 		=> Young Space (Eden, Suvivor) scan from tail.
 * 		=> Old/Single humongous Region, scan from head.
 * 		=> Multiple humonguous Regions, scan from head.
 */
uint SemeruHeapRegionManager::find_contiguous(size_t num, bool empty_only) {
	uint found = 0;						// Same as cur.
	size_t length_found = 0;
	uint cur = 0;							// Start from 0 ? nor from current index ?

	while (length_found < num && cur < max_length()) {
		SemeruHeapRegion* hr = _regions.get_by_index(cur);
		if ((!empty_only && !is_available(cur)) || (is_available(cur) && hr != NULL && hr->is_empty())) {
			// This region is a potential candidate for allocation into.
			length_found++;
		} else {
			// This region is not a candidate. The next region is the next possible one.
			found = cur + 1;		// Record the founded first Region index.
			length_found = 0;		
		}
		cur++;
	}

	if (length_found == num) {
		for (uint i = found; i < (found + num); i++) {
			SemeruHeapRegion* hr = _regions.get_by_index(i);
			// sanity check
			guarantee((!empty_only && !is_available(i)) || (is_available(i) && hr != NULL && hr->is_empty()),
								"Found region sequence starting at " UINT32_FORMAT ", length " SIZE_FORMAT
								" that is not empty at " UINT32_FORMAT ". Hr is " PTR_FORMAT, found, num, i, p2i(hr));
		}
		return found;
	} else {
		return G1_NO_HRM_INDEX;
	}
}

SemeruHeapRegion* SemeruHeapRegionManager::next_region_in_heap(const SemeruHeapRegion* r) const {
	guarantee(r != NULL, "Start region must be a valid region");
	guarantee(is_available(r->hrm_index()), "Trying to iterate starting from region %u which is not in the heap", r->hrm_index());
	for (uint i = r->hrm_index() + 1; i < _allocated_heapregions_length; i++) {
		SemeruHeapRegion* hr = _regions.get_by_index(i);
		if (is_available(i)) {
			return hr;
		}
	}
	return NULL;
}

void SemeruHeapRegionManager::iterate(SemeruHeapRegionClosure* blk) const {
	uint len = max_length();

	for (uint i = 0; i < len; i++) {
		if (!is_available(i)) {
			continue;
		}
		guarantee(at(i) != NULL, "Tried to access region %u that has a NULL SemeruHeapRegion*", i);
		bool res = blk->do_heap_region(at(i));
		if (res) {
			blk->set_incomplete();
			return;
		}
	}
}


/**
 * Tag : find contiguous available un-committed Regions from reserved space, SemeruHeapRegionManager->_regions.
 * 	Here uses the SemeruHeapRegionManager->_available_map for fast test.	
 * 
 */
uint SemeruHeapRegionManager::find_unavailable_from_idx(uint start_idx, uint* res_idx) const {
	guarantee(res_idx != NULL, "checking");
	guarantee(start_idx <= (max_length() + 1), "checking");

	uint num_regions = 0;

	uint cur = start_idx;
	while (cur < max_length() && is_available(cur)) {   // Find all the availabel Regions in un-committed reserved space.
		cur++;
	}
	if (cur == max_length()) {
		return num_regions;
	}
	*res_idx = cur;				// res_idx points to the last available, un-committed Region in reserved space.
	while (cur < max_length() && !is_available(cur)) {
		cur++;
	}
	num_regions = cur - *res_idx;		// The found available un-committed Regions.
#ifdef ASSERT
	for (uint i = *res_idx; i < (*res_idx + num_regions); i++) {
		assert(!is_available(i), "just checking");
	}
	assert(cur == max_length() || num_regions == 0 || is_available(cur),
				 "The region at the current position %u must be available or at the end of the heap.", cur);
#endif
	return num_regions;
}

uint SemeruHeapRegionManager::find_highest_free(bool* expanded) {
	// Loop downwards from the highest region index, looking for an
	// entry which is either free or not yet committed.  If not yet
	// committed, expand_at that index.
	uint curr = max_length() - 1;
	while (true) {
		SemeruHeapRegion *hr = _regions.get_by_index(curr);
		if (hr == NULL || !is_available(curr)) {
			uint res = expand_at(curr, 1, NULL);
			if (res == 1) {
				*expanded = true;
				return curr;
			}
		} else {
			if (hr->is_free()) {
				*expanded = false;
				return curr;
			}
		}
		if (curr == 0) {
			return G1_NO_HRM_INDEX;
		}
		curr--;
	}
}

bool SemeruHeapRegionManager::allocate_containing_regions(MemRegion range, size_t* commit_count, WorkGang* pretouch_workers) {
	size_t commits = 0;
	uint start_index = (uint)_regions.get_index_by_address(range.start());
	uint last_index = (uint)_regions.get_index_by_address(range.last());

	// Ensure that each G1 region in the range is free, returning false if not.
	// Commit those that are not yet available, and keep count.
	for (uint curr_index = start_index; curr_index <= last_index; curr_index++) {
		if (!is_available(curr_index)) {
			commits++;
			expand_at(curr_index, 1, pretouch_workers);
		}
		SemeruHeapRegion* curr_region  = _regions.get_by_index(curr_index);
		if (!curr_region->is_free()) {
			return false;
		}
	}

	allocate_free_regions_starting_at(start_index, (last_index - start_index) + 1);
	*commit_count = commits;
	return true;
}

/**
 * Tag : Apply closure to SemeruHeapRegion parallely.
 * 	hrclaimer, get a region from freelist. 
 * 	SemeruHeapRegionClosure : The closure applied to the got SemeruHeapRegion.
 * 
 */
void SemeruHeapRegionManager::par_iterate(SemeruHeapRegionClosure* blk, SemeruHeapRegionClaimer* hrclaimer, const uint start_index) const {
	// Every worker will actually look at all regions, skipping over regions that
	// are currently not committed.
	// This also (potentially) iterates over regions newly allocated during GC. This
	// is no problem except for some extra work.
	const uint n_regions = hrclaimer->n_regions();
	for (uint count = 0; count < n_regions; count++) {
		const uint index = (start_index + count) % n_regions;
		assert(index < n_regions, "sanity");
		// Skip over unavailable regions
		if (!is_available(index)) {
			continue;
		}
		SemeruHeapRegion* r = _regions.get_by_index(index);
		// We'll ignore regions already claimed.
		// However, if the iteration is specified as concurrent, the values for
		// is_starts_humongous and is_continues_humongous can not be trusted,
		// and we should just blindly iterate over regions regardless of their
		// humongous status.
		if (hrclaimer->is_region_claimed(index)) {
			continue;
		}
		// OK, try to claim it
		if (!hrclaimer->claim_region(index)) {
			continue;
		}
		bool res = blk->do_heap_region(r);
		if (res) {
			return;
		}
	}
}

uint SemeruHeapRegionManager::shrink_by(uint num_regions_to_remove) {
	assert(length() > 0, "the region sequence should not be empty");
	assert(length() <= _allocated_heapregions_length, "invariant");
	assert(_allocated_heapregions_length > 0, "we should have at least one region committed");
	assert(num_regions_to_remove < length(), "We should never remove all regions");

	if (num_regions_to_remove == 0) {
		return 0;
	}

	uint removed = 0;
	uint cur = _allocated_heapregions_length - 1;
	uint idx_last_found = 0;
	uint num_last_found = 0;

	while ((removed < num_regions_to_remove) &&
			(num_last_found = find_empty_from_idx_reverse(cur, &idx_last_found)) > 0) {
		uint to_remove = MIN2(num_regions_to_remove - removed, num_last_found);

		shrink_at(idx_last_found + num_last_found - to_remove, to_remove);

		cur = idx_last_found;
		removed += to_remove;
	}

	verify_optional();

	return removed;
}

void SemeruHeapRegionManager::shrink_at(uint index, size_t num_regions) {
#ifdef ASSERT
	for (uint i = index; i < (index + num_regions); i++) {
		assert(is_available(i), "Expected available region at index %u", i);
		assert(at(i)->is_empty(), "Expected empty region at index %u", i);
		assert(at(i)->is_free(), "Expected free region at index %u", i);
	}
#endif
	uncommit_regions(index, num_regions);
}

uint SemeruHeapRegionManager::find_empty_from_idx_reverse(uint start_idx, uint* res_idx) const {
	guarantee(start_idx < _allocated_heapregions_length, "checking");
	guarantee(res_idx != NULL, "checking");

	uint num_regions_found = 0;

	jlong cur = start_idx;
	while (cur != -1 && !(is_available(cur) && at(cur)->is_empty())) {
		cur--;
	}
	if (cur == -1) {
		return num_regions_found;
	}
	jlong old_cur = cur;
	// cur indexes the first empty region
	while (cur != -1 && is_available(cur) && at(cur)->is_empty()) {
		cur--;
	}
	*res_idx = cur + 1;
	num_regions_found = old_cur - cur;

#ifdef ASSERT
	for (uint i = *res_idx; i < (*res_idx + num_regions_found); i++) {
		assert(at(i)->is_empty(), "just checking");
	}
#endif
	return num_regions_found;
}

void SemeruHeapRegionManager::verify() {
	guarantee(length() <= _allocated_heapregions_length,
						"invariant: _length: %u _allocated_length: %u",
						length(), _allocated_heapregions_length);
	guarantee(_allocated_heapregions_length <= max_length(),
						"invariant: _allocated_length: %u _max_length: %u",
						_allocated_heapregions_length, max_length());

	bool prev_committed = true;
	uint num_committed = 0;
	HeapWord* prev_end = heap_bottom();
	for (uint i = 0; i < _allocated_heapregions_length; i++) {
		if (!is_available(i)) {
			prev_committed = false;
			continue;
		}
		num_committed++;
		SemeruHeapRegion* hr = _regions.get_by_index(i);
		guarantee(hr != NULL, "invariant: i: %u", i);
		guarantee(!prev_committed || hr->bottom() == prev_end,
							"invariant i: %u " HR_FORMAT " prev_end: " PTR_FORMAT,
							i, HR_FORMAT_PARAMS(hr), p2i(prev_end));
		guarantee(hr->hrm_index() == i,
							"invariant: i: %u hrm_index(): %u", i, hr->hrm_index());
		// Asserts will fire if i is >= _length
		HeapWord* addr = hr->bottom();
		guarantee(addr_to_region(addr) == hr, "sanity");
		// We cannot check whether the region is part of a particular set: at the time
		// this method may be called, we have only completed allocation of the regions,
		// but not put into a region set.
		prev_committed = true;
		prev_end = hr->end();
	}
	for (uint i = _allocated_heapregions_length; i < max_length(); i++) {
		guarantee(_regions.get_by_index(i) == NULL, "invariant i: %u", i);
	}

	guarantee(num_committed == _num_committed, "Found %u committed regions, but should be %u", num_committed, _num_committed);
	_free_list.verify();
}

#ifndef PRODUCT
void SemeruHeapRegionManager::verify_optional() {
	verify();
}
#endif // PRODUCT

SemeruHeapRegionClaimer::SemeruHeapRegionClaimer(uint n_workers) :
		_n_workers(n_workers), _n_regions(G1SemeruCollectedHeap::heap()->_hrm->_allocated_heapregions_length), _claims(NULL) {
	assert(n_workers > 0, "Need at least one worker.");
	uint* new_claims = NEW_C_HEAP_ARRAY(uint, _n_regions, mtGC);
	memset(new_claims, Unclaimed, sizeof(*_claims) * _n_regions);
	_claims = new_claims;
}

SemeruHeapRegionClaimer::~SemeruHeapRegionClaimer() {
	if (_claims != NULL) {
		FREE_C_HEAP_ARRAY(uint, _claims);
	}
}

uint SemeruHeapRegionClaimer::offset_for_worker(uint worker_id) const {
	assert(worker_id < _n_workers, "Invalid worker_id.");
	return _n_regions * worker_id / _n_workers;
}

bool SemeruHeapRegionClaimer::is_region_claimed(uint region_index) const {
	assert(region_index < _n_regions, "Invalid index.");
	return _claims[region_index] == Claimed;
}

bool SemeruHeapRegionClaimer::claim_region(uint region_index) {
	assert(region_index < _n_regions, "Invalid index.");
	uint old_val = Atomic::cmpxchg(Claimed, &_claims[region_index], Unclaimed);
	return old_val == Unclaimed;
}
