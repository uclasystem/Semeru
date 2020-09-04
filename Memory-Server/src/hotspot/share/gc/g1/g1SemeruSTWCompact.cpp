#include "gc/g1/g1SemeruSTWCompact.inline.hpp"

// Have to use some G1SemeruConcurrentMark's structure
#include "gc/g1/g1SemeruConcurrentMark.hpp"






/**
 * Semeru MS - Constructor of the G1SemeruSTWCompact
 * 	
 * [X] Share all the G1SemeruConcurrentMark's concurrent thread source. 
 * 		 But define its own code to be executed.
 * 		 So must initialize this instance after initialize G1SemeruConcurrentMark, _semeru_cm.
 * 
 */
G1SemeruSTWCompact::G1SemeruSTWCompact(G1SemeruCollectedHeap* 	g1h,
																	 		 G1SemeruConcurrentMark* 	semeru_cm) :
	_semeru_cm_thread(semeru_cm->_semeru_cm_thread),	// shared with CM
	_semeru_h(g1h),
	_completed_initialization(false),
	_mem_server_cset(&(semeru_cm->_mem_server_cset)),		// [XX] Filled by Memory Server.
	_max_num_tasks(semeru_cm->_max_num_tasks),				// shared with CM
	_num_active_tasks(semeru_cm->_num_active_tasks),	// shared with CM
	//_tasks(NULL),							// code task
	_compact_task_queues(new SemeruCompactTaskQueueSet((int) _max_num_tasks)),
	_terminator((int) _max_num_tasks, _compact_task_queues),  // will be reset in set_concurrency()
	_concurrent(false),
	_has_aborted(false),
	_compaction_points(NULL),
	_gc_timer_cm(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
	_gc_tracer_cm(new (ResourceObj::C_HEAP, mtGC) G1OldTracer()),

	// _verbose_level set below
	_init_times(),
	_remark_times(),
	_remark_mark_times(),
	_remark_weak_ref_times(),
	_cleanup_times(),
	_total_cleanup_time(0.0),
	_accum_task_vtime(semeru_cm->_accum_task_vtime),

	// concurrent thread resources
	_concurrent_workers(semeru_cm->_concurrent_workers),
	_num_concurrent_workers(semeru_cm->_num_concurrent_workers),
	_max_concurrent_workers(semeru_cm->_max_concurrent_workers)
{

	// 1) Build the _mem_server_cset

	// The Semeru Memory Server Scanned CSet is shared with G1SemeruConcurrentMark->_mem_server_cset


	// 2) Build the G1SemeruSTWCompactGangTask code .

	// Build the tasks executed by each worker.
	_compact_tasks = NEW_C_HEAP_ARRAY(G1SemeruSTWCompactTerminatorTask*, _max_num_tasks, mtGC);			
	_accum_task_vtime = NEW_C_HEAP_ARRAY(double, _max_num_tasks, mtGC);

	// so that the assertion in MarkingTaskQueue::task_queue doesn't fail
	// set_concurrency() function will rewrite this value to active_tasks
	_num_active_tasks = _max_num_tasks;





	// Compaction fields
	//
	
	// Destination Regions for Compaction.
	_compaction_points = NEW_C_HEAP_ARRAY(G1SemeruCompactionPoint*, _max_num_tasks, mtGC);
  for (uint i = 0; i < _max_num_tasks; i++) {
    _compaction_points[i] = new G1SemeruCompactionPoint();
  }




	
	// We don't register the G1SemeruCompactTask here
	// But We allocate and register the compatc task queue here.
	//
	for (uint i = 0; i < _max_num_tasks; ++i) {
		// The Star Task queu for G1SemeruCompactTask Cross_region_ref update
		SemeruCompactTaskQueue* cross_region_ref_update_queue = new SemeruCompactTaskQueue();  
		cross_region_ref_update_queue->initialize();
		_compact_task_queues->register_queue(i, cross_region_ref_update_queue);

		_compact_tasks[i] = new G1SemeruSTWCompactTerminatorTask(i, this, cross_region_ref_update_queue, _semeru_h->max_regions());


		_accum_task_vtime[i] = 0.0;
	}






	// Initialization of G1SemeruSTWCompact is done.
	_completed_initialization = true;

}







/**
 * Semeru Memory Server : Cliam server Regions for comaction.
 * 	There are some constraints for the compacted Region cliaming.  
 *  1) Merge the Rgions with many cross-region refences together.
 * 		[?] How to implement this ??
 * 			Select the one with most cross-region referenced scanned region of the las compact Region.
 * 
 */
SemeruHeapRegion*
G1SemeruSTWCompact::claim_region_for_comapct(uint worker_id, SemeruHeapRegion* prev_compact) {

	SemeruHeapRegion* curr_region	= NULL;

	#ifdef ASSERT
	if(_mem_server_cset == NULL){
		tty->print("%s, Memory Server Scanned CSet is NULL. \n",__func__);
		curr_region = NULL;
		goto err;
	}
	#endif

	//G1SemeruCMCSetRegions* mem_server_cset = this->mem_server_cset();
	// No need  to check here.

	do{

		// claim a already scanned Region by CM
		//
		// [XXX] who has the most cross-region references with prev_compact region ??
		//
		curr_region	=	_mem_server_cset->claim_cm_scanned_next();


		// Cliam a Region successfully
		if( curr_region != NULL ){
			return curr_region;
		} // end of curr_region != NULL
	
	}while(curr_region != NULL);


err:
	return curr_region; // run out of Memory Server CSet regions.
}










/**
 * Semeru MS - Trigger the compact
 * 		
 * 1) Compact the scanned Region of memory server CSet.	Ø
 * 2) Evacuate alive objects marked in SemeruHeapRegion's alive_bitmap.
 * 
 */
void G1SemeruSTWCompact::semeru_stw_compact() {
	//_restart_for_overflow = false;		// freshly scan, not Remark

	_num_concurrent_workers = _num_active_tasks;  // This value is gotten from G1SemeruConcurrentMark

	uint active_workers = MAX2(1U, _num_concurrent_workers);

	// Setting active workers is not guaranteed since fewer
	// worker threads may currently exist and more may not be
	// available.
	active_workers = _concurrent_workers->update_active_workers(active_workers);
	log_info(semeru, mem_compact)("Using %u workers of %u for Semeru MS compacting", active_workers, _concurrent_workers->total_workers());

	// Parallel task terminator is set in "set_concurrency_and_phase()"
	set_concurrency_and_phase(active_workers, true /* concurrent */);  // actually here is executed in STW.

	// Build the G1SemeruSTWCompactGangTask here.
	// How about move them into G1SemeruSTWCompact, and get one to run here.
	G1SemeruSTWCompactGangTask compacting_task(this, active_workers);  		// Invoke the G1SemeruSTWCompactGangTask WorkGang to run.
	_concurrent_workers->run_task(&compacting_task);		// STWCompact share ConcurrentMark's concurrent workers.
	print_stats();
	
}



void G1SemeruSTWCompact::set_concurrency_and_phase(uint active_tasks, bool concurrent) {
	set_concurrency(active_tasks);

	_concurrent = concurrent;		// g1SemeruConcurrentMark->_concurrent specify which phase we are : CM or Remark.

	if (!concurrent) {
		// At this point we should be in a STW phase, and completed marking.
		assert_at_safepoint_on_vm_thread();
		assert(out_of_regions(),"%s, out_of_regions() check failed. \n",__func__ );
	}
}




/**
 * [?] What's the terminator used for ?
 *  
 */
void G1SemeruSTWCompact::set_concurrency(uint active_tasks) {
	assert(active_tasks <= _max_num_tasks, "we should not have more");

	_num_active_tasks = active_tasks;

	// Need to update the three data structures below according to the
	// number of active threads for this phase.
	// The paralell task terminator is used to synchronize the paralel STW compact tasks.
	// The _task_queues is the StarTask queue set, used by the thread. 
	// [?] is the queue_set used for work stealing ?
	_terminator = TaskTerminator((int) active_tasks, _compact_task_queues); 
	
	// _first_overflow_barrier_sync.set_n_workers((int) active_tasks);
	// _second_overflow_barrier_sync.set_n_workers((int) active_tasks);
}












//
// G1SemeruSTWtask
//

G1SemeruSTWCompactTerminatorTask::G1SemeruSTWCompactTerminatorTask(uint worker_id,	
																												G1SemeruSTWCompact* sc, 
																												SemeruCompactTaskQueue* inter_region_ref_q, 
																												uint max_regions ):
	_worker_id(worker_id),
	_semeru_h(G1SemeruCollectedHeap::heap()),
	_semeru_sc(sc),
	_alive_bitmap(NULL), // get from processed Region
	_curr_compacting_region(NULL),
	_has_aborted(false),
	_has_timed_out(false),
	_cp(NULL),
	_humongous_regions_removed(0),
	_inter_region_ref_queue(inter_region_ref_q)
{

	// #1 Get resource from the global list at G1SemeruSTWCompact
	_cp = _semeru_sc->compaction_point(_worker_id); 


}


/**
 * [?] exit termination ? 
 * What does this mean ? 
 *  
 */
bool G1SemeruSTWCompactTerminatorTask::should_exit_termination(){


	// This is called when we are in the termination protocol. We should
	// quit if, for some reason, this task wants to abort or the global
	// stack is not empty (this means that we can get work from it).
	return !inter_region_ref_taskqueue()->is_empty() || has_aborted();
}




/**
 * The main entry of Memory Server compaction.
 * Execute the 4 compction phase and some sub-phases.
 *  
 *  
 * 
 */
	void  G1SemeruSTWCompactTerminatorTask::do_memory_server_compaction() {
		assert(Thread::current()->is_ConcurrentGC_thread(), "Not a concurrent GC thread");
		ResourceMark rm;			// [?] What's this resource used for ?
		double start_vtime = os::elapsedVTime();
		//volatile bool *is_cpu_server_in_stw = ;
		flags_of_cpu_server_state* cpu_server_flags  = _semeru_sc->_semeru_h->cpu_server_flags();
		flags_of_mem_server_state* mem_server_flags	 = _semeru_sc->_semeru_h->mem_server_flags();

		//_worker_id = worker_id; // get the assigned woker_id by task dispatchr.
		//_inter_region_ref_queue = _semeru_sc->_compact_task_queues->queue(worker_id);
		assert(_inter_region_ref_queue!=NULL, "Get Cross_region_ref_update queue failed.");

		// [X] Remember to reset the compaction_point at the end of this work.
		// [??] Each compact task has a separate compaction_point ??
		//_cp = _semeru_sc->compaction_point(_worker_id); 

		SemeruHeapRegion* region_to_evacuate = NULL;

		log_debug(semeru, mem_compact)("%s, Enter SemeruSWTCompact worker[0x%x] \n", __func__, worker_id());



		// Claim scanned Regions from the MS CSet.
		do{
				// Check CPU server STW states.
				// Compact one Region at least. When we reach here, we already confirmed the cpu server STW mode.
				if(cpu_server_flags->_is_cpu_server_in_stw == false)
					break; // end the compaction.

				// Start: When the compacting for a Region is started,
				// do not interrupt it until the end of compacting.

				region_to_evacuate = _semeru_sc->claim_region_for_comapct(worker_id(), region_to_evacuate);
				if(region_to_evacuate != NULL && region_to_evacuate->alive_ratio() < COMPACT_THRESHOLD  ){
					log_debug(semeru,mem_compact)("%s, worker[0x%x] Claimed Region[0x%lx] to be evacuted.", __func__, worker_id(), (size_t)region_to_evacuate->hrm_index() );

					//	if(region_to_evacuate->hrm_index() == 0x7)
					//		check_cross_region_reg_queue(region_to_evacuate, "Before phase1, Region[0x7]");	


					// Phase#1 Sumarize alive objects' destinazion
					// 1) put forwarding pointer in alive object's markOop
					//
					// [??] Make this phase Concurrent ??
					//
					phase1_prepare_for_compact(region_to_evacuate);

					// Phase#2 Adjust object's intra-Region feild pointer
					// The adjustment is based on forwarding pointer.
					// This has to be finished bofore data copy, which may overwrite the original alive objects and their forwarding pointer.
					phase2_adjust_intra_region_pointer(region_to_evacuate);

					// Phase#2.1
					// Record the new address for the objects in target_obj_queue
					// Only these objects are cross-region referenced. 
					// Their new addr is stored in the markOop for now.
					record_new_addr_for_target_obj(region_to_evacuate);

	
					//
					// 1) It's safe to add the Region into Memory Server Flags now
					// 		CPU server can read the data now.
					// 2) If Claimed, must finish the compacting.
					//
					mem_server_flags->add_claimed_region(region_to_evacuate->hrm_index());

					// Phase#3 Do the compaction
					// Multiple worker threads do this parallelly
					phase3_compact_region(region_to_evacuate);


					// Debug Drain the CompactTask _cross_region_ref_update_queue
					//check_overflow_taskqueue("phase4 prepare.");

					// Check the Region's cross_region_ref queue
					//check_cross_region_reg_queue(region_to_evacuate, "Before phase4");					

					log_debug(semeru,mem_compact)("%s, worker[0x%x] Evacuation for Region[0x%lx] is done.", __func__, worker_id(), (size_t)region_to_evacuate->hrm_index() );
				}

			// End: Check if we need to stop the compacting work
			// and switch to the inter-region fields update phase.
		}while( cpu_server_flags->_is_cpu_server_in_stw && region_to_evacuate != NULL && !_semeru_sc->has_aborted() );




		// Only the last thread can set the flags value.
		// The Inter_region_ref queue can be non-empty when we get a termination offer.
		// 1) [?] Will this sync these threads here ? 
		//     and then go into the next block at the same time ?
		// 2) Can we also do work stealing here ?
		bool all_task_finished =	_semeru_sc->terminator()->offer_semeru_compact_termination(this);
		if(all_task_finished){
		//
		// Do the Phase#4, update inter-Region reference here.
		//
			log_debug(semeru,mem_compact)("%s, worker[0x%x] enter the final task block.", __func__, worker_id());
					// Phase#4 Inter-Region reference fields update.
					// Should adjust the inter-region reference before do the compaction.
					// 1) The target Region is in Memory Server CSet
					// 2) The target Region is in Another Server(CPU server, or another Memory server)

					// A.Synchronize with other comacption threads .
					//
					// B. Synchronized with other servers, CPU server or Memory server, to get the cross-region-ref-update queue.
					// 1）We have finished compact one Region, update its Cross_region_ref_queue to CPU server.
					// 2) If we recieved STW windown end signal, send current cross_region_ref_queue and Wait for information back
					// 3) After 2) is done, start update all the inter-region reference.


					//
					// Warning : This is a single compaction thread model.
					//

					// End condition
					// 1) STW window is closed.
          // 2) Empty the scanned Region CSet. 
					if(cpu_server_flags->_is_cpu_server_in_stw == false ||
						  region_to_evacuate == NULL ){

						log_debug(semeru,mem_compact)("%s, in STW Window ? %d . or Memoery Server Compacting is Finished ? %d.\n", 
																				__func__, cpu_server_flags->_is_cpu_server_in_stw, region_to_evacuate == NULL ? true : false  );

						// wait on cross_region_reference exchange
						// This flag means all the claimed Region are compacted.
						// CPU server has to re-read the Compacted_region information now.
						mem_server_flags->_mem_server_wait_on_data_exchange = true; // cpu server can send its data.
						
						// If the memory server finished the compaction earlier than CPU server's evacuation, busy wit on the lock.
						//
						// start exchange ==>> 
		
						log_debug(semeru,mem_compact)("%s, worker[0x%x] Wait on CPU server to send all the evacuated Cross_region_ref queue.\n", __func__, worker_id());
						while(cpu_server_flags->_cpu_server_data_sent == false){
						// memory server busy wait on the RDMA flag.
						// 1) CPU server(and other server) needs to send their data here.
						// 2) CPU server needs to read data from current this.
						//
						// CPU server and other server needs to use different pages!!
						}

						// Busy wait on exchanging finished <===



						//debug
						// Can't update cross-region reference before the end of STW window.
						log_debug(semeru,mem_compact)("%s, Start updating inter-region reference. worker[0x%x] \n", __func__, worker_id() );
						phase4_adjust_inter_region_pointer(region_to_evacuate);
					
						// Move out to task scheduler,
						// Only when both
						//mem_server_flags->_is_mem_server_in_compact = false; // compaction is totally done.
					
						//log_debug(semeru,mem_compact)("%s, Memoery Server Compacting is totally Finished.\n", __func__);
					}// STW window is going to close.


		}// end of all task finished.
		else{
			// Also can process the Inter_region_ref update queue , if the cpu server flags satisfy the condition.
			if(cpu_server_flags->_is_cpu_server_in_stw == false ||
						  region_to_evacuate == NULL ){

				// Busy wait on the data exchange finished.
				log_debug(semeru,mem_compact)("%s, worker[0x%x] Wait on CPU server to send all the evacuated Cross_region_ref queue.\n", __func__, worker_id() );
				while(cpu_server_flags->_cpu_server_data_sent == false){
					// memory server busy wait on the RDMA flag.
					// 1) CPU server(and other server) needs to send their data here.
					// 2) CPU server needs to read data from current this.
					//
					// CPU server and other server needs to use different pages!!
				}

				log_debug(semeru,mem_compact)("%s, Start updating inter-region reference. worker[0x%x] \n", __func__, worker_id() );
				phase4_adjust_inter_region_pointer(region_to_evacuate);

				//
				// Can we do work stealing here ??
				//


			} // cpu STW windown closed, or evacuated all the scanned Regions.

		}


		//
		// Finish of current Compaction window.
		// 2 posible conditions:
		// 1) The CPU STW window is closed.
		// 2) All the scanned Regions are processed.
		// 3) The CPU server is changed to none STW mode. 
		assert(_semeru_sc->mem_server_scanned_cset()->is_compact_finished() || cpu_server_flags->_is_cpu_server_in_stw == false  || _semeru_sc->has_aborted(),
										"%s, Semeru Memoery server's compaction stop unproperly. \n", __func__ );
		

		// Reset fields
		_cp->reset_compactionPoint();

		// statistics 
		double end_vtime = os::elapsedVTime();
		_semeru_sc->update_accum_task_vtime(worker_id(), end_vtime - start_vtime);

		log_debug(semeru,mem_compact)("%s, worker[0x%x] terminated. ", __func__, worker_id());
	}


/**
 * Phase#1, 
 * 1) calculate the destination address for alive objects
 *    Put forwarding pointer at alive objects' markOop
 * 
 * 2) The Region is claimed in G1SemeruSTWCompactGangTask::work()
 * 
 * Warning : this is a per Region processing. Pass the necessary stateless structures in.
 * 					e.g. the alive_bitmap and 
 * 
 */
void G1SemeruSTWCompactTerminatorTask::phase1_prepare_for_compact(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, worker[0x%x] Enter Semeru MS Compact Phase#1, preparation ", __func__, worker_id());

	// get _cp from G1SemeruSTWCompact->compaction_point(worker_id)
	G1SemeruCalculatePointersClosure semeru_ms_prepare(_semeru_sc, hr->alive_bitmap(), _cp, &_humongous_regions_removed);  
	semeru_ms_prepare.do_heap_region(hr);

	// update compaction_top to Region's top
	_cp->update();
}



/**
 * Phase#2, Adjust intra-Region pointers
 * Record the objects who is needed to update inter-Region pointers.
 * 
 * [x] All the forwarding pointers are stored in alive objects' markOop.
 * 			No need for the CompactionPoint.
 * 
 */






/**
 * Adjust intra-Region references for a single Region.
 * 
 * [x] This class is only used in .cpp file.
 * 
 */
class G1SemeruAdjustRegionClosure : public SemeruHeapRegionClosure {
  G1SemeruSTWCompact* _semeru_sc;
  G1CMBitMap* _bitmap;
  uint _worker_id;	// for debug
	SemeruCompactTaskQueue* _inter_region_ref_queue;  // points to G1SemeruSTWCompactGangTask->_inter_region_ref_queue

 public:
  G1SemeruAdjustRegionClosure(G1SemeruSTWCompact* semeru_sc, G1CMBitMap* bitmap, uint worker_id, SemeruCompactTaskQueue* inter_region_ref_queue) :
    _semeru_sc(semeru_sc),
    _bitmap(bitmap), 
		_worker_id(worker_id),
		_inter_region_ref_queue(inter_region_ref_queue) { }

  bool do_heap_region(SemeruHeapRegion* r) {
    G1SemeruAdjustClosure adjust_pointer(r, _inter_region_ref_queue); // build the closure by passing down the compact queue
    if (r->is_humongous()) {
      oop obj = oop(r->humongous_start_region()->bottom());  // get the humongous object
      obj->oop_iterate(&adjust_pointer, MemRegion(r->bottom(), r->top()));    // traverse the humongous object's fields.
    } else if (r->is_open_archive()) {
      // Only adjust the open archive regions, the closed ones
      // never change.
      G1SemeruAdjustLiveClosure adjust_oop(&adjust_pointer);
      r->apply_to_marked_objects(_bitmap, &adjust_oop);
      // Open archive regions will not be compacted and the marking information is
      // no longer needed. Clear it here to avoid having to do it later.
      _bitmap->clear_region(r);
    } else {
      G1SemeruAdjustLiveClosure adjust_oop(&adjust_pointer);
      r->apply_to_marked_objects(_bitmap, &adjust_oop);
    }
    return false;
  }


};



void G1SemeruSTWCompactTerminatorTask::phase2_adjust_intra_region_pointer(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, worker[0x%x]  Enter Semeru MS Compact Phase#2, pointer adjustment. ", __func__, this->_worker_id);

	G1SemeruAdjustRegionClosure adjust_region( _semeru_sc, hr->alive_bitmap(), _worker_id, _inter_region_ref_queue );
	adjust_region.do_heap_region(hr);
}




/**
 * Record the new address for the objects stored in Target_obj_queue.
 * Assumption
 * 	1) We are in a STW mode now. So no new cross-region reference will be added for this Region.
 *  2) Invoke this function after calculate and put the new addr in the alive objects header, markOop.
 *  	 After evacuation, the alive ojbects maybe override, and we get its new address.
 *  3) The Target_obj_queue is drained. 
 * 		 And all popped the objects are inserted into the _cross_region_ref_update_queue during its CM tracing procedure.
 * 			e.g. in function 
 * 
 */
void G1SemeruSTWCompactTerminatorTask::record_new_addr_for_target_obj(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, Store new address for the objects in Target_obj_queue of Region[0x%lx] , worker [0x%x] ", 
																																								__func__, (size_t)hr->hrm_index(), this->_worker_id);
	
	HashQueue* cross_region_ref_ptr = hr->cross_region_ref_update_queue();
	//cross_region_ref_ptr->organize(); // sort and de-duplicate

	size_t len = cross_region_ref_ptr->length(); // new length.
	size_t i;
	ElemPair* elem_ptr;
	oop new_addr;

	log_debug(semeru, mem_compact)("%s, worker[0x%x] Cross_region_ref_queue of Region[0x%lx], lenth 0x%lx  ", 
																														__func__, worker_id(),  (size_t)hr->hrm_index(),  len );

	for( i =0; i< len; i++){
		elem_ptr = cross_region_ref_ptr->retrieve_item(i);
		
		// For HashMap, it's ok to find some items to null.
		// if(elem_ptr->from == NULL){
		// 	continue;
		// }

		// new_addr = elem_ptr->from->forwardee(); // If the object is not moved, to can also be null.
		// //elem_ptr->to = new_addr;
		
		// //debug
		// log_trace(semeru,mem_compact)("%s, worker[0x%x] store target obj[0x%lx] <old addr 0x%lx, new addr 0x%lx >",__func__, 
		// 																		worker_id(), i, (size_t)elem_ptr->from, (size_t)new_addr );
	}// end of for

}

