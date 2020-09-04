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
#include "classfile/classLoaderDataGraph.hpp"
#include "gc/g1/g1Analytics.hpp"
//#include "gc/g1/g1CollectedHeap.inline.hpp"
//#include "gc/g1/g1ConcurrentMark.inline.hpp"
//#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1MMUTracker.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1VMOperations.hpp"
#include "gc/shared/concurrentGCPhaseManager.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/debug.hpp"


// Semeru
#include "gc/g1/g1SemeruConcurrentMark.inline.hpp"
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"
#include "gc/g1/g1SemeruConcurrentMarkThread.inline.hpp"
#include "semeru/debug_function.h"


// ======= Semeru Concurrent Mark Thread ========
//  Semeru concurrent threads need to be controlled separately. 
//  And they execute specific CMTasks,
//  They have different phases 


// Check order in EXPAND_CURRENT_PHASES
STATIC_ASSERT(ConcurrentGCPhaseManager::UNCONSTRAINED_PHASE <
              ConcurrentGCPhaseManager::IDLE_PHASE);

/**
 * Tag : Purpose of each Phase
 * 
 * CLEAR_CLAIMED_MARKS : Clean the marked bit on prev/next_bitmap?
 * ANY : purpose ??
 * 
 * [?] Purpose of assigning values ??
 *      if there is no value, the phase is just a name ?? 
 *      For each expander,  (tag,vlaue, ignore_title), the format should be :
 *      tag = value  // ignore_title  
 * 
 * 
 */
#define EXPAND_CONCURRENT_PHASES(expander)                                 \
  expander(ANY, = ConcurrentGCPhaseManager::UNCONSTRAINED_PHASE, NULL)    \
  expander(IDLE, = ConcurrentGCPhaseManager::IDLE_PHASE, NULL)             \
  expander(SEMERU_CONCURRENT_CYCLE,, "Semeru Concurrent Cycle Start")      \
  expander(CLEAR_CLAIMED_MARKS,, "Concurrent Clear Claimed Marks")         \
  expander(SCAN_ROOT_REGIONS,, "(Abandoned in Semeru)Concurrent Scan Root Regions")  \
  expander(SEMERU_CONCURRENT_MARK,, "Semeru Concurrent Mark")              \
  expander(SEMERU_MEM_SERVER_CONCURRENT,, "Semeru Concurrent Mark")        \
  expander(MARK_FROM_ROOTS,, "(Abandoned in Semeru)Concurrent Mark From Roots")      \
  expander(PRECLEAN,, "Concurrent Preclean")                               \
  /*expander(BEFORE_REMARK,, NULL)      */                                  \
  expander(SEMERU_REMARK,, "Semeru STW Remark")                            \
  expander(SEMERU_COMPACT,, "Semeru STW Compact")                          \
  expander(REBUILD_REMEMBERED_SETS,, "Concurrent Rebuild Remembered Sets") \
  expander(CLEANUP_FOR_NEXT_MARK,, "Concurrent Cleanup for Next Mark")     \
  /* */


/**
 * Rewrite the concurrent phase
 *  
 */
class G1SemeruConcurrentPhase : public AllStatic {
public:
  enum {
#define CONCURRENT_PHASE_ENUM(tag, value, ignore_title) tag value,
    EXPAND_CONCURRENT_PHASES(CONCURRENT_PHASE_ENUM)
#undef CONCURRENT_PHASE_ENUM
    PHASE_ID_LIMIT
  };
};



/**
 * Semeru Concurrent Threads
 * => Execute all of the Semeru memory server concurrent marking, refinement and other concurrent tasks.  
 * 
 */
G1SemeruConcurrentMarkThread::G1SemeruConcurrentMarkThread(G1SemeruConcurrentMark* semeru_cm) :
  ConcurrentGCThread(),
  _vtime_start(0.0),
  _vtime_accum(0.0),
  _vtime_mark_accum(0.0),
  _semeru_cm(semeru_cm),    // use semeru marker
  _semeru_sc(NULL),         // [XX] STW compacter for Semeru MS.
  _semeru_ms_gc_should_terminated(false),
  _state(Idle),
  _phase_manager_stack() {

  set_name("Semeru Memory Server Concurrent Thread");
  create_and_start();     // [x] This will 1) create pthread based CT. 2) Execute the G1SemeruConcurrentThread->run_service() by the newly created pthread based CT.

  //debug
  tty->print("%s, Initialize the _semeru_sc !!! \n", __func__);


  // debug
  #ifdef ASSERT
  log_debug(gc,thread)("%s, Created thread %s, 0x%lx successfully.\n", __func__, name(), (size_t)this);
  #endif
}





