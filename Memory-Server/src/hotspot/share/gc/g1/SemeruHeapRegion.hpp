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

#ifndef SHARE_VM_GC_G1_SEMERU_HEAPREGION_HPP
#define SHARE_VM_GC_G1_SEMERU_HEAPREGION_HPP

//#include "gc/g1/g1BlockOffsetTable.hpp"
#include "gc/g1/g1HeapRegionTraceType.hpp"
#include "gc/g1/heapRegionTracer.hpp"
#include "gc/g1/heapRegionType.hpp"
#include "gc/g1/survRateGroup.hpp"
#include "gc/shared/ageTable.hpp"
#include "gc/shared/cardTable.hpp"
#include "gc/shared/spaceDecorator.hpp"
#include "utilities/macros.hpp"

// Semeru
#include "gc/g1/heapRegion.hpp"   // Reuse class in original heapRegion.hpp
#include "gc/shared/rdmaStructure.hpp"    // .hpp not to inlcude inline.hpp, will crash the include hierarchy
#include "gc/g1/g1ConcurrentMarkBitMap.hpp"
#include "gc/g1/g1SemeruBlockOffsetTable.hpp"   // g1SemeruBlockOffsetTable.inline.hpp -> SemeruHeapRegion.hpp -> g1SemeruBlockOffsetTable.hpp


// The inlcude order is that : HeapRegionSet include SemeruHeapRegion.
//                             SemeruHeapRegionManager.hpp include HeapRegionSet.
//    Don't make a circle here.
//#include "gc/g1/SemeruHeapRegionManager.hpp"   

// A SemeruHeapRegion is the smallest piece of a G1CollectedHeap that
// can be collected independently.

// NOTE: Although a SemeruHeapRegion is a Space, its
// Space::initDirtyCardClosure method must not be called.
// The problem is that the existence of this method breaks
// the independence of barrier sets from remembered sets.
// The solution is to remove this method from the definition
// of a Space.

// Each heap region is self contained. top() and end() can never
// be set beyond the end of the region. For humongous objects,
// the first region is a StartsHumongous region. If the humongous
// object is larger than a heap region, the following regions will
// be of type ContinuesHumongous. In this case the top() of the
// StartHumongous region and all ContinuesHumongous regions except
// the last will point to their own end. The last ContinuesHumongous
// region may have top() equal the end of object if there isn't
// room for filler objects to pad out to the end of the region.


class G1CMBitMap;
class G1IsAliveAndApplyClosure;
class HeapRegionRemSet;
class HeapRegionRemSetIterator;
class SemeruHeapRegionSetBase;
class nmethod;

// Semeru
class G1SemeruCollectedHeap;
class SemeruHeapRegion;


/**
 * Semeru SemeruHeapRegion.
 * 
 * There are 3 kinds of fields based on the sent direction.
 *    1) CPU server send first kind of fields to Memory server
 *    2) Memory server send 2nd kind of fields to CPU server
 *    3) CPU server and Memory server keep different values for same fields.
 * 
 *  
 * Based on the sent timmings, the fields are also divided into 2 types:
 *    1) Sent at the JVM initialziaiton, one time communication.
 *    2) Frequently sent/recieve during the execution.
 * 
 * For the fields  sent and received together, group them into a class.
 * For effiency, better to access data in cache line alignment.
 * 
 * 
 * For the TargetObjQueue
 *  
 *  CPU Server  - Producer 
 *     CPU server builds the TargetObjQueue from 3 roots. And send the TargetQueue to Memory sever at the end of each CPU server GC.
 *     First, from thread stack variables. This is done during CPU server GC.
 *     Second, Cross-Region references recoreded by the Post Write Barrier ?
 *     Third, the SATB buffer queue, recoreded by the Pre Write Barrier.
 *  
 *  Memory Server - Consumer 
 *     Receive the TargetObjQueue and use them as the scavenge roots.
 * 
 *  Moved the definition to taskqueue.hpp.
 *  Because these structures are used by multiple components.
 */




  //
  // 1) Fields stted by CPU server and needed by Memory server.
  //    CPU server needs to push (explicitly send) these data to Memory server proactivly. 
  //






class CPUToMemoryAtInit : public CHeapRDMAObj< CPUToMemoryAtInit, CPU_TO_MEM_AT_INIT_ALLOCTYPE>{

public:
  // 1.1) These fields are integrated into the super-instance.
  //
  // The index of this region in the heap region sequence.
  uint  _hrm_index;


  // Declare the  functions in SemeruHeapRegion directly.
  CPUToMemoryAtInit(uint hrm_index): _hrm_index(hrm_index) {  }



};


/**
 * Fields for GC purpose.
 * Synchronized before/after each CPU STW GC.
 *  
 */
class CPUToMemoryAtGC : public CHeapRDMAObj< CPUToMemoryAtGC, CPU_TO_MEM_AT_GC_ALLOCTYPE> {

public:


  // Free(Reserved, but not commited), Eden, Survivor, Old, Humonguous 
  // Only CPU server do the Region allocation.
  HeapRegionType _type;     

  // For a humongous region, region in which it starts.
  // This is only address value for the Region, 
  // Both CPU and Memory server both have this instance at the same address.
  SemeruHeapRegion* _humongous_start_region;

   
  // Fields used by the SemeruHeapRegionSetBase class and subclasses.
  // [?] what's these 2 fields used for ??
  SemeruHeapRegion*           _next;
  SemeruHeapRegion*           _prev;



  // Target object queue. Contains all the target objects of the cross-region references into current Region.
  // This queue is built by the CPU sever GC.
  // This queue is sent to Memory Server via the RDMA. 
  // It should be allocated in fixed address : defined in SEMERU_START_ADDR
  //
  // [x] Only allocate && initialize this queue in Semeru heap.
  //TargetObjQueue* _target_obj_queue;



