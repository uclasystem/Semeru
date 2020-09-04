/**
 * This file contains all the class or structure used by RDMA transferring.
 *  
 */


#ifndef SHARE_GC_SHARED_RDMA_ALLOCATION
#define SHARE_GC_SHARED_RDMA_ALLOCATION

//#include "utilities/align.hpp"
//#include "utilities/sizes.hpp"

#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "memory/allocation.hpp"  // Include all the headers of orginal allocation.hpp
#include "logging/log.hpp"



#define MEM_SERVER_CSET_BUFFER_SIZE		(size_t)(512 - 8) 	


/**
 * CHeapRDMAObj allocation type.
 * This is used for allocating queue.
 * Decide its start address based on the queue type and corresponding value defined in globalDefinitions.hpp
 *  
 */
enum CHeapAllocType {
  CPU_TO_MEM_AT_INIT_ALLOCTYPE,   // 0,
  CPU_TO_MEM_AT_GC_ALLOCTYPE,     // 1,
  MEM_TO_CPU_AT_GC_ALLOCTYPE,     // 2,
  SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE,  // 3
  METADATA_SPACE_ALLOCTYPE,       // 4

  START_OF_QUEUE_WITH_REGION_INDEX,         // a holder to separate instance allocation and queue allocation.
  ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE,         // 6,
  CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE,  // 7, 
  HEAP_REGION_MANAGER_ALLOCTYPE,            // 8, used for the SemeruHeapRegion manager.
  NON_ALLOC_TYPE     // non type
};

/**
 * This allocator is used to allocate objects into fixed address for RDMA communications between CPU and memory server.
 * 
 * Template Parameter:
 *    class E : Used to calculate class array's element size.
 *    CHeapAllocType Alloc_type : Used to choose which RDMA meta space is allocated into.
 *  
 * 
 * [?] CHeapObj allocates object into C-Heap by malloc().
 *     We commit space from reserved space.
 *    
 * [x] We only need to use the operator new to invoke class's constructor.
 * 
 * [x] The RDMA class should use flexible array to store data.
 * 
 * [x] Its subclass may have non-static fields, so do NOT inherit from AllStatic class.
 * 
 */
template <class E , CHeapAllocType Alloc_type = NON_ALLOC_TYPE> 
class CHeapRDMAObj{
  friend class MmapArrayAllocator<E>;

private:

 


public:

  //
  //  Variables
  //

  // Used to calculate the offset for different instan's offset.
  // [xx] Each instance of this template class has its own copy of member static variables.
  //      So, based on different CHeapAllocType value, _alloc_ptr should be initiazlied to different value.
  //
  // [??] This filed is useless here for now.
  static char   *_alloc_ptr; 
  static size_t  _instance_size;  // only used for normal instance allocation. e.g. the obj in SemeruHeapRegion.


  //
  //  Functions
  //

  //
  // Warning : this funtion is invoked latter than operator new.
  CHeapRDMAObj(){

  }

  /**
   * The default allocation method.
   * The allocation address is not fixed.
   * Use mtGC as MEMFLAGS.
   */
  ALWAYSINLINE void* operator new(size_t instance_size) throw() {
    return (void*)AllocateHeap(instance_size, mtGC);           // [x] Allocate object into C-Heap
  }


  /**
   * new object instance #1
   * Allocate space for non-array object instance with [flexible array].
   *  1) Single structure.
   *  2) Content are stored in a flexible array
   *  3) Fixed start address.
   *  4) Both the instance and flexible array share the commited space.
   *      So, if don't want to wast space, do not allocate fields for the class itself.
   * 
   * More explanation
   * 
   * 1) this function is only used to COMMIT space directly.
   *    There is no need to commit space on reserved space. But reserve sapce first is more safe.
   * 2) the new operation first, invoke this override function to allocate space
   *    second, it invokes the constructor to do initialization.
   *    BUT the return value of operator new, has to be void*.
   * 3) The override operator always work like static, 
   *     It can only invoke static functions.
   * 4) The first parameter, size, is assigned by Operator new. It's the size of the class.
   *    The commit size is passed by the caller,  
   *      a. to reserve space for the flexible array at the end of the class
   *      b. for alignment.
	 */
  ALWAYSINLINE void* operator new(size_t instance_size, size_t commit_size , char* requested_addr) throw() {
    assert(commit_size > instance_size, "Committed size is too small. ");

    // discard the parameter, size, which is defined by sizeof(clas)
    // [XX] Commit the space directly. Witout reserving procedure.
    return (void*)commit_at(commit_size, mtGC, requested_addr);
  }



