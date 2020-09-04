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

#ifndef SHARE_VM_GC_G1_SEMERU_G1CONCURRENTMARK_HPP
#define SHARE_VM_GC_G1_SEMERU_G1CONCURRENTMARK_HPP

#include "gc/g1/g1ConcurrentMarkBitMap.hpp"
//#include "gc/g1/g1ConcurrentMarkObjArrayProcessor.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1RegionMarkStatsCache.hpp"
//#include "gc/g1/heapRegionSet.hpp"
#include "gc/shared/taskqueue.hpp"
#include "memory/allocation.hpp"

// Semeru headers
#include "gc/g1/SemeruHeapRegionSet.hpp"
#include "gc/g1/g1SemeruConcurrentMarkObjArrayProcessor.hpp"

class ConcurrentGCTimer;
class G1CMOopClosure;
class G1OldTracer;
class G1RegionToSpaceMapper;
class G1SurvivorRegions;

// Semeru
class G1SemeruCMTask;
class G1SemeruCollectedHeap;
class G1SemeruConcurrentMark;
class G1SemeruConcurrentMarkThread;
class G1SemeruCMOopClosure;

// MS Compact
class G1SemeruSTWCompact;


#ifdef _MSC_VER
#pragma warning(push)
// warning C4522: multiple assignment operators specified
#pragma warning(disable:4522)
#endif

/** 
 * This is a container class for either an oop or a continuation address for
 * mark stack entries. Both are pushed onto the mark stack.
 *
 * [?] Why do we need such a wrapper for the oop ? 
 *    => To unify both the oop and obj_array slice.
 * 
 */
class G1SemeruTaskQueueEntry {
private:
  void* _holder;

  static const uintptr_t ArraySliceBit = 1; //[x] the flag bit. If it's a ArraySlice, attach this bit at the end of the _holder.

  G1SemeruTaskQueueEntry(oop obj) : _holder(obj) {
    assert(_holder != NULL, "Not allowed to set NULL task queue element");
  }
  G1SemeruTaskQueueEntry(HeapWord* addr) : _holder((void*)((uintptr_t)addr | ArraySliceBit)) { }
public:
  G1SemeruTaskQueueEntry(const G1SemeruTaskQueueEntry& other) { _holder = other._holder; }
  G1SemeruTaskQueueEntry() : _holder(NULL) { }

  static G1SemeruTaskQueueEntry from_slice(HeapWord* what) { return G1SemeruTaskQueueEntry(what); }
  static G1SemeruTaskQueueEntry from_oop(oop obj) { return G1SemeruTaskQueueEntry(obj); }

  G1SemeruTaskQueueEntry& operator=(const G1SemeruTaskQueueEntry& t) {
    _holder = t._holder;
    return *this;
  }

  volatile G1SemeruTaskQueueEntry& operator=(const volatile G1SemeruTaskQueueEntry& t) volatile {
    _holder = t._holder;
    return *this;
  }

  oop obj() const {
   // assert(!is_array_slice(), "Trying to read array slice " PTR_FORMAT " as oop", p2i(_holder));
    return (oop)_holder;
  }

  HeapWord* slice() const {
   // assert(is_array_slice(), "Trying to read oop " PTR_FORMAT " as array slice", p2i(_holder));
    return (HeapWord*)((uintptr_t)_holder & ~ArraySliceBit);
  }

  // semeru
  // Warning: if the _holder points to a object array slice, the last bit is setted to 1.
  char* holder_addr() const {
    return (char*)_holder;
  }

  bool is_oop() const { return !is_array_slice(); }
  bool is_array_slice() const { return ((uintptr_t)_holder & ArraySliceBit) != 0; }
  bool is_null() const { return _holder == NULL; }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef GenericTaskQueue<G1SemeruTaskQueueEntry, mtGC> G1SemeruCMTaskQueue;
typedef GenericTaskQueueSet<G1SemeruCMTaskQueue, mtGC> G1SemeruCMTaskQueueSet;

// Closure used by CM during concurrent reference discovery
// and reference processing (during remarking) to determine
// if a particular object is alive. It is primarily used
// to determine if referents of discovered reference objects
// are alive. An instance is also embedded into the
// reference processor as the _is_alive_non_header field
class G1SemeruCMIsAliveClosure : public BoolObjectClosure {
  G1SemeruCollectedHeap *_semeru_h;

public:
  G1SemeruCMIsAliveClosure(G1SemeruCollectedHeap* semeru_h) : _semeru_h(semeru_h) {}

  //implement the virtual function.
  bool do_object_b(oop obj);
};


class G1SemeruCMSubjectToDiscoveryClosure : public BoolObjectClosure {
  G1SemeruCollectedHeap *_semeru_h;

public:
  G1SemeruCMSubjectToDiscoveryClosure(G1SemeruCollectedHeap* semeru_h) : _semeru_h(semeru_h) {}

  // Implement the pure virtual function.
  bool do_object_b(oop obj);
};




// Represents the overflow mark stack used by concurrent marking.
//
// Stores oops in a huge buffer in virtual memory that is always fully committed.
// Resizing may only happen during a STW pause when the stack is empty.
//
// Memory is allocated on a "chunk" basis, i.e. a set of oops. For this, the mark
// stack memory is split into evenly sized chunks of oops. Users can only
// add or remove entries on that basis.
// Chunks are filled in increasing address order. Not completely filled chunks
// have a NULL element as a terminating element.
//
// Every chunk has a header containing a single pointer element used for memory
// management. This wastes some space, but is negligible (< .1% with current sizing).
//
// Memory management is done using a mix of tracking a high water-mark indicating
// that all chunks at a lower address are valid chunks, and a singly linked free
// list connecting all empty chunks.
class G1SemeruCMMarkStack {
public:
  // Number of TaskQueueEntries that can fit in a single chunk.
  static const size_t EntriesPerChunk = 8192 - 1 /* One reference for the next pointer */;
private:
  struct TaskQueueEntryChunk {
    TaskQueueEntryChunk* next;
    G1SemeruTaskQueueEntry data[EntriesPerChunk];
  };

