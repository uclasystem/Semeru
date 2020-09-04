/**
 * for RDMA communication structure.
 *  
 */

#include "gc/shared/rdmaStructure.inline.hpp"   // why can't find this header by using shared/rdmaStructure.hpp





//
// Structure - TaskQueueRDMASuper
//




//
// Structure - GenericTaskQueueRDMA
//








template<class E, CHeapAllocType Alloc_type, unsigned int N>
int GenericTaskQueueRDMA<E, Alloc_type, N>::next_random_queue_id() {
  return randomParkAndMiller(&_seed);  // defined in taskqueue.inlinel.hpp
}








//
// OverflowTargetObjQueue
//










// The size of the flexible array _region_cset[] is limited by global macro, utilities/globalDefinitions.hpp :
//	#define MEMORY_SERVER_CSET_OFFSET     (size_t)0x8000000   // +128MB
//	#define MEMORY_SERVER_CSET_SIZE       (size_t)0x1000      // 4KB 
received_memory_server_cset::received_memory_server_cset(){
	
	reset(); // reset fields.
	
}



flags_of_cpu_server_state::flags_of_cpu_server_state():
_is_cpu_server_in_stw(false),
_cpu_server_data_sent(false)
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of field _is_cpu_server_in_stw : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_is_cpu_server_in_stw)	);
	#endif
}


flags_of_mem_server_state::flags_of_mem_server_state():
_mem_server_wait_on_data_exchange(false),
_is_mem_server_in_compact(false),
_compacted_region_length(0)
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of first field _mem_server_wait_on_data_exchange : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_mem_server_wait_on_data_exchange)	);
	#endif
}