/**
 *  Phase#3 : Do a compaction for a single Region.
 * 						 This behavior is multiple thread safe.
 * 
 * Source : SemeruHeapRegion->_alive_bitmap
 * Dest	  : SemeruHeapRegion->_dest_region_ms + _dest_offset_ms
 * 
 * [?] How to handle the hugongous Regions separately ?
 * 		=> in phase#2, we set humongous Region's forwarding pointer to itself, which will not move any objects in them.
 * 
 */
void G1SemeruSTWCompactTerminatorTask::phase3_compact_region(SemeruHeapRegion* hr) {

	log_debug(semeru, mem_compact)("%s, Enter Semeru MS Compact Phase#3, object compactation, worker [0x%x] ", __func__, this->_worker_id);

	assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");

	// Warning : for a void parameter constructor, do not assign () at the end.
  G1SemeruCompactRegionClosure semeru_ms_compact;			// the closure to evacuate a single alive object to dest
  hr->apply_to_marked_objects(hr->alive_bitmap(), &semeru_ms_compact);  // Do this compaction in the bitmap.

	//
	// [?]Check the compaction is not interrupped. How ?
	//
	
	//check_overflow_taskqueue("At the end of phase3, copy");




	// clear process.
	// 1) Restore Compaction,e.g. _compaction_top, information to normal fields, e.g. _top
  // 2) Clear not used range.
	hr->complete_compaction();
}