  // functions

  CPUToMemoryAtGC(uint hrm_index):
   _type(),
   _humongous_start_region(NULL),
   _next(NULL),
   _prev(NULL)   
   { }


};




//
// 2) Fields setted by Memory server and needed by CPU server.
//    CPU server will swap in these pages on demand.


class MemoryToCPUAtGC : public CHeapRDMAObj< MemoryToCPUAtGC, MEM_TO_CPU_AT_GC_ALLOCTYPE>{

public:
  // this field identify this Region is already traced by the concurrent threads.
  // This value is setted by Semeru memory srver BUT also accessed by the CPU server.
  bool _cm_scanned;    


  volatile size_t        _marked_alive_bytes;  // Marked alive objects. [ Abandoned ? ]
  volatile double        _alive_ratio;         // Used to decide GC or not.

  //
  // functions
  //
  MemoryToCPUAtGC(uint hrm_index):
    _cm_scanned(false),
    _marked_alive_bytes(0),
    _alive_ratio(0.0)
  {

  }


  void reset() {
    _cm_scanned = 0;  // Should alrady be restted by CPU server.
    _marked_alive_bytes = 0;
    _alive_ratio = 0;
  }

};


/**
 * CPU <--> Memory server needs to communicate with each other.
 * e.g. Both CPU and Memory server can adjust the _top pointer.
 *  CPU Server : both Mutator and GC
 *  MS         : GC Compact
 * 
 */
class SyncBetweenMemoryAndCPU : public CHeapRDMAObj< SyncBetweenMemoryAndCPU, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>{

public:

  // override the value in G1SemeruContiguousSpace
  // Used by CPU server for object allocation for both Mutators and GC.
  HeapWord* volatile _top; 

  G1SemeruBlockOffsetTablePart _bot_part;  // [?] 1 byte for each card. To record the start object offset for each card.


  // When we need to retire an allocation region, while other threads
  // are also concurrently trying to allocate into it, we typically
  // allocate a dummy object at the end of the region to ensure that
  // no more allocations can take place in it. However, sometimes we
  // want to know where the end of the last "real" object we allocated
  // into the region was and this is what this keeps track.
  //
  // Will be rewritten by CPU server.
  // [?] in Semeru MS, we only need to know this value, but no need to set it?
  HeapWord* _pre_dummy_top;   

  // Cross Region reference update queue
  HashQueue* _cross_region_ref_update_queue; 
  BitQueue* _cross_region_ref_target_queue;    // the target oop bitmap


  SyncBetweenMemoryAndCPU(uint hrm_index, G1SemeruBlockOffsetTable* bot, SemeruHeapRegion* gsp) :
  _top(NULL),       // set in intialization
  _bot_part(bot,gsp), 
  _pre_dummy_top(NULL),
  _cross_region_ref_update_queue(NULL)
  { }

};















// The complicating factor is that BlockOffsetTable diverged
// significantly, and we need functionality that is only in the G1 version.
// So I copied that code, which led to an alternate G1 version of
// OffsetTableContigSpace.  If the two versions of BlockOffsetTable could
// be reconciled, then G1OffsetTableContigSpace could go away.

// The idea behind time stamps is the following. We want to keep track of
// the highest address where it's safe to scan objects for each region.
// This is only relevant for current GC alloc regions so we keep a time stamp
// per region to determine if the region has been allocated during the current
// GC or not. If the time stamp is current we report a scan_top value which
// was saved at the end of the previous GC for retained alloc regions and which is
// equal to the bottom for all other regions.
// There is a race between card scanners and allocating gc workers where we must ensure
// that card scanners do not read the memory allocated by the gc workers.
// In order to enforce that, we must not return a value of _top which is more recent than the
// time stamp. This is due to the fact that a region may become a gc alloc region at
// some point after we've read the timestamp value as being < the current time stamp.
// The time stamps are re-initialized to zero at cleanup and at Full GCs.
// The current scheme that uses sequential unsigned ints will fail only if we have 4b
// evacuation pauses between two cleanups, which is _highly_ unlikely.
class G1SemeruContiguousSpace: public CompactibleSpace {
  friend class VMStructs;
  //HeapWord* volatile _top;
 protected:
  // G1SemeruBlockOffsetTablePart _bot_part;  // [?] 1 byte for each card. To record the start object offset for each card.
  Mutex _par_alloc_lock;
  // // When we need to retire an allocation region, while other threads
  // // are also concurrently trying to allocate into it, we typically
  // // allocate a dummy object at the end of the region to ensure that
  // // no more allocations can take place in it. However, sometimes we
  // // want to know where the end of the last "real" object we allocated
  // // into the region was and this is what this keeps track.
  // HeapWord* _pre_dummy_top;

 public:
  G1SemeruContiguousSpace(G1SemeruBlockOffsetTable* bot);

  //void set_top(HeapWord* value) { _top = value; }
  //HeapWord* top() const { return _top; }

 protected:
  // Reset the G1SemeruContiguousSpace.
  virtual void initialize(MemRegion mr, bool clear_space, bool mangle_space);

  //HeapWord* volatile* top_addr() { return &_top; }


  // // Try to allocate at least min_word_size and up to desired_size from this Space.
  // // Returns NULL if not possible, otherwise sets actual_word_size to the amount of
  // // space allocated.
  // // This version assumes that all allocation requests to this Space are properly
  // // synchronized.
  // inline HeapWord* allocate_impl(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);
  // // Try to allocate at least min_word_size and up to desired_size from this Space.
  // // Returns NULL if not possible, otherwise sets actual_word_size to the amount of
  // // space allocated.
  // // This version synchronizes with other calls to par_allocate_impl().
  // inline HeapWord* par_allocate_impl(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);