  size_t _max_chunk_capacity;    // Maximum number of TaskQueueEntryChunk elements on the stack.

  TaskQueueEntryChunk* _base;    // Bottom address of allocated memory area.
  size_t _chunk_capacity;        // Current maximum number of TaskQueueEntryChunk elements.

  char _pad0[DEFAULT_CACHE_LINE_SIZE];
  TaskQueueEntryChunk* volatile _free_list;  // Linked list of free chunks that can be allocated by users.
  char _pad1[DEFAULT_CACHE_LINE_SIZE - sizeof(TaskQueueEntryChunk*)];
  TaskQueueEntryChunk* volatile _chunk_list; // List of chunks currently containing data.
  volatile size_t _chunks_in_chunk_list;
  char _pad2[DEFAULT_CACHE_LINE_SIZE - sizeof(TaskQueueEntryChunk*) - sizeof(size_t)];

  volatile size_t _hwm;          // High water mark within the reserved space.
  char _pad4[DEFAULT_CACHE_LINE_SIZE - sizeof(size_t)];

  // Allocate a new chunk from the reserved memory, using the high water mark. Returns
  // NULL if out of memory.
  TaskQueueEntryChunk* allocate_new_chunk();

  // Atomically add the given chunk to the list.
  void add_chunk_to_list(TaskQueueEntryChunk* volatile* list, TaskQueueEntryChunk* elem);
  // Atomically remove and return a chunk from the given list. Returns NULL if the
  // list is empty.
  TaskQueueEntryChunk* remove_chunk_from_list(TaskQueueEntryChunk* volatile* list);

  void add_chunk_to_chunk_list(TaskQueueEntryChunk* elem);
  void add_chunk_to_free_list(TaskQueueEntryChunk* elem);

  TaskQueueEntryChunk* remove_chunk_from_chunk_list();
  TaskQueueEntryChunk* remove_chunk_from_free_list();

  // Resizes the mark stack to the given new capacity. Releases any previous
  // memory if successful.
  bool resize(size_t new_capacity);

 public:
  G1SemeruCMMarkStack();
  ~G1SemeruCMMarkStack();

  // Alignment and minimum capacity of this mark stack in number of oops.
  static size_t capacity_alignment();

  // Allocate and initialize the mark stack with the given number of oops.
  bool initialize(size_t initial_capacity, size_t max_capacity);

  // Pushes the given buffer containing at most EntriesPerChunk elements on the mark
  // stack. If less than EntriesPerChunk elements are to be pushed, the array must
  // be terminated with a NULL.
  // Returns whether the buffer contents were successfully pushed to the global mark
  // stack.
  bool par_push_chunk(G1SemeruTaskQueueEntry* buffer);

  // Pops a chunk from this mark stack, copying them into the given buffer. This
  // chunk may contain up to EntriesPerChunk elements. If there are less, the last
  // element in the array is a NULL pointer.
  bool par_pop_chunk(G1SemeruTaskQueueEntry* buffer);

  // Return whether the chunk list is empty. Racy due to unsynchronized access to
  // _chunk_list.
  bool is_empty() const { return _chunk_list == NULL; }

  size_t capacity() const  { return _chunk_capacity; }

  // Expand the stack, typically in response to an overflow condition
  void expand();

  // Return the approximate number of oops on this mark stack. Racy due to
  // unsynchronized access to _chunks_in_chunk_list.
  size_t size() const { return _chunks_in_chunk_list * EntriesPerChunk; }

  void set_empty();

  // Apply Fn to every oop on the mark stack. The mark stack must not
  // be modified while iterating.
  template<typename Fn> void iterate(Fn fn) const PRODUCT_RETURN;
};

// Root Regions are regions that contain objects from nTAMS to top. These are roots
// for marking, i.e. their referenced objects must be kept alive to maintain the
// SATB invariant.
// We could scan and mark them through during the initial-mark pause, but for
// pause time reasons we move this work to the concurrent phase.
// We need to complete this procedure before the next GC because it might determine
// that some of these "root objects" are dead, potentially dropping some required
// references.
// Root regions comprise of the complete contents of survivor regions, and any
// objects copied into old gen during GC.
// class G1SemeruCMRootRegions {
//   SemeruHeapRegion** _root_regions;
//   size_t const _max_regions;

//   volatile size_t _num_root_regions; // Actual number of root regions.

//   volatile size_t _claimed_root_regions; // Number of root regions currently claimed.

//   volatile bool _scan_in_progress;
//   volatile bool _should_abort;

//   void notify_scan_done();

// public:
//   G1SemeruCMRootRegions(uint const max_regions);
//   ~G1SemeruCMRootRegions();

//   // Reset the data structure to allow addition of new root regions.
//   void reset();

//   void add(SemeruHeapRegion* hr);

//   // Reset the claiming / scanning of the root regions.
//   void prepare_for_scan();

//   // Forces get_next() to return NULL so that the iteration aborts early.
//   void abort() { _should_abort = true; }

//   // Return true if the CM thread are actively scanning root regions,
//   // false otherwise.
//   bool scan_in_progress() { return _scan_in_progress; }

//   // Claim the next root region to scan atomically, or return NULL if
//   // all have been claimed.
//   SemeruHeapRegion* claim_next();

//   // The number of root regions to scan.
//   uint num_root_regions() const;

//   void cancel_scan();

//   // Flag that we're done with root region scanning and notify anyone
//   // who's waiting on it. If aborted is false, assume that all regions
//   // have been claimed.
//   void scan_finished();

//   // If CM threads are still scanning root regions, wait until they
//   // are done. Return true if we had to wait, false otherwise.
//   bool wait_until_scan_finished();
// };


/**
 * Semeru Memory Server - Regions received from CPU server.
 *  2 categories 
 *    1) CM Scanned Regions, already scanned by CM. Remark adn evacuate the regions during STW window.
 *    2) Freshly evicted Regions, apply CM on these Regions.
 * 
 * Work as a linked cycle:
 * 	[ empty slots -- _bottom(first filed) -- filled slots -- _top(first available)-- empty slots   ]
 * 
 * e.g. 
 * 	content : _freshly_evicted_regions[]
 * 	_bottom : _claimed_freshly_evicted_regions
 *  _top		: _num_freshly_evicted_regions
 * 
 * 
 * This Structure is only built and used in Semeru Memory Server.
 *    This structure is built from the region index set from CPU server.
 */
class G1SemeruCMCSetRegions {
  SemeruHeapRegion** _cm_scanned_regions;
  SemeruHeapRegion** _freshly_evicted_regions;  