/**
 * Semeru Memory Server
 * [?] But seems that we don't need to build a separate VM_Operation,
 *     It can be merged into Concurrent Marking task.
 *     Because there is no need to let the memory srever suspend the mutators.
 *     The CPU server already does this and inform the memory server its state.
 *     So the memory server only needs to switch its execution function.
 *  
 */
class CMRemark : public VoidClosure {
  G1SemeruConcurrentMark* _semeru_cm;
public:
  CMRemark(G1SemeruConcurrentMark* semeru_cm) : _semeru_cm(semeru_cm) {}

  void do_void(){
    _semeru_cm->remark();
  }
};


class CMCleanup : public VoidClosure {
  G1SemeruConcurrentMark* _semeru_cm;
public:
  CMCleanup(G1SemeruConcurrentMark* semeru_cm) : _semeru_cm(semeru_cm) {}

  void do_void(){
    _semeru_cm->cleanup();
  }
};

double G1SemeruConcurrentMarkThread::mmu_sleep_time(G1Policy* g1_policy, bool remark) {
  // There are 3 reasons to use SuspendibleThreadSetJoiner.
  // 1. To avoid concurrency problem.
  //    - G1MMUTracker::add_pause(), when_sec() and its variation(when_ms() etc..) can be called
  //      concurrently from ConcurrentMarkThread and VMThread.
  // 2. If currently a gc is running, but it has not yet updated the MMU,
  //    we will not forget to consider that pause in the MMU calculation.
  // 3. If currently a gc is running, ConcurrentMarkThread will wait it to be finished.
  //    And then sleep for predicted amount of time by delay_to_keep_mmu().
  SuspendibleThreadSetJoiner sts_join;

  const G1Analytics* analytics = g1_policy->analytics();
  double now = os::elapsedTime();
  double prediction_ms = remark ? analytics->predict_remark_time_ms()
                                : analytics->predict_cleanup_time_ms();
  G1MMUTracker *mmu_tracker = g1_policy->mmu_tracker();
  return mmu_tracker->when_ms(now, prediction_ms);
}

void G1SemeruConcurrentMarkThread::delay_to_keep_mmu(G1Policy* g1_policy, bool remark) {
  if (g1_policy->adaptive_young_list_length()) {
    jlong sleep_time_ms = mmu_sleep_time(g1_policy, remark);
    if (!_semeru_cm->has_aborted() && sleep_time_ms > 0) {
      os::sleep(this, sleep_time_ms, false);
    }
  }
}


/**
 * Semeru Memory Server - Record the elapsed time for each phase.
 *  
 *  For a { }
 *  Constructor start timer,
 *  Deconstructor end the timer.
 * 
 */
class G1SemeruConcPhaseTimer : public GCTraceConcTimeImpl<LogLevel::Info, LOG_TAGS(gc, marking)> {
  G1SemeruConcurrentMark* _semeru_cm;

 public:
  G1SemeruConcPhaseTimer(G1SemeruConcurrentMark* semeru_cm, const char* title) :
    GCTraceConcTimeImpl<LogLevel::Info,  LogTag::_gc, LogTag::_marking>(title),
    _semeru_cm(semeru_cm)
  {
    _semeru_cm->gc_timer_cm()->register_gc_concurrent_start(title);
  }

  ~G1SemeruConcPhaseTimer() {
    _semeru_cm->gc_timer_cm()->register_gc_concurrent_end();
  }
};

static const char* const concurrent_phase_names[] = {
#define CONCURRENT_PHASE_NAME(tag, ignore_value, ignore_title) XSTR(tag),
  EXPAND_CONCURRENT_PHASES(CONCURRENT_PHASE_NAME)
#undef CONCURRENT_PHASE_NAME
  NULL                          // terminator
};
// Verify dense enum assumption.  +1 for terminator.
STATIC_ASSERT(G1SemeruConcurrentPhase::PHASE_ID_LIMIT + 1 ==
              ARRAY_SIZE(concurrent_phase_names));