/**
 * Drain the both overflow queue and taskqueue
 * 
 * Update by following the outgoing direction.
 * 
 * [XX] Slow Version [XX]
 * 	The problem is that, we need to check the target region for each objects.
 * 
 * 
 */
void G1SemeruSTWCompactTerminatorTask::update_cross_region_ref_taskqueue(){
  StarTask ref;
	oop new_target_oop_addr;
	oop old_target_oop_addr;
	SemeruHeapRegion* target_region;
  size_t count;
  SemeruCompactTaskQueue* inter_region_ref_queue = this->inter_region_ref_taskqueue();

  log_debug(semeru,mem_compact)("\n%s, start for updating Inter-Region ref, worker[0x%lx]", __func__, (size_t)worker_id() );


  // #1 Drain the overflow queue
  count =0;
	while (inter_region_ref_queue->pop_overflow(ref)) {

		 // the old addr can points to Regions in other severs.
		 // We have to fetch these Regions' _cross_region_ref_update_queue here.
    old_target_oop_addr = RawAccess<>::oop_load((oop*)ref);		
		if(old_target_oop_addr!= NULL ){

			assert((size_t)(HeapWord*)old_target_oop_addr != (size_t)0xbaadbabebaadbabe, "Wrong fields.");

			target_region = _semeru_sc->_semeru_h->heap_region_containing(old_target_oop_addr); // [??] This is too slow ?

			new_target_oop_addr = target_region->cross_region_ref_update_queue()->get(old_target_oop_addr);
			//assert(new_target_oop_addr != (oop)MAX_SIZE_T, "The corresponding item for old target oop 0x%lx can't be found.", (size_t)old_target_oop_addr );
			//Debug
			if(new_target_oop_addr == (oop)(HeapWord*)MAX_SIZE_T){
				tty->print("Overflow :Wrong in %s,  worker[0x%x]  old_target_oop_addr 0x%lx is not in Region[0x%lx]'s cross_region_ref queue \n", __func__, 
																																		worker_id(), (size_t)(HeapWord*)old_target_oop_addr, (size_t)target_region->hrm_index() );
				continue;
			}

			if(new_target_oop_addr!=NULL ){
				RawAccess<IS_NOT_NULL>::oop_store((oop*)ref, new_target_oop_addr);
			}else{
				log_debug(semeru,mem_compact)("%s, old target oop 0x%lx is not moved. ", __func__,(size_t)(HeapWord*)old_target_oop_addr );
			}

			log_debug(semeru,mem_compact)(" Overflow: update ref[0x%lx] 0x%lx from obj 0x%lx to new obj 0x%lx",
										           																count, (size_t)(HeapWord*)(oop*)ref ,(size_t)(HeapWord*)old_target_oop_addr, 
																															new_target_oop_addr == NULL ? (size_t)(HeapWord*)old_target_oop_addr : (size_t)(HeapWord*)new_target_oop_addr );

		}else{
       log_debug(semeru,mem_compact)(" Overflow: ERROR Find filed 0x%lx points to 0x%lx",(size_t)(oop*)ref, (size_t)(HeapWord*)old_target_oop_addr);
    }

    count++;
  }// end of while


  // #1 Drain the task queue
  count =0;
  while (inter_region_ref_queue->pop_local(ref, 0 /*threshold*/)) { 

 		 // the old addr can points to Regions in other severs.
		 // We have to fetch these Regions' _cross_region_ref_update_queue here.
    old_target_oop_addr = RawAccess<>::oop_load((oop*)ref);		
		if(old_target_oop_addr!= NULL ){

			assert((size_t)(HeapWord*)old_target_oop_addr != (size_t)0xbaadbabebaadbabe, "Wrong fields.");

			target_region = _semeru_sc->_semeru_h->heap_region_containing(old_target_oop_addr); // [??] This is too slow ?

			new_target_oop_addr = target_region->cross_region_ref_update_queue()->get(old_target_oop_addr);
			//assert(new_target_oop_addr != (oop)MAX_SIZE_T, "The corresponding item for old target oop 0x%lx can't be found.", (size_t)old_target_oop_addr );
			//Debug
			if(new_target_oop_addr == (oop)(HeapWord*)MAX_SIZE_T){
				tty->print(" Wrong in %s, worker[0x%x]  old_target_oop_addr 0x%lx is not in Region[0x%lx]'s cross_region_ref queue \n", __func__, 
																																worker_id(), (size_t)(HeapWord*)old_target_oop_addr, (size_t)target_region->hrm_index() );
				continue;
			}

			if(new_target_oop_addr!=NULL){
				RawAccess<IS_NOT_NULL>::oop_store((oop*)ref, new_target_oop_addr);
			}else{
				log_debug(semeru,mem_compact)("%s, old target oop 0x%lx is not moved. ", __func__,(size_t)(HeapWord*)old_target_oop_addr );
			}

			log_debug(semeru,mem_compact)(" update ref[0x%lx] 0x%lx from obj 0x%lx to new obj 0x%lx",
										           																count, (size_t)(oop*)ref ,(size_t)(HeapWord*)old_target_oop_addr, 
																															new_target_oop_addr == NULL ? (size_t)(HeapWord*)old_target_oop_addr : (size_t)(HeapWord*)new_target_oop_addr );

		}else{
       log_debug(semeru,mem_compact)(" ERROR Find filed 0x%lx points to 0x%lx",(size_t)(oop*)ref, (size_t)(HeapWord*)old_target_oop_addr);
    }
    count++;
  }// end of while

  assert(inter_region_ref_queue->is_empty(), "should drain the queue");

  log_debug(semeru,mem_compact)("%s, End for updating Inter-Region ref, worker[0x%lx] \n", __func__, (size_t)worker_id());
}