  //size_t const _max_cm_scanned_regions;
  //size_t const _max_freshly_evicted_regions;
  size_t const _max_regions;    // both cm_scanned and freshly_evicted use the same value.

  volatile size_t _num_cm_scanned_regions; // Actual number of cm scanned regions. Works as  _top, the first available slot.
  volatile size_t _num_freshly_evicted_regions;   // Actual number of freshly evicted regions.

  volatile size_t _claimed_cm_scanned_regions;     // Works as _bottom, the first filled slot.
  volatile size_t _claimed_freshly_evicted_regions; // Number of root regions currently claimed.

  volatile bool _compact_in_progress;
  volatile bool _scan_in_progress;

  volatile bool _should_abort_compact;
  volatile bool _should_abort_scan;

  void notify_compact_done();     // [?] Notify the CPU server, STW compaction is done, can trigger the field update phase.
  void notify_scan_done();        // [?] Do we need this ?

public:
   G1SemeruCMCSetRegions(uint const max_regions);
   ~G1SemeruCMCSetRegions();

  // Reset the data structure to allow addition of new root regions.
  void reset();

  bool already_enqueued_scanned(size_t enqueue_region_index);
  bool already_enqueued_freshly_evicted(size_t enqueue_region_index);

  void add_cm_scanned_regions(SemeruHeapRegion* hr);
  void add_freshly_evicted_regions(SemeruHeapRegion* hr);

  // Reset the claiming / scanning of the root regions.
  void prepare_for_compact();
  void prepare_for_scan();

  // Forces get_next() to return NULL so that the iteration aborts early.
  void abort_compact() { _should_abort_compact = true; }
  void abort_scan()    { _should_abort_scan = true; }


  bool compact_in_progress()  { return _compact_in_progress;  }
  // Return true if the CM thread are actively scanning root regions,
  // false otherwise.
  bool scan_in_progress()     { return _scan_in_progress; }

  // Claim the next root region to scan atomically, or return NULL if
  // all have been claimed.
  SemeruHeapRegion* claim_cm_scanned_next();
  SemeruHeapRegion* claim_freshly_evicted_next();

  // The number of root regions to scan.
  size_t num_cm_scanned_regions() const;
  size_t num_freshly_evicted_regions() const;

  bool is_compact_finished();
  bool is_cm_scan_finished();

  void cancel_compact();
  void cancel_scan();

  // Flag that we're done with root region scanning and notify anyone
  // who's waiting on it. If aborted is false, assume that all regions
  // have been claimed.
  void compact_finished();
  void scan_finished();

  // If CM threads are still scanning root regions, wait until they
  // are done. Return true if we had to wait, false otherwise.
  bool wait_until_compact_finished();
  bool wait_until_scan_finished();


  // Estimate the destination Region for each Scanned Region in a fast way.
  // void estimate_dset_region_for_scanned_cset();

};







/** 
 * This class manages data structures and methods for doing liveness analysis in
 * G1's Semeru concurrent cycle.
 *
 */
class G1SemeruConcurrentMark : public CHeapObj<mtGC> {
  friend class G1SemeruConcurrentMarkThread;          // [?] Change to Semeru thread
  friend class G1SemeruCMRefProcTaskProxy;
  friend class G1SemeruCMRefProcTaskExecutor;         // To access the pricate field.
  friend class G1SemeruCMKeepAliveAndDrainClosure;
  friend class G1SemeruCMDrainMarkingStackClosure;
  friend class G1CMBitMapClosure;                     // [?] Create a Semeru version ?     
  friend class G1SemeruCMConcurrentMarkingTask;
  friend class G1SemeruCMRemarkTask;
  friend class G1SemeruCMTask;

  // For STW Compact
  friend class G1SemeruSTWCompact;    // Need to use G1SemeruConcurrentMark to initialize itself
  



  // [?] Seems that G1SemeruConcurrentMarkThread is only a manager of all the concurrent threads.
  //     The real concurrent threads are stored in _concurrent_workers.
  G1SemeruConcurrentMarkThread*     _semeru_cm_thread;    // The manager of all the concurrent threads
  G1SemeruCollectedHeap*            _semeru_h;            // The heap
  
  bool                              _completed_initialization; // Set to true when initialization is complete

  // [xx] Semeru abandons these bitmao now. [xx]
  // Concurrent marking support structures
  // These 2 bitmap covered the whole heap region
  G1CMBitMap              _mark_bitmap_1;    //the real content of _prev/next_mark_bitmap ?
  G1CMBitMap              _mark_bitmap_2;
  G1CMBitMap*             _prev_mark_bitmap; // Completed mark bitmap
  G1CMBitMap*             _next_mark_bitmap; // Under-construction mark bitmap