// Returns the phase number for name, or a negative value if unknown.
static int lookup_concurrent_phase(const char* name) {
  const char* const* names = concurrent_phase_names;
  for (uint i = 0; names[i] != NULL; ++i) {
    if (strcmp(name, names[i]) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// The phase must be valid and must have a title.
static const char* lookup_concurrent_phase_title(int phase) {
  static const char* const titles[] = {
#define CONCURRENT_PHASE_TITLE(ignore_tag, ignore_value, title) title,
    EXPAND_CONCURRENT_PHASES(CONCURRENT_PHASE_TITLE)
#undef CONCURRENT_PHASE_TITLE
  };
  // Verify dense enum assumption.
  STATIC_ASSERT(G1SemeruConcurrentPhase::PHASE_ID_LIMIT == ARRAY_SIZE(titles));

  assert(0 <= phase, "precondition");
  assert((uint)phase < ARRAY_SIZE(titles), "precondition");
  const char* title = titles[phase];
  assert(title != NULL, "precondition");
  return title;
}


/**
 * Semeru - What's the purpose of the Phase Manger ?
 *  
 *  
 * 
 * 
 */
class G1SemeruConcPhaseManager : public StackObj {
  G1SemeruConcurrentMark* _semeru_cm;   // Points to the passed one.
  ConcurrentGCPhaseManager _manager;    // Create a new ConcurrentGC Phase Manager.  [?] What's the purpose of the manager?

public:
  G1SemeruConcPhaseManager(int phase, G1SemeruConcurrentMarkThread* thread) :
    _semeru_cm(thread->semeru_cm()),
    _manager(phase, thread->phase_manager_stack())
  { }

  // ~ConcurrentGCPhaseManager will pop current PhaseManager from stack.
  //
  ~G1SemeruConcPhaseManager() {
    // Deactivate the manager if marking aborted, to avoid blocking on
    // phase exit when the phase has been requested.
    if (_semeru_cm->has_aborted()) {
      _manager.deactivate();          // [?] Do we need to deactivate the PhaseManager explicitly ?
    }
  }

  void set_phase(int phase, bool force) {
    _manager.set_phase(phase, force);
  }
};




/** 
 * Semeru Memory Server - Combine phase management and timing into one convenient utility.
 * 
 * [x] Maintain separate phase manager and timer for the Semeru Memory Server GC。
 *  G1SemeruConcPhaseTimer, used to record time of each phase.
 *  Usually use the {} scope. 
 * 
 * 
 * [?] What's the difference between phaseManager.set_phase()
 *     And phase created here.
 * 
 * [?] According to the constructor, build a new _manager here ??
 * 
 */ 
class G1SemeruConcPhase : public StackObj {
  G1SemeruConcPhaseTimer _timer;
  G1SemeruConcPhaseManager _manager;

public:
  G1SemeruConcPhase(int phase, G1SemeruConcurrentMarkThread* thread) :
    _timer(thread->semeru_cm(), lookup_concurrent_phase_title(phase)),
    _manager(phase, thread)
  { }
};

const char* const* G1SemeruConcurrentMarkThread::concurrent_phases() const {
  return concurrent_phase_names;
}

/**
 * Tag : Request to start a specific Phase ? 
 *  
 */
bool G1SemeruConcurrentMarkThread::request_concurrent_phase(const char* phase_name) {
  int phase = lookup_concurrent_phase(phase_name);
  if (phase < 0) return false;

  while (!ConcurrentGCPhaseManager::wait_for_phase(phase,
                                                   phase_manager_stack())) {
    assert(phase != G1SemeruConcurrentPhase::ANY, "Wait for ANY phase must succeed");
    if ((phase != G1SemeruConcurrentPhase::IDLE) && !during_cycle()) {
      // If idle and the goal is !idle, start a collection.
      G1CollectedHeap::heap()->collect(GCCause::_wb_conc_mark);
    }
  }
  return true;
}


/**
 * Semeru Memory Server
 *  
 * Confirm all the enviorments are initialized well.
 * And then trigger the G1SemeruConcurrentMarkThread->run_service() to run.
 * 
 */
void G1SemeruConcurrentMarkThread::wait_for_universe_init() {
  MutexLockerEx x(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  
  log_debug(semeru,thread)("%s, G1SemeruConcurrentMarkThread, 0x%lx, waiting for is_init_completed, %d and  _should_terminate, %d.",
                                                              __func__, (size_t)this, (int)is_init_completed(),(int)_should_terminate);

  while (!is_init_completed() && !_should_terminate) {
    SemeruCGC_lock->wait(Mutex::_no_safepoint_check_flag, 1);
  }

  log_debug(semeru,thread)("%s, G1SemeruConcurrentMarkThread, 0x%lx, is prepared well to run. \n",__func__, (size_t)this);
  
}



void G1SemeruConcurrentMarkThread::run() {

  #ifdef ASSERT
	log_debug(semeru,thread)("G1SemeruConcurrentMarkThread:%s, Execute Concurrent thread 0x%lx 's service.\n", __func__, (size_t)this);
	#endif

  ConcurrentGCThread::initialize_in_thread();
  wait_for_universe_init();  // Confirm the heap is initialized. Wait on lock CGC_lock  and init->_init_completed.

  run_service();

  ConcurrentGCThread::terminate();  // If exit from the ConcurrentGCThread's service, terminate this thread ?
}



/**
 * Semeru Memory Server
 *  
 *  Run the Semeru concurrent marking, STW Remark , STW compaction in background. 
 *  [?] Which thread is executing this service ?
 *    => a special concurrent thread
 * 
 *  [?] This service keeps running in background ?
 * 
 *  [?] Is this a deamon thread, running in the background ??
 *        => So it needs to check if new Regions are assigned to it.
 *  
 *  [?] In the original design, we need to switch the STW, Concurrent mode for different phases.  
 *      But for Semeru, we don't have such a requirements. 
 *      After the CPU server informs Memory server, it's in GC or not,
 *      memory server can switch between different work sharply by just switching a function.
 * 
 */
void G1SemeruConcurrentMarkThread::run_service() {
  _vtime_start = os::elapsedVTime();

  G1SemeruCollectedHeap* semeru_heap = G1SemeruCollectedHeap::heap();
  G1Policy* g1_policy = semeru_heap->g1_policy();
  flags_of_cpu_server_state* cpu_server_flags = semeru_heap->cpu_server_flags();
  flags_of_mem_server_state* mem_server_flags = semeru_heap->mem_server_flags();

  //
  // Semeru phase manager
  //  Enter the same phase, set state as IDLE. 
  G1SemeruConcPhaseManager cpmanager(G1SemeruConcurrentPhase::IDLE, this);   // [?]rewrite the phase manager for semeru

    
  log_debug(semeru,thread)("%s, entering G1SemeruConcurrentMarkThread(0x%lx)->run_service(), and wait on SemeruGC_lock. \n", 
                                                                  __func__, (size_t)Thread::current());
 
  
  //  Let the Semeru Concurrent threads, current thread, wait on SemeruCGC_lock.
  //  Before invoke sleep_before_next_cycle(), MUST set G1SemeruConcurrentMarkThread->_state to Idle or Started. 
  sleep_before_next_cycle();    // [XX]Concurrent thread is waiting to be waken up.
    
  
  log_debug(semeru,thread)("%s, G1SemeruConcurrentMarkThread(0x%lx)->run_service() is waken up. \n", 
                                                                  __func__, (size_t)Thread::current());
  

  // Start a new iteration ?
  // it's proper to put this start here ??
  _semeru_cm->concurrent_cycle_start();     // Set timer, tracer

  // Do initialization for the Contiguous Memory Server Marking/Compacting.
  // This is a one-time initializaiton
  if(_semeru_cm){
    _semeru_cm->pre_initial_mark();
  }

  // [?] What's  the purpose of these phase ?
  //    Just for Log ? Can also synchronize, schedule some thing?
  //    e.g. Concurrent Tracing and STW Compact are different phases, we need to schedule and switch between these 2 phases.
  //
  cpmanager.set_phase(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_CYCLE, false /* force */);


  // [x] Keep runing until the G1SemeruConcurrentThread is stopped.
  //     only ConcurrentThread->_should_terminate can end the MS GC.
  while (!should_terminate()  && !semeru_ms_gc_should_terminated() ) {
  

    GCTraceConcTime(Info, gc) tt("Semeru Memory Server Concurrent Service");

    {
      ResourceMark rm;
      HandleMark   hm;
      double cycle_start = os::elapsedVTime();

      // 
      // Interrupped by CPU server 2-sided RDMA message here, reschedule each phase
      //  Phase 1) dispatch recieved CSet and target Oop Queue
      //  Phase 2) Utilize the STW time to do compact.
      //  Phase 3) Switch to concurrent tracing.
      {
        // Phase 1) Dispatch the received CSets from Memory Server
        // Check if we received new CSet from CPU server.
        // This behavior is executed at the start of scheduling Concurrent Tracing Phase and STW Compact Phase.
        // CPU server keeps pushing new CSet to Memory server.
        // [X] This procedure has to be handled by the main thread. This is NOT MT safe.
        //
        // The CSet is transferred from CPU Server,
        // The root is the Target_object_queue, we don't need  G1SemeruConcurrentMark->_finger as bitmap closure.
        // The scanning sequence is controlled by G1SemeruCMTask->_curr_region.
        //  
        //  a. Divide the received Regions to 2 sets, Scanned, Freshly evicted.
        //  b. Apply STW Remark to the Scanned Regions, and try to compact them during the STW window.
        //  c. And then do concurrent marking for the Freshly evicted Regions.


        #ifdef ASSERT
        if(Thread::current() != NULL && Thread::current()->is_Named_thread()){
         log_debug(semeru,thread)("%s, Runnting Thread, %s, gc_id[%u], 0x%lx is running here. \n", __func__, 
                                                ((G1SemeruConcurrentMarkThread*)Thread::current())->name(), 
                                                ((G1SemeruConcurrentMarkThread*)Thread::current())->gc_id(),
                                                (size_t)Thread::current());
        }else{
          log_debug(semeru,thread)("%s, Unknown Runnting thread [0x%lx] is running here. \n",__func__, (size_t)Thread::current());
        }

        // Check 1-sided synchronization write.
        // Check the version's of all the Region.
        print_region_write_check_flag(sizeof(uint32_t));


        #endif


				log_debug(semeru, mem_trace)("%s, Scan Memory Server CSet.", __func__);
        received_memory_server_cset* recv_mem_server_cset = semeru_heap->recv_mem_server_cset();

        // Enqueue the received Regions to _cm_scanned_region, _freshly_evicetd_regions.
        dispatch_received_regions(recv_mem_server_cset);

      }  // end of phase 1)'s code block



  
      // Phase 2) Utilize the STW window to compact the alrady scanned Regions in CSet.
      // The compact phase can be interrupped by CPU server's 2-sided RDMA message.
      // After interruption, it sends the necessary data to CPU server to do field updates.
      {
        // [x] Check CPU is in STW state

        // Within STW Window.
        cpmanager.set_phase(G1SemeruConcurrentPhase::SEMERU_COMPACT, false);   // [?] used for debuging this phase

        // Estimate the destination Region for the Scanned CSet for paralell compacting operation.
        // [X] Right now, this is only used for 
        // _semeru_sc->mem_server_scanned_cset()->estimate_dset_region_for_scanned_cset();
 
        if (_semeru_sc->has_aborted()) {
          // Abort the compact phase, continue the  concurrent tracing.
          log_debug(semeru, mem_compact)("%s, G1SemeruSTWCompact is aborted OR there is no scanned Region in CSet . \n",__func__);
          break;
        }

        // CPU server goes into STW window, 
        // no matter we do compact or not, we need to set all the memory server flags normally.
        //
        if(cpu_server_flags->_is_cpu_server_in_stw ) {

          if(_semeru_sc->_mem_server_cset->is_compact_finished() == false){

            
            // Do the Compact action.
					  log_debug(semeru,mem_compact)("%s, Memory Server compact starts .", __func__);

            mem_server_flags->set_all_flags_to_start_mode();
            
            //
            // Skip the Compact for now..
            //
            // _semeru_sc->semeru_stw_compact();
          }

          // Exit the  STW window. 
          mem_server_flags->set_all_flags_to_end_mode();

        }
        
        
        log_debug(semeru, mem_compact)("%s, No need start compact if either true: not in STW windown ? %d, scanned_region emtpry ? %d . \n",
                                __func__, !cpu_server_flags->_is_cpu_server_in_stw, _semeru_sc->_mem_server_cset->is_compact_finished() );
      } // End of STW Compact code block
  



      //
      // 3) Concurrent Tracing Phase :  Concurrently trace the fresh evicted Regions.
      //
      {
        //
        // [??] Need a way to be interruped and switch to STW compact.
        //
        cpmanager.set_phase(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_MARK, false); 
      
        
        jlong mark_start = os::elapsed_counter();
        const char* cm_title = lookup_concurrent_phase_title(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_MARK);
        log_info(gc, marking)("%s (%.3fs)", cm_title,  TimeHelper::counter_to_seconds(mark_start));


        // Do the Concurrent Tracing for the freshly evicted Regions in MS CSet.
        {
          G1SemeruConcPhase p(G1SemeruConcurrentPhase::SEMERU_MEM_SERVER_CONCURRENT, this);
					
					log_debug(semeru,mem_trace)("%s, Memory Server concurrent tracing starts .", __func__);

          _semeru_cm->semeru_concurrent_marking();    // The main content of Concurrent Marking 
        }

  

        // [??] If all the freshly evicted Regions are scanned, waiting for the CPU server interruption
        //
        log_debug(semeru, gc)("%s, MS Concurrent Tracing processed all the freshly evicted Regions, wait for CPU server intteruption. \n",__func__);

        // wait on a lock here...... TO BE DONE
        // 


      } // End of Concurrent tracing code block 



      //
      // 4) After finish the incremental Remarking, compact the Region.
      //

      // Abandon the remember set rebuild process.
      // if (!_semeru_cm->has_aborted()) {
      //   G1SemeruConcPhase p(G1SemeruConcurrentPhase::REBUILD_REMEMBERED_SETS, this);
      //   _semeru_cm->rebuild_rem_set_concurrently();
      // }


    }  // End of all the concurrent  Cycles


    //
    // Debug - Terminate the ConcurrentThread
    //

    //#ifdef ASSERT
      // Infinite loop. 
      // Debug - Sleep and wake up to check CSet.
      os::sleep(this, 1000, false); // sleep a while to wait for CSet..
    //#endif
    //set_semeru_ms_gc_terminated();
    //this->_should_terminate = true;


  } // End of the while loop


  
      double end_time = os::elapsedVTime();
      // Update the total virtual time before doing this, since it will try
      // to measure it to get the vtime for this marking.
      _vtime_accum = (end_time - _vtime_start);

  
  // Update the number of full collections that have been
  // completed. This will also notify the FullGCCount_lock in case a
  // Java thread is waiting for a full GC to happen (e.g., it
  // called System.gc() with +ExplicitGCInvokesConcurrent).
  {
      SuspendibleThreadSetJoiner sts_join;

      // Set thread state to IDLE.
      // We have to reset the current concurrent thread as IDLE or STAETED, to rerun the srvice.
      semeru_heap->increment_old_marking_cycles_completed(true /* concurrent */);

      //[?] Update some timer information 
      _semeru_cm->concurrent_cycle_end();
  }

  cpmanager.set_phase(G1SemeruConcurrentPhase::IDLE, _semeru_cm->has_aborted() /* force */);




  //_semeru_cm->root_regions()->cancel_scan();
  _semeru_cm->mem_server_cset()->cancel_compact();
  _semeru_cm->mem_server_cset()->cancel_scan();


  #ifdef ASSERT
    log_debug(semeru,thread)("%s, End of G1SemeruConcurrentMarkThread's service.",__func__);
  #endif
}


/**
 * Semeru Memory Server - Dispatch the received Regions CSet into scanned/freshly_evicted queues.
 * 
 * Warning : 
 *  1) this function isn't MT safe. 
 *  2) May add one Region multiple times ？
 *      => It should be OK. because the first scavenge will process the region's Target_obj_queue.
 */
void G1SemeruConcurrentMarkThread::dispatch_received_regions(received_memory_server_cset* recv_mem_server_cset){

  assert(recv_mem_server_cset!= NULL, "%s, must initiate the G1SemeruCollectHeap->_mem_server_cset \n", __func__);

  G1SemeruCollectedHeap* semeru_heap = G1SemeruCollectedHeap::heap();
  //size_t* received_num = mem_server_cset->num_received_regions();
  volatile int received_region_ind = recv_mem_server_cset->pop(CUR_MEMORY_SERVER_ID);  // can be negative 
   SemeruHeapRegion* region_received = NULL;

  while(received_region_ind != -1){

		// Debug
		log_info(semeru,mem_trace)("%s, Receive an Evicted Region[%d] \n", __func__,received_region_ind);

    region_received = semeru_heap->hrm()->at(received_region_ind);
    //region_received->reset_fields_after_transfer();  // reset some fields, whose value are differenct between CPU and Memory server.
    assert(region_received != NULL, "%s, received Region is invalid.", __func__);   // [?] how to confirm if this region is available ?

    if(region_received->is_region_cm_scanned()){
			log_info(semeru,mem_trace)(" Region[%d] is already scanned. \n", received_region_ind);

      // add region into _cm_scanned_regions[] queue
      // The add operation may add one Region mutiple times. 
      semeru_cm()->mem_server_cset()->add_cm_scanned_regions(region_received);
    }else{
			log_info(semeru,mem_trace)(" Region[%d] is fresh. \n", received_region_ind);

      // add region into _freshly_evicted_regions[] queue
      semeru_cm()->mem_server_cset()->add_freshly_evicted_regions(region_received);
    }

    // process next region_id
		received_region_ind = recv_mem_server_cset->pop(CUR_MEMORY_SERVER_ID);

  }// Received CSet isn't emtpy.


}










/**
 * Semeru Memory Server - Stop the running concurrent service ?
 *  => [?] Why do we need a Mutex to stop the concurrent service ?? 
 *    
 *    [?] If the purpose is to stop the concurrent service, 
 *        why do the concurrent threads need to acuire the SemeruCGC_lock ?
 *        
 * 
 * 
 */
void G1SemeruConcurrentMarkThread::stop_service() {
  MutexLockerEx ml(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  SemeruCGC_lock->notify_all();   // [?] notify which threads ??
}


/**
 * Put the current G1SemeruConcurrentMarkThread's WorkGang wait on SemeruCGC_lock, 
 * until all the conditions are satisfied.
 * 
 * 1) G1SemeruConcurrentMarkThread->_state is setted as Started.
 * 2) ConcurrentGCThread->_should_terminate is NOT setted.
 * 3) And then this thread can be waken up by SemeruCGC_lock.
 * 
 * 
 */
void G1SemeruConcurrentMarkThread::sleep_before_next_cycle() {
  // We join here because we don't want to do the "shouldConcurrentMark()"
  // below while the world is otherwise stopped.
  assert(!in_progress(), "should have been cleared");

	{
  	MutexLockerEx x(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  	while (!started() && !should_terminate()) {    // [?] A loop :means after waking up, the conditions 1) and 2) should also be satisfied. 
    	
      tty->print("%s, started: %d, should_terminate %d \n", __func__, started(), should_terminate() );
      
      SemeruCGC_lock->wait(Mutex::_no_safepoint_check_flag);    // If the threads already wait here, no need to use a while loop?
  	}
	}

	// debug - memory server
	int sleep_time = 10;
	log_debug(semeru,rdma)("%s, Sleep Concurrent GC thread %d seconds to let connect to CPU server. \n",__func__, sleep_time);
	//sleep(sleep_time);
	os::sleep(this, sleep_time*100, false);

  if (started()) {      // G1SemeruConcurrentMarkThread->_state Started 
    set_in_progress();  // switch to G1SemeruConcurrentMarkThread->_state InProgress from Started.
  }
}

/**
 * Debug function : Let current concurent thread wait on SemeruCGC_lock.
 * 
 * [?] the problem is that, the mutator also suspend here ? WHY ?
 * The VM Thread is also suspend some where ??
 *  
 * The sate setting of the G1ConcurrentMarkThread has to be:
 * 1) Started
 * 2) InProgres
 * 3) Idle.
 * 
 */
void G1SemeruConcurrentMarkThread::sleep_on_semerucgc_lock(){
  set_idle();        // 2) Then we can set it to Idle.

  assert(!in_progress(), "should have been cleared");

  MutexLockerEx x(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  while (!started() && !should_terminate()) {       // [?] Why does here use a while() look ?
    SemeruCGC_lock->wait(Mutex::_no_safepoint_check_flag);    // If the threads already wait here, no need to use a while loop?
  }

  if (started()) {
    set_in_progress();
  }

}
