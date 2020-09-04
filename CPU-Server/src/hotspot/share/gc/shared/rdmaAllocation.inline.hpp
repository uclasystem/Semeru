/**
 * 1) All the inline functions should be visible for the compiler, when it compiles a translation unit(or compilation unit).
 * So, these functions need to be defined in a header file and included by the cpp, who needs them. 
 *
 * 2) put the keyword inline before definition is good enough. This will eliminate the multiple definitions problem,
 *    when inlcude to multiple cpp files.
 */

#ifndef SHARE_GC_SHARED_RDMA_ALLOCATION_INLINE
#define SHARE_GC_SHARED_RDMA_ALLOCATION_INLINE



#include "gc/shared/rdmaAllocation.hpp"



//
// Structure - CHeapRDMAObj
//


// Commit the space at already reserved space directly.
template <class E, CHeapAllocType Alloc_type>
E* CHeapRDMAObj<E, Alloc_type>::commit_at(size_t commit_size, MEMFLAGS flags, char* requested_addr) {
  // commit_size is already calculated well.
	//size_t size = size_for(length);

  // why here is !ExecMem ?
  os::commit_memory_or_exit(requested_addr, commit_size, !ExecMem, "Allocator (commit)");  // Commit the space.

  return (E*)requested_addr;
}



/**
 * Byte size for the whole OverflowTargetObjQueue - Used for Operator new.
 * 		1) instance size , PAGE_SIZE alignment.
 * 		2) class E* _elems size. points to start_addr + align_up(instance_size, PAGE_SIZE).
 * 		
 * 	4KB alginment.
 * 
 * 
 */
template <class E, CHeapAllocType Alloc_type>
size_t CHeapRDMAObj<E, Alloc_type>::commit_size_for_queue(size_t instance_size, size_t elem_length) {
  size_t size = elem_length * sizeof(E) + align_up(instance_size, PAGE_SIZE);;
  int alignment = os::vm_allocation_granularity();  // 4KB alignment.
  return align_up(size, alignment);
}




#endif // SHARE_GC_SHARED_RDMA_ALLOCATION_INLINE