 public:
  // void reset_after_compaction() { set_top(compaction_top()); }

  // size_t used() const { return byte_size(bottom(), top()); }
  // size_t free() const { return byte_size(top(), end()); }
  // bool is_free_block(const HeapWord* p) const { return p >= top(); }

  // MemRegion used_region() const { return MemRegion(bottom(), top()); }

  // void object_iterate(ObjectClosure* blk);
  // void safe_object_iterate(ObjectClosure* blk);

  // void mangle_unused_area() PRODUCT_RETURN;
  // void mangle_unused_area_complete() PRODUCT_RETURN;

  // See the comment above in the declaration of _pre_dummy_top for an
  // explanation of what it is.
  // void set_pre_dummy_top(HeapWord* pre_dummy_top) {
  //   assert(is_in(pre_dummy_top) && pre_dummy_top <= top(), "pre-condition");
  //   _pre_dummy_top = pre_dummy_top;
  // }
  // HeapWord* pre_dummy_top() {
  //   return (_pre_dummy_top == NULL) ? top() : _pre_dummy_top;
  // }
  // void reset_pre_dummy_top() { _pre_dummy_top = NULL; }

  // virtual void clear(bool mangle_space);

  // HeapWord* block_start(const void* p);
  // HeapWord* block_start_const(const void* p) const;

  // // Allocation (return NULL if full).  Assumes the caller has established
  // // mutually exclusive access to the space.
  // HeapWord* allocate(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);
  // // Allocation (return NULL if full).  Enforces mutual exclusion internally.
  // HeapWord* par_allocate(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);

  // virtual HeapWord* allocate(size_t word_size);
  // virtual HeapWord* par_allocate(size_t word_size);

  HeapWord* saved_mark_word() const { ShouldNotReachHere(); return NULL; }

  // // MarkSweep support phase3
  // virtual HeapWord* initialize_threshold();
  // virtual HeapWord* cross_threshold(HeapWord* start, HeapWord* end);

  // virtual void print() const;

  // void reset_bot() {
  //   _bot_part.reset_bot();
  // }

  // void print_bot_on(outputStream* out) {
  //   _bot_part.print_on(out);
  // }
};











/**
 * Tag: SemeruHeapRegion management handler.
 *      SemeruHeapRegion is also a CHeapObj, allocated into native memory. 
 * 
 * 
 * [?] Values of some fields are fixed, no need to update between CPU and memory server.
 *     But some fields' value may be only setted by one server and needed by another server.
 *     How to handle this case ??
 * 
 */
class SemeruHeapRegion: public G1SemeruContiguousSpace {
  friend class VMStructs;
  friend class SemeruHeapRegionManager; // Allocate & initialize its private field, target_oop_queue.
  // Allow scan_and_forward to call (private) overrides for auxiliary functions on this class
  template <typename SpaceType>
  friend void CompactibleSpace::scan_and_forward(SpaceType* space, CompactPoint* cp);
 
 // Degrade the protection from private to protected. 
 // Becasue we need to merge them together for update purpose.
 //private:
public:


  // RDMA communication structure, allocate into RDMA meta space
  CPUToMemoryAtInit   *_cpu_to_mem_init;
  CPUToMemoryAtGC     *_cpu_to_mem_gc;
  MemoryToCPUAtGC     *_mem_to_cpu_gc;
  SyncBetweenMemoryAndCPU   *_sync_mem_cpu;

  //
  // Other fields. No need to sync between CPU and memory server.
  //

  // Every SemeruHeapRegion should has its own _alive_bitmap.
  // Memory server CSet Regions' bitmap will be sent to CPU server for fields update.
  // CPU server only caches the necessary bitmap to save CPU local memory size.
  // So it's better to cut the bitmap into slices, one slice per SemeruHeapRegion.
  //
  // [x] Disable the prev_bitmap design by always setting the _prev_top_at_mark_start to the Region->_bottom. 
  //
  // [??] Change it to a pointer based instance.
  //      Because the CPU server doesn't need this field for all the Regions.
  //
  G1CMBitMap  _alive_bitmap;        // pointed by G1SemeruCMTask->_alive_bitmap.
  G1CMBitMap  _target_oop_bitmap;   // Points to _sync_mem_cpu->_cross_region_ref_target_queue->_target_bitmap
  bool        scan_failure;     // identify if the concurrent tracing is failed.

  // 1-sied RDMA write check flags
  // Points to FLAGS_OF_CPU_WRITE_CHECK_OFFSET, 4KB
  // 32 bytes for each tag High| -- DIRTY_TAG --|-- VERSION_TAG --|Low
  // Points to g1h->_rdma_write_check_flags[]
  volatile uint32_t* _write_check_flag;

  uint32_t  _version_tag; // store the version value, low 16 bits.


  //
  // Functions

  // get the low 16 bits
  inline uint32_t write_check_tag_version_val() { return *_write_check_flag & VERSION_MASK;  }
  // check the high 16 bits
  inline bool     write_check_tag_dirty()       { return  (*_write_check_flag >>  DIRTY_TAG_OFFSET);  }

  // Tracing start check
  inline void     store_write_verion_tag()      { _version_tag = *_write_check_flag & VERSION_MASK; }

  // Tracing end check
  inline bool   is_override_during_tracing()    { 
    if(write_check_tag_dirty())   // Condition#1, dirty_tag has to be 0.
      return true;
    if(write_check_tag_version_val() != _version_tag) // Condition#2, the _version_tag has to stay the same.
      return true;

    return false;
  }