/**
 * Update the fields points to other region.
 * The region can be in current or other servers.
 * 
 * Warning. This is the old/original region, before compaction. 
 * But the field addr stored in SemeruHeapRegion->_inter_region_ref_queue is the new address.
 * 
 */
void G1SemeruSTWCompactTerminatorTask::phase4_adjust_inter_region_pointer(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, Enter Semeru MS Compact Phase#4, Inter-Region reference adjustment, worker [0x%x] ", __func__, this->_worker_id);

	// Drain current CompactTask's cross region ref update queue.
	update_cross_region_ref_taskqueue();

}






//
// Closures for G1SemeruSTWCompactGangTask -> Phase #1, preparation.
//


G1SemeruCalculatePointersClosure::G1SemeruCalculatePointersClosure( G1SemeruSTWCompact* semeru_sc,
																																		G1CMBitMap* bitmap,
                                                             				G1SemeruCompactionPoint* cp,
																																		uint* humongous_regions_removed) :
  _semeru_sc(semeru_sc),
  _bitmap(bitmap),
  _cp(cp),
	_humongous_regions_removed(humongous_regions_removed) { 

		#ifdef ASSERT
			tty->print("%s, initialized G1SemeruCalculatePointersClosure. \n", __func__);
		#endif
}



/**
 * Tag : the FullGC threads is summarizing a source Region's destination Region.
 * 
 * [x] The claimed source Region will be added into G1FullGCPrepareTask->G1SemeruCompactionPoint
 *      which points to collector()->compaction_point(worker_id).
 * 
 */
