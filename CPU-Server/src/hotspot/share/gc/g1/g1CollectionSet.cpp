/*
 * Copyright (c) 2016, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSet.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1ParScanThreadState.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/g1/heapRegionSet.hpp"
#include "logging/logStream.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/quickSort.hpp"


//mhr: modify for transfer
#include "gc/shared/rdmaStructure.hpp"
#include "gc/g1/g1Allocator.inline.hpp"
//mhr: modify for cache
//#include "gc/g1/ratio.hpp"

G1CollectorState* G1CollectionSet::collector_state() {
  return _g1h->collector_state();
}

G1GCPhaseTimes* G1CollectionSet::phase_times() {
  return _policy->phase_times();
}

CollectionSetChooser* G1CollectionSet::cset_chooser() {
  return _cset_chooser;
}

double G1CollectionSet::predict_region_elapsed_time_ms(HeapRegion* hr) {
  return _policy->predict_region_elapsed_time_ms(hr, collector_state()->in_young_only_phase());
}

G1CollectionSet::G1CollectionSet(G1CollectedHeap* g1h, G1Policy* policy) :
  _g1h(g1h),
  _policy(policy),
  _cset_chooser(new CollectionSetChooser()),
  _eden_region_length(0),
  _survivor_region_length(0),
  _old_region_length(0),
  _collection_set_regions(NULL),
  _collection_set_cur_length(0),
  _collection_set_max_length(0),
  _optional_regions(NULL),
  _optional_region_length(0),
  _optional_region_max_length(0),
  _bytes_used_before(0),
  _recorded_rs_lengths(0),
  _inc_build_state(Inactive),
  _inc_bytes_used_before(0),
  _inc_recorded_rs_lengths(0),
  _inc_recorded_rs_lengths_diffs(0),
  _inc_predicted_elapsed_time_ms(0.0),
  _inc_predicted_elapsed_time_ms_diffs(0.0) {
}

G1CollectionSet::~G1CollectionSet() {
  if (_collection_set_regions != NULL) {
    FREE_C_HEAP_ARRAY(uint, _collection_set_regions);
  }

  //mhr: modify
  if (_original_survivor_regions != NULL) {
    FREE_C_HEAP_ARRAY(uint, _original_survivor_regions);
  }

  free_optional_regions();
  delete _cset_chooser;
}

void G1CollectionSet::init_region_lengths(uint eden_cset_region_length,
                                          uint survivor_cset_region_length) {
  assert_at_safepoint_on_vm_thread();

  _eden_region_length     = eden_cset_region_length;
  _survivor_region_length = survivor_cset_region_length;

  assert((size_t) young_region_length() == _collection_set_cur_length,
         "Young region length %u should match collection set length " SIZE_FORMAT, young_region_length(), _collection_set_cur_length);

  _old_region_length      = 0;
  _optional_region_length = 0;
}

void G1CollectionSet::initialize(uint max_region_length) {
  guarantee(_collection_set_regions == NULL, "Must only initialize once.");
  _collection_set_max_length = max_region_length;
  _collection_set_regions = NEW_C_HEAP_ARRAY(uint, max_region_length, mtGC);
  //mhr: modify
  _original_survivor_regions = NEW_C_HEAP_ARRAY(uint, max_region_length, mtGC);
}

void G1CollectionSet::initialize_optional(uint max_length) {
  assert(_optional_regions == NULL, "Already initialized");
  assert(_optional_region_length == 0, "Already initialized");
  assert(_optional_region_max_length == 0, "Already initialized");
  _optional_region_max_length = max_length;
  _optional_regions = NEW_C_HEAP_ARRAY(HeapRegion*, _optional_region_max_length, mtGC);
}

void G1CollectionSet::free_optional_regions() {
  _optional_region_length = 0;
  _optional_region_max_length = 0;
  if (_optional_regions != NULL) {
    FREE_C_HEAP_ARRAY(HeapRegion*, _optional_regions);
    _optional_regions = NULL;
  }
}

void G1CollectionSet::set_recorded_rs_lengths(size_t rs_lengths) {
  _recorded_rs_lengths = rs_lengths;
}

// Add the heap region at the head of the non-incremental collection set
void G1CollectionSet::add_old_region(HeapRegion* hr) {
  assert_at_safepoint_on_vm_thread();

  assert(_inc_build_state == Active || hr->index_in_opt_cset() != G1OptionalCSet::InvalidCSetIndex,
         "Precondition, actively building cset or adding optional later on");
  assert(hr->is_old(), "the region should be old");

  assert(!hr->in_collection_set(), "should not already be in the CSet");
  _g1h->register_old_region_with_cset(hr);

  _collection_set_regions[_collection_set_cur_length++] = hr->hrm_index();
  assert(_collection_set_cur_length <= _collection_set_max_length, "Collection set now larger than maximum size.");

  _bytes_used_before += hr->used();
  size_t rs_length = hr->rem_set()->occupied();
  _recorded_rs_lengths += rs_length;
  _old_region_length += 1;

  log_trace(gc, cset)("Added old region %d to collection set", hr->hrm_index());
}

void G1CollectionSet::add_optional_region(HeapRegion* hr) {
  assert(!optional_is_full(), "Precondition, must have room left for this region");
  
  
  //mhr: modify
  // assert(hr->is_old(), "the region should be old");
  assert(!hr->in_collection_set(), "should not already be in the CSet");

  _g1h->register_optional_region_with_cset(hr);

  _optional_regions[_optional_region_length] = hr;
  uint index = _optional_region_length++;
  hr->set_index_in_opt_cset(index);

  log_trace(gc, cset)("Added region %d to optional collection set (%u)", hr->hrm_index(), _optional_region_length);
}

// Initialize the per-collection-set information
void G1CollectionSet::start_incremental_building() {
  assert(_collection_set_cur_length == 0, "Collection set must be empty before starting a new collection set.");
  assert(_inc_build_state == Inactive, "Precondition");

  _inc_bytes_used_before = 0;

  _inc_recorded_rs_lengths = 0;
  _inc_recorded_rs_lengths_diffs = 0;
  _inc_predicted_elapsed_time_ms = 0.0;
  _inc_predicted_elapsed_time_ms_diffs = 0.0;
  _inc_build_state = Active;
}

void G1CollectionSet::finalize_incremental_building() {
  assert(_inc_build_state == Active, "Precondition");
  assert(SafepointSynchronize::is_at_safepoint(), "should be at a safepoint");

  // The two "main" fields, _inc_recorded_rs_lengths and
  // _inc_predicted_elapsed_time_ms, are updated by the thread
  // that adds a new region to the CSet. Further updates by the
  // concurrent refinement thread that samples the young RSet lengths
  // are accumulated in the *_diffs fields. Here we add the diffs to
  // the "main" fields.

  if (_inc_recorded_rs_lengths_diffs >= 0) {
    _inc_recorded_rs_lengths += _inc_recorded_rs_lengths_diffs;
  } else {
    // This is defensive. The diff should in theory be always positive
    // as RSets can only grow between GCs. However, given that we
    // sample their size concurrently with other threads updating them
    // it's possible that we might get the wrong size back, which
    // could make the calculations somewhat inaccurate.
    size_t diffs = (size_t) (-_inc_recorded_rs_lengths_diffs);
    if (_inc_recorded_rs_lengths >= diffs) {
      _inc_recorded_rs_lengths -= diffs;
    } else {
      _inc_recorded_rs_lengths = 0;
    }
  }
  _inc_predicted_elapsed_time_ms += _inc_predicted_elapsed_time_ms_diffs;

  _inc_recorded_rs_lengths_diffs = 0;
  _inc_predicted_elapsed_time_ms_diffs = 0.0;
}

void G1CollectionSet::clear() {
  assert_at_safepoint_on_vm_thread();
  _collection_set_cur_length = 0;
  _optional_region_length = 0;
}

void G1CollectionSet::iterate(HeapRegionClosure* cl) const {
  iterate_from(cl, 0, 1);
}

void G1CollectionSet::iterate_from(HeapRegionClosure* cl, uint worker_id, uint total_workers) const {
  size_t len = _collection_set_cur_length;
  OrderAccess::loadload();
  if (len == 0) {
    return;
  }
  size_t start_pos = (worker_id * len) / total_workers;
  size_t cur_pos = start_pos;

  do {
    HeapRegion* r = _g1h->region_at(_collection_set_regions[cur_pos]);
    bool result = cl->do_heap_region(r);
    if (result) {
      cl->set_incomplete();
      return;
    }
    cur_pos++;
    if (cur_pos == len) {
      cur_pos = 0;
    }
  } while (cur_pos != start_pos);
}

void G1CollectionSet::update_young_region_prediction(HeapRegion* hr,
                                                     size_t new_rs_length) {
  // Update the CSet information that is dependent on the new RS length
  assert(hr->is_young(), "Precondition");
  assert(!SafepointSynchronize::is_at_safepoint(), "should not be at a safepoint");

  // We could have updated _inc_recorded_rs_lengths and
  // _inc_predicted_elapsed_time_ms directly but we'd need to do
  // that atomically, as this code is executed by a concurrent
  // refinement thread, potentially concurrently with a mutator thread
  // allocating a new region and also updating the same fields. To
  // avoid the atomic operations we accumulate these updates on two
  // separate fields (*_diffs) and we'll just add them to the "main"
  // fields at the start of a GC.

  ssize_t old_rs_length = (ssize_t) hr->recorded_rs_length();
  ssize_t rs_lengths_diff = (ssize_t) new_rs_length - old_rs_length;
  _inc_recorded_rs_lengths_diffs += rs_lengths_diff;

  double old_elapsed_time_ms = hr->predicted_elapsed_time_ms();
  double new_region_elapsed_time_ms = predict_region_elapsed_time_ms(hr);
  double elapsed_ms_diff = new_region_elapsed_time_ms - old_elapsed_time_ms;
  _inc_predicted_elapsed_time_ms_diffs += elapsed_ms_diff;

  hr->set_recorded_rs_length(new_rs_length);
  hr->set_predicted_elapsed_time_ms(new_region_elapsed_time_ms);
}

void G1CollectionSet::add_young_region_common(HeapRegion* hr) {
  assert(hr->is_young(), "invariant");
  assert(_inc_build_state == Active, "Precondition");

  size_t collection_set_length = _collection_set_cur_length;
  assert(collection_set_length <= INT_MAX, "Collection set is too large with %d entries", (int)collection_set_length);
  hr->set_young_index_in_cset((int)collection_set_length);


  //mhr: debug
  // if(hr->hrm_index() == 0) {
  //   printf("0!\n");
  // }

  _collection_set_regions[collection_set_length] = hr->hrm_index();
  // Concurrent readers must observe the store of the value in the array before an
  // update to the length field.
  OrderAccess::storestore();
  _collection_set_cur_length++;
  assert(_collection_set_cur_length <= _collection_set_max_length, "Collection set larger than maximum allowed.");

  // This routine is used when:
  // * adding survivor regions to the incremental cset at the end of an
  //   evacuation pause or
  // * adding the current allocation region to the incremental cset
  //   when it is retired.
  // Therefore this routine may be called at a safepoint by the
  // VM thread, or in-between safepoints by mutator threads (when
  // retiring the current allocation region)
  // We need to clear and set the cached recorded/cached collection set
  // information in the heap region here (before the region gets added
  // to the collection set). An individual heap region's cached values
  // are calculated, aggregated with the policy collection set info,
  // and cached in the heap region here (initially) and (subsequently)
  // by the Young List sampling code.
  // Ignore calls to this due to retirement during full gc.

  if (!_g1h->collector_state()->in_full_gc()) {
    //mhr: modify
    // size_t rs_length = hr->rem_set()->occupied();
    // double region_elapsed_time_ms = predict_region_elapsed_time_ms(hr);

    // // Cache the values we have added to the aggregated information
    // // in the heap region in case we have to remove this region from
    // // the incremental collection set, or it is updated by the
    // // rset sampling code
    // hr->set_recorded_rs_length(rs_length);
    // hr->set_predicted_elapsed_time_ms(region_elapsed_time_ms);

    // _inc_recorded_rs_lengths += rs_length;
    // _inc_predicted_elapsed_time_ms += region_elapsed_time_ms;
    // _inc_bytes_used_before += hr->used();
  }

  assert(!hr->in_collection_set(), "invariant");
  _g1h->register_young_region_with_cset(hr);
}

void G1CollectionSet::add_survivor_regions(HeapRegion* hr) {
  assert(hr->is_survivor(), "Must only add survivor regions, but is %s", hr->get_type_str());
  add_young_region_common(hr);
}

void G1CollectionSet::add_eden_region(HeapRegion* hr) {
  assert(hr->is_eden(), "Must only add eden regions, but is %s", hr->get_type_str());
  add_young_region_common(hr);
}

#ifndef PRODUCT
class G1VerifyYoungAgesClosure : public HeapRegionClosure {
public:
  bool _valid;
public:
  G1VerifyYoungAgesClosure() : HeapRegionClosure(), _valid(true) { }

  virtual bool do_heap_region(HeapRegion* r) {
    guarantee(r->is_young(), "Region must be young but is %s", r->get_type_str());

    SurvRateGroup* group = r->surv_rate_group();

    if (group == NULL) {
      log_error(gc, verify)("## encountered NULL surv_rate_group in young region");
      _valid = false;
    }

    if (r->age_in_surv_rate_group() < 0) {
      log_error(gc, verify)("## encountered negative age in young region");
      _valid = false;
    }

    return false;
  }

  bool valid() const { return _valid; }
};

bool G1CollectionSet::verify_young_ages() {
  assert_at_safepoint_on_vm_thread();

  G1VerifyYoungAgesClosure cl;
  iterate(&cl);

  if (!cl.valid()) {
    LogStreamHandle(Error, gc, verify) log;
    print(&log);
  }

  return cl.valid();
}

class G1PrintCollectionSetDetailClosure : public HeapRegionClosure {
  outputStream* _st;
public:
  G1PrintCollectionSetDetailClosure(outputStream* st) : HeapRegionClosure(), _st(st) { }

  virtual bool do_heap_region(HeapRegion* r) {
    assert(r->in_collection_set(), "Region %u should be in collection set", r->hrm_index());
    _st->print_cr("  " HR_FORMAT ", P: " PTR_FORMAT "N: " PTR_FORMAT ", age: %4d",
                  HR_FORMAT_PARAMS(r),
                  p2i(r->prev_top_at_mark_start()),
                  p2i(r->next_top_at_mark_start()),
                  r->age_in_surv_rate_group_cond());
    return false;
  }
};

void G1CollectionSet::print(outputStream* st) {
  st->print_cr("\nCollection_set:");

  G1PrintCollectionSetDetailClosure cl(st);
  iterate(&cl);
}
#endif // !PRODUCT





//mhr: modify
//mhr: explanation:
  //Choose all regions with high garbage ratio and cache ratio
  //sort based on life time:
  //choose conditioned on cache ratio and garbage ratio to determine whether to collect it on memory server or CPU server
void G1CollectionSet::finalize_parts_with_ratio(G1SurvivorRegions* survivors) {
  double young_start_time_sec = os::elapsedTime();
  size_t cache_threshold_in_pages = _policy->cache_threshold_in_pages(); //mhr: need to implement
  size_t max_cset_length = _policy->calc_max_cserver_cset_length();
  size_t new_collection_set_length = 0;
  size_t candidates_length = 0;
  _bytes_used_before = 0; //useless
  _eden_region_length = _survivor_region_length = 0;
  _rebuild_set_length = 0;

  HeapRegionManager* hrm = _g1h->hrm();

  uint len = hrm->max_length();
  HeapRegion* hr = NULL;

  HeapRegion** candidates_regions = NEW_C_HEAP_ARRAY(HeapRegion*, len, mtGC);

  initialize_optional(len);
  
  received_memory_server_cset* rmsc = _g1h->recv_mem_server_cset(); 
  rmsc->reset();                                                                                                                     
 
  _survivor_set_cur_length = 0;

  for (uint i = 0; i < len; i++) {
    if (!hrm->is_available(i)) {
      continue;
    }
    hr = hrm->at(i);
    //mhr: modify
    //mhr: new
    if(hr->is_free()) {
      continue;
    }
    guarantee(hr != NULL, "Tried to access region %u that has a NULL HeapRegion*", i);
    if(hr->is_young()) {
      //_collection_set_regions[new_collection_set_length++] = i;
      _bytes_used_before += hr->used();
      if(hr->is_eden()) {
        _eden_region_length++;
      }
      else{
        _survivor_region_length++;
      }
      if(i==0){
        printf("0!\n");
      }
      add_young_region_common(hr);
      new_collection_set_length++;
    }
    else if(hr->is_old()){
      if(hr->_mem_to_cpu_gc->_cm_scanned)
        hr->cross_region_ref_target_queue()->_age++;
      size_t cache_pages = cache_ratio_pages(hr);

      
      tty->print("Region %u: scanned? %d, cache: %lf, alive ratio: %lf, queue_age: %d\n", hr->hrm_index(),hr->_mem_to_cpu_gc->_cm_scanned,
       (double )cache_pages*PAGE_SIZE/HeapRegion::GrainBytes, hr->_mem_to_cpu_gc->_alive_ratio, hr->cross_region_ref_target_queue()->_age);

      if(hr->_mem_to_cpu_gc->_cm_scanned && (double )(cache_pages*PAGE_SIZE/HeapRegion::GrainBytes) - hr->_mem_to_cpu_gc->_alive_ratio > 0.2) {
        candidates_regions[candidates_length++] = hr;
      }
      else {
        if(cache_pages > cache_threshold_in_pages) {
          if(hr->_mem_to_cpu_gc->_cm_scanned && hr->_mem_to_cpu_gc->_alive_ratio < 0.65) {
            candidates_regions[candidates_length++] = hr;
          }
        }
        else if(!_g1h->_allocator->is_retained_old_region(hr)){
          if(hr->_mem_to_cpu_gc->_cm_scanned) {
            if(hr->_mem_to_cpu_gc->_alive_ratio < 0.30){
              candidates_regions[candidates_length++] = hr;
            }
            else if(hr->cross_region_ref_target_queue()->_age > 7 && cache_pages < (HeapRegion::GrainBytes/PAGE_SIZE-cache_threshold_in_pages)) {
              rmsc->add(hr->hrm_index());
              _g1h->old_set_remove(hr);
              add_optional_region(hr);

              //hr->cross_region_ref_update_queue()->reset();

              hr->cross_region_ref_target_queue()->reset();
              //hr->_mem_to_cpu_gc->_cm_scanned = false;
              hr->reset_region_cm_scanned();
              _collection_set_regions[_collection_set_cur_length++] = hr->hrm_index();
              _rebuild_set_length++;
            }
          }
          else if(!hr->cross_region_ref_target_queue()->_marked_from_root && cache_pages < (HeapRegion::GrainBytes/PAGE_SIZE-cache_threshold_in_pages)){
            rmsc->add(hr->hrm_index());
            _g1h->old_set_remove(hr);
            add_optional_region(hr);
          }
        }
      }
      
    }
    else { // humonguous region and young region fall into this path.

      log_debug(semeru)("%s, region[0x%x] humonguous? %d", __func__, hr->hrm_index(), hr->is_humongous() );

    }


  }

  // Clear the fields that point to the survivor list - they are all young now.
  survivors->convert_to_eden();
  // The number of recorded young regions is the incremental
  // collection set's current size
  // set_recorded_rs_lengths(_inc_recorded_rs_lengths);
  double young_end_time_sec = os::elapsedTime();
  phase_times()->record_young_cset_choice_time_ms((young_end_time_sec - young_start_time_sec) * 1000.0);

  QuickSort::sort(candidates_regions, candidates_length, G1CollectionSet::compare_region_ages, true);
  for (size_t i = 0; i < candidates_length && _collection_set_cur_length < max_cset_length; i++){
    hr = candidates_regions[i];
    //mhr: modify
    //mhr: new
    _g1h->old_set_remove(hr);
    _collection_set_regions[_collection_set_cur_length++] = hr->hrm_index();
    _bytes_used_before += hr->used();
    _g1h->register_old_region_with_cset(hr);
    log_trace(gc, cset)("Added region %d to collection set", hr->hrm_index());
  }
  printf("%lu\n", _collection_set_cur_length);
  _old_region_length = _collection_set_cur_length - young_region_length();
  stop_incremental_building();
  FREE_C_HEAP_ARRAY(HeapRegion*, candidates_regions);
  //mhr: debug
  printf("_collection_set_cur_length: %lu\n _collection_set: ", _collection_set_cur_length);
  for(size_t i = 0; i < _collection_set_cur_length; i++)
    printf("%d ", _collection_set_regions[i]);
  printf("\n");

    //mhr: debug
  printf("_optional_set_cur_length: %u\n _collection_set: ", _optional_region_length);
  for(size_t i = 0; i < _optional_region_length; i++)
    printf("%d ", _optional_regions[i]->hrm_index());
  printf("\n");

}




//mhr: modify
//mhr: explanation:
  //Choose all regions with high garbage ratio and cache ratio
  //sort based on life time:
  //choose conditioned on cache ratio and garbage ratio to determine whether to collect it on memory server or CPU server
void G1CollectionSet::finalize_parts(G1SurvivorRegions* survivors) {
  double young_start_time_sec = os::elapsedTime();

  //mhr: TODO
  //mhr: not sure
  // finalize_incremental_building();
  size_t cache_threshold_in_pages = _policy->cache_threshold_in_pages(); //mhr: need to implement
  size_t max_cset_length = _policy->calc_max_cserver_cset_length();
  size_t new_collection_set_length = 0;
  size_t candidates_length = 0;
  _bytes_used_before = 0; //useless
  _eden_region_length = _survivor_region_length = 0;
  _rebuild_set_length = 0;

  HeapRegionManager* hrm = _g1h->hrm();

  uint len = hrm->max_length();
  HeapRegion* hr = NULL;


  //mhr: modify
  //mhr: new
  HeapRegion** candidates_regions = NEW_C_HEAP_ARRAY(HeapRegion*, len, mtGC);

  initialize_optional(len);
  
  received_memory_server_cset* rmsc = _g1h->recv_mem_server_cset(); 
  rmsc->reset();                                                                                                                     
 
  _survivor_set_cur_length = 0;

  for (uint i = 0; i < len; i++) {
    if (!hrm->is_available(i)) {
      continue;
    }
    hr = hrm->at(i);
    //mhr: modify
    //mhr: new
    if(hr->is_free()) {
      continue;
    }
    guarantee(hr != NULL, "Tried to access region %u that has a NULL HeapRegion*", i);
    if(hr->is_young()) {
      //_collection_set_regions[new_collection_set_length++] = i;
      _bytes_used_before += hr->used();
      if(hr->is_eden()) {
        _eden_region_length++;
      }
      else{
        _survivor_region_length++;
      }
      if(i==0){
        printf("0!\n");
      }
      add_young_region_common(hr);
      new_collection_set_length++;
    }
    else if(hr->is_old()){
      
      log_debug(semeru)("Region %u marked from root: %d\n", i, hr->cross_region_ref_target_queue()->_marked_from_root);
      log_debug(semeru)("Region %u scanned?: %d\n", i, hr->_mem_to_cpu_gc->_cm_scanned);
      log_debug(semeru)("Region %u alive ratio: %lf\n", i, hr->_mem_to_cpu_gc->_alive_ratio);

      if(cache_ratio_pages(hr) > cache_threshold_in_pages && hr->_mem_to_cpu_gc->_cm_scanned) {
         /*&& hr->_mem_to_cpu_gc->_alive_ratio < 0.5*/
        log_debug(semeru)("Candidate Region %u scanned?: %d\n", i, hr->_mem_to_cpu_gc->_cm_scanned);
        log_debug(semeru)("Candidate Region %u alive ratio: %lf\n", i, hr->_mem_to_cpu_gc->_alive_ratio);
        //mhr: debug
        candidates_regions[candidates_length++] = hr;

      }
      else if(!_g1h->_allocator->is_retained_old_region(hr) && !hr->_mem_to_cpu_gc->_cm_scanned && !hr->cross_region_ref_target_queue()->_marked_from_root){
        rmsc->add(hr->hrm_index());

        // mhr: add as optional
        _g1h->old_set_remove(hr);
        add_optional_region(hr);
      }
    }
    else { // humonguous region fall into this path.

      log_debug(semeru)("%s, region[0x%x] humonguous? %d", __func__, hr->hrm_index(), hr->is_humongous() );

      //rmsc->add(hr->hrm_index());
      // mhr: add as optional
      //_g1h->old_set_remove(hr);
      //add_optional_region(hr);
    }


  }

  //_collection_set_cur_length = ;
  // printf("%lu\n", _collection_set_cur_length);

  //uint survivor_region_length = survivors->length();
  //uint eden_region_length = _g1h->eden_regions_count();
  //init_region_lengths(eden_region_length, survivor_region_length);
  //verify_young_cset_indices();

  // Clear the fields that point to the survivor list - they are all young now.
  survivors->convert_to_eden();

  // printf("%lu\n", _collection_set_cur_length);

  // The number of recorded young regions is the incremental
  // collection set's current size
  // set_recorded_rs_lengths(_inc_recorded_rs_lengths);
  double young_end_time_sec = os::elapsedTime();
  phase_times()->record_young_cset_choice_time_ms((young_end_time_sec - young_start_time_sec) * 1000.0);

  //hr->reclaimable_bytes() gives the garbage_bytes
  // size_t garbage_threshold_in_bytes = _policy->garbage_threshold_in_bytes();  //mhr: need to implement

  QuickSort::sort(candidates_regions, candidates_length, G1CollectionSet::compare_region_ages, true);

  // printf("%lu\n", _collection_set_cur_length);

  //size_t max_cset_size = _policy->calc_max_cserver_cset_length();
  size_t cset_boundary = 0;
  // for (size_t i = 0; i < candidates_length && _collection_set_cur_length < max_cset_length; i++){
  //   cset_boundary++;
  //   hr = candidates_regions[i];
  //   //mhr: modify
  //   //mhr: new
  //   _g1h->old_set_remove(hr);
  //   _collection_set_regions[_collection_set_cur_length++] = hr->hrm_index();
  //   _bytes_used_before += hr->used();
  //   _g1h->register_old_region_with_cset(hr);
  //   log_trace(gc, cset)("Added region %d to collection set", hr->hrm_index());
  // }
  for(; cset_boundary < candidates_length; cset_boundary++) {
    hr = candidates_regions[cset_boundary];
    // if(_g1h->_allocator->is_retained_old_region(hr)) {
    //   continue;
    // }
    if(hr->_mem_to_cpu_gc->_alive_ratio < 0.30) {
      _g1h->old_set_remove(hr);
      _collection_set_regions[_collection_set_cur_length++] = hr->hrm_index();
      _bytes_used_before += hr->used();
      _g1h->register_old_region_with_cset(hr);
      log_trace(gc, cset)("Added region %d to collection set", hr->hrm_index());
    }
    else {
      //hr->cross_region_ref_update_queue()->_age++;
      hr->cross_region_ref_target_queue()->_age++;
      int threshold = RebuildThreshold;
      if(hr->cross_region_ref_target_queue()->_age > threshold) {
        rmsc->add(hr->hrm_index());
        // mhr: add as optional
        _g1h->old_set_remove(hr);
        add_optional_region(hr);


        //hr->cross_region_ref_update_queue()->reset();
        hr->cross_region_ref_target_queue()->reset();
        //hr->_mem_to_cpu_gc->_cm_scanned = false;
        hr->reset_region_cm_scanned();
        log_debug(semeru)("Rescan Region %u marked from root: %d %d\n", hr->hrm_index(), hr->cross_region_ref_target_queue()->_marked_from_root, hr->cross_region_ref_target_queue()->_marked_from_root);
        log_debug(semeru)("Rescan Region %u scanned?: %d\n", hr->hrm_index(), hr->_mem_to_cpu_gc->_cm_scanned);
        log_debug(semeru)("Rescan Region %u alive ratio: %lf\n", hr->hrm_index(), hr->_mem_to_cpu_gc->_alive_ratio);
        _collection_set_regions[_collection_set_cur_length++] = hr->hrm_index();
        _rebuild_set_length++;
      }
    }
  }






  
  printf("%lu\n", _collection_set_cur_length);

  _old_region_length = _collection_set_cur_length - young_region_length();

  stop_incremental_building();

  //QuickSort::sort(_collection_set_regions, _collection_set_cur_length, G1CollectionSet::compare_region_idx, true);
  
  FREE_C_HEAP_ARRAY(HeapRegion*, candidates_regions);




  //mhr: debug
  printf("_collection_set_cur_length: %lu\n _collection_set: ", _collection_set_cur_length);
  for(size_t i = 0; i < _collection_set_cur_length; i++)
    printf("%d ", _collection_set_regions[i]);
  printf("\n");

    //mhr: debug
  printf("_optional_set_cur_length: %u\n _collection_set: ", _optional_region_length);
  for(size_t i = 0; i < _optional_region_length; i++)
    printf("%d ", _optional_regions[i]->hrm_index());
  printf("\n");

}