 /**
  * new object instance #2.
  * Allocate object instances based on Alloc_type and index.
  * 
  * 1) Each object instance should be equal here.
  * 2) Commit the whole space at first allocation.
  *    And then bump the pointer, CHeapRDMAObj<ClassType E, AllocType>::_alloc_ptr for each allocation.
  * 
  */
 ALWAYSINLINE void* operator new(size_t instance_size, size_t index ) throw() {
    // Calculate the queue's entire size, 4KB alignment
    // The commit_size for every queue type is fixed.
    size_t commit_size = 0;
    char* requested_addr = NULL;
    char* old_val;
    char* ret;
    switch(Alloc_type)  // based on the instantiation of Template
    {
      case CPU_TO_MEM_AT_INIT_ALLOCTYPE :
        // 1) page alignment
        commit_size = align_up(instance_size, PAGE_SIZE);

        // 1) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET);
          CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(CPU_TO_MEMORY_INIT_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){

            log_debug(semeru, alloc)("Commit the start area to 0x%lx for CPU_TO_MEM_AT_INIT_ALLOCTYPE , size 0x%lx .", 
                                                                (size_t)requested_addr, (size_t)CPU_TO_MEMORY_INIT_SIZE_LIMIT);

            old_val = CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_INIT_ALLOCTYPE , item[1].", 
                                                                (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET + CPU_TO_MEMORY_INIT_SIZE_LIMIT), 
                                                        "%s, Exceed the CPU_TO_MEMORY_INIT_SIZE_LIMIT's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under CPU_TO_MEM_AT_INIT_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  CPU_TO_MEM_AT_INIT_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Allocate [0x%lx, 0x%lx). Bump _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_INIT_ALLOCTYPE.", 
                                            (size_t)requested_addr, (size_t)commit_size, (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr);

        break;


      case CPU_TO_MEM_AT_GC_ALLOCTYPE :
        // 1) page alignment
        commit_size = align_up(instance_size, PAGE_SIZE);

        // 2) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + CPU_TO_MEMORY_GC_OFFSET);
          CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.

          
          if( (char*)commit_at(CPU_TO_MEMORY_GC_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){

            log_debug(semeru, alloc)("Commit the start area to 0x%lx for CPU_TO_MEMORY_GC_SIZE_LIMIT , size 0x%lx .", 
                                                                (size_t)requested_addr, (size_t)CPU_TO_MEMORY_GC_SIZE_LIMIT);

            old_val = CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_GC_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_GC_OFFSET + CPU_TO_MEMORY_GC_SIZE_LIMIT), 
                                                        "%s, Exceed the CPU_TO_MEMORY_INIT_SIZE_LIMIT's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under CPU_TO_MEM_AT_GC_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_GC_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  CPU_TO_MEM_AT_GC_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Allocate [0x%lx, 0x%lx). Bump _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_GC_ALLOCTYPE.", 
                                            (size_t)requested_addr, (size_t)commit_size, (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr);

        break;


      case MEM_TO_CPU_AT_GC_ALLOCTYPE :
        // 1) page alignment
        commit_size = align_up(instance_size, PAGE_SIZE);

        // 2) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET);
          CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(MEMORY_TO_CPU_GC_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){

            log_debug(semeru, alloc)("Commit the start area to 0x%lx for MEMORY_TO_CPU_GC_SIZE_LIMIT , size 0x%lx .", 
                                                                (size_t)requested_addr, (size_t)MEMORY_TO_CPU_GC_SIZE_LIMIT);

            old_val = CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for MEM_TO_CPU_AT_GC_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET + MEMORY_TO_CPU_GC_SIZE_LIMIT), 
                                                        "%s, Exceed the MEM_TO_CPU_AT_GC_ALLOCTYPE's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under MEM_TO_CPU_AT_GC_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  MEM_TO_CPU_AT_GC_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Allocate [0x%lx, 0x%lx). Bump _alloc_ptr to 0x%lx for MEM_TO_CPU_AT_GC_ALLOCTYPE.", 
                                            (size_t)requested_addr, (size_t)commit_size, (size_t)CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr);

        break;



      case SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE :
        // 1) page alignment
        commit_size = align_up(instance_size, PAGE_SIZE);

        // 2) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + SYNC_MEMORY_AND_CPU_OFFSET);
          CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(SYNC_MEMORY_AND_CPU_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){

            log_debug(semeru, alloc)("Commit the start area to 0x%lx for SYNC_MEMORY_AND_CPU_SIZE_LIMIT , size 0x%lx .", 
                                                                (size_t)requested_addr, (size_t)SYNC_MEMORY_AND_CPU_SIZE_LIMIT);

            old_val = CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + SYNC_MEMORY_AND_CPU_OFFSET + SYNC_MEMORY_AND_CPU_SIZE_LIMIT), 
                                                        "%s, Exceed the SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Allocate [0x%lx, 0x%lx). Bump _alloc_ptr to 0x%lx for SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE.", 
                                            (size_t)requested_addr, (size_t)commit_size, (size_t)CHeapRDMAObj<E, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>::_alloc_ptr);

        break;



      case METADATA_SPACE_ALLOCTYPE :
        // 1) page alignment
        commit_size = align_up(instance_size, PAGE_SIZE);

        // 2) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + KLASS_INSTANCE_OFFSET);
          CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(KLASS_INSTANCE_OFFSET_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){

            log_debug(semeru, alloc)("Commit the start area to 0x%lx for METADATA_SPACE_ALLOCTYPE , size 0x%lx .", 
                                                                (size_t)requested_addr, (size_t)KLASS_INSTANCE_OFFSET_SIZE_LIMIT);

            old_val = CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for METADATA_SPACE_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + KLASS_INSTANCE_OFFSET + KLASS_INSTANCE_OFFSET_SIZE_LIMIT), 
                                                        "%s, Exceed the METADATA_SPACE_ALLOCTYPE's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under METADATA_SPACE_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + KLASS_INSTANCE_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  METADATA_SPACE_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Allocate [0x%lx, 0x%lx). Bump _alloc_ptr to 0x%lx for METADATA_SPACE_ALLOCTYPE.", 
                                            (size_t)requested_addr, (size_t)commit_size, (size_t)CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr);

        break;

      case HEAP_REGION_MANAGER_ALLOCTYPE :

        // 1) Change to page_alignment  for this type
        commit_size = align_up(instance_size, PAGE_SIZE);

        // 2) Precommit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + HEAP_REGION_MANAGER_OFFSET);
          CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(HEAP_REGION_MANAGER_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){

            log_debug(semeru, alloc)("Commit the start area to 0x%lx for HEAP_REGION_MANAGER , size 0x%lx .", 
                                                                (size_t)requested_addr, (size_t)HEAP_REGION_MANAGER_SIZE_LIMIT);

            old_val = CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for HEAP_REGION_MANAGER.", 
                                                                (size_t)CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + HEAP_REGION_MANAGER_OFFSET + HEAP_REGION_MANAGER_SIZE_LIMIT), 
                                                        "%s, Exceed the METADATA_SPACE_ALLOCTYPE's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under METADATA_SPACE_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + KLASS_INSTANCE_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, METADATA_SPACE_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  METADATA_SPACE_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Allocate [0x%lx, 0x%lx). Bump _alloc_ptr to 0x%lx for HEAP_REGION_MANAGER_ALLOCTYPE.", 
                                            (size_t)requested_addr, (size_t)commit_size, (size_t)CHeapRDMAObj<E, HEAP_REGION_MANAGER_ALLOCTYPE>::_alloc_ptr);