 protected: 
  //
  // ############################# End of update fields section #############################
  //
  // The fields below are only used for CPU server or Memory server.
  // no need to communicate with each other.



  // The remembered set for this region.
  // (Might want to make this "inline" later, to avoid some alloc failure
  // issues.)
  // 
  // [x] Not needed by the Semeru memory server. 
  //
  HeapRegionRemSet* _rem_set;  //[x] Region Local RemSet



  // Auxiliary functions for scan_and_forward support.
  // See comments for CompactibleSpace for more information.
  inline HeapWord* scan_limit() const {
    return top();
  }

  inline bool scanned_block_is_obj(const HeapWord* addr) const {
    return true; // Always true, since scan_limit is top
  }

  inline size_t scanned_block_size(const HeapWord* addr) const {
    return SemeruHeapRegion::block_size(addr); // Avoid virtual call
  }

  void report_region_type_change(G1HeapRegionTraceType::Type to);

  // Returns whether the given object address refers to a dead object, and either the
  // size of the object (if live) or the size of the block (if dead) in size.
  // May
  // - only called with obj < top()
  // - not called on humongous objects or archive regions
  inline bool is_obj_dead_with_size(const oop obj, const G1CMBitMap* const prev_bitmap, size_t* size) const;

 protected:


  // True iff an attempt to evacuate an object in the region failed.
  bool _evacuation_failed;



#ifdef ASSERT
  SemeruHeapRegionSetBase* _containing_set;
#endif // ASSERT

  // We use concurrent marking to determine the amount of live data
  // in each heap region.
  size_t _prev_marked_bytes;    // Bytes known to be live via last completed marking.
  size_t _next_marked_bytes;    // Bytes known to be live via in-progress marking.

  // The calculated GC efficiency of the region.
  double _gc_efficiency;

  // The index in the optional regions array, if this region
  // is considered optional during a mixed collections.
  uint _index_in_opt_cset;
  int  _young_index_in_cset;
  SurvRateGroup* _surv_rate_group;
  int  _age_index;



  //  
  //
  // The start of the unmarked area. The unmarked area extends from this
  // word until the top and/or end of the region, and is the part
  // of the region for which no marking was done, i.e. objects may
  // have been allocated in this part since the last mark phase.
  // "prev" is the top at the start of the last completed marking.
  // "next" is the top at the start of the in-progress marking (if any.)
  HeapWord* _prev_top_at_mark_start;  // Keep  this value NULL to disable dual marking bitmap for Semeru MS.
  HeapWord* _next_top_at_mark_start;  // [??] Add this value into SyncBetweenMemoryAndCPU
  // If a collection pause is in progress, this is the top at the start
  // of that pause.

  void init_top_at_mark_start() {
    assert(_prev_marked_bytes == 0 &&
           _next_marked_bytes == 0,
           "Must be called after zero_marked_bytes.");
    HeapWord* bot = bottom();
    _prev_top_at_mark_start = bot;
    _next_top_at_mark_start = bot;
  }

  // Cached attributes used in the collection set policy information

  // The RSet length that was added to the total value
  // for the collection set.
  size_t _recorded_rs_length;

  // The predicted elapsed time that was added to total value
  // for the collection set.
  double _predicted_elapsed_time_ms;

  // Iterate over the references in a humongous objects and apply the given closure
  // to them.
  // Humongous objects are allocated directly in the old-gen. So we need special
  // handling for concurrent processing encountering an in-progress allocation.
  template <class Closure, bool is_gc_active>
  inline bool do_oops_on_card_in_humongous(MemRegion mr,
                                           Closure* cl,
                                           G1SemeruCollectedHeap* g1h);

  // Returns the block size of the given (dead, potentially having its class unloaded) object
  // starting at p extending to at most the prev TAMS using the given mark bitmap.
  inline size_t block_size_using_bitmap(const HeapWord* p, const G1CMBitMap* const prev_bitmap) const;

public:
  // Semeru Memory Server dedicated fields
  // These fields are only used by Memory server, no need to synchronize with CPU server.


  // Use StarTask queue to store the oop*, not the oop, who points to object in another Region.
  // objects contain inter-Region reference
  //G1CMBitMap             _inter_region_ref_bitmap;

  // a oop* (&field) queue,
  // This queue is easy for updating the field value.
  //
  // [x] Move to G1SemeruSTWCompactGangTask. This can save space.
  // 
  //OopStarTaskQueue      *_inter_region_ref_queue;   




 public:
  SemeruHeapRegion(uint hrm_index,
             G1SemeruBlockOffsetTable* bot,
             MemRegion mr);

  // Initializing the SemeruHeapRegion not only resets the data structure, but also
  // resets the BOT for that heap region.
  // The default values for clear_space means that we will do the clearing if
  // there's clearing to be done ourselves. We also always mangle the space.
  virtual void initialize(MemRegion mr, bool clear_space = false, bool mangle_space = SpaceDecorator::Mangle);

  // Initialize SemeruHeapRegion with alive/dest bitmap information.
  // [XX] If call this function, MUST assign the entire 4 parameters. 
  //      Because the size_t can be trated as bool.
  virtual void initialize(MemRegion mr, 
                            size_t region_index,
                            bool clear_space = false, 
                            bool mangle_space = SpaceDecorator::Mangle);

  //
  // Override basic fields
  //