double G1CollectionSet::finalize_young_part(double target_pause_time_ms, G1SurvivorRegions* survivors) {
  double young_start_time_sec = os::elapsedTime();

  finalize_incremental_building();

  guarantee(target_pause_time_ms > 0.0,
            "target_pause_time_ms = %1.6lf should be positive", target_pause_time_ms);

  size_t pending_cards = _policy->pending_cards();
  double base_time_ms = _policy->predict_base_elapsed_time_ms(pending_cards);
  double time_remaining_ms = MAX2(target_pause_time_ms - base_time_ms, 0.0);

  log_trace(gc, ergo, cset)("Start choosing CSet. pending cards: " SIZE_FORMAT " predicted base time: %1.2fms remaining time: %1.2fms target pause time: %1.2fms",
                            pending_cards, base_time_ms, time_remaining_ms, target_pause_time_ms);

  // The young list is laid with the survivor regions from the previous
  // pause are appended to the RHS of the young list, i.e.
  //   [Newly Young Regions ++ Survivors from last pause].

  uint survivor_region_length = survivors->length();
  uint eden_region_length = _g1h->eden_regions_count();
  init_region_lengths(eden_region_length, survivor_region_length);

  verify_young_cset_indices();

  // Clear the fields that point to the survivor list - they are all young now.
  survivors->convert_to_eden();

  _bytes_used_before = _inc_bytes_used_before;
  time_remaining_ms = MAX2(time_remaining_ms - _inc_predicted_elapsed_time_ms, 0.0);

  log_trace(gc, ergo, cset)("Add young regions to CSet. eden: %u regions, survivors: %u regions, predicted young region time: %1.2fms, target pause time: %1.2fms",
                            eden_region_length, survivor_region_length, _inc_predicted_elapsed_time_ms, target_pause_time_ms);

  // The number of recorded young regions is the incremental
  // collection set's current size
  set_recorded_rs_lengths(_inc_recorded_rs_lengths);

  double young_end_time_sec = os::elapsedTime();
  phase_times()->record_young_cset_choice_time_ms((young_end_time_sec - young_start_time_sec) * 1000.0);

  return time_remaining_ms;
}

