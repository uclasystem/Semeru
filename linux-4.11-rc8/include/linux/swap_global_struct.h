/**
 * Include some global structures used for Semeru swapping
 * 
 * Warning : 
 *  1) Only include this file in .ccp file for debuging.
 *  2) The inline function can be invoked in multiple .ccp file.
 * 		 And these .ccp files may be merged into one .o 
 *     So, we have to declare the inline function as static.
 * 		 static functions can only be used in the .cpp included it by making a copy.
 * 		 In this case, every .ccp will have its self copy of inline function without conflict.
 */ 

#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/mm_inline.h>
#include <linux/list.h>
#include <asm-generic/bug.h>


//
//  ###################### Module control option  ######################
//

// enable the swp_entry_t to virtual address remap or not
// The memory range not in the RANGE will be not swapped out by adding them into unevictable list.
#define ENABLE_SWP_ENTRY_VIRT_REMAPPING 1

// This sync is uselesss. Because all the unmapped dirty page will be writteen to swap partition immediately.
//#define SYNC_PAGE_OUT



//
// ###################### Debug options ######################
//



//
// Enable debug information printing 
//

//#define DEBUG_SERVER_HOME  // Do kernel bug @ local server


//#define DEBUG_SWAP_PATH 1
//#define DEBUG_SWAP_PATH_DETAIL 1

//#define DEBUG_FLUSH_LIST 1
//#define DEBUG_FLUSH_LIST_DETAIL 1

//#define DEBUG_BIO_MERGE 1
//#define DEBUG_BIO_MERGE_DETAIL 1

// #define DEBUG_REQUEST_TAG 1

//#define DEBUG_LATENCY_CLIENT 1
//#define DEBUG_MODE_BRIEF 1 
//#define DEBUG_MODE_DETAIL 1
//#define DEBUG_BD_ONLY 1			// Build and install BD & RDMA modules, but not connect them.
//#define DEBUG_RDMA_ONLY		   1			// Only build and install RDMA modules.

//#define ASSERT 1		// general debug 






//
// Basic Macro
//

#define ONE_MB    ((size_t)1048576)				// 1024 x 2014 bytes
#define ONE_GB    ((size_t)1073741824)   	// 1024 x 1024 x 1024 bytes


//
// RDMA Related
//

// Already defined in kernel.

#ifndef PAGE_SIZE
	#define PAGE_SIZE		      						((size_t)4096)	// bytes, use the define of kernel.
#endif

//#define PAGE_SHIFT
//#define PAGE_MASK

#define REGION_SIZE_GB    						((size_t)4)   	// RDMA manage granularity, not the Heap Region.
#define RDMA_DATA_REGION_NUM     			8


// 1) Limit the outstanding rdma wr.
//    1-sided RDMA only need to issue a ib_rdma_wr to read/write the remote data.
//    2-sided RDMA need to post a recv wr to receive the data sent by remote side.
// 2) Both the send/recv queue will cost swiotlb buffer, so we can't make them too large.
//    For Semeru, most of our data are transfered by 1-sided RDMA.
//    So we don't need a large queue for the recv queue.
// 3) [X] This is the queue depth for the only qp. Nothing to do with the core number.
//
#define RDMA_SEND_QUEUE_DEPTH		4096		// for the qp. Find the max number without warning.
#define RDMA_RECV_QUEUE_DEPTH		32

extern uint64_t RMEM_SIZE_IN_PHY_SECT;			// [?] Where is it defined ? 

#define REGION_BIT					ilog2(REGION_SIZE_GB) + ilog2(ONE_GB)
#define REGION_MASK					(size_t)(((size_t)1 << REGION_BIT) -1)


// Each request can have multiple bio, but each bio can only have 1  pages ??
#define MAX_REQUEST_SGL								 32 		// number of segments, get from ibv_query_device. Use 30, or it's not safe..
//#define MAX_SEGMENT_IN_REQUEST			 32 // use the MAX_REQUEST_SGL
//#define ONE_SIEDED_RDMA_BUF_SIZE			(u64)MAX_REQUEST_SGL * PAGE_SIZE



// Synchronization mask
#define	DIRTY_TAG_MASK 	 (uint32_t)( ((1<<16) - 1) << 16)		// high 16 bits of the uint32_t
#define VERSION_MASK	 	 (uint32_t)( (1<<16) - 1 )						// low 16 bits of the uint32_t