bool G1SemeruCalculatePointersClosure::do_heap_region(SemeruHeapRegion* hr) {
  if (hr->is_humongous()) {     // 1) Humongous objects are not moved
    oop obj = oop(hr->humongous_start_region()->bottom());
    if (_bitmap->is_marked(obj)) {      // Both start and following humongous Regions should be marked
      if (hr->is_starts_humongous()) {  // Only process the start humongous Region.
        obj->forward_to(obj);  // not moving the humongous objects.
      }
    } else {
      free_humongous_region(hr);
    }
  } else if (!hr->is_pinned()) {
    prepare_for_compaction(hr);   // 2) Normal objects  
  }

  // Reset data structures not valid after Full GC.
  reset_region_metadata(hr);

  return false;
}



/**
 * Tag : a humongous Region need to be freed separately ?? 
 *  
 */
void G1SemeruCalculatePointersClosure::free_humongous_region(SemeruHeapRegion* hr) {

	//debug
	tty->print("%s, Error Not implement this function yet. \n", __func__);


  // FreeRegionList dummy_free_list("Dummy Free List for G1MarkSweep");

  // hr->set_containing_set(NULL);
  // _humongous_regions_removed++;

  // _g1h->free_humongous_region(hr, &dummy_free_list);
  // prepare_for_compaction(hr);   // Add this Region into Destination Region candidates
  // dummy_free_list.remove_all();
}