void G1CollectionSet::add_as_old(HeapRegion* hr) {
  cset_chooser()->pop(); // already have region via peek()
  _g1h->old_set_remove(hr);
  add_old_region(hr);
}

void G1CollectionSet::add_as_optional(HeapRegion* hr) {
  assert(_optional_regions != NULL, "Must not be called before array is allocated");
  cset_chooser()->pop(); // already have region via peek()
  _g1h->old_set_remove(hr);
  add_optional_region(hr);
}

bool G1CollectionSet::optional_is_full() {
  assert(_optional_region_length <= _optional_region_max_length, "Invariant");
  return _optional_region_length == _optional_region_max_length;
}

void G1CollectionSet::clear_optional_region(const HeapRegion* hr) {
  assert(_optional_regions != NULL, "Must not be called before array is allocated");
  uint index = hr->index_in_opt_cset();
  _optional_regions[index] = NULL;
}

//
int G1CollectionSet::compare_region_ages(const HeapRegion* a, const HeapRegion* b) {
  // int age_a = a->minimum_age();
  // int age_b = b->minimum_age();
  // if (age_a > age_b) {
  //   return 1;
  // } else if (age_a == age_b) {
  //   return 0;
  // } else {
  //   return -1;
  // }
  double ratio_a = a->_mem_to_cpu_gc->_alive_ratio;
  double ratio_b = b->_mem_to_cpu_gc->_alive_ratio;
  if (ratio_a > ratio_b) {
    return 1;
  } else if (ratio_a == ratio_b) {
    return 0;
  } else {
    return -1;
  }
}