// The high 16 bits can only be 1 or 0.
#define DIRTY_TAG_SEND_START	 (uint32_t)(1<<16)				// 1,0000,0000,0000,0000, OR this value to set the high 16 bits as 1.
#define DIRTY_TAG_SEND_END		 (uint32_t)( (1<<16) - 1)	// 0,1111,1111,1111,1111, AND this vlaue to set high 16 bits as 0.







//
//################################## Address information ##################################


#define SEMERU_START_ADDR   ((size_t)0x400000000000)

#define RDMA_ALIGNMENT_BYTES    64  // cache line.

// RDMA structure space
// [  Small meta data  ]  [ aliv_bitmap per region ]   [ dest_bitmap per region ] [ reserved for now]
#define RDMA_STRUCTURE_SPACE_SIZE  ((size_t) ONE_GB *4)

#define RDMA_DATA_SPACE_START_ADDR (size_t)(SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE)


// Number of Memory server
#define NUM_OF_MEMORY_SERVER	2

// Memory server #1, Region[1] to Region[5]
#define MEMORY_SERVER_0_REGION_START_ID		1
#define MEMORY_SERVER_0_START_ADDR	(size_t)(SEMERU_START_ADDR + MEMORY_SERVER_0_REGION_START_ID * RDMA_STRUCTURE_SPACE_SIZE)

// Memory server #2, Region[5] to Region[9]
#define MEMORY_SERVER_1_REGION_START_ID		5
//#define MEMORY_SERVER_1_REGION_START_ID		9		//debug, single server
#define MEMORY_SERVER_1_START_ADDR	(size_t)(SEMERU_START_ADDR + MEMORY_SERVER_1_REGION_START_ID * RDMA_STRUCTURE_SPACE_SIZE)

//
// Offset for each Part

// 1. Alive bitmatp
//    Used for concurrent marking.
//    range [0, 512MB). 1 : 64, 1 bit for a HeapWord. Reserve 512MB to cover a 32GB heap.
//    [x] Need to pad the unused space for RDMA buffer.
#define ALIVE_BITMAP_OFFSET      (size_t)0x0     // 0x400,000,000,000
#define ALIVE_BITMAP_SIZE        (size_t)(512*ONE_MB)  // end at 0x400,020,000,000



// 2. Klass instance space.
//    Used for store klass instance, class loader related information.
//    range [1GB, 1GB+256MB). The usage is based on application.
//    [?] Pre commit tall the space ?
#define KLASS_INSTANCE_OFFSET               (size_t)(ALIVE_BITMAP_OFFSET + ALIVE_BITMAP_SIZE)    // +512MB, 0x400,020,000,000
#define KLASS_INSTANCE_OFFSET_SIZE_LIMIT    (size_t)(256*ONE_MB)                                 //       0x400,030,000,000


// 3. Small meta data 
//

// 3.1 Meta of HeapRegion.
// These information need to be synchronized between CPU server and memory server.
// Reserve 4K per region is enough.

// 3.1 SemeruHeapRegion Manager
//     The structure of SemeruHeapRegion. 4K for each Region is enough.
//     [x] precommit all the space.
#define HEAP_REGION_MANAGER_OFFSET           (size_t)(KLASS_INSTANCE_OFFSET + KLASS_INSTANCE_OFFSET_SIZE_LIMIT)  // +768MB,  0x400,030,000,000
#define HEAP_REGION_MANAGER_SIZE_LIMIT       (size_t)(4*ONE_MB) // each SemeruHeapRegion should less than 4K, this is enough for 1024 HeapRegion.


// 3.1.1 CPU Server To Memory server, Initialization
// [x] precommit
#define CPU_TO_MEMORY_INIT_OFFSET     (size_t)(HEAP_REGION_MANAGER_OFFSET + HEAP_REGION_MANAGER_SIZE_LIMIT) // +768M +4MB, 0x400,030,400,000
#define CPU_TO_MEMORY_INIT_SIZE_LIMIT (size_t) 4*ONE_MB    //

// 3.1.2 CPU Server To Memory server, GC
// [x] precommit
#define CPU_TO_MEMORY_GC_OFFSET       (size_t)(CPU_TO_MEMORY_INIT_OFFSET + CPU_TO_MEMORY_INIT_SIZE_LIMIT) // +768M +8MB, 0x400,030,800,000
#define CPU_TO_MEMORY_GC_SIZE_LIMIT   (size_t) 4*ONE_MB    //


