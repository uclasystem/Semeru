/**
 * Contain the Block Device related functions and structure.
 * 
 * Warning : 
 *  1) Only include this file in .ccp file for debuging.
 *  2) The inline function can be invoked in multiple .ccp file.
 * 		 And these .ccp files may be merged into one .o 
 *     So, we have to declare the inline function as static.
 * 		 static functions can only be used in the .cpp included it by making a copy.
 * 		 In this case, every .ccp will have its self copy of inline function without conflict.
 */ 

#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_BD_LAYER_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_BD_LAYER_H

#include <linux/swap_global_struct.h>
#include <linux/blkdev.h>

//
// ###################### MACRO #########################
//

#define SWAP_ARRAY_LENGTH   		32*1024*1024   // 32M, for 128GB heap

// madvise
#define MADV_FLUSH_RANGE_TO_REMOTE	20  // flush a range of virtual memory to swap partition.




//
// Disk hardware information
//

// According to the implementation of kernle, it aleays assume the (logical ?) sector size to be 512 bytes.
#define RMEM_PHY_SECT_SIZE		(u64)512 	// physical sector seize, used by driver (to disk).
#define RMEM_LOGICAL_SECT_SIZE		(u64)512		// logical sector seize, used by kernel (to i/o).

#define RMEM_QUEUE_DEPTH           	(u64)4096  	// number of request->tags, for each queue. At least twice the number of RDMA_READ_WRITE_QUEUE_DEPTH.
//#define RMEM_QUEUE_DEPTH           	(u64)256  	// [DEBUG] number of request->tags, for all the queue.
#define RMEM_QUEUE_MAX_SECT_SIZE	(u64)1024 	// The max number of sectors per request, /sys/block/sda/queue/max_hw_sectors_kb is 256
#define DEVICE_NAME_LEN			(u64)32









//
// ###################### swp_entry to virtual address remapping ######################
//

#define INITIAL_VALUE (unsigned long)-1  // the max value 


// 64 bits per tiem.
// warning : global variable doesn't support dynamic memory allocation.
extern  unsigned long swp_entry_to_virtual_remapping[];   // defined in swap.c



// insert item: (swp_off, virtual_page_index)
void insert_swp_entry( struct page *page  , unsigned long virt_addr  );

static inline void reset_swap_remamping_virt_addr(swp_entry_t entry){
	VM_BUG_ON(swp_offset(entry) < SWAP_ARRAY_LENGTH);

	swp_entry_to_virtual_remapping[swp_offset(entry)] = INITIAL_VALUE;
}

// [x] virtual address is countted in PAGE.
// The stored value is the offset to RDMA_DATA_SPACE_START_ADDR.
// We can't use the absolute virtual address to as the sector address.
// It will not pass the sector size check. [sector 0, sector N)
static inline unsigned long retrieve_swap_remmaping_virt_addr(swp_entry_t entry){
	return swp_entry_to_virtual_remapping[swp_offset(entry)];
}

unsigned long retrieve_swap_remmaping_virt_addr_via_offset(pgoff_t offset);

//
// ###################### Debug functions ######################
//

void print_swap_entry_t_and_sector_addr(struct page * page, const char* message);
bool print_swapped_annoymous_page_info(struct page *page, struct page_vma_mapped_walk *pvmw, const char* message);

// LRU debug functions
// Fluhs per-cpu cache and then print the whole lru-flush-list
void print_lru_flush_list_via_virt(unsigned long addr, const char* message);

// Print the caller process mem_cgroup's LRU list set.
void print_lru_flush_list_via_memcgroup(const char* message);

// Only print the content of current lru-flush-list
void print_lru_flush_list_via_lruvec(struct lruvec *lruvec, const char* message);
void print_lru_flush_list_via_list(struct list_head *head, const char* message);

//
// madvise
//

// Defind in mm/vmscan.c
unsigned long semeru_shrink_flush_list(void);

//
// rmap 
//


//
// ######################  Page Table/Page Frame related functions ######################
//

// Every cpp should implement the function.
// pte_t *walk_page_table(struct mm_struct *mm, u64 addr);


//
// struct page
//
enum check_mode{
	CHECK_FLUSH_MOD,

	NR_CHECK_MODE
};



void print_pte_flags(pte_t pte, enum check_mode print_mode,  const char* message);

// Print the pte values for the user virtual range [start, end).
void print_pte_flags_virt_range(unsigned long start, unsigned long end, enum check_mode print_mode,  const char* message);




void print_page_flags(struct page *page, enum check_mode print_mode, const char* message);




//
// ######################  BIO related functions ######################
//






void print_bio_info(struct bio * bio_ptr, const char* message);
void print_bio_within_range(struct bio * bio_ptr, const char* message);
void print_io_request_info(struct request *io_rq, const char* message);
void print_io_request_within_range(struct request *rq, const char* message);

bool sector_within_range(struct bio *bio_ptr);



#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_BD_LAYER_H