static int compare_region_idx(const uint a, const uint b) {
  if (a > b) {
    return 1;
  } else if (a == b) {
    return 0;
  } else {
    return -1;
  }
}

size_t G1CollectionSet::cache_ratio_pages(HeapRegion* hr) {
  // int pid = os::current_process_id();
  // HeapWord* btm = hr->bottom();
  // HeapWord* top = hr->bottom() + HeapRegion::GrainBytes/8;
  // //return ((unsigned long)(pages_inmem_ratio(pid, (unsigned long)btm, (unsigned long)top)))*((((unsigned long)top - (unsigned long)top)) / 0x1000);
  // return 1*((((unsigned long)top - (unsigned long)btm)) / 0x1000);
  // //return HeapRegion::GrainBytes/4/1024*80/100;


  // Example of use swap out ratio
  // Kernel doesn't float point, so we can only renturn the swapped out page numbers.
  // The requested range can only be at [SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE, to the heap END(32GB max))
  size_t request_so_start_addr = (size_t)hr->bottom();//SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE;
  size_t request_so_range = HeapRegion::GrainBytes; // one Region, assume 256 MBytes
  size_t swapped_out_pages  = syscall(SYS_NUM_SWAP_OUT_PAGES, request_so_start_addr, request_so_range);
  tty->print("%s, swapped out 0x%lx pages (%f) for range[0x%lx, 0x%lx) \n", 
                __func__, swapped_out_pages, (double)swapped_out_pages*PAGE_SIZE/HeapRegion::GrainBytes, request_so_start_addr, request_so_start_addr+request_so_range  );
  return HeapRegion::GrainBytes/PAGE_SIZE-swapped_out_pages;

}