  // Semeru memory sever

  // Heap bounds
  MemRegion const         _heap;

  // Root region tracking and claiming
  // [?] Initial phase add regions into the Root Region ??
  //
  // G1SemeruCMRootRegions   _root_regions;
  G1SemeruCMCSetRegions   _mem_server_cset; // also pointed by G1SemeruSTWCompact->_mem_server_cset

  // For grey objects

  // The global sermeru_task queue for G1SemeruCMTask->_semeru_task_queue, store the overflowed object waiting to be scanned.
  G1SemeruCMMarkStack     _global_mark_stack; // Grey objects behind global finger

  // [!!] Semeru memory server also uses the _finger to control the CSet Region claiming.
  HeapWord* volatile      _finger;            // The global finger, region aligned,
                                              // always pointing to the end of the
                                              // last claimed region

  uint                    _worker_id_offset;  // [?] which worker's offset ?
  uint                    _max_num_tasks;    // Maximum number of semeru concurrent tasks
  uint                    _num_active_tasks; // Number of tasks currently active

  // Semeru CM Tasks,
  // The CMTask define what operations need to be done by the attached CM Thread.
  G1SemeruCMTask**              _tasks;         // Task queue array (max_worker_id length)

  G1SemeruCMTaskQueueSet*       _task_queues;   // [?]Task queue set, each G1SemeruCMTask->_task_queue points  here ?

  // [?] What's the working mechanism for this terminator ?
  //  for example, synchronize multiple user workers ? do they share the variables in the TaskTerminator ?
  TaskTerminator                _terminator;    // For termination

  // Two sync barriers that are used to synchronize tasks when an
  // overflow occurs. The algorithm is the following. All tasks enter
  // the first one to ensure that they have all stopped manipulating
  // the global data structures. After they exit it, they re-initialize
  // their data structures and task 0 re-initializes the global data
  // structures. Then, they enter the second sync barrier. This
  // ensure, that no task starts doing work before all data
  // structures (local and global) have been re-initialized. When they
  // exit it, they are free to start working again.
  //
  // [?] What does the overflow mean ?? 
  //    transfer task_entry from local to global marking stack ??
  //
  //
  WorkGangBarrierSync     _first_overflow_barrier_sync;      
  WorkGangBarrierSync     _second_overflow_barrier_sync;

  // This is set by any task, when an overflow on the global data
  // structures is detected
  volatile bool           _has_overflown;
  // True: marking is concurrent, false: we're in remark
  volatile bool           _concurrent;
  // Set at the end of a Full GC so that marking aborts
  volatile bool           _has_aborted;

  // Used when remark aborts due to an overflow to indicate that
  // another concurrent marking phase should start
  volatile bool           _restart_for_overflow;

  ConcurrentGCTimer*      _gc_timer_cm;     // A timer to record the elapsed time for each concurrent phase.

  G1OldTracer*            _gc_tracer_cm;    // [?] G1 Old space logging systems

  // Timing statistics. All of them are in ms
  NumberSeq _init_times;
  NumberSeq _remark_times;
  NumberSeq _remark_mark_times;
  NumberSeq _remark_weak_ref_times;
  NumberSeq _cleanup_times;
  double    _total_cleanup_time;

  double*   _accum_task_vtime;   // Accumulated task vtime

  WorkGang* _concurrent_workers;     // [x] The real threads to execute the workload. Execute the G1SemeruCMTask.
  uint      _num_concurrent_workers; // The number of marking worker threads we're using
  uint      _max_concurrent_workers; // Maximum number of marking worker threads

  void verify_during_pause(G1HeapVerifier::G1VerifyType type, VerifyOption vo, const char* caller);

  void finalize_marking();

  void weak_refs_work_parallel_part(BoolObjectClosure* is_alive, bool purged_classes);
  void weak_refs_work(bool clear_all_soft_refs);

  void report_object_count(bool mark_completed);

  void swap_mark_bitmaps();

  void reclaim_empty_regions();

  // After reclaiming empty regions, update heap sizes.
  void compute_new_sizes();

  // Clear statistics gathered during the concurrent cycle for the given region after
  // it has been reclaimed.
  void clear_statistics(SemeruHeapRegion* r);

  // Resets the global marking data structures, as well as the
  // task local ones; should be called during initial mark.
  void reset();

  // Resets all the marking data structures. Called when we have to restart
  // marking or when marking completes (via set_non_marking_state below).
  void reset_marking_for_restart();

  // We do this after we're done with marking so that the marking data
  // structures are initialized to a sensible and predictable state.
  void reset_at_marking_complete();

  // Called to indicate how many threads are currently active.
  void set_concurrency(uint active_tasks);

  // Should be called to indicate which phase we're in (concurrent
  // mark or remark) and how many threads are currently active.
  void set_concurrency_and_phase(uint active_tasks, bool concurrent);

  // Prints all gathered CM-related statistics
  void print_stats();

  HeapWord*               finger()           { return _finger;   }
  bool                    concurrent()       { return _concurrent; }
  uint                    active_tasks()     { return _num_active_tasks; }
  ParallelTaskTerminator* terminator() const { return _terminator.terminator(); }

  // Claims the next available region to be scanned by a marking
  // task/thread. It might return NULL if the next region is empty or
  // we have run out of regions. In the latter case, out_of_regions()
  // determines whether we've really run out of regions or the task
  // should call claim_region() again. This might seem a bit
  // awkward. Originally, the code was written so that claim_region()
  // either successfully returned with a non-empty region or there
  // were no more regions to be claimed. The problem with this was
  // that, in certain circumstances, it iterated over large chunks of
  // the heap finding only empty regions and, while it was working, it
  // was preventing the calling task to call its regular clock
  // method. So, this way, each task will spend very little time in
  // claim_region() and is allowed to call the regular clock method
  // frequently.
  //SemeruHeapRegion* claim_region(uint worker_id);

