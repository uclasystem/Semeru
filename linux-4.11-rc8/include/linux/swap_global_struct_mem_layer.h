/**
 *  Only contain the Memory Related structure and functions
 * 
 * Warning : 
 *  1) Only include this file in .ccp file for debuging.
 *  2) The inline function can be invoked in multiple .ccp file.
 * 		 And these .ccp files may be merged into one .o 
 *     So, we have to declare the inline function as static.
 * 		 static functions can only be used in the .cpp included it by making a copy.
 * 		 In this case, every .ccp will have its self copy of inline function without conflict.
 */ 

#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H

#include <linux/swap_global_struct.h>

//
// ###################### MACRO #########################
//


//#define SWAP_OUT_MONITOR_VADDR_START		(size_t)(SEMERU_START_ADDR+ RDMA_STRUCTURE_SPACE_SIZE)  // 0x400,100,000,000
#define SWAP_OUT_MONITOR_VADDR_START		(u64)0x40000000  // for debug
#define SWAP_OUT_MONITOR_UNIT_LEN_LOG		26		 						 // 1<<26, 64M per entry. The query can span multiple entries.
#define SWAP_OUT_MONITOR_OFFSET_MASK		(u64)(~((1<<SWAP_OUT_MONITOR_UNIT_LEN_LOG) -1))		//0xfffffffff0000000
#define SWAP_OUT_MONITOR_ARRAY_LEN			(u64)2*1024*1024	 //2M item, Coverred heap size: SWAP_OUT_MONITOR_ARRAY_LEN * (1<<SWAP_OUT_MONITOR_UNIT_LENG_LOG)






//
// ###################### Functions ######################
//
extern atomic_t on_demand_swapin_number;
extern atomic_t hit_on_swap_cache_number;

extern u32 jvm_region_swap_out_counter[]; // 4 bytes for each counter is good enough.



// Invoked in syscall sys_swap_stat_reset_and_check
static inline void reset_swap_info(void){
	atomic_set(&on_demand_swapin_number,0);
	atomic_set(&hit_on_swap_cache_number,0);
}

// Multiple thread safe.
static void on_demand_swapin_inc(void){
	atomic_inc(&on_demand_swapin_number);
}

static void hit_on_swap_cache_inc(void){
	atomic_inc(&hit_on_swap_cache_number);
}

static int get_on_demand_swapin_number(void){
	return (int)atomic_read(&on_demand_swapin_number);
}

static int get_hit_on_swap_cache_number(void){
	return (int)atomic_read(&hit_on_swap_cache_number);
}


// The swap out procedure is done by kernel.
// 1) It should be 1-thread.
// 2) If swap out/in one page frequently, will this cause error ?
//	  Each page can only be swapped out once.
//	  If it's swapped in, we already decrease it from the count.
static inline void swap_out_one_page_record(u64 vaddr){
	u64 entry_ind = (vaddr - SWAP_OUT_MONITOR_VADDR_START) >> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	jvm_region_swap_out_counter[entry_ind]++;

	#ifdef DEBUG_MODE_DETAIL
		printk("%s, swap out page, entry[0x%llx] vaddr 0x%llx \n", __func__, entry_ind, vaddr);
	#endif
}


// We are recording pte <-> Page map, not the actual swapin.
//
static inline void swap_in_one_page_record(u64 vaddr){
	u64 entry_ind = (vaddr - SWAP_OUT_MONITOR_VADDR_START) >> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	jvm_region_swap_out_counter[entry_ind]--;

	#ifdef DEBUG_MODE_DETAIL
		printk("%s, swap in page, entry[0x%llx], vaddr 0x%llx \n", __func__, entry_ind, vaddr);
	#endif
}

/**
 * Count and return the swap out pages number per Region/Unit
 * 1) We assume all the pages are touched and allocated before.
 * 2) end_vaddr - start_vaddr should be Region alignment and 1 Region at least.
 * 		For the corner case, end_vaddr must > start_vaddr, 
 * 		the size can't be 0.
 */
static inline u64 swap_out_pages_for_range(u64 start_vaddr, u64 end_vaddr){
	u64 entry_start = (start_vaddr - SWAP_OUT_MONITOR_VADDR_START)>> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	u64 entry_end		=	(end_vaddr -1 - SWAP_OUT_MONITOR_VADDR_START)>> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	u64 swap_out_total = 0;
	u32 i;

	#ifdef DEBUG_MODE_BRIEF
	printk("%s, Get the swapped out pages for addr[0x%llx, 0x%llx), entry[0x%llx, 0x%llx) \n", __func__, 
																															(u64)start_vaddr, (u64)end_vaddr,
																															entry_start, entry_end);
	#endif

	for(i=entry_start; i<=entry_end; i++ ){
		swap_out_total += jvm_region_swap_out_counter[i];
	}

	return swap_out_total;
}



//
// ###################### Debug Functions ######################
//

int is_page_in_swap_cache(pte_t	pte); 
struct page* page_in_swap_cache(pte_t	pte); 


#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H