/**
 *  Handle the Remerber set infor mation.
 *  
 */
void G1SemeruCalculatePointersClosure::reset_region_metadata(SemeruHeapRegion* hr) {


	//debug
	tty->print("%s, Error Not implement this function yet. \n", __func__);

  // hr->rem_set()->clear();
  // hr->clear_cardtable();

  // if (_g1h->g1_hot_card_cache()->use_cache()) {
  //   _g1h->g1_hot_card_cache()->reset_card_counts(hr);
  // }
}




/**
 * Semeru MS : Add a Region into current compaction thread's CompactionPoint.
 *  
 */
void G1SemeruCalculatePointersClosure::prepare_for_compaction(SemeruHeapRegion* hr) {


	//debug
	tty->print("%s, Calculate destination for alive objects in Region[0x%lx] \n",__func__, (size_t)hr->hrm_index());

  if (!_cp->is_initialized()) {   	// if G1SemeruCompactionPoint is not setted, compact to itself.
    hr->set_compaction_top(hr->bottom());
    _cp->initialize(hr, true);			// Enqueue current Source Region as the first Destination Region.
  }
  // Enqueue this Region into destination Region queue.
  _cp->add(hr);
  prepare_for_compaction_work(_cp, hr);

}


/**
 * Tag : Calculate the destination for source Region's alive objects.
 *  
 * Parameter:
 *   cp : the destination Region. Multiple source Regions may be compacted to it.
 *   hr : The source Region, who should be compacted to cp.
 * 
 */