// 3.1.3 Memory server To CPU server 
// [x] precommit
#define MEMORY_TO_CPU_GC_OFFSET       (size_t)(CPU_TO_MEMORY_GC_OFFSET + CPU_TO_MEMORY_GC_SIZE_LIMIT) // +768M +12MB, 0x400,030,C00,000
#define MEMORY_TO_CPU_GC_SIZE_LIMIT   (size_t) 4*ONE_MB    //


// 3.1.4 Synchonize between CPU server and memory server
// [x] precommit
#define SYNC_MEMORY_AND_CPU_OFFSET       (size_t)(MEMORY_TO_CPU_GC_OFFSET + MEMORY_TO_CPU_GC_SIZE_LIMIT) // +768M +16MB, 0x400,031,000,000
#define SYNC_MEMORY_AND_CPU_SIZE_LIMIT   (size_t) 4*ONE_MB    //



// 4. Block Offset Table 

// 4.1 The g1SemeruCollectedHeap->G1SemeruBlockOffsetTable, _bot
// [x] precommit
#define BOT_GLOBAL_STRUCT_OFFSET            (size_t)(SYNC_MEMORY_AND_CPU_OFFSET + SYNC_MEMORY_AND_CPU_SIZE_LIMIT)  // +768M +20MB, 0x400,031,400,000
#define BOT_GLOBAL_STRUCT_SIZE_LIMIT        (size_t)(PAGE_SIZE) 


// 4.2 The G1SemeruBlockOffsetTable->_offset_array
//     Every SemeruHeapRegion will use a part of the _offset_array.
//     1 u_char for a Card,512 bytes.  1 : 512,  128MB bot can cover 64GB heap.
//     [x]precommit by us for debug, no need to pad.
#define BLOCK_OFFSET_TABLE_OFFSET             (size_t)(BOT_GLOBAL_STRUCT_OFFSET + BOT_GLOBAL_STRUCT_SIZE_LIMIT)    // +768M +20MB +4K, 0x400,031,401,000
#define BLOCK_OFFSET_TABLE_OFFSET_SIZE_LIMIT  (size_t)128*ONE_MB    // 1 : 512,





// 5. JVM global flags.
//

// 5.1 Memory server CSet
// [x] precommit
#define MEMORY_SERVER_CSET_OFFSET     (size_t)(BLOCK_OFFSET_TABLE_OFFSET + BLOCK_OFFSET_TABLE_OFFSET_SIZE_LIMIT)    // +1GB +4K, 0x400,050,000,000
#define MEMORY_SERVER_CSET_SIZE       (size_t)PAGE_SIZE   // 4KB 

// 5.2 cpu server state, STW or Mutator 
// Used as CPU <--> Memory server state exchange
// [x] precommit
#define FLAGS_OF_CPU_SERVER_STATE_OFFSET    (size_t)(MEMORY_SERVER_CSET_OFFSET + MEMORY_SERVER_CSET_SIZE)  // +4KB, 0x400,008,001,000
#define FLAGS_OF_CPU_SERVER_STATE_SIZE      (size_t)PAGE_SIZE      // 4KB 


// 5.3 memory server flags 
// [x] precommit
#define FLAGS_OF_MEM_SERVER_STATE_OFFSET    (size_t)(FLAGS_OF_CPU_SERVER_STATE_OFFSET + FLAGS_OF_CPU_SERVER_STATE_SIZE)  // +4KB, 0x400,008,002,000
#define FLAGS_OF_MEM_SERVER_STATE_SIZE      (size_t)PAGE_SIZE      // 4KB 

// 5.4 one-sided RDMA write check flags
// 4 bytes per HeapRegion |-- 16 bits for dirty --|-- 16 bits for version --|
// Assume the number of Region is 1024, 
// Reserve 4KB for the write check flags.
// [x] precommit
#define FLAGS_OF_CPU_WRITE_CHECK_OFFSET       (size_t)(FLAGS_OF_MEM_SERVER_STATE_OFFSET + FLAGS_OF_MEM_SERVER_STATE_SIZE)  // +4KB, 0x400,008,003,000
#define FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT   (size_t)PAGE_SIZE     // 4KB 




// The space upper should be all committed contiguously.
// and then can register them as RDMA buffer.