  // Try to allocate at least min_word_size and up to desired_size from this Space.
  // Returns NULL if not possible, otherwise sets actual_word_size to the amount of
  // space allocated.
  // This version assumes that all allocation requests to this Space are properly
  // synchronized.
  inline HeapWord* allocate_impl(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);
  // Try to allocate at least min_word_size and up to desired_size from this Space.
  // Returns NULL if not possible, otherwise sets actual_word_size to the amount of
  // space allocated.
  // This version synchronizes with other calls to par_allocate_impl().
  inline HeapWord* par_allocate_impl(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);


  // MarkSweep support phase3
  virtual HeapWord* initialize_threshold();
  virtual HeapWord* cross_threshold(HeapWord* start, HeapWord* end);


  void mangle_unused_area() PRODUCT_RETURN;
  void mangle_unused_area_complete() PRODUCT_RETURN;

  void set_top(HeapWord* value) { _sync_mem_cpu->_top = value; }
  HeapWord* top() const { return _sync_mem_cpu->_top; }
  HeapWord* volatile* top_addr() { return &(_sync_mem_cpu->_top); }

  void object_iterate(ObjectClosure* blk);

  void safe_object_iterate(ObjectClosure* blk);
   

  void reset_after_compaction() { set_top(compaction_top()); }

  size_t used() const { return byte_size(bottom(), top()); }
  size_t free() const { return byte_size(top(), end()); }
  bool is_free_block(const HeapWord* p) const { return p >= top(); }

  MemRegion used_region() const { return MemRegion(bottom(), top()); }


  // Allocation (return NULL if full).  Assumes the caller has established
  // mutually exclusive access to the space.
  HeapWord* allocate(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);
  // Allocation (return NULL if full).  Enforces mutual exclusion internally.
  HeapWord* par_allocate(size_t min_word_size, size_t desired_word_size, size_t* actual_word_size);

  virtual HeapWord* allocate(size_t word_size);
  virtual HeapWord* par_allocate(size_t word_size);



  void set_pre_dummy_top(HeapWord* pre_dummy_top) {
    assert(is_in(pre_dummy_top) && pre_dummy_top <= top(), "pre-condition");
    _sync_mem_cpu->_pre_dummy_top = pre_dummy_top;
  }

  HeapWord* pre_dummy_top() {
    return (_sync_mem_cpu->_pre_dummy_top == NULL) ? top() : _sync_mem_cpu->_pre_dummy_top;
  }

  void reset_pre_dummy_top() { _sync_mem_cpu->_pre_dummy_top = NULL; }

  HeapWord* block_start(const void* p);
  HeapWord* block_start_const(const void* p) const;

  void reset_bot() {
    _sync_mem_cpu->_bot_part.reset_bot();
  }

  void print_bot_on(outputStream* out) {
    _sync_mem_cpu->_bot_part.print_on(out);
  }


  virtual void clear(bool mangle_space);

  // reset some fields after RDMA transfer.
  // Even CPU server and Memory server has same Region Index,
  // The address and structure of SemeruHeapRegion is different.
  // This is also true for some other structures.
  // We need to switch these structures after each transfer between CPU server and Memory server.
  void reset_fields_after_transfer(){
    // reset block offet table information.
    this->_sync_mem_cpu->_bot_part.reset_fields_after_transfer(this);
  }


  //
  // End of fields override.
  //

  void allocate_init_target_oop_bitmap(uint hrm_index);
  void allocate_init_cross_region_ref_update_queue(uint hrm_index);

  static int    SemeruLogOfHRGrainBytes;
  static int    SemeruLogOfHRGrainWords;

  static size_t SemeruGrainBytes;   // The Region size, decided in function SemeruHeapRegion::setup_heap_region_size
  static size_t SemeruGrainWords;      
  static size_t SemeruCardsPerRegion;  

  // get current Region's alive/dest bitmap
  G1CMBitMap* alive_bitmap()  { return &_alive_bitmap;  }

	bool is_marked_alive(oop obj) const {	 return _alive_bitmap.is_marked( obj);	}

  // allocate space for the alive/dest bitmap.
  G1RegionToSpaceMapper* create_alive_bitmap_storage(size_t region_idnex);
  G1RegionToSpaceMapper* create_target_oop_bitmap_storage(size_t region_idnex);


  static size_t align_up_to_region_byte_size(size_t sz) {
    return (sz + (size_t) SemeruGrainBytes - 1) &
                                      ~((1 << (size_t) SemeruLogOfHRGrainBytes) - 1);
  }


  // Returns whether a field is in the same region as the obj it points to.
  template <typename T>
  static bool is_in_same_region(T* p, oop obj) {
    assert(p != NULL, "p can't be NULL");
    assert(obj != NULL, "obj can't be NULL");
    return (((uintptr_t) p ^ cast_from_oop<uintptr_t>(obj)) >> SemeruLogOfHRGrainBytes) == 0;
  }

  static size_t max_region_size();
  static size_t min_region_size_in_words();

  // // It sets up the heap region size (SemeruGrainBytes / GrainWords), as
  // // well as other related fields that are based on the heap region
  // // size (SemeruLogOfHRGrainBytes / LogOfHRGrainWords /
  // // CardsPerRegion). All those fields are considered constant
  // // throughout the JVM's execution, therefore they should only be set
  // // up once during initialization time.
  // static void setup_heap_region_size(size_t initial_heap_size, size_t max_heap_size);

  // Semeru
  static void setup_semeru_heap_region_size(size_t initial_heap_size, size_t max_heap_size);

  // All allocated blocks are occupied by objects in a SemeruHeapRegion
  bool block_is_obj(const HeapWord* p) const;

  // Returns whether the given object is dead based on TAMS and bitmap.
  bool is_obj_dead(const oop obj, const G1CMBitMap* const prev_bitmap) const;

  // Returns the object size for all valid block starts
  // and the amount of unallocated words if called on top()
  size_t block_size(const HeapWord* p) const;