  SemeruHeapRegion* claim_region(uint worker_id);


  // Determines whether we've run out of regions to scan. Note that
  // the finger can point past the heap end in case the heap was expanded
  // to satisfy an allocation without doing a GC. This is fine, because all
  // objects in those regions will be considered live anyway because of
  // SATB guarantees (i.e. their TAMS will be equal to bottom).
  bool out_of_regions() { return _finger >= _heap.end(); }


  // Semeru Memory Server
  // If the regions in memory server CSet are all processed.
  bool out_of_memory_server_cset()  {
    // both cm_scanned and freshly_evicted regions are empty.
    return (mem_server_cset()->is_compact_finished() && mem_server_cset()->is_cm_scan_finished() );
  }

  // Returns the task with the given id
  G1SemeruCMTask* task(uint id) {
    // During initial mark we use the parallel gc threads to do some work, so
    // we can only compare against _max_num_tasks.
    assert(id < _max_num_tasks, "Task id %u not within bounds up to %u", id, _max_num_tasks);
    return _tasks[id];
  }

  // Access / manipulation of the overflow flag which is set to
  // indicate that the global stack has overflown
  bool has_overflown()           { return _has_overflown; }
  void set_has_overflown()       { _has_overflown = true; }
  void clear_has_overflown()     { _has_overflown = false; }
  bool restart_for_overflow()    { return _restart_for_overflow; }

  // Methods to enter the two overflow sync barriers
  void enter_first_sync_barrier(uint worker_id);
  void enter_second_sync_barrier(uint worker_id);

  // Clear the given bitmap in parallel using the given WorkGang. If may_yield is
  // true, periodically insert checks to see if this method should exit prematurely.
  void clear_bitmap(G1CMBitMap* bitmap, WorkGang* workers, bool may_yield);

  // Region statistics gathered during marking.
  // This is the global RegionMarkStats, <Region_id, live_words>, for all the G1SemeruCMTask.
  G1RegionMarkStats* _region_mark_stats;
  
  // Top pointer for each region at the start of the rebuild remembered set process
  // for regions which remembered sets need to be rebuilt. A NULL for a given region
  // means that this region does not be scanned during the rebuilding remembered
  // set phase at all.
  HeapWord* volatile* _top_at_rebuild_starts;
public:
  void add_to_liveness(uint worker_id, oop const obj, size_t size);
  // Liveness of the given region as determined by concurrent marking, i.e. the amount of
  // live words between bottom and nTAMS.
  size_t liveness(uint region) const { return _region_mark_stats[region]._live_words; }

  // Sets the internal top_at_region_start for the given region to current top of the region.
  inline void update_top_at_rebuild_start(SemeruHeapRegion* r);
  // TARS for the given region during remembered set rebuilding.
  inline HeapWord* top_at_rebuild_start(uint region) const;

  // Clear statistics gathered during the concurrent cycle for the given region after
  // it has been reclaimed.
  void clear_statistics_in_region(uint region_idx);
  // Notification for eagerly reclaimed regions to clean up.
  void humongous_object_eagerly_reclaimed(SemeruHeapRegion* r);
  // Manipulation of the global mark stack.
  // The push and pop operations are used by tasks for transfers
  // between task-local queues and the global mark stack.
  bool mark_stack_push(G1SemeruTaskQueueEntry* arr) {
    if (!_global_mark_stack.par_push_chunk(arr)) {
      set_has_overflown();
      return false;
    }
    return true;
  }

  bool mark_stack_pop(G1SemeruTaskQueueEntry* arr) {
    return _global_mark_stack.par_pop_chunk(arr);
  }

  size_t mark_stack_size() const                { return _global_mark_stack.size(); }
  size_t partial_mark_stack_size_target() const { return _global_mark_stack.capacity() / 3; }
  bool mark_stack_empty() const                 { return _global_mark_stack.is_empty(); }

  //G1SemeruCMRootRegions* root_regions() { return &_root_regions; }

  // Transfer content from G1SemeruCollectedHeap->_recv_mem_server_cset
  // The structure of G1SemeruCMCSetRegions support Multiple-Thread safe.
  G1SemeruCMCSetRegions*  mem_server_cset()   { return &_mem_server_cset;  }

  void concurrent_cycle_start();
  // Abandon current marking iteration due to a Full GC.
  void concurrent_cycle_abort();
  void concurrent_cycle_end();

  void update_accum_task_vtime(int i, double vtime) {
    _accum_task_vtime[i] += vtime;
  }

  double all_task_accum_vtime() {
    double ret = 0.0;
    for (uint i = 0; i < _max_num_tasks; ++i)
      ret += _accum_task_vtime[i];
    return ret;
  }

  // Attempts to steal an object from the task queues of other tasks
  bool try_stealing(uint worker_id, G1SemeruTaskQueueEntry& task_entry);

  G1SemeruConcurrentMark(G1SemeruCollectedHeap* semeru_h,
                   G1RegionToSpaceMapper* prev_bitmap_storage,
                   G1RegionToSpaceMapper* next_bitmap_storage);


  ~G1SemeruConcurrentMark();

  G1SemeruConcurrentMarkThread* semeru_cm_thread() { return _semeru_cm_thread; }   //[?] Change to Semeru CM thread

  const G1CMBitMap* const prev_mark_bitmap() const { return _prev_mark_bitmap; }
  G1CMBitMap* next_mark_bitmap() const { return _next_mark_bitmap; }


  // Calculates the number of concurrent GC threads to be used in the marking phase.
  uint calc_active_marking_workers();

  // Moves all per-task cached data into global state.
  void flush_all_task_caches();
  // Prepare internal data structures for the next mark cycle. This includes clearing
  // the next mark bitmap and some internal data structures. This method is intended
  // to be called concurrently to the mutator. It will yield to safepoint requests.
  void cleanup_for_next_mark();