void G1CollectionSet::finalize_old_part(double time_remaining_ms) {
  double non_young_start_time_sec = os::elapsedTime();
  double predicted_old_time_ms = 0.0;
  double predicted_optional_time_ms = 0.0;
  double optional_threshold_ms = time_remaining_ms * _policy->optional_prediction_fraction();
  uint expensive_region_num = 0;

  if (collector_state()->in_mixed_phase()) {
    cset_chooser()->verify();
    const uint min_old_cset_length = _policy->calc_min_old_cset_length();
    const uint max_old_cset_length = MAX2(min_old_cset_length, _policy->calc_max_old_cset_length());
    bool check_time_remaining = _policy->adaptive_young_list_length();

    initialize_optional(max_old_cset_length - min_old_cset_length);
    log_debug(gc, ergo, cset)("Start adding old regions for mixed gc. min %u regions, max %u regions, "
                              "time remaining %1.2fms, optional threshold %1.2fms",
                              min_old_cset_length, max_old_cset_length, time_remaining_ms, optional_threshold_ms);

    HeapRegion* hr = cset_chooser()->peek();
    while (hr != NULL) {
      if (old_region_length() + optional_region_length() >= max_old_cset_length) {
        // Added maximum number of old regions to the CSet.
        log_debug(gc, ergo, cset)("Finish adding old regions to CSet (old CSet region num reached max). "
                                  "old %u regions, optional %u regions",
                                  old_region_length(), optional_region_length());
        break;
      }

      // Stop adding regions if the remaining reclaimable space is
      // not above G1HeapWastePercent.
      size_t reclaimable_bytes = cset_chooser()->remaining_reclaimable_bytes();
      double reclaimable_percent = _policy->reclaimable_bytes_percent(reclaimable_bytes);
      double threshold = (double) G1HeapWastePercent;
      if (reclaimable_percent <= threshold) {
        // We've added enough old regions that the amount of uncollected
        // reclaimable space is at or below the waste threshold. Stop
        // adding old regions to the CSet.
        log_debug(gc, ergo, cset)("Finish adding old regions to CSet (reclaimable percentage not over threshold). "
                                  "reclaimable: " SIZE_FORMAT "%s (%1.2f%%) threshold: " UINTX_FORMAT "%%",
                                  byte_size_in_proper_unit(reclaimable_bytes), proper_unit_for_byte_size(reclaimable_bytes),
                                  reclaimable_percent, G1HeapWastePercent);
        break;
      }

      double predicted_time_ms = predict_region_elapsed_time_ms(hr);
      time_remaining_ms = MAX2(time_remaining_ms - predicted_time_ms, 0.0);
      // Add regions to old set until we reach minimum amount
      if (old_region_length() < min_old_cset_length) {
        predicted_old_time_ms += predicted_time_ms;
        add_as_old(hr);
        // Record the number of regions added when no time remaining
        if (time_remaining_ms == 0.0) {
          expensive_region_num++;
        }
      } else {
        // In the non-auto-tuning case, we'll finish adding regions
        // to the CSet if we reach the minimum.
        if (!check_time_remaining) {
          log_debug(gc, ergo, cset)("Finish adding old regions to CSet (old CSet region num reached min).");
          break;
        }
        // Keep adding regions to old set until we reach optional threshold
        if (time_remaining_ms > optional_threshold_ms) {
          predicted_old_time_ms += predicted_time_ms;
          add_as_old(hr);
        } else if (time_remaining_ms > 0) {
          // Keep adding optional regions until time is up
          if (!optional_is_full()) {
            predicted_optional_time_ms += predicted_time_ms;
            add_as_optional(hr);
          } else {
            log_debug(gc, ergo, cset)("Finish adding old regions to CSet (optional set full).");
            break;
          }
        } else {
          log_debug(gc, ergo, cset)("Finish adding old regions to CSet (predicted time is too high).");
          break;
        }
      }
      hr = cset_chooser()->peek();
    }
    if (hr == NULL) {
      log_debug(gc, ergo, cset)("Finish adding old regions to CSet (candidate old regions not available)");
    }

    cset_chooser()->verify();
  }

  stop_incremental_building();

  log_debug(gc, ergo, cset)("Finish choosing CSet regions old: %u, optional: %u, "
                            "predicted old time: %1.2fms, predicted optional time: %1.2fms, time remaining: %1.2f",
                            old_region_length(), optional_region_length(),
                            predicted_old_time_ms, predicted_optional_time_ms, time_remaining_ms);
  if (expensive_region_num > 0) {
    log_debug(gc, ergo, cset)("CSet contains %u old regions that were added although the predicted time was too high.",
                              expensive_region_num);
  }

  double non_young_end_time_sec = os::elapsedTime();
  phase_times()->record_non_young_cset_choice_time_ms((non_young_end_time_sec - non_young_start_time_sec) * 1000.0);

  QuickSort::sort(_collection_set_regions, _collection_set_cur_length, compare_region_idx, true);
}