  // Scans through the region using the bitmap to determine what
  // objects to call size_t ApplyToMarkedClosure::apply(oop) for.
  template<typename ApplyToMarkedClosure>
  inline void apply_to_marked_objects(G1CMBitMap* bitmap, ApplyToMarkedClosure* closure);

  // Semeru memory server tracing
  // fault tolerance
  template<typename ApplyToMarkedClosure>
  inline void semeru_apply_to_marked_objects(G1CMBitMap* bitmap, ApplyToMarkedClosure* closure);

  // Override for scan_and_forward support.
  void prepare_for_compaction(CompactPoint* cp);
  // Update heap region to be consistent after compaction.
  void complete_compaction();

  inline HeapWord* par_allocate_no_bot_updates(size_t min_word_size, size_t desired_word_size, size_t* word_size);
  inline HeapWord* allocate_no_bot_updates(size_t word_size);
  inline HeapWord* allocate_no_bot_updates(size_t min_word_size, size_t desired_word_size, size_t* actual_size);

  // If this region is a member of a HeapRegionManager, the index in that
  // sequence, otherwise -1.
  uint hrm_index() const { return _cpu_to_mem_init->_hrm_index; }
  void set_hrm_index(uint hrm_index)  { _cpu_to_mem_init->_hrm_index = hrm_index; }

  // The number of bytes marked live in the region in the last marking phase.
  size_t marked_bytes()    { return _prev_marked_bytes; }
  size_t live_bytes() {
    return (top() - prev_top_at_mark_start()) * HeapWordSize + marked_bytes();
  }

  // // Semeru
  // template<typename ApplyToMarkedClosure>
  // inline void semeru_apply_to_marked_objects(G1CMBitMap* bitmap, ApplyToMarkedClosure* closure);

  size_t marked_alive_bytes() { return _mem_to_cpu_gc->_marked_alive_bytes; }
  void  add_to_marked_alive_bytes(size_t incr_bytes)  { _mem_to_cpu_gc->_marked_alive_bytes += incr_bytes;  }

  // The number of bytes counted in the next marking.
  size_t next_marked_bytes() { return _next_marked_bytes; }
  // The number of bytes live wrt the next marking.
  size_t next_live_bytes() {
    return
      (top() - next_top_at_mark_start()) * HeapWordSize + next_marked_bytes();
  }

  // A lower bound on the amount of garbage bytes in the region.
  size_t garbage_bytes() {
    size_t used_at_mark_start_bytes =
      (prev_top_at_mark_start() - bottom()) * HeapWordSize;
    return used_at_mark_start_bytes - marked_bytes();
  }

  // Return the amount of bytes we'll reclaim if we collect this
  // region. This includes not only the known garbage bytes in the
  // region but also any unallocated space in it, i.e., [top, end),
  // since it will also be reclaimed if we collect the region.
  size_t reclaimable_bytes() {
    size_t known_live_bytes = live_bytes();
    assert(known_live_bytes <= capacity(), "sanity");
    return capacity() - known_live_bytes;
  }

  // An upper bound on the number of live bytes in the region.
  size_t max_live_bytes() { return used() - garbage_bytes(); }

  void add_to_marked_bytes(size_t incr_bytes) {
    _next_marked_bytes = _next_marked_bytes + incr_bytes;
  }

  void zero_marked_bytes()      {
    _prev_marked_bytes = _next_marked_bytes = 0;
  }

  const char* get_type_str() const { return _cpu_to_mem_gc->_type.get_str(); }
  const char* get_short_type_str() const { return _cpu_to_mem_gc->_type.get_short_str(); }
  G1HeapRegionTraceType::Type get_trace_type() { return _cpu_to_mem_gc->_type.get_trace_type(); }

  bool is_free() const { return _cpu_to_mem_gc->_type.is_free(); }

  bool is_young()    const { return _cpu_to_mem_gc->_type.is_young();    }
  bool is_eden()     const { return _cpu_to_mem_gc->_type.is_eden();     }
  bool is_survivor() const { return _cpu_to_mem_gc->_type.is_survivor(); }

  bool is_humongous() const { return _cpu_to_mem_gc->_type.is_humongous(); }
  bool is_starts_humongous() const { return _cpu_to_mem_gc->_type.is_starts_humongous(); }
  bool is_continues_humongous() const { return _cpu_to_mem_gc->_type.is_continues_humongous();   }

  bool is_old() const { return _cpu_to_mem_gc->_type.is_old(); }

  bool is_old_or_humongous() const { return _cpu_to_mem_gc->_type.is_old_or_humongous(); }

  bool is_old_or_humongous_or_archive() const { return _cpu_to_mem_gc->_type.is_old_or_humongous_or_archive(); }

  // A pinned region contains objects which are not moved by garbage collections.
  // Humongous regions and archive regions are pinned.
  bool is_pinned() const { return _cpu_to_mem_gc->_type.is_pinned(); }

  // An archive region is a pinned region, also tagged as old, which
  // should not be marked during mark/sweep. This allows the address
  // space to be shared by JVM instances.
  bool is_archive()        const { return _cpu_to_mem_gc->_type.is_archive(); }
  bool is_open_archive()   const { return _cpu_to_mem_gc->_type.is_open_archive(); }
  bool is_closed_archive() const { return _cpu_to_mem_gc->_type.is_closed_archive(); }

  // For a humongous region, region in which it starts.
  SemeruHeapRegion* humongous_start_region() const {
    return _cpu_to_mem_gc->_humongous_start_region;
  }