void G1SemeruCalculatePointersClosure::prepare_for_compaction_work(G1SemeruCompactionPoint* cp,
                                                                                  SemeruHeapRegion* hr) {
  G1SemeruPrepareCompactLiveClosure prepare_compact(cp);
  hr->set_compaction_top(hr->bottom());     // SemeruHeapRegion->_compaction_top is that if using this Region as a Destination, this is his top.
  hr->apply_to_marked_objects(_bitmap, &prepare_compact);
}


/**
 * If any Region is freed.
 *  
 */
bool G1SemeruCalculatePointersClosure::freed_regions() {
  if (_humongous_regions_removed > 0) {
    // Free regions from dead humongous regions.
    return true;
  }

  if (!_cp->has_regions()) {
    // No regions in queue, so no free ones either.
    return false;
  }

  if (_cp->current_region() != _cp->regions()->last()) {
    // The current region used for compaction is not the last in the
    // queue. That means there is at least one free region in the queue.
    return true;
  }

  // No free regions in the queue.
  return false;
}


// Live object closure
//

G1SemeruPrepareCompactLiveClosure::G1SemeruPrepareCompactLiveClosure(G1SemeruCompactionPoint* cp) :
    _cp(cp) { }


/**
 * Tag : Preparation Phase #2, calculate the destination for each alive object in the source/current Region. 
 *        G1SemeruCompactionPoint* _cp, stores the destination Regions.
 */
size_t G1SemeruPrepareCompactLiveClosure::apply(oop object) {
  size_t size = object->size();
  _cp->forward(object, size);
  return size;
}





//
// Closures for G1SemeruSTWCompactGangTask -> Phase #2, adjust pointer
//










//
// Phase#3, do the alive object copy.
//




/**
 * Evacuate an alive object to the destination.
 * 
 * [?] Guarantee the MT safe ?
 * 		Case 1)  pre-calculate the alive objects destination. 
 * 			e.g. add one more pass to store the destination address in each alive object's markoop.
 * 			[x] Semeru MS takes this design. pre-estimate the destination address for the alive objects.
 * 			And then each compact thread can do the copy parallelly.
 * 
 * 		Case 2) Add local cache for the Concurrent GC Threads.
 * 			Need to be synchronized when requesting the thread local cache from Region.
 * 
 * [x] In order to support interruption and save scanning time, don't use the forwarding pointer desgin.
 * 		 Put forwarding pointer in alive object's markOop is un-recoverable.
 * 		 Pick a suitable Region to be evacuated to current destination Region. 
 * 	
 * [?] Need to maintain a <src, dst> pair to record the object moving information for inter-region field update.
 * 
 */
size_t G1SemeruCompactRegionClosure::apply(oop obj) {
  size_t size = obj->size();
  HeapWord* destination = (HeapWord*)obj->forwardee();   // [?] already moved
  if (destination == NULL) {
    // Object not moving
    return size;
  }

  // copy object and reinit its mark
  HeapWord* obj_addr = (HeapWord*) obj;
  assert(obj_addr != destination, "everything in this pass should be moving");
  Copy::aligned_conjoint_words(obj_addr, destination, size);      // 4 bytes alignment copy ?
  oop(destination)->init_mark_raw();    // initialize the MarkOop.
  assert(oop(destination)->klass() != NULL, "should have a class");

	log_trace(semeru,mem_compact)("Phase4,copy obj from 0x%lx to 0x%lx ", (size_t)obj_addr, (size_t)destination );

  return size;
}