  // Clear the previous marking bitmap during safepoint.
  void clear_prev_bitmap(WorkGang* workers);

  // These two methods do the work that needs to be done at the start and end of the
  // initial mark pause.
  void pre_initial_mark();
  void post_initial_mark();

  //
  // Semeru Memory Server functions
  //

  // Concurrent Mark.
  void semeru_concurrent_mark_a_region( SemeruHeapRegion* region_to_scan); 

  // The compact is executed in STW mode.
  void semeru_stw_compact_a_region( SemeruHeapRegion* region_to_scan);


  // Scan all the root regions and mark everything reachable from
  // them.
  void scan_root_regions();

  // Scan a single root region from nTAMS to top and mark everything reachable from it.
  void scan_root_region(SemeruHeapRegion* hr, uint worker_id);

  // Do concurrent phase of marking, to a tentative transitive closure.
  void mark_from_roots();

  // the concurrent marking phase for a new region
  void semeru_concurrent_marking();

  // Do concurrent preclean work.
  void preclean();

  void remark();

  void cleanup();
  // Mark in the previous bitmap. Caution: the prev bitmap is usually read-only, so use
  // this carefully.
  inline void mark_in_prev_bitmap(oop p);

  // Clears marks for all objects in the given range, for the prev or
  // next bitmaps.  Caution: the previous bitmap is usually
  // read-only, so use this carefully!
  void clear_range_in_prev_bitmap(MemRegion mr);

  inline bool is_marked_in_prev_bitmap(oop p) const;

  // Verify that there are no collection set oops on the stacks (taskqueues /
  // global mark stack) and fingers (global / per-task).
  // If marking is not in progress, it's a no-op.
  void verify_no_cset_oops() PRODUCT_RETURN;

  inline bool do_yield_check();

  bool has_aborted()      { return _has_aborted; }
  // debug
  void set_concurrentmark_aborted() { _has_aborted = true; }

  void print_summary_info();

  void print_worker_threads_on(outputStream* st) const;
  void threads_do(ThreadClosure* tc) const;

  void print_on_error(outputStream* st) const;

  // Mark the given object on the next bitmap if it is below nTAMS.
  inline bool mark_in_next_bitmap(uint worker_id, SemeruHeapRegion* const hr, oop const obj);
  inline bool mark_in_next_bitmap(uint worker_id, oop const obj);

  inline bool is_marked_in_next_bitmap(oop p) const;

  // Returns true if initialization was successfully completed.
  bool completed_initialization() const {
    return _completed_initialization;
  }

  ConcurrentGCTimer* gc_timer_cm() const { return _gc_timer_cm; }
  G1OldTracer* gc_tracer_cm() const { return _gc_tracer_cm; }

private:
  // Rebuilds the remembered sets for chosen regions in parallel and concurrently to the application.
  void rebuild_rem_set_concurrently();
};


/** 
 * A class representing a marking task.
 *  
 *  Task isn't a Thread. Task is the computation, inherit from TerminatorTerminator, to be executed by ONE woker, inhrit from AbstractGangTask.
 *  Contents of the G1SemeruCMTask:
 *    1) The attadched concurrent thread, recorded by _worker_id, G1SemeruConcurrentMark->_concurrent_workers[_worker_id]
 *    2) Trace the cliamed Region, pointed by _curr_region.
 * 
 */
class G1SemeruCMTask : public TerminatorTerminator {
private:
  enum PrivateConstants {
    // The regular clock call is called once the scanned words reaches
    // this limit
    words_scanned_period          = 12*1024,
    // The regular clock call is called once the number of visited
    // references reaches this limit
    refs_reached_period           = 1024,
    // Initial value for the hash seed, used in the work stealing code
    init_hash_seed                = 17
  };

  // Number of entries in the per-task stats entry. This seems enough to have a very
  // low cache miss rate.
  static const uint RegionMarkStatsCacheSize = 1024;

  G1SemeruCMObjArrayProcessor       _objArray_processor;  // Specified to process big object array.

  uint                              _worker_id;     // [?] Only one concurrent Thread can cliam this Region.
                                                    // Let other available concurrent threads to steal work from here ?

  G1SemeruCollectedHeap*            _semeru_h;      // Only process the semeru heap.
  G1SemeruConcurrentMark*           _semeru_cm;
  G1CMBitMap*                       _next_mark_bitmap;    // abandoned 

  G1CMBitMap*                       _alive_bitmap;  // points to the scanned Region's alive_bitmap.
  G1CMBitMap*                       _dest_bitmap;

  // the task(entry) queue of this task
  G1SemeruCMTaskQueue*              _semeru_task_queue;      // The StarTask queue for CM

  // This is a Task local cache.
  // It points to a global strucure : G1SemeruCompact->_region_mark_stats
  G1RegionMarkStatsCache      _mark_stats_cache;    // [x] Store the CM scanning information. e.g. scanned alive objects.
 
  // Number of calls to this task
  uint                        _calls;

  // When the virtual timer reaches this time, the marking step should exit
  double                      _time_target_ms;
  // Start time of the current marking step
  double                      _start_time_ms;

  // Oop closure used for iterations over oops
  //G1CMOopClosure*             _cm_oop_closure;    // [??] The closure for concurrent marking. Trace an object.

  G1SemeruCMOopClosure*       _semeru_cm_oop_closure; // the closure for scan a marked object

  // Region this task is scanning, NULL if we're not scanning any
  SemeruHeapRegion*           _curr_region;
  // Local finger of this task, NULL if we're not scanning a region
  HeapWord*                   _finger;
  // Limit of the region this task is scanning, NULL if we're not scanning one
  HeapWord*                   _region_limit;