  // Makes the current region be a "starts humongous" region, i.e.,
  // the first region in a series of one or more contiguous regions
  // that will contain a single "humongous" object.
  //
  // obj_top : points to the top of the humongous object.
  // fill_size : size of the filler object at the end of the region series.
  void set_starts_humongous(HeapWord* obj_top, size_t fill_size);

  // Makes the current region be a "continues humongous'
  // region. first_hr is the "start humongous" region of the series
  // which this region will be part of.
  void set_continues_humongous(SemeruHeapRegion* first_hr);

  // Unsets the humongous-related fields on the region.
  void clear_humongous();

  // If the region has a remembered set, return a pointer to it.
  HeapRegionRemSet* rem_set() const {
    return _rem_set;
  }


  // points to _sync_mem_cpu->_cross_region_ref_target_oop_queue
  G1CMBitMap* target_obj_queue() {
     return &_target_oop_bitmap;    
  }

  HashQueue* cross_region_ref_update_queue() const{
    return _sync_mem_cpu->_cross_region_ref_update_queue;
  }

  // Change this code to  check if a Region is in Memory Server CSet.
  //
  inline bool in_collection_set() const;



  // Methods used by the SemeruHeapRegionSetBase class and subclasses.

  // Getter and setter for the next and prev fields used to link regions into
  // linked lists.
  SemeruHeapRegion* next()              { return _cpu_to_mem_gc->_next; }
  SemeruHeapRegion* prev()              { return _cpu_to_mem_gc->_prev; }


  void set_next(SemeruHeapRegion* next) { _cpu_to_mem_gc->_next = next; }
  void set_prev(SemeruHeapRegion* prev) { _cpu_to_mem_gc->_prev = prev; }


  // Every region added to a set is tagged with a reference to that
  // set. This is used for doing consistency checking to make sure that
  // the contents of a set are as they should be and it's only
  // available in non-product builds.
#ifdef ASSERT
  void set_containing_set(SemeruHeapRegionSetBase* containing_set) {
    assert((containing_set == NULL && _containing_set != NULL) ||
           (containing_set != NULL && _containing_set == NULL),
           "containing_set: " PTR_FORMAT " "
           "_containing_set: " PTR_FORMAT,
           p2i(containing_set), p2i(_containing_set));

    _containing_set = containing_set;
  }

  SemeruHeapRegionSetBase* containing_set() { return _containing_set; }
#else // ASSERT
  void set_containing_set(SemeruHeapRegionSetBase* containing_set) { }

  // containing_set() is only used in asserts so there's no reason
  // to provide a dummy version of it.
#endif // ASSERT


  // Reset the SemeruHeapRegion to default values.
  // If skip_remset is true, do not clear the remembered set.
  // If clear_space is true, clear the SemeruHeapRegion's memory.
  // If locked is true, assume we are the only thread doing this operation.
  void hr_clear(bool skip_remset, bool clear_space, bool locked = false);
  // Clear the card table corresponding to this region.
  void clear_cardtable();

  // Get the start of the unmarked area in this region.
  // [?] meaning of these 2 tops ?
  //
  HeapWord* prev_top_at_mark_start() const { return _prev_top_at_mark_start; }
  HeapWord* next_top_at_mark_start() const { return _next_top_at_mark_start; }

  // Note the start or end of marking. This tells the heap region
  // that the collector is about to start or has finished (concurrently)
  // marking the heap.

  // Notify the region that concurrent marking is starting. Initialize
  // all fields related to the next marking info.
  inline void note_start_of_marking();

  // Notify the region that concurrent marking has finished. Copy the
  // (now finalized) next marking info fields into the prev marking
  // info fields.
  inline void note_end_of_marking();

  // Notify the region that we are about to start processing
  // self-forwarded objects during evac failure handling.
  void note_self_forwarding_removal_start(bool during_initial_mark,
                                          bool during_conc_mark);

  // Notify the region that we have finished processing self-forwarded
  // objects during evac failure handling.
  void note_self_forwarding_removal_end(size_t marked_bytes);

  void reset_during_compaction() {
    assert(is_humongous(),
           "should only be called for humongous regions");

    zero_marked_bytes();
    init_top_at_mark_start();
  }

  void calc_gc_efficiency(void);
  double gc_efficiency() { return _gc_efficiency;}

  uint index_in_opt_cset() const { return _index_in_opt_cset; }
  void set_index_in_opt_cset(uint index) { _index_in_opt_cset = index; }

  int  young_index_in_cset() const { return _young_index_in_cset; }
  void set_young_index_in_cset(int index) {
    assert( (index == -1) || is_young(), "pre-condition" );
    _young_index_in_cset = index;
  }

  int age_in_surv_rate_group() {
    assert( _surv_rate_group != NULL, "pre-condition" );
    assert( _age_index > -1, "pre-condition" );
    return _surv_rate_group->age_in_group(_age_index);
  }

  void record_surv_words_in_group(size_t words_survived) {
    assert( _surv_rate_group != NULL, "pre-condition" );
    assert( _age_index > -1, "pre-condition" );
    int age_in_group = age_in_surv_rate_group();
    _surv_rate_group->record_surviving_words(age_in_group, words_survived);
  }

  int age_in_surv_rate_group_cond() {
    if (_surv_rate_group != NULL)
      return age_in_surv_rate_group();
    else
      return -1;
  }

  SurvRateGroup* surv_rate_group() {
    return _surv_rate_group;
  }

  void install_surv_rate_group(SurvRateGroup* surv_rate_group) {
    assert( surv_rate_group != NULL, "pre-condition" );
    assert( _surv_rate_group == NULL, "pre-condition" );
    assert( is_young(), "pre-condition" );

    _surv_rate_group = surv_rate_group;
    _age_index = surv_rate_group->next_age_index();
  }