        break;




      case NON_ALLOC_TYPE :
          //debug
          tty->print("Error in %s. Can't reach here. \n",__func__);
        break;


      default:
        requested_addr = NULL;
        break;
    }



    // discard the parameter, size, which is defined by sizeof(clas)
    return (void*)requested_addr;
  }








  /**
   * new object array #1
   * 
   * Allocate space for Object Array.
   *  1) multiple queue with index information.
   *  2) Content is not a flexible array.
   *  3) Fixed start_addr.
   * 
   * Parameters
   *  size, the instance size 
   *  element_size,  the size of 
   *  index, 
   *  Alloc_type, which kind of RDMA meta data is using.
   * 
   * MT Safe :
   *  Make this allocation Multiple-Thread safe ?
   *  
   * Warning : For object array, the ClassType E, is the object array's type, 
   *  e.g. For Target Object Queue, it should be StarTask
   *       For Cross Region Ref Update Queue, it should be ElemPair.
   */
  ALWAYSINLINE void* operator new(size_t instance_size, size_t element_length, size_t index ) throw() {
    // Calculate the queue's entire size, 4KB alignment
    // The commit_size for every queue type is fixed.
    size_t commit_size = commit_size_for_queue(instance_size, element_length); // need to record how much memory is used
    char* requested_addr = NULL;
    char* old_val;
    char* ret;
    switch(Alloc_type)  // based on the instantiation of Template
    {
      case ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE :
        requested_addr = (char*)(SEMERU_START_ADDR + CROSS_REGION_REF_TARGET_Q_OFFSET + index * commit_size) ;

        assert(requested_addr + commit_size < (char*)(SEMERU_START_ADDR + CROSS_REGION_REF_TARGET_Q_OFFSET +CROSS_REGION_REF_TARGET_Q_SIZE_LIMIT), 
                                                        "%s, Exceed the TARGET_OBJ_QUEUE's space range. \n", __func__ );
        
        // [?] How to handle the failure ?
        //     The _alloc_ptr is useless here.
        old_val = CHeapRDMAObj<E, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>::_alloc_ptr;
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>::_alloc_ptr), old_val);
        assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
        
        break;



    case CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE :
        
        assert(false, "Not implement.");
        
        // requested_addr = (char*)(SEMERU_START_ADDR + CROSS_REGION_REF_UPDATE_Q_OFFSET + index * commit_size) ;

        // assert(requested_addr + commit_size < (char*)(SEMERU_START_ADDR + CROSS_REGION_REF_UPDATE_Q_OFFSET +CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT), 
        //                                                 "%s, Exceed the TARGET_OBJ_QUEUE's space range. \n", __func__ );
        
        // // [?] How to handle the failure ?
        // //     The _alloc_ptr is useless here.
        // old_val = CHeapRDMAObj<E, CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE>::_alloc_ptr;
        // ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE>::_alloc_ptr), old_val);
        // assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
        
        break;



      case NON_ALLOC_TYPE :
          //debug
          tty->print("Error in %s. Can't reach here. \n",__func__);
        break;

      default:
        requested_addr = NULL;
        break;
    }



    // discard the parameter, size, which is defined by sizeof(clas)
    return (void*)commit_at(commit_size, mtGC, requested_addr);
  }


  // commit space on reserved space
 // static char* commit_at(size_t length, MEMFLAGS flags, char* requested_addr)

  static E*     commit_at(size_t commit_byte_size, MEMFLAGS flags, char* requested_addr);
  static size_t commit_size_for_queue(size_t instance_size, size_t elem_length); 
  // debug
  static E* test_new_operator( size_t size, size_t commit_size, char* requested_addr);
};






// https://stackoverflow.com/questions/610245/where-and-why-do-i-have-to-put-the-template-and-typename-keywords
// Each instantiation of the tmplate class has different instance of static variable
// operator new is invoked before the constructor,
// so it's useless to initiate the value here.
// We assign the values in operator new.
template <class E , CHeapAllocType Alloc_type>
char* CHeapRDMAObj<E, Alloc_type>::_alloc_ptr = NULL;


template <class E , CHeapAllocType Alloc_type>
size_t CHeapRDMAObj<E, Alloc_type>::_instance_size = 0;






#endif // SHARE_GC_SHARED_RDMA_ALLOCATION