HeapRegion* G1OptionalCSet::region_at(uint index) {
  return _cset->optional_region_at(index);
}

void G1OptionalCSet::prepare_evacuation(double time_limit) {
  assert(_current_index == _current_limit, "Before prepare no regions should be ready for evac");

  uint prepared_regions = 0;
  double prediction_ms = 0;

  //mhr: modify

  _prepare_failed = true;
  for (uint i = _current_index; i < _cset->optional_region_length(); i++) {
    HeapRegion* hr = region_at(i);
    // This region will be included in the next optional evacuation.
    prepare_to_evacuate_optional_region(hr);
    prepared_regions++;
    _current_limit++;
    _prepare_failed = false;
  }


  // _prepare_failed = true;
  // for (uint i = _current_index; i < _cset->optional_region_length(); i++) {
  //   HeapRegion* hr = region_at(i);
  //   prediction_ms += _cset->predict_region_elapsed_time_ms(hr);
  //   if (prediction_ms > time_limit) {
  //     log_debug(gc, cset)("Prepared %u regions for optional evacuation. Predicted time: %.3fms", prepared_regions, prediction_ms);
  //     return;
  //   }

  //   // This region will be included in the next optional evacuation.
  //   prepare_to_evacuate_optional_region(hr);
  //   prepared_regions++;
  //   _current_limit++;
  //   _prepare_failed = false;
  // }



  log_debug(gc, cset)("Prepared all %u regions for optional evacuation. Predicted time: %.3fms",
                      prepared_regions, prediction_ms);
}