  void uninstall_surv_rate_group() {
    if (_surv_rate_group != NULL) {
      assert( _age_index > -1, "pre-condition" );
      assert( is_young(), "pre-condition" );

      _surv_rate_group = NULL;
      _age_index = -1;
    } else {
      assert( _age_index == -1, "pre-condition" );
    }
  }

  void set_free();

  void set_eden();
  void set_eden_pre_gc();
  void set_survivor();

  void move_to_old();
  void set_old();

  void set_open_archive();
  void set_closed_archive();

  // Determine if an object has been allocated since the last
  // mark performed by the collector. This returns true iff the object
  // is within the unmarked area of the region.
  bool obj_allocated_since_prev_marking(oop obj) const {
    return (HeapWord *) obj >= prev_top_at_mark_start();
  }

  /**
   * Tag : how can this be possible ?? 
   *    Allocated since next marking ???
   * 
   */
  bool obj_allocated_since_next_marking(oop obj) const {
    return (HeapWord *) obj >= next_top_at_mark_start();
  }

  // Returns the "evacuation_failed" property of the region.
  bool evacuation_failed() { return _evacuation_failed; }

  // Sets the "evacuation_failed" property of the region.
  void set_evacuation_failed(bool b) {
    _evacuation_failed = b;

    if (b) {
      _next_marked_bytes = 0;
    }
  }

  // Iterate over the objects overlapping part of a card, applying cl
  // to all references in the region.  This is a helper for
  // G1RemSet::refine_card*, and is tightly coupled with them.
  // mr is the memory region covered by the card, trimmed to the
  // allocated space for this region.  Must not be empty.
  // This region must be old or humongous.
  // Returns true if the designated objects were successfully
  // processed, false if an unparsable part of the heap was
  // encountered; that only happens when invoked concurrently with the
  // mutator.
  template <bool is_gc_active, class Closure>
  inline bool oops_on_card_seq_iterate_careful(MemRegion mr, Closure* cl);

  size_t recorded_rs_length() const        { return _recorded_rs_length; }
  double predicted_elapsed_time_ms() const { return _predicted_elapsed_time_ms; }

  void set_recorded_rs_length(size_t rs_length) {
    _recorded_rs_length = rs_length;
  }

  void set_predicted_elapsed_time_ms(double ms) {
    _predicted_elapsed_time_ms = ms;
  }

  // Routines for managing a list of code roots (attached to the
  // this region's RSet) that point into this heap region.
  void add_strong_code_root(nmethod* nm);
  void add_strong_code_root_locked(nmethod* nm);
  void remove_strong_code_root(nmethod* nm);

  // Applies blk->do_code_blob() to each of the entries in
  // the strong code roots list for this region
  void strong_code_roots_do(CodeBlobClosure* blk) const;


  //
  // The Semeru section
  //

  void set_region_cm_scanned()    { _mem_to_cpu_gc->_cm_scanned = true;  }
  void reset_region_cm_scanned()  { _mem_to_cpu_gc->_cm_scanned = false;  }
  bool is_region_cm_scanned()     { return _mem_to_cpu_gc->_cm_scanned; }

  void    set_alive_words(size_t words)  { _mem_to_cpu_gc->_marked_alive_bytes = words;  }
  size_t  alive_words()                  { return _mem_to_cpu_gc->_marked_alive_bytes; }  // abandoned ?
  
  void    set_alive_ratio(double ratio)  {  _mem_to_cpu_gc->_alive_ratio = ratio;  }
  double  alive_ratio()                  { return _mem_to_cpu_gc->_alive_ratio;  }  


  // Verify that the entries on the strong code root list for this
  // region are live and include at least one pointer into this region.
  void verify_strong_code_roots(VerifyOption vo, bool* failures) const;

  void print() const;
  void print_on(outputStream* st) const;

  // vo == UsePrevMarking -> use "prev" marking information,
  // vo == UseNextMarking -> use "next" marking information
  // vo == UseFullMarking -> use "next" marking bitmap but no TAMS
  //
  // NOTE: Only the "prev" marking information is guaranteed to be
  // consistent most of the time, so most calls to this should use
  // vo == UsePrevMarking.
  // Currently, there is only one case where this is called with
  // vo == UseNextMarking, which is to verify the "next" marking
  // information at the end of remark.
  // Currently there is only one place where this is called with
  // vo == UseFullMarking, which is to verify the marking during a
  // full GC.
  void verify(VerifyOption vo, bool *failures) const;

  // Override; it uses the "prev" marking information
  virtual void verify() const;

  void verify_rem_set(VerifyOption vo, bool *failures) const;
  void verify_rem_set() const;



  //
  // Debug functions
  //void check_target_obj_queue(const char* message);

  void check_cross_region_reg_queue( const char* message);

};



// SemeruHeapRegionClosure is used for iterating over regions.
// Terminates the iteration when the "do_heap_region" method returns "true".
class SemeruHeapRegionClosure : public StackObj {
  friend class G1CollectionSet;         // [?] do we need this ?
  friend class CollectionSetChooser;
  friend class SemeruHeapRegionManager; // to access its private fields.

  bool _is_complete;
  void set_incomplete() { _is_complete = false; }

 public:
  SemeruHeapRegionClosure(): _is_complete(true) {}

  // Typically called on each region until it returns true.
  // [?] Let the sub-class to define its own operations on a SemeruHeapRegion ? 
  //
  virtual bool do_heap_region(SemeruHeapRegion* r) = 0;

  // True after iteration if the closure was applied to all heap regions
  // and returned "false" in all cases.
  bool is_complete() { return _is_complete; }
};

#endif // SHARE_VM_GC_G1_HEAPREGION_HPP