//
// G1SemeruSTWCompactGangTask 
//



/**
 * Semeru MS - WorkGang, apply compaction closure to the scanned Regions.
 * 
 * 	1) this worker is scheduled form GangWorker::run_task(WorkData data)
 *  
 * About the TaskQueue and Terminator
 *  1) The Inter-Region-Ref update queue is got from G1SemeruSTWCompact dynamically.
 * 	2) We only drain the task queue after receiving the STW Window Closed signal.
 *  => Based on 1) and 2), we may get a non-empty task queue for current Task..
 * 
 */
void  G1SemeruSTWCompactGangTask::work(uint worker_id){

		{
			// Can this sts_join sync all the running GangWorkers ??
			SuspendibleThreadSetJoiner sts_join;	


			G1SemeruSTWCompactTerminatorTask* compact_task = _semeru_sc->task(worker_id);		// get the compct task
			//compact_task->record_start_time();					// [?] profiling ??
			if (!_semeru_sc->has_aborted()) {		// [?] aborted ? suspendible control ?
				do {

					// Execute the Memory Server Compaction code.
					compact_task->do_memory_server_compaction();

					// debug
					//_semeru_sc->do_yield_check();		// yield for what ? Must pair with SuspendibleThreadSetJoiner

					// [XX] if task has_aborted(), then re-do the marking until finished.
					// Usually, 
				} while (!_semeru_sc->has_aborted() && compact_task->has_aborted());  
		
			}

		} // end of thread join

}










//
// Debug functions
//


void G1SemeruSTWCompact::print_stats() {
	if (!log_is_enabled(Debug, gc, stats)) {
		return;
	}
	log_debug(gc, stats)("---------------------------------------------------------------------");
	for (size_t i = 0; i < _num_active_tasks; ++i) {
		//	_tasks[i]->print_stats();  // Semeru MS doesn't have any code tasks

		tty->print("%s, Not implemented yet \n",__func__);

		log_debug(gc, stats)("---------------------------------------------------------------------");
	}
}


/**
 * Just print, not pop any items. 
 */
void G1SemeruSTWCompactTerminatorTask::check_cross_region_reg_queue( SemeruHeapRegion* hr,  const char* message){
	size_t length = hr->cross_region_ref_update_queue()->length();
	size_t i;
	HashQueue* cross_region_reg_queue = hr->cross_region_ref_update_queue();
	ElemPair* q_iter;

	tty->print("%s,check_cross_region_reg_queue, Start for Region[0x%lx] \n", message, (size_t)hr->hrm_index() );

	for(i=0; i < length; i++){
		// q_iter = cross_region_reg_queue->retrieve_item(i);

		// if(q_iter->from != NULL){
		// 			// error Check
		// 	if(hr->is_in_reserved(q_iter->from) == false ){
		// 		tty->print("	Wong obj 0x%lx in Region[0x%lx]'s cross_region_reg_queue \n", (size_t)q_iter->from , (size_t)hr->hrm_index() );
		// 	}else{
		// 		//tty->print("	non-null item[0x%lx] from 0x%lx, to 0x%lx \n", i, (size_t)q_iter->from, (size_t)q_iter->to );
		// 	}

		// }



	}

	tty->print("%s,check_cross_region_reg_queue, End for Region[0x%lx] \n", message, (size_t)hr->hrm_index() );

}

// Drain the both overflow queue and taskqueue
void G1SemeruSTWCompactTerminatorTask::check_overflow_taskqueue( const char* message){
  StarTask ref;
  size_t count;
  SemeruCompactTaskQueue* inter_region_ref_queue = this->inter_region_ref_taskqueue();

  log_debug(semeru,mem_compact)("\n%s, start for Semeru MS CompactTask [0x%lx]", message, (size_t)worker_id() );


  // #1 Drain the overflow queue
  count =0;
	while (inter_region_ref_queue->pop_overflow(ref)) {
    oop const obj = RawAccess<>::oop_load((oop*)ref);
		if(obj!= NULL && (size_t)(HeapWord*)obj != (size_t)0xbaadbabebaadbabe){
			 log_debug(semeru,mem_compact)(" Overflow: ref[0x%lx] 0x%lx points to obj 0x%lx",
										           																								count, (size_t)(HeapWord*)(oop*)ref ,(size_t)(HeapWord*)obj);

		}else{
       log_debug(semeru,mem_compact)(" Overflow: ERROR Find filed 0x%lx points to 0x%lx",(size_t)(HeapWord*)(oop*)ref, (size_t)(HeapWord*)obj);
    }

    count++;
  }


  // #1 Drain the task queue
  count =0;
  while (inter_region_ref_queue->pop_local(ref, 0 /*threshold*/)) { 
    oop const obj = RawAccess<>::oop_load((oop*)ref);
		if(obj!= NULL && (size_t)(HeapWord*)obj != (size_t)0xbaadbabebaadbabe){
		 log_debug(semeru,mem_compact)(" ref[0x%lx] 0x%lx points to obj 0x%lx",
										           																	count, (size_t)(HeapWord*)(oop*)ref ,(size_t)(HeapWord*)obj);

		}else{
      log_debug(semeru,mem_compact)(" ERROR Find filed 0x%lx points to 0x%lx",(size_t)(HeapWord*)(oop*)ref, (size_t)(HeapWord*)obj );
    }

    count++;
  }



  assert(inter_region_ref_queue->is_empty(), "should drain the queue");

  log_debug(semeru,mem_compact)("%s, End for Semeru MS CompactTask [0x%lx] \n", message, (size_t)worker_id());
}

