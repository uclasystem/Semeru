/**
 * for RDMA communication structure.
 *  
 */

#include "gc/shared/rdmaAllocation.inline.hpp"   // why can't find this header by using shared/rdmaStructure.hpp






/**
 * Structure - CHeapRDMAObj
 */





template <class E , CHeapAllocType Alloc_type> 
E* CHeapRDMAObj<E, Alloc_type>::test_new_operator( size_t size, size_t commit_size, char* requested_addr ){
    tty->print("received parameters size %lu, commit_size %lu, requested_addr 0x%lx \n",
                      size, commit_size, (size_t)requested_addr);

    return NULL;
}