bool G1OptionalCSet::prepare_failed() {
  return _prepare_failed;
}

void G1OptionalCSet::complete_evacuation() {
  _evacuation_failed = false;
  for (uint i = _current_index; i < _current_limit; i++) {
    HeapRegion* hr = region_at(i);
    _cset->clear_optional_region(hr);
    if (hr->evacuation_failed()){
      _evacuation_failed = true;
    }
  }
  _current_index = _current_limit;
}

bool G1OptionalCSet::evacuation_failed() {
  return _evacuation_failed;
}

G1OptionalCSet::~G1OptionalCSet() {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  while (!is_empty()) {
    // We want to return regions not evacuated to the
    // chooser in reverse order to maintain the old order.
    HeapRegion* hr = _cset->remove_last_optional_region();
    assert(hr != NULL, "Should be valid region left");
    _pset->record_unused_optional_region(hr);
    g1h->old_set_add(hr);
    g1h->clear_in_cset(hr);
    hr->set_index_in_opt_cset(InvalidCSetIndex);
    _cset->cset_chooser()->push(hr);
  }
  _cset->free_optional_regions();
}

uint G1OptionalCSet::size() {
  return _cset->optional_region_length() - _current_index;
}

bool G1OptionalCSet::is_empty() {
  return size() == 0;
}