// 5.x Padding for debug
//     Make it easier to register RDMA buffer.
//     Commit a contiguous space for RDMA Meta Space.
//     Points to the last item.
//#define RDMA_PADDING_OFFSET       (size_t)(FLAGS_OF_CPU_WRITE_CHECK_OFFSET + FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT) 
//#define RDMA_PADDING_SIZE_LIMIT   (size_t)(ONE_GB - RDMA_PADDING_OFFSET > 0 ? : 0 )  // Must be less than 1GB.




// 6. Cross-Region reference update queue
// Record the <old_addr, new_addr > for the target object queue.
// [?] Not sure how much space is needed for the cross-region-reference queue, give all the rest space to it. Need to shrink it latter.
//
// #define CROSS_REGION_REF_UPDATE_Q_OFFSET      (size_t)(FLAGS_OF_CPU_WRITE_CHECK_OFFSET + FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT)   // 0x400,0D0,401,000
// #define CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT  (size_t)(RDMA_STRUCTURE_SPACE_SIZE - CROSS_REGION_REF_UPDATE_Q_OFFSET)  // Warning : 
// #define CROSS_REGION_REF_UPDATE_Q_LEN         (size_t)(1<< 22)    // 256k per Region.
// #define CROSS_REGION_REF_UPDATE_Q_LEN_SQRT    (size_t)1000003


#define CROSS_REGION_REF_TARGET_Q_OFFSET        (size_t)(FLAGS_OF_CPU_WRITE_CHECK_OFFSET + FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT)
#define CROSS_REGION_REF_TARGET_Q_LEN           (size_t)(512*ONE_MB/8/64)  // Region size/ bits per HeapWord / bits per size_t. 
#define CROSS_REGION_REF_TARGET_Q_SIZE_LIMIT    (size_t)(512 * ONE_MB + 1 * ONE_MB)  // 32GB heap + reserved instance size, 4KB per instance/region.  



#define HASH_MUL                              (size_t)1000000007
struct AddrPair{
  char* st;
  char* ed;
};

#define MAX_CSERVER_CSET_LENGTH               14


//
// x. End of RDMA structure commit size
//
#define END_OF_RDMA_COMMIT_ADDR   (size_t)(SEMERU_START_ADDR + CROSS_REGION_REF_UPDATE_Q_OFFSET + CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT)


// properties for the whole Semeru heap.
// [ RDMA meta data sapce] [RDMA data space]

#define MAX_FREE_MEM_GB   ((size_t) REGION_SIZE_GB * RDMA_DATA_REGION_NUM + RDMA_STRUCTURE_SPACE_SIZE/ONE_GB)    //for local memory management
#define MAX_REGION_NUM    ((size_t) MAX_FREE_MEM_GB/REGION_SIZE_GB)     //for msg passing, ?
#define MAX_SWAP_MEM_GB   (u64)(REGION_SIZE_GB * RDMA_DATA_REGION_NUM)		// Space managed by SWAP








/**
 * Bit operations 
 * 
 */
#define GB_SHIFT 				30 
#define CHUNK_SHIFT			(u64)(GB_SHIFT + ilog2(REGION_SIZE_GB))	 // Used to calculate the chunk index in Client (File chunk). Initialize it before using.
#define	CHUNK_MASK			(u64)( ((u64)1 << CHUNK_SHIFT)  -1)		// get the address within a chunk

#define RMEM_LOGICAL_SECT_SHIFT		(u64)(ilog2(RMEM_LOGICAL_SECT_SIZE))  // the power to 2, shift bits.

//
// File address to Remote virtual memory address translation
//









/**
 * ################### utility functions ####################
 */

//
// Calculate the number's power of 2.
// [?] can we use the MACRO ilog2() ?
// uint64_t power_of_2(uint64_t  num){
    
//     uint64_t  power = 0;
//     while( (num = (num >> 1)) !=0 ){
//         power++;
//     }

//     return power;
// }










// from kernel 
/*  host to network long long
 *  endian dependent
 *  http://www.bruceblinn.com/linuxinfo/ByteOrder.html
 */
#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
		    (unsigned int)ntohl(((int)(x >> 32))))
#define htonll(x) ntohll(x)

#define htonll2(x) cpu_to_be64((x))
#define ntohll2(x) cpu_to_be64((x))











//
// ###################### Debug functions ######################
//
bool within_range(u64 val);





#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H