  //
  // Semeru Memory Server concurrent marking and compacting process
  //

  // [?] For the marking and compacting, use two lists seperately.
  // [?] When to initialize these two list ?
  // Region this task is scanning, NULL if we're not scanning any
  //SemeruHeapRegion*                 _curr_marking_region;     //[x] _curr_region points to current marking Region now.
  // Limit of the region this task is scanning, NULL if we're not scanning one
  //HeapWord*                   _marking_region_limit;

  // Region this task is scanning, NULL if we're not scanning any
  SemeruHeapRegion*                 _curr_compacting_region;
  // Limit of the region this task is scanning, NULL if we're not scanning one
  HeapWord*                   _compacting_region_limit;



  // Number of words this task has scanned
  size_t                      _words_scanned;
  // When _words_scanned reaches this limit, the regular clock is
  // called. Notice that this might be decreased under certain
  // circumstances (i.e. when we believe that we did an expensive
  // operation).
  size_t                      _words_scanned_limit;
  // Initial value of _words_scanned_limit (i.e. what it was
  // before it was decreased).
  size_t                      _real_words_scanned_limit;

  // Number of references this task has visited
  size_t                      _refs_reached;
  // When _refs_reached reaches this limit, the regular clock is
  // called. Notice this this might be decreased under certain
  // circumstances (i.e. when we believe that we did an expensive
  // operation).
  size_t                      _refs_reached_limit;      // [??] What's the purpose to set such a limit ??
  // Initial value of _refs_reached_limit (i.e. what it was before
  // it was decreased).
  size_t                      _real_refs_reached_limit;

  // If true, then the task has aborted for some reason
  bool                        _has_aborted;
  // Set when the task aborts because it has met its time quota
  bool                        _has_timed_out;
  // True when we're draining SATB buffers; this avoids the task
  // aborting due to SATB buffers being available (as we're already
  // dealing with them)
  bool                        _draining_satb_buffers;

  // Number sequence of past step times
  NumberSeq                   _step_times_ms;
  // Elapsed time of this task
  double                      _elapsed_time_ms;
  // Termination time of this task
  double                      _termination_time_ms;
  // When this task got into the termination protocol
  double                      _termination_start_time_ms;

  TruncatedSeq                _marking_step_diffs_ms;     // [?] What's this time used for ??

  // Updates the local fields after this task has claimed
  // a new region to scan
  void setup_for_region(SemeruHeapRegion* hr);
  // Makes the limit of the region up-to-date
  void update_region_limit();

  // Called when either the words scanned or the refs visited limit
  // has been reached
  void reached_limit();
  // Recalculates the words scanned and refs visited limits
  void recalculate_limits();
  // Decreases the words scanned and refs visited limits when we reach
  // an expensive operation
  void decrease_limits();
  // Checks whether the words scanned or refs visited reached their
  // respective limit and calls reached_limit() if they have
  void check_limits() {
    if (_words_scanned >= _words_scanned_limit ||
        _refs_reached >= _refs_reached_limit) {
      reached_limit();
    }
  }

  // Supposed to be called regularly during a marking step as
  // it checks a bunch of conditions that might cause the marking step
  // to abort
  // Return true if the marking step should continue. Otherwise, return false to abort
  bool regular_clock_call();

  bool semeru_ms_regular_clock_call();

  // Set abort flag if regular_clock_call() check fails
  inline void abort_marking_if_regular_check_fail();

  inline void semeru_ms_abort_marking_if_regular_check_fail();

  // Test whether obj might have already been passed over by the
  // mark bitmap scan, and so needs to be pushed onto the mark stack.
  bool is_below_finger(oop obj, HeapWord* global_finger) const;

  template<bool scan> void process_grey_task_entry(G1SemeruTaskQueueEntry task_entry);
public:

  // Semeru
  bool semeru_cm_task_do_addr(HeapWord* const addr);

  // Apply the closure on the given area of the objArray. Return the number of words
  // scanned.
  inline size_t scan_objArray(objArrayOop obj, MemRegion mr);
  // Resets the task; should be called right at the beginning of a marking phase.
  void reset(G1CMBitMap* next_mark_bitmap);
  // Clears all the fields that correspond to a claimed region.
  void clear_region_fields();

  // The main method of this class which performs a marking step
  // trying not to exceed the given duration. However, it might exit
  // prematurely, according to some conditions (i.e. SATB buffers are
  // available for processing).
  void do_marking_step(double target_ms,
                       bool do_termination,
                       bool is_serial);

  // Semeru Memory Server : Concurrent or STW Remark a claimed Region.
  // [?] How to let multiple concurrent workers to scan a single Region ??
  void do_semeru_marking_step(double time_target_ms,
															 bool do_termination,
															 bool is_serial);


  //
  // Semeru
  //
  G1CMBitMap*    alive_bitmap()  { return _alive_bitmap; }
//  G1CMBitMap*    dest_bitmap()   { return _dest_bitmap;  }  // Should be a key-value pair, not a bitmap.

  // Mark an object alive in current scanning region, pointed by _curr_region.
  inline bool mark_in_alive_bitmap(uint const worker_id, oop const obj);
  inline bool make_reference_alive(oop obj); 


  // [?] The closure for Semeru memory server compact.
  //
  void  semeru_mem_server_compact();


  // These two calls start and stop the timer
  void record_start_time() {
    _elapsed_time_ms = os::elapsedTime() * 1000.0;
  }
  void record_end_time() {
    _elapsed_time_ms = os::elapsedTime() * 1000.0 - _elapsed_time_ms;
  }

  // Returns the worker ID associated with this task.
  uint worker_id() { return _worker_id; }

  // From TerminatorTerminator. It determines whether this task should
  // exit the termination protocol after it's entered it.
  virtual bool should_exit_termination();