void G1OptionalCSet::prepare_to_evacuate_optional_region(HeapRegion* hr) {
  log_trace(gc, cset)("Adding region %u for optional evacuation", hr->hrm_index());
  G1CollectedHeap::heap()->clear_in_cset(hr);
  _cset->add_old_region(hr);
}

#ifdef ASSERT
class G1VerifyYoungCSetIndicesClosure : public HeapRegionClosure {
private:
  size_t _young_length;
  int* _heap_region_indices;
public:
  G1VerifyYoungCSetIndicesClosure(size_t young_length) : HeapRegionClosure(), _young_length(young_length) {
    _heap_region_indices = NEW_C_HEAP_ARRAY(int, young_length, mtGC);
    for (size_t i = 0; i < young_length; i++) {
      _heap_region_indices[i] = -1;
    }
  }
  ~G1VerifyYoungCSetIndicesClosure() {
    FREE_C_HEAP_ARRAY(int, _heap_region_indices);
  }

  virtual bool do_heap_region(HeapRegion* r) {
    const int idx = r->young_index_in_cset();

    assert(idx > -1, "Young index must be set for all regions in the incremental collection set but is not for region %u.", r->hrm_index());
    assert((size_t)idx < _young_length, "Young cset index too large for region %u", r->hrm_index());

    assert(_heap_region_indices[idx] == -1,
           "Index %d used by multiple regions, first use by region %u, second by region %u",
           idx, _heap_region_indices[idx], r->hrm_index());

    _heap_region_indices[idx] = r->hrm_index();

    return false;
  }
};

void G1CollectionSet::verify_young_cset_indices() const {
  assert_at_safepoint_on_vm_thread();

  G1VerifyYoungCSetIndicesClosure cl(_collection_set_cur_length);
  iterate(&cl);
}
#endif