  // Resets the local region fields after a task has finished scanning a
  // region; or when they have become stale as a result of the region
  // being evacuated.
  void giveup_current_region();

  HeapWord* finger()            { return _finger; }

  /**
   * [x] What's the difference between _has_aborted and _should_terminated
   *    G1SemeruCMTask->_has_aborted, abort a CMTask, which is executed by a worker.
   *        => abort the G1SemeruCMTask::do_semeru_marking_step()
   *    G1SemeruConcurrentMark->_has_aborted, abort the current mark's all tasks ?
   *      
   *    ConcurrentMarkThread->_should_terminated, abort the thread handler.
   *        => terminate the G1SemeruConcurrentMarkThread::run_service().
   */
  bool has_aborted()            { return _has_aborted; }
  void set_has_aborted()        { _has_aborted = true; }
  void clear_has_aborted()      { _has_aborted = false; }

  void set_cm_oop_closure(G1SemeruCMOopClosure* cm_oop_closure);

  // Increment the number of references this task has visited.
  void increment_refs_reached() { ++_refs_reached; }

  // Grey the object by marking it.  If not already marked, push it on
  // the local queue if below the finger. obj is required to be below its region's NTAMS.
  // Returns whether there has been a mark to the bitmap.
  //inline bool make_reference_grey(oop obj);
  // [xx] change to make_reference_alive(oop obj)


  // Grey the object (by calling make_grey_reference) if required,
  // e.g. obj is below its containing region's NTAMS.
  // Precondition: obj is a valid heap object.
  // Returns true if the reference caused a mark to be set in the next bitmap.
  template <class T>
  inline bool deal_with_reference(T* p);

  // Scans an object and visits its children.
  inline void scan_task_entry(G1SemeruTaskQueueEntry task_entry);

  // Pushes an object on the local queue.
  inline void push(G1SemeruTaskQueueEntry task_entry);

  // Move entries to the global stack.
  void move_entries_to_global_stack();
  // Move entries from the global stack, return true if we were successful to do so.
  bool get_entries_from_global_stack();

  // semeru
  bool get_entries_from_global_stack_may_switch_region();
  bool delete_entries_from_global_stack();

  // Pops and scans objects from the local queue. If partially is
  // true, then it stops when the queue size is of a given limit. If
  // partially is false, then it stops when the queue is empty.
  void drain_local_queue(bool partially);

  // Fault tolerance
  void fault_tolerance_drain_local_queue();

  // Moves entries from the global stack to the local queue and
  // drains the local queue. If partially is true, then it stops when
  // both the global stack and the local queue reach a given size. If
  // partially if false, it tries to empty them totally.
  void drain_global_stack(bool partially);

  // semeru
  void drain_global_stack_may_switch_region(bool partially);
  void falut_tolerance_drain_global_stack();

  // Keeps picking SATB buffers and processing them until no SATB
  // buffers are available.
  void drain_satb_buffers();

  // Moves the local finger to a new location
  inline void move_finger_to(HeapWord* new_finger) {
    assert(new_finger >= _finger && new_finger < _region_limit, "invariant");
    _finger = new_finger;
  }

  G1SemeruCMTask(uint worker_id,
           G1SemeruConcurrentMark *cm,
           G1SemeruCMTaskQueue* task_queue,
           G1RegionMarkStats* mark_stats,
           uint max_regions);

  inline void update_liveness(oop const obj, size_t const obj_size);

  // Clear (without flushing) the mark cache entry for the given region.
  void clear_mark_stats_cache(uint region_idx);
  // Evict the whole statistics cache into the global statistics. Returns the
  // number of cache hits and misses so far.
  Pair<size_t, size_t> flush_mark_stats_cache();
  // Prints statistics associated with this task
  void print_stats();

  // Restore the marked statistics to the Region.
  void restore_region_mark_stats();

  //
  // Target object queue related
  //

  inline void trim_target_object_queue(TargetObjQueue* target_obj_queue);

  inline void trim_target_object_queue_to_threshold(TargetObjQueue* target_obj_queue, uint threshold);

  inline void dispatch_reference(StarTask ref);


  //
  // Cross Region reference queue
  //
  inline void scan_cross_region_ref_queue(HashQueue* cross_region_ref_q);



};



  // Closure definition
  //

  // Closure for do_heap_region(G1CMBitMap*, Closure*)
  // Process each alive object find in the target_oop_bitmap
  class SemeruScanTargetOopClosure : public StackObj {
    G1SemeruCMTask* _semeru_cm_scan_task;

  public:
    SemeruScanTargetOopClosure(G1SemeruCMTask* semeru_cm_scan_task) : _semeru_cm_scan_task(semeru_cm_scan_task) { };
    size_t apply(oop object);
  };




// Class that's used to to print out per-region liveness
// information. It's currently used at the end of marking and also
// after we sort the old regions at the end of the cleanup operation.
class G1SemeruPrintRegionLivenessInfoClosure : public SemeruHeapRegionClosure {
  // Accumulators for these values.
  size_t _total_used_bytes;
  size_t _total_capacity_bytes;
  size_t _total_prev_live_bytes;
  size_t _total_next_live_bytes;

  // Accumulator for the remembered set size
  size_t _total_remset_bytes;

  // Accumulator for strong code roots memory size
  size_t _total_strong_code_roots_bytes;

  static double bytes_to_mb(size_t val) {
    return (double) val / (double) M;
  }

public:
  // The header and footer are printed in the constructor and
  // destructor respectively.
  G1SemeruPrintRegionLivenessInfoClosure(const char* phase_name);
  virtual bool do_heap_region(SemeruHeapRegion* r);
  ~G1SemeruPrintRegionLivenessInfoClosure();
};

#endif // SHARE_VM_GC_G1_SEMERU_G1CONCURRENTMARK_HPP
