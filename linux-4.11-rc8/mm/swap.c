/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/sysctl/vm.txt.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/mm_inline.h>
#include <linux/percpu_counter.h>
#include <linux/memremap.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/backing-dev.h>
#include <linux/memcontrol.h>
#include <linux/gfp.h>
#include <linux/uio.h>
#include <linux/hugetlb.h>
#include <linux/page_idle.h>

#include "internal.h"


//semeru
#include <linux/swap_global_struct_bd_layer.h>
#include <linux/swap_global_struct_mem_layer.h>
#include <linux/swapops.h>


#define CREATE_TRACE_POINTS
#include <trace/events/pagemap.h>


//
// Semeru support
//

// 64 bits per tiem.
//unsigned long *swp_entry_to_virtual_remapping = (unsigned long*)kzalloc(SWAP_ARRAY_LENGTH, GFP_KERNEL);

unsigned long swp_entry_to_virtual_remapping[SWAP_ARRAY_LENGTH];


// Record the swap out ratio for the JVM Heap Region
// 1) Reset it to 0 before using by a process.
// 2) Can only be used by one process at one time. 
u32 jvm_region_swap_out_counter[SWAP_OUT_MONITOR_ARRAY_LEN];

atomic_t on_demand_swapin_number;
atomic_t hit_on_swap_cache_number;

//
// static functions
//



/**
 * Get the pte_t value  of the user space virtual address.
 * For the kernel space virtual address, allocated by kmalloc or kzalloc, user the virt_to_phys is good.
 * 
 * 1) This is a 5 level PageTable, since 4.11-rc2.
 * 		check https://lwn.net/Articles/717293/
 *    
 * 		7 bits are discarded.
 *    9 bits each level.
 * 
 *          size per entry.
 *    pgd : 256TB
 * 		p4d : 512GB
 *    pud : 1GB 
 * 		pmd : 2MB,   // huge page bit.
 * 		pte : 4KB
 * 
 * Modify the transparent hugepage, /sys/kernel/mm/transparent_hugepage/enabled to madvise.
 * to always [madvise] never
 * OR we have to delete the last level pte.
 * 
 * [XX] If too many pages need to be walked, switch to huge page.
 * 			Or this function will cause significantly overhead.
 * 
 * 
 */
static pte_t* walk_page_table(struct mm_struct *mm, u64 addr){
 pgd_t *pgd;
 p4d_t *p4d;
 pud_t *pud;
 pmd_t *pmd;
 pte_t *ptep;

 pgd = pgd_offset(mm, addr);

 if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))  // [?] What's the purpose of bad bit ?
   return NULL;

 p4d = p4d_offset(pgd, addr);
 if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
   return NULL;

 pud = pud_offset(p4d, addr);
 if (pud_none(*pud) || unlikely(pud_bad(*pud)))
   return NULL;

 pmd = pmd_offset(pud, addr);
 if (pmd_none(*pmd))
   return NULL;

 ptep = pte_offset_map(pmd, addr);

 return ptep;
}





/**
 * Insert a (swp_entry_offset, virtual address) pair into the array.
 * 
 *  
 * 
 * 
 * More Explanation : 
 * 	1) For a shared page, use the first process's virtual address as the remapped address.
 * 			 They have the same swp_entry_t, will go into the same slot, getting the same remapped address.
 * 	2) Insert the virt page INDEX, right shift to erase the low page bits. 
 * 
 * [X] For the remapped virt addr, it's offset to the RDMA_DATA_SPACE_START_ADDR.
 * 		 If we use the absolute virtual address, it will beyound the sector check, which start from 0.
 * 
 */
void insert_swp_entry( struct page *page  , unsigned long virt_addr  ){
	swp_entry_t entry = { .val = page_private(page) };
	// Change the swp_offset to the full value.
	// swp_offset is not arch specific
	swp_entry_to_virtual_remapping[swp_offset(entry)] = ( (virt_addr -  RDMA_DATA_SPACE_START_ADDR) >> PAGE_SHIFT );

	#ifdef DEBUG_SWAP_PATH
			printk("%s,Build Remap from swp_entry_t[0x%llx] (type: 0x%llx, offset: 0x%llx) to virt_addr 0x%llx pages\n", 
																__func__, (u64)entry.val, (u64)swp_type(entry), (u64)swp_offset(entry), (u64)(virt_addr >> PAGE_SHIFT) );
	#endif

}

// inline it 
// unsigned long retrieve_swap_remmaping_virt_addr(swp_entry_t entry){
// 	return swp_entry_to_virtual_remapping[swp_offset(entry)];
// }





//
// Page IO debug functions
// Invokation scope is within this .cpp file.
//

void print_swap_entry_t_and_sector_addr(struct page * page, const char* message){
	swp_entry_t swap = { .val = page_private(page) };

	// Page basic information
	printk("%s Start, Page info : page 0x%llx, physical address 0x%llx \n", message,
																			(u64)page,
																			(u64)page_to_phys(page));

	// swap_entry_t information
	printk(" Swap entry info : swap_entry_t.val 0x%llx, swap_type 0x%llx, swap_offset 0x%llx \n",
																					(u64)swap.val,
																					(u64)swp_type(swap),
																					(u64)swp_offset(swap));

	// sector address information
	printk("%s End, Sector info: swap out sector addr : 0x%llx (sector size 512 B)\n\n", message, 
															(u64)((sector_t)__page_file_index(page) << (PAGE_SHIFT - 9)) );


}







//
// Debug function
//



bool within_range(u64 val){
	
	// 1) normal swap, for swp_entry_t --> virtual remap
	//    Only data Region can be swapped out.
	if( val >= (u64)RDMA_DATA_SPACE_START_ADDR && val < (u64)(RDMA_DATA_SPACE_START_ADDR + (size_t)MAX_SWAP_MEM_GB*ONE_GB )  ){
		return 1;
	}

	// 2) madvise  [1GB, 2GB)
	// Debug mode.
	if( val >= (u64)0x40000000 && val < (u64)0x80000000  ){
		return 1;
	}

	return 0;
}


bool print_swapped_annoymous_page_info(struct page *page, struct page_vma_mapped_walk *pvmw, const char* message){

	// Only cares about annoymous page
	if( PageAnon(page) && within_range((u64)pvmw->address) ){
		printk("%s, start, page->_mapcount: %d, page->mapping: 0x%llx \n", 
																				message, 
																				page->_mapcount.counter, 
																				(u64)page->mapping);
		printk("    virtual addr: 0x%llx, pte: 0x%llx ,\n     physical addr:0x%llx, page: 0x%llx \n", 
																				(u64)pvmw->address, 
																				(u64)pte_val(*pvmw->pte), 
																				(u64)page_to_phys(page),
																				(u64)page);
		printk("    page->private: 0x%llx, in swap_cache ? %d \n End \n", (u64)page_private(page), PageSwapCache(page) );

		// Check page->flags 
		print_pte_flags( *(pvmw->pte) , CHECK_FLUSH_MOD, "pvmw.pte");
		print_page_flags(page,CHECK_FLUSH_MOD , "Page");

		return 1; // enable debug
	}


	return 0; // not debug
}



//
// LRU list debug 
//




/**
 * Get the page's corresponding virtual address via the  reverse mapping.
 * It could have multiple mapped virtual address.
 * 
 * Parameter:
 * 	struct page ,
 * 	virt[] : the reutrned results. assume mapped virtual ddress is less than 8. 
 * 
 * Return value : Found mapped return address.
 * 
 */

int page_to_user_virt_addr(struct page* page, unsigned long virt[]){

	struct anon_vma *anon_vma;
	pgoff_t pgoff_start, pgoff_end;
	struct anon_vma_chain *avc;
	int count = 0;
	//struct rmap_walk_control rwc; // not specify any special control
		
	anon_vma = page_anon_vma(page);
	if (!anon_vma){
		printk(KERN_ERR "%s, anno_vma is empty. page 0x%llx \n", __func__, (u64)page);
		goto err;	
	}

	// [?] is this lock necessary ??
	//anon_vma_lock_read(anon_vma);

	
	pgoff_start = page_to_pgoff(page);
	pgoff_end = pgoff_start + hpage_nr_pages(page) - 1;
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root,
			pgoff_start, pgoff_end) {
		struct vm_area_struct *vma = avc->vma;
		unsigned long address = vma_address(page, vma);

		// store the found virtual addres
		if(unlikely(count > 7)){
			printk(KERN_ERR "%s, too many mapped virtual address. skip vrit_addr 0x%llx. \n",__func__, (u64)address );
			continue;
		}
		virt[count++] = address;
	}

err:
	return count;
}





/**
 * Print the lru-list via the user virtual address.
 * 	virt addr -> struct page -> mem_cgroup ->  lru set. 
 * 
 * [x] We need a struct page to the node information and cgroup information.
 * 		 And then we can get the corresponding LRU list set.
 * 		 We get the struct page via the a user space virt addr.
 */
void print_lru_flush_list_via_virt(unsigned long addr, const char* message){
	struct lruvec *lruvec;
	struct list_head* list_ptr;
	unsigned long count = 0;
	int number_of_mapped_virt, i;
	unsigned long virt_addr[8];
	struct page *page = NULL;
	// pagelist NODE specific management data
	struct pglist_data *pagepgdat;
	pte_t* 		pte_ptr;


	// Prepare functions.
	// Flush the CPU local variables
	// Better to develop a separate flush function for lru-flush-page 
	lru_flush_list_drain_all();


	//1) Get the struct page
	pte_ptr = walk_page_table(current->mm, (u64)addr );
	page = pfn_to_page(pte_pfn(*pte_ptr));
	if(page == NULL){
		printk(KERN_ERR " %s, empty page. \n", message);
		goto err;
	}
	#ifdef DEBUG_FLUSH_LIST
	else{
		printk(KERN_INFO "%s Get mem_zone via vird addr 0x%llx,  page 0x%llx \n", message, (u64)addr, (u64)page);
	}
	#endif


	//2) traverse all the online NODE's memory 
	// for_each_online_pgdat(pagepgdat){


		pagepgdat = page_pgdat(page);
		if (pagepgdat == NULL){
			printk(KERN_ERR "%s, This pagepgdat is NULL, skip it. \n", message);
			goto err;
		}


		// Get the lru-list set for this node, cgroup.
		// lru list set is per-cgroup. we have to  get hte memory cgroup information via a struct page.
		lruvec = mem_cgroup_page_lruvec(page, pagepgdat);  //page can't be null;  pglist_data->lruvec
	
		printk(KERN_INFO "&lruvec->lists[SEMERU_LRU_FLUSH_LIST], list_head 0x%llx  \n", (u64)&lruvec->lists[SEMERU_LRU_FLUSH_LIST]);

		// 3) Check the flush page list.
		list_for_each(list_ptr, &lruvec->lists[SEMERU_LRU_FLUSH_LIST] )
		{
			page =	lru_to_page(list_ptr->next);  // get  struct page containing list_ptr->prev
			number_of_mapped_virt = page_to_user_virt_addr(page, virt_addr);

			if(number_of_mapped_virt == 0 ){
				printk(KERN_INFO "	Entry [0x%llx], virtual page addr 0x%llx, page 0x%llx \n", (u64)(count++), (u64)0, (u64)page);

				#ifdef DEBUG_FLUSH_LIST_DETAIL
					printk(KERN_INFO "		list_ptr 0x%llx ,prev 0x%llx, next 0x%llx \n\n", (u64)list_ptr, (u64)list_ptr->prev, (u64)list_ptr->next );
				#endif
			}else{
				for(i=0; i< number_of_mapped_virt; i++)
					printk(KERN_INFO "	Entry [0x%llx], virtual page addr[%d] 0x%llx, page 0x%llx \n", (u64)(count++), i, (u64)virt_addr[i], (u64)page);
				
				#ifdef DEBUG_FLUSH_LIST_DETAIL
					printk(KERN_INFO "		list_ptr 0x%llx ,prev 0x%llx, next 0x%llx \n\n", (u64)list_ptr, (u64)list_ptr->prev, (u64)list_ptr->next );
				#endif
			}

		}// traverse each entry of the lru list.


	//}// each pglist_data
err:
	printk(KERN_INFO "%s END  \n \n", message);
}





/**
 * Print the lru-list of a mem_cgroup.
 *  
 * 1) The user process invoke the syscall and then go into here.
 *  We get the caller's mem_cgroup and then print its flush-list.
 * 
 * 2) scan all the online NODE's  LRU list set.
 * 
 */
void print_lru_flush_list_via_memcgroup(const char* message){
	struct lruvec *lruvec;
	struct list_head* list_ptr;
	unsigned long count = 0;
	int number_of_mapped_virt, i;
	unsigned long virt_addr[8];
	struct page *page = NULL;
	// pagelist NODE specific management data
	struct pglist_data *pagepgdat;
	struct mem_cgroup *memcg;


	// Prepare functions.
	// Flush the CPU local variables
	// Better to develop a separate flush function for lru-flush-page 
	lru_flush_list_drain_all();

	// 1) get the caller process's mem_cgroup
	memcg = mem_cgroup_from_task(current); // get current process't mem_cgroup

	//2) traverse all the online NODE's memory 
	for_each_online_pgdat(pagepgdat){

		// 2.1 Get the mem_cgroup_per_node[]->lruvec
		lruvec = mem_cgroup_lruvec(pagepgdat, memcg);

		printk(KERN_INFO " \n Node[%d],  &lruvec->lists[SEMERU_LRU_FLUSH_LIST], list_head 0x%llx  \n", 
																					pagepgdat->node_id, (u64)&lruvec->lists[SEMERU_LRU_FLUSH_LIST]);

		// 3) Check the flush page list.
		list_for_each(list_ptr, &lruvec->lists[SEMERU_LRU_FLUSH_LIST] )
		{
			page =	lru_to_page(list_ptr->next);  // get  struct page containing list_ptr->prev
			number_of_mapped_virt = page_to_user_virt_addr(page, virt_addr);

			if(number_of_mapped_virt == 0 ){
				printk(KERN_INFO "%s,	Entry [0x%llx], virtual page addr 0x%llx, page 0x%llx \n", message, (u64)(count++), (u64)0, (u64)page);

				// debug page flags 
				printk(KERN_INFO "	in swapcache ? %d, anony ? %d, PageLRU %d \n", PageSwapCache(page), PageAnon(page),PageLRU(page));

				#ifdef DEBUG_FLUSH_LIST_DETAIL
					printk(KERN_INFO "		list_ptr 0x%llx ,prev 0x%llx, next 0x%llx \n\n", (u64)list_ptr, (u64)list_ptr->prev, (u64)list_ptr->next );
				#endif
			}else{
				for(i=0; i< number_of_mapped_virt; i++)
					printk(KERN_INFO "%s,	Entry [0x%llx], virtual page addr[%d] 0x%llx, page 0x%llx \n", message, (u64)(count++), i, (u64)virt_addr[i], (u64)page);

					// debug page flags 
					printk(KERN_INFO "	in swapcache ? %d, anony ? %d, PageLRU %d, PageDirty %d\n", PageSwapCache(page), PageAnon(page),PageLRU(page), PageDirty(page) );
				
				#ifdef DEBUG_FLUSH_LIST_DETAIL
					printk(KERN_INFO "		list_ptr 0x%llx ,prev 0x%llx, next 0x%llx \n\n", (u64)list_ptr, (u64)list_ptr->prev, (u64)list_ptr->next );
				#endif
			}

		}// traverse each entry of the lru list.



	}// each pglist_data
//err:
	printk(KERN_INFO "%s END  \n \n", message);
}


//
// Check PageTable Entry, pte.
//







/**
 * Print the value of pte.
 *  
 */
void print_pte_flags(pte_t pte, enum check_mode mode,  const char* message){

	printk(KERN_INFO "\n %s START \n", message);

	switch(mode){
		case CHECK_FLUSH_MOD:
			if(!pte_none(pte)){
				printk(KERN_INFO "	pte_t.pte 0x%llx, none 0.  present %d, dirty %d, pte_young %d \n", 
												 (u64)pte.pte,  pte_present(pte), pte_dirty(pte), pte_young(pte));
			}else{
				printk(KERN_INFO "	pte_t.pte 0x%llx, none ? 1. \n", 
													 (u64)pte.pte );
			}

			break;

	default:
		//nothong to check
		printk(KERN_ERR "%s, Wrong mode. \n\n", message);
	}

	printk(KERN_INFO "%s END \n\n", message);

}




void print_pte_flags_virt_range(unsigned long start, unsigned long end, enum check_mode print_mode,  const char* message){

	struct mm_struct *mm = current->mm;
	unsigned long page_iter; // 4K default.
	unsigned long count;
	pte_t *pte;
	struct page *page = NULL;

	count = 0;
	for( page_iter = start; page_iter < end; page_iter += PAGE_SIZE){
		pte = walk_page_table(mm, (u64)page_iter );
		page = pfn_to_page(pte_pfn(*pte));
		printk(KERN_INFO "%s, virt_page[0x%lx] 0x%llx, pte_t.pte 0x%llx , page 0x%llx \n", message, count++, (u64)page_iter, (u64)pte->pte, (u64)page );
		print_pte_flags(*pte, print_mode, message);
		print_page_flags(page, print_mode, message ); // debug
	}

}




//
// Check physical memory flags 
//


/**
 * print all the necesaary flags, 
 * we can divide them into several groups according the needs.
 *  
 */
void print_page_flags(struct page *page, enum check_mode mode, const char* message){

	switch(mode){

		case CHECK_FLUSH_MOD:
			printk(KERN_INFO "%s,	page 0x%llx, swapcache %d, anony %d, PageLRU %d, PageDirty %d, PageWriteback %d . page_mapcount(+1) %d, page->_refcount(the value) %d \n", 
												message,	(u64)page,  PageSwapCache(page), PageAnon(page),PageLRU(page), PageDirty(page), PageWriteback(page), page_mapcount(page), page_count(page) );

			break;

	default:
		// Print the message at the head.
		printk(KERN_INFO "%s,	page 0x%llx, swapcache %d, anony %d, PageLRU %d, PageDirty %d, PageWriteback %d . page_mapped(>=0?) %d, page->_refcount(the value) %d \n", 
												message,(u64)page,  PageSwapCache(page), PageAnon(page),PageLRU(page), PageDirty(page), PageWriteback(page), page_mapped(page), page_count(page) );
	}
}




/**
 * Not flush any per-cpu local variables,
 * only print current contents of the lru_flush_list
 *  
 */
void print_lru_flush_list_via_lruvec(struct lruvec *lruvec, const char* message){
		struct page *page;
		struct list_head* list_ptr;
		int count =0;

		printk(KERN_INFO "%s Start \n", message);

		list_for_each(list_ptr, &lruvec->lists[SEMERU_LRU_FLUSH_LIST] )
		{
			page =	lru_to_page(list_ptr->next); // get  struct page containing list_ptr->prev
			printk(KERN_INFO "	entry[%d] page 0x%llx \n", count++, (u64)page);
	
		}// traverse each entry of the lru list.

		printk(KERN_INFO "%s END \n\n", message);
}


/**
 * Print the entries of the passed in list_head.
 *  
 */
void print_lru_flush_list_via_list(struct list_head *head, const char* message){
		struct page *page;
		struct list_head* list_ptr;
		u64 count =0;

		printk(KERN_INFO "\n %s Start \n", message);

		list_for_each(list_ptr, head )
		{
			page =	lru_to_page(list_ptr->next); // get  struct page containing list_ptr->prev
			printk(KERN_INFO "	%s, entry[0x%llx] page 0x%llx \n", message, count++, (u64)page);
			print_page_flags( page, CHECK_FLUSH_MOD, message);
	
		}// traverse each entry of the lru list.

		printk(KERN_INFO "%s END \n\n", message);
}




// Debug for BIO / I/O Request
//

void print_bio_info(struct bio * bio_ptr, const char* message){

  struct bio_vec    bv;     // iterate the struct page attached to the bio.
	struct bvec_iter  iter;
  u64 phys_addr;

  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , Start\n", __func__, message);
  
	// This may be a bio list.
  for_each_bio(bio_ptr){
		
		printk(KERN_INFO "	bio->bi_iter sector addr: 0x%llx, (virt addr 0x%llx) ,  byte len 0x%llx \n", 
															(u64)bio_ptr->bi_iter.bi_sector, 	(u64)(bio_ptr->bi_iter.bi_sector << 9), (u64)bio_ptr->bi_iter.bi_size );
		
		bio_for_each_segment(bv, bio_ptr, iter) {
      struct page *page = bv.bv_page;   
      phys_addr = page_to_phys(page);
      printk(KERN_INFO "	handle struct page: 0x%llx , physical address : 0x%llx \n", 
                           (u64)page, (u64)phys_addr );
    }

	}

  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , End\n", __func__, message);
  
}




/**
 * Print the i/o request information only when it's in the specific range.
 *  
 */
void print_bio_within_range(struct bio * bio_ptr, const char* message){
	u64  start_addr = 0;

	if((void*)bio_ptr != NULL ){
    start_addr   = (u64)bio_ptr->bi_iter.bi_sector << 9;    // request->_sector. Sector start address, change to bytes.
	

		if(within_range(start_addr)){
			print_bio_info(bio_ptr, message);
		}

	}
}






void print_io_request_info(struct request *io_rq, const char* message){

  struct bio        *bio_ptr = io_rq->bio;
  struct bio_vec    bv;     // iterate the struct page attached to the bio.
	struct bvec_iter  iter;
  u64 phys_addr;

  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , Start\n", __func__, message);
  

  for_each_bio(bio_ptr){
		printk(KERN_INFO "	bio->bi_iter sector addr : 0x%llx, (remapped virt_addr) 0x%llx , byte len 0x%llx \n", 
															(u64)bio_ptr->bi_iter.bi_sector, (u64)(bio_ptr->bi_iter.bi_sector<< 9), (u64)bio_ptr->bi_iter.bi_size );

		bio_for_each_segment(bv, bio_ptr, iter) {
      struct page *page = bv.bv_page;   
      phys_addr = page_to_phys(page);
      printk(KERN_INFO "%s: handle struct page: 0x%llx , physical address : 0x%llx \n", 
                          __func__,  (u64)page, (u64)phys_addr );
    }
	}

  printk(KERN_INFO " %s, request->tag %d. \n\n", __func__, io_rq->tag );

  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , End\n", __func__, message);
  
}




/**
 * Print the i/o request information only when it's in the specific range.
 *  
 */
void print_io_request_within_range(struct request *rq, const char* message){

  u64  start_addr      = blk_rq_pos(rq) << 9;    // request->_sector. Sector start address, change to bytes.

	if(within_range(start_addr)){
		print_io_request_info(rq, message);
	}
}





bool sector_within_range(struct bio * bio_ptr){
	sector_t sector_addr;
	u64 virt_addr;

	if(bio_ptr == NULL)
		return 0;

	sector_addr = bio_ptr->bi_iter.bi_sector;  // count in sector
	virt_addr = sector_addr << 9; // 512 bytes/sector

	// 1) normal swap, for swp_entry_t --> virtual remap
	if( virt_addr >= (u64)0x400000000000 && virt_addr < (u64)0x400080000000  ){
		return 1;
	}

	// 2) madvise  [1GB, 2GB)
	if( virt_addr >= (u64)0x40000000 && virt_addr < (u64)0x80000000  ){
		return 1;
	}

	return 0;
}







//
// Debug function end
//












//
// end of Semeru
//





/* How many pages do we try to swap or page in/out together? */
int page_cluster;

static DEFINE_PER_CPU(struct pagevec, lru_add_pvec);
static DEFINE_PER_CPU(struct pagevec, lru_rotate_pvecs);
static DEFINE_PER_CPU(struct pagevec, lru_deactivate_file_pvecs);
static DEFINE_PER_CPU(struct pagevec, lru_deactivate_pvecs);		//batch. enqueue pages into pagevec first, and then enqueue the active/inactive list.
#ifdef CONFIG_SMP
static DEFINE_PER_CPU(struct pagevec, activate_page_pvecs);
#endif

// Semeru CPU
static DEFINE_PER_CPU(struct pagevec, lru_flush_to_remote_pvecs); // cpu local cache for flush-page-list.



/*
 * This path almost never happens for VM activity - pages are normally
 * freed via pagevecs.  But it gets used by networking.
 */
static void __page_cache_release(struct page *page)
{
	if (PageLRU(page)) {
		struct zone *zone = page_zone(page);
		struct lruvec *lruvec;
		unsigned long flags;

		spin_lock_irqsave(zone_lru_lock(zone), flags);
		lruvec = mem_cgroup_page_lruvec(page, zone->zone_pgdat);
		VM_BUG_ON_PAGE(!PageLRU(page), page);
		__ClearPageLRU(page);
		del_page_from_lru_list(page, lruvec, page_off_lru(page));
		spin_unlock_irqrestore(zone_lru_lock(zone), flags);
	}
	__ClearPageWaiters(page);
	mem_cgroup_uncharge(page);
}

static void __put_single_page(struct page *page)
{
	__page_cache_release(page);
	free_hot_cold_page(page, false);
}

static void __put_compound_page(struct page *page)
{
	compound_page_dtor *dtor;

	/*
	 * __page_cache_release() is supposed to be called for thp, not for
	 * hugetlb. This is because hugetlb page does never have PageLRU set
	 * (it's never listed to any LRU lists) and no memcg routines should
	 * be called for hugetlb (it has a separate hugetlb_cgroup.)
	 */
	if (!PageHuge(page))
		__page_cache_release(page);
	dtor = get_compound_page_dtor(page);
	(*dtor)(page);
}

void __put_page(struct page *page)
{
	if (unlikely(PageCompound(page)))
		__put_compound_page(page);
	else
		__put_single_page(page);
}
EXPORT_SYMBOL(__put_page);

/**
 * put_pages_list() - release a list of pages
 * @pages: list of pages threaded on page->lru
 *
 * Release a list of pages which are strung together on page.lru.  Currently
 * used by read_cache_pages() and related error recovery code.
 */
void put_pages_list(struct list_head *pages)
{
	while (!list_empty(pages)) {
		struct page *victim;

		victim = list_entry(pages->prev, struct page, lru);
		list_del(&victim->lru);
		put_page(victim);
	}
}
EXPORT_SYMBOL(put_pages_list);

/*
 * get_kernel_pages() - pin kernel pages in memory
 * @kiov:	An array of struct kvec structures
 * @nr_segs:	number of segments to pin
 * @write:	pinning for read/write, currently ignored
 * @pages:	array that receives pointers to the pages pinned.
 *		Should be at least nr_segs long.
 *
 * Returns number of pages pinned. This may be fewer than the number
 * requested. If nr_pages is 0 or negative, returns 0. If no pages
 * were pinned, returns -errno. Each page returned must be released
 * with a put_page() call when it is finished with.
 */
int get_kernel_pages(const struct kvec *kiov, int nr_segs, int write,
		struct page **pages)
{
	int seg;

	for (seg = 0; seg < nr_segs; seg++) {
		if (WARN_ON(kiov[seg].iov_len != PAGE_SIZE))
			return seg;

		pages[seg] = kmap_to_page(kiov[seg].iov_base);
		get_page(pages[seg]);
	}

	return seg;
}
EXPORT_SYMBOL_GPL(get_kernel_pages);

/*
 * get_kernel_page() - pin a kernel page in memory
 * @start:	starting kernel address
 * @write:	pinning for read/write, currently ignored
 * @pages:	array that receives pointer to the page pinned.
 *		Must be at least nr_segs long.
 *
 * Returns 1 if page is pinned. If the page was not pinned, returns
 * -errno. The page returned must be released with a put_page() call
 * when it is finished with.
 */
int get_kernel_page(unsigned long start, int write, struct page **pages)
{
	const struct kvec kiov = {
		.iov_base = (void *)start,
		.iov_len = PAGE_SIZE
	};

	return get_kernel_pages(&kiov, 1, write, pages);
}
EXPORT_SYMBOL_GPL(get_kernel_page);


/**
 * Flush pages in pagevec to the corresponding lru list.
 * Apply function, move_fn to each page in pagevec.
 * 
 * Parameters
 * 	pagevec : current cpu's per-cpu pagevec
 * 	move_fn : the applied function to each page.		
 * 	arg			: usually NULL.	
 * 
 * 	lruvec , get from the page's memory zone. e.g. based on the NUMA Node, cgroup.
 * 
 */
static void pagevec_lru_move_fn(struct pagevec *pvec,
	void (*move_fn)(struct page *page, struct lruvec *lruvec, void *arg),
	void *arg)
{
	int i;
	struct pglist_data *pgdat = NULL;		// zone based page swap management data structure.
	struct lruvec *lruvec;
	unsigned long flags = 0;

	for (i = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];
		struct pglist_data *pagepgdat = page_pgdat(page);

		if (pagepgdat != pgdat) {
			if (pgdat)
				spin_unlock_irqrestore(&pgdat->lru_lock, flags);
			pgdat = pagepgdat;
			spin_lock_irqsave(&pgdat->lru_lock, flags);
		}

		lruvec = mem_cgroup_page_lruvec(page, pgdat); // Get the lru list structure for this node, cgroup.
		(*move_fn)(page, lruvec, arg);			// apply the move_fn to this page and lrulist.
	}
	if (pgdat)
		spin_unlock_irqrestore(&pgdat->lru_lock, flags);
	release_pages(pvec->pages, pvec->nr, pvec->cold);		// [?] free the pages, left in the pvec ?
	pagevec_reinit(pvec);		// reset current pagevec.
}

static void pagevec_move_tail_fn(struct page *page, struct lruvec *lruvec,
				 void *arg)
{
	int *pgmoved = arg;

	if (PageLRU(page) && !PageUnevictable(page)) {
		del_page_from_lru_list(page, lruvec, page_lru(page));
		ClearPageActive(page);
		add_page_to_lru_list_tail(page, lruvec, page_lru(page));
		(*pgmoved)++;
	}
}

/*
 * pagevec_move_tail() must be called with IRQ disabled.
 * Otherwise this may cause nasty races.
 */
static void pagevec_move_tail(struct pagevec *pvec)
{
	int pgmoved = 0;

	pagevec_lru_move_fn(pvec, pagevec_move_tail_fn, &pgmoved);
	__count_vm_events(PGROTATED, pgmoved);
}

/*
 * Writeback is about to end against a page which has been marked for immediate
 * reclaim.  If it still appears to be reclaimable, move it to the tail of the
 * inactive list.
 */
void rotate_reclaimable_page(struct page *page)
{
	if (!PageLocked(page) && !PageDirty(page) &&
	    !PageUnevictable(page) && PageLRU(page)) {
		struct pagevec *pvec;
		unsigned long flags;

		get_page(page);
		local_irq_save(flags);
		pvec = this_cpu_ptr(&lru_rotate_pvecs);
		if (!pagevec_add(pvec, page) || PageCompound(page))
			pagevec_move_tail(pvec);
		local_irq_restore(flags);
	}
}

/**
 * [?] only record the statistics for file backed page ?
 * 		 
 * [?] What's the meaning of rotated ?
 *  
 */
static void update_page_reclaim_stat(struct lruvec *lruvec,
				     int file, int rotated)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;

	reclaim_stat->recent_scanned[file]++;
	if (rotated)
		reclaim_stat->recent_rotated[file]++;
}

static void __activate_page(struct page *page, struct lruvec *lruvec,
			    void *arg)
{
	if (PageLRU(page) && !PageActive(page) && !PageUnevictable(page)) {
		int file = page_is_file_cache(page);
		int lru = page_lru_base_type(page);

		del_page_from_lru_list(page, lruvec, lru);
		SetPageActive(page);
		lru += LRU_ACTIVE;
		add_page_to_lru_list(page, lruvec, lru);
		trace_mm_lru_activate(page);

		__count_vm_event(PGACTIVATE);
		update_page_reclaim_stat(lruvec, file, 1);
	}
}

#ifdef CONFIG_SMP
static void activate_page_drain(int cpu)
{
	struct pagevec *pvec = &per_cpu(activate_page_pvecs, cpu);

	if (pagevec_count(pvec))
		pagevec_lru_move_fn(pvec, __activate_page, NULL);
}

static bool need_activate_page_drain(int cpu)
{
	return pagevec_count(&per_cpu(activate_page_pvecs, cpu)) != 0;
}

/**
 * Tag : move pages from inactive list to active list,
 * 
 * [?] need to increase the page->_refcount by get_page() ?
 *  
 */
void activate_page(struct page *page)
{
	page = compound_head(page);
	if (PageLRU(page) && !PageActive(page) && !PageUnevictable(page)) {
		struct pagevec *pvec = &get_cpu_var(activate_page_pvecs);

		get_page(page);
		if (!pagevec_add(pvec, page) || PageCompound(page))
			pagevec_lru_move_fn(pvec, __activate_page, NULL);
		put_cpu_var(activate_page_pvecs);
	}
}

#else
static inline void activate_page_drain(int cpu)
{
}

static bool need_activate_page_drain(int cpu)
{
	return false;
}

void activate_page(struct page *page)
{
	struct zone *zone = page_zone(page);

	page = compound_head(page);
	spin_lock_irq(zone_lru_lock(zone));
	__activate_page(page, mem_cgroup_page_lruvec(page, zone->zone_pgdat), NULL);
	spin_unlock_irq(zone_lru_lock(zone));
}
#endif

static void __lru_cache_activate_page(struct page *page)
{
	struct pagevec *pvec = &get_cpu_var(lru_add_pvec);
	int i;

	/*
	 * Search backwards on the optimistic assumption that the page being
	 * activated has just been added to this pagevec. Note that only
	 * the local pagevec is examined as a !PageLRU page could be in the
	 * process of being released, reclaimed, migrated or on a remote
	 * pagevec that is currently being drained. Furthermore, marking
	 * a remote pagevec's page PageActive potentially hits a race where
	 * a page is marked PageActive just after it is added to the inactive
	 * list causing accounting errors and BUG_ON checks to trigger.
	 */
	for (i = pagevec_count(pvec) - 1; i >= 0; i--) {
		struct page *pagevec_page = pvec->pages[i];

		if (pagevec_page == page) {
			SetPageActive(page);
			break;
		}
	}

	put_cpu_var(lru_add_pvec);
}

/*
 * Mark a page as having seen activity.
 *
 * inactive,unreferenced	->	inactive,referenced
 * inactive,referenced		->	active,unreferenced
 * active,unreferenced		->	active,referenced
 *
 * When a newly allocated page is not yet visible, so safe for non-atomic ops,
 * __SetPageReferenced(page) may be substituted for mark_page_accessed(page).
 */
void mark_page_accessed(struct page *page)
{
	page = compound_head(page);
	if (!PageActive(page) && !PageUnevictable(page) &&
			PageReferenced(page)) {

		/*
		 * If the page is on the LRU, queue it for activation via
		 * activate_page_pvecs. Otherwise, assume the page is on a
		 * pagevec, mark it active and it'll be moved to the active
		 * LRU on the next drain.
		 */
		if (PageLRU(page))
			activate_page(page);
		else
			__lru_cache_activate_page(page);
		ClearPageReferenced(page);
		if (page_is_file_cache(page))
			workingset_activation(page);
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}
	if (page_is_idle(page))
		clear_page_idle(page);
}
EXPORT_SYMBOL(mark_page_accessed);

/**
 * Add a page into the LRU list cache.
 * 
 * [X] The first time we add a page into the LRU List/LRU list cache.
 * we need to increase the page->_refcount bu  get_page();
 *  
 */
static void __lru_cache_add(struct page *page)
{
	struct pagevec *pvec = &get_cpu_var(lru_add_pvec);

	get_page(page);
	if (!pagevec_add(pvec, page) || PageCompound(page))
		__pagevec_lru_add(pvec);
	put_cpu_var(lru_add_pvec);
}

/**
 * lru_cache_add: add a page to the page lists
 * @page: the page to add
 */
void lru_cache_add_anon(struct page *page)
{
	if (PageActive(page))
		ClearPageActive(page);
	__lru_cache_add(page);
}

void lru_cache_add_file(struct page *page)
{
	if (PageActive(page))
		ClearPageActive(page);
	__lru_cache_add(page);
}
EXPORT_SYMBOL(lru_cache_add_file);

/**
 * lru_cache_add - add a page to a page list
 * @page: the page to be added to the LRU.
 *
 * Queue the page for addition to the LRU via pagevec. The decision on whether
 * to add the page to the [in]active [file|anon] list is deferred until the
 * pagevec is drained. This gives a chance for the caller of lru_cache_add()
 * have the page added to the active list using mark_page_accessed().
 */
void lru_cache_add(struct page *page)
{
	VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);
	VM_BUG_ON_PAGE(PageLRU(page), page);
	__lru_cache_add(page);
}

/**
 * add_page_to_unevictable_list - add a page to the unevictable list
 * @page:  the page to be added to the unevictable list
 *
 * Add page directly to its zone's unevictable list.  To avoid races with
 * tasks that might be making the page evictable, through eg. munlock,
 * munmap or exit, while it's not on the lru, we want to add the page
 * while it's locked or otherwise "invisible" to other tasks.  This is
 * difficult to do when using the pagevec cache, so bypass that.
 */
void add_page_to_unevictable_list(struct page *page)
{
	struct pglist_data *pgdat = page_pgdat(page);
	struct lruvec *lruvec;

	spin_lock_irq(&pgdat->lru_lock);
	lruvec = mem_cgroup_page_lruvec(page, pgdat);
	ClearPageActive(page);
	SetPageUnevictable(page);
	SetPageLRU(page);
	add_page_to_lru_list(page, lruvec, LRU_UNEVICTABLE);
	spin_unlock_irq(&pgdat->lru_lock);
}

/**
 * lru_cache_add_active_or_unevictable
 * @page:  the page to be added to LRU
 * @vma:   vma in which page is mapped for determining reclaimability
 *
 * Place @page on the active or unevictable LRU list, depending on its
 * evictability.  Note that if the page is not evictable, it goes
 * directly back onto it's zone's unevictable list, it does NOT use a
 * per cpu pagevec.
 */
void lru_cache_add_active_or_unevictable(struct page *page,
					 struct vm_area_struct *vma)
{
	VM_BUG_ON_PAGE(PageLRU(page), page);

	if (likely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) != VM_LOCKED)) {
		SetPageActive(page);
		lru_cache_add(page);
		return;
	}

	if (!TestSetPageMlocked(page)) {
		/*
		 * We use the irq-unsafe __mod_zone_page_stat because this
		 * counter is not modified from interrupt context, and the pte
		 * lock is held(spinlock), which implies preemption disabled.
		 */
		__mod_zone_page_state(page_zone(page), NR_MLOCK,
				    hpage_nr_pages(page));
		count_vm_event(UNEVICTABLE_PGMLOCKED);
	}
	add_page_to_unevictable_list(page);
}

/*
 * If the page can not be invalidated, it is moved to the
 * inactive list to speed up its reclaim.  It is moved to the
 * head of the list, rather than the tail, to give the flusher
 * threads some time to write it out, as this is much more
 * effective than the single-page writeout from reclaim.
 *
 * If the page isn't page_mapped and dirty/writeback, the page
 * could reclaim asap using PG_reclaim.
 *
 * 1. active, mapped page -> none
 * 2. active, dirty/writeback page -> inactive, head, PG_reclaim
 * 3. inactive, mapped page -> none
 * 4. inactive, dirty/writeback page -> inactive, head, PG_reclaim
 * 5. inactive, clean -> inactive, tail
 * 6. Others -> none
 *
 * In 4, why it moves inactive's head, the VM expects the page would
 * be write it out by flusher threads as this is much more effective
 * than the single-page writeout from reclaim.
 */
static void lru_deactivate_file_fn(struct page *page, struct lruvec *lruvec,
			      void *arg)
{
	int lru, file;
	bool active;

	if (!PageLRU(page))
		return;

	if (PageUnevictable(page))
		return;

	/* Some processes are using the page */
	if (page_mapped(page))
		return;

	active = PageActive(page);
	file = page_is_file_cache(page);
	lru = page_lru_base_type(page);

	del_page_from_lru_list(page, lruvec, lru + active);
	ClearPageActive(page);
	ClearPageReferenced(page);
	add_page_to_lru_list(page, lruvec, lru);

	if (PageWriteback(page) || PageDirty(page)) {
		/*
		 * PG_reclaim could be raced with end_page_writeback
		 * It can make readahead confusing.  But race window
		 * is _really_ small and  it's non-critical problem.
		 */
		SetPageReclaim(page);
	} else {
		/*
		 * The page's writeback ends up during pagevec
		 * We moves tha page into tail of inactive.
		 */
		list_move_tail(&page->lru, &lruvec->lists[lru]);
		__count_vm_event(PGROTATED);
	}

	if (active)
		__count_vm_event(PGDEACTIVATE);
	update_page_reclaim_stat(lruvec, file, 0);
}

/**
 * Apply the lru_deactivate_fn to each page.
 * lruvec is current memor zone, e.g. NODE, cgroup,'s lru list set.
 *  
 */
static void lru_deactivate_fn(struct page *page, struct lruvec *lruvec,
			    void *arg)
{
	if (PageLRU(page) && PageActive(page) && !PageUnevictable(page)) {
		int file = page_is_file_cache(page);
		int lru = page_lru_base_type(page);  // return the inactive list BASE type. LRU_INACTIVE_ANON, or  LRU_INACTIVE_FILE.

		del_page_from_lru_list(page, lruvec, lru + LRU_ACTIVE);  // delete from active list, it's Base + LRU_ACTIVE.
		ClearPageActive(page);
		ClearPageReferenced(page);  // [?] what's for ?
		add_page_to_lru_list(page, lruvec, lru);		// add into deactive list.BASE always points to the inactive list.

		__count_vm_event(PGDEACTIVATE);
		update_page_reclaim_stat(lruvec, file, 0);
	}
}


//
// Semeru CPU

/**
 * Apply the lru_deactivate_fn to each page.
 * lruvec is current memor zone, e.g. NODE, cgroup,'s lru list set.
 * 
 * 1) Remove the page from both active and inactive list,
 * 2) Add the page into flush page list.
 * 
 * 
 * [?] we don't use the get_page()/put_page() ?
 * 	Because we only transfer the page from avtive/inactive list to flush_list,
 * 		 
 * 		 
 * 
 * 
 */
static void semeru_lru_flush_to_remote_fn(struct page *page, struct lruvec *lruvec,
			    void *arg)
{
	if (PageLRU(page) && !PageUnevictable(page)) {
		int file = page_is_file_cache(page);
		int lru = page_lru_base_type(page);  // return the inactive list BASE type. 2 types: anony_base, file_base.


		#ifdef DEBUG_FLUSH_LIST_DETAIL 
			printk(KERN_INFO "Before enqueue, list_head 0x%llx, prev 0x%llx, next 0x%llx, \n  &(page->lru) 0x%llx, &(page->lru)->prev 0x%llx, &(page->lru)->next 0x%llx  \n",  
													(u64)&lruvec->lists[SEMERU_LRU_FLUSH_LIST],   	
													(u64)lruvec->lists[SEMERU_LRU_FLUSH_LIST].prev,
													(u64)lruvec->lists[SEMERU_LRU_FLUSH_LIST].next,
													(u64)&(page->lru),
													(u64)page->lru.prev,
													(u64)page->lru.next);
		#endif

		if(PageActive(page)){
			// Path#1, remove page from active list.
			#ifdef DEBUG_FLUSH_LIST
			printk(KERN_INFO "%s, Transfer page 0x%llx from active list to flush list. \n", __func__, (u64)page);
			#endif

			del_page_from_lru_list(page, lruvec, lru + LRU_ACTIVE);  // delete from active list, it's Base + LRU_ACTIVE.
			ClearPageActive(page);
			ClearPageReferenced(page);  // [?] what's for ?
			//add_page_to_lru_list(page, lruvec, SEMERU_LRU_FLUSH_LIST);		// add into deactive list.BASE always points to the inactive list.

			__count_vm_event(PGDEACTIVATE);
			update_page_reclaim_stat(lruvec, file, 0);
		}else{
			// Path#2, remove page from inactive list.
			// if PageLRU(page) is true, it can only in active or inactive list.

			#ifdef DEBUG_FLUSH_LIST
				printk(KERN_INFO "%s, Transfer page 0x%llx from inactive list to flush list. \n", __func__, (u64)page);
			#endif

			// The page may NOT in LRU list.
			del_page_from_lru_list(page, lruvec, lru); // Base points to inactive list.

			// no bits is modifield here ?
			//add_page_to_lru_list(page, lruvec, SEMERU_LRU_FLUSH_LIST);

			//update_page_reclaim_stat(lruvec, file, 0); // only update when deactive from active list.
		}


		#ifdef DEBUG_FLUSH_LIST_DETAIL 
			printk(KERN_INFO "After enqueue, list_head 0x%llx, prev 0x%llx, next 0x%llx, \n  &(page->lru) 0x%llx, &(page->lru)->prev 0x%llx, &(page->lru)->next 0x%llx  \n",  
													(u64)&lruvec->lists[SEMERU_LRU_FLUSH_LIST],   	
													(u64)lruvec->lists[SEMERU_LRU_FLUSH_LIST].prev,
													(u64)lruvec->lists[SEMERU_LRU_FLUSH_LIST].next,
													(u64)&(page->lru),
													(u64)page->lru.prev,
													(u64)page->lru.next);
		#endif

	}// page is in LRU list.

	// Add the page into Flush list, no matter if it's in LRU list.
	add_page_to_lru_list(page, lruvec, SEMERU_LRU_FLUSH_LIST);
	SetPageLRU(page);

	// debug
	// print current list's page 
	//print_lru_flush_list_minor(lruvec, "Enqueue a page to flush-list");

}



// Semeru End


/*
 * Drain pages out of the cpu's pagevecs.
 * Either "cpu" is the current CPU, and preemption has already been
 * disabled; or "cpu" is being hot-unplugged, and is already dead.
 */
void lru_add_drain_cpu(int cpu)
{
	struct pagevec *pvec = &per_cpu(lru_add_pvec, cpu);

	if (pagevec_count(pvec))
		__pagevec_lru_add(pvec);

	pvec = &per_cpu(lru_rotate_pvecs, cpu);
	if (pagevec_count(pvec)) {
		unsigned long flags;

		/* No harm done if a racing interrupt already did this */
		local_irq_save(flags);
		pagevec_move_tail(pvec);
		local_irq_restore(flags);
	}

	pvec = &per_cpu(lru_deactivate_file_pvecs, cpu);
	if (pagevec_count(pvec))
		pagevec_lru_move_fn(pvec, lru_deactivate_file_fn, NULL);

	pvec = &per_cpu(lru_deactivate_pvecs, cpu);
	if (pagevec_count(pvec))
		pagevec_lru_move_fn(pvec, lru_deactivate_fn, NULL);

	// Semeru CPU 
	// Drain the newly added lru_flush_to_remote_pvecs
	pvec = &per_cpu(lru_flush_to_remote_pvecs, cpu);
	if (pagevec_count(pvec))
		pagevec_lru_move_fn(pvec, semeru_lru_flush_to_remote_fn, NULL);


	activate_page_drain(cpu);
}


/**
 * deactivate_file_page - forcefully deactivate a file page
 * @page: page to deactivate
 *
 * This function hints the VM that @page is a good reclaim candidate,
 * for example if its invalidation fails due to the page being dirty
 * or under writeback.
 */
void deactivate_file_page(struct page *page)
{
	/*
	 * In a workload with many unevictable page such as mprotect,
	 * unevictable page deactivation for accelerating reclaim is pointless.
	 */
	if (PageUnevictable(page))
		return;

	if (likely(get_page_unless_zero(page))) {
		struct pagevec *pvec = &get_cpu_var(lru_deactivate_file_pvecs);

		if (!pagevec_add(pvec, page) || PageCompound(page))
			pagevec_lru_move_fn(pvec, lru_deactivate_file_fn, NULL);
		put_cpu_var(lru_deactivate_file_pvecs);
	}
}

/**
 * deactivate_page - deactivate a page
 * @page: page to deactivate
 *
 * deactivate_page() moves @page to the inactive list if @page was on the active
 * list and was not an unevictable page.  This is done to accelerate the reclaim
 * of @page.
 */
void deactivate_page(struct page *page)
{
	if (PageLRU(page) && PageActive(page) && !PageUnevictable(page)) {  // Page is in active list
		struct pagevec *pvec = &get_cpu_var(lru_deactivate_pvecs);  //get this cpu's inactive pagevec.

		get_page(page);  // [?] increase page->_refcount. both actiave_page/deactivate_page need to get_page() ??
		if (!pagevec_add(pvec, page) || PageCompound(page))  // per-cpu pagevec is full, OR compound page, transfer the page into global inactive list.
			pagevec_lru_move_fn(pvec, lru_deactivate_fn, NULL);
		put_cpu_var(lru_deactivate_pvecs);	// release the per-cpu pagevec.
	}
}

//
// Semeru CPU
//

/**
 * semeru_flush_page - Add a page into flush-page list (Derived from deactivate a page)
 * @page: page to deactivate
 *
 * semeru_flush_page() moves @page to the inactive list if @page was on the active
 * list and was not an unevictable page.  This is done to accelerate the reclaim
 * of @page.
 */
void semeru_flush_page(struct page *page)
{
	if (PageLRU(page) && !PageUnevictable(page)) {  // active/inactive pages are both can be transffered flush-page-list
		struct pagevec *pvec = &get_cpu_var(lru_flush_to_remote_pvecs);  //get this cpu's inactive pagevec.

		get_page(page);  // [?] Move a page from lru_cache to flush_list, need to get the page ??
		if (!pagevec_add(pvec, page) || PageCompound(page))  // #1 Add the page into page vector first
			pagevec_lru_move_fn(pvec, semeru_lru_flush_to_remote_fn, NULL); //#2 If the page vector is not Empty,transfer its page into the flush_list.
		put_cpu_var(lru_flush_to_remote_pvecs);	// release the per-cpu pagevec.
	}
}




/*
 * Drain pages out of the cpu's pagevecs.
 * Either "cpu" is the current CPU, and preemption has already been
 * disabled; or "cpu" is being hot-unplugged, and is already dead.
 */
void lru_flush_list_drain_cpu(int cpu)
{
	struct pagevec *pvec;

	// Semeru CPU 
	// Drain the newly added lru_flush_to_remote_pvecs
	pvec = &per_cpu(lru_flush_to_remote_pvecs, cpu);
	if (pagevec_count(pvec))
		pagevec_lru_move_fn(pvec, semeru_lru_flush_to_remote_fn, NULL);


	activate_page_drain(cpu);
}


// Drain current cpu's local pagevecs
void lru_flush_list_drain(void)
{
	lru_flush_list_drain_cpu(get_cpu());  // disable preempt, and flush pagevecs
	put_cpu();		// enable preempt
}



static void lru_flush_list_drain_per_cpu(struct work_struct *dummy)
{
	lru_flush_list_drain();  // drain current cpu's local variable
}

// define a work_struct for each cpu, name is lru_flush_list_drain_work
// [?] What's the purpose of the work_struct 
static DEFINE_PER_CPU(struct work_struct, lru_flush_list_drain_work); 




/**
 * Flush all the cpu's local pagevec.
 *  
 */
void lru_flush_list_drain_all(void)
{

	static DEFINE_MUTEX(lock);
	static struct cpumask has_work;
	int cpu;

	/*
	 * Make sure nobody triggers this path before mm_percpu_wq is fully
	 * initialized.
	 */
	if (WARN_ON(!mm_percpu_wq))
		return;

	mutex_lock(&lock);
	get_online_cpus();  // acquire all the online cpu
	cpumask_clear(&has_work);

	for_each_online_cpu(cpu) {
		struct work_struct *work = &per_cpu(lru_flush_list_drain_work, cpu); // get hte work_struct of this cpu.

		if (pagevec_count( &per_cpu(lru_flush_to_remote_pvecs, cpu)) ) {
			INIT_WORK(work, lru_flush_list_drain_per_cpu);	// Assign the flush function to each work_struct
			queue_work_on(cpu, mm_percpu_wq, work);
			cpumask_set_cpu(cpu, &has_work);
		}
	}

	for_each_cpu(cpu, &has_work)
		flush_work(&per_cpu(lru_flush_list_drain_work, cpu));

	put_online_cpus();  // release all the online cpu
	mutex_unlock(&lock);
}




/**
 * [x] Flush the pages in per-cpu pagevec into each memory zone global active/inactive list.
 * This function is made to ensure that the CPU will not be asked to free up CPU-local pages while it is running in user space. 
 * 
 */
void lru_add_drain(void)
{
	lru_add_drain_cpu(get_cpu());  // disable preempt, and flush pagevecs
	put_cpu();		// enable preempt
}

static void lru_add_drain_per_cpu(struct work_struct *dummy)
{
	lru_add_drain();
}

static DEFINE_PER_CPU(struct work_struct, lru_add_drain_work); // define a work_struct for each cpu, name is lru_add_drain_work

void lru_add_drain_all(void)
{
	static DEFINE_MUTEX(lock);
	static struct cpumask has_work;
	int cpu;

	/*
	 * Make sure nobody triggers this path before mm_percpu_wq is fully
	 * initialized.
	 */
	if (WARN_ON(!mm_percpu_wq))
		return;

	mutex_lock(&lock);
	get_online_cpus();
	cpumask_clear(&has_work);

	for_each_online_cpu(cpu) {
		struct work_struct *work = &per_cpu(lru_add_drain_work, cpu);

		if (pagevec_count(&per_cpu(lru_add_pvec, cpu)) ||
		    pagevec_count(&per_cpu(lru_rotate_pvecs, cpu)) ||
		    pagevec_count(&per_cpu(lru_deactivate_file_pvecs, cpu)) ||
		    pagevec_count(&per_cpu(lru_deactivate_pvecs, cpu)) ||
		    need_activate_page_drain(cpu)) {
			INIT_WORK(work, lru_add_drain_per_cpu);
			queue_work_on(cpu, mm_percpu_wq, work);
			cpumask_set_cpu(cpu, &has_work);
		}
	}

	for_each_cpu(cpu, &has_work)
		flush_work(&per_cpu(lru_add_drain_work, cpu));

	put_online_cpus();
	mutex_unlock(&lock);
}

/**
 * release_pages - batched put_page()
 * @pages: array of pages to release
 * @nr: number of pages
 * @cold: whether the pages are cache cold
 *
 * Decrement the reference count on all the pages in @pages.  If it
 * fell to zero, remove the page from the LRU and free it.
 * 
 * 
 * [x] Didn't check the page->_mapcount before release it?
 * => Check the page->_refcount.
 * 		Because page->_refcount includes both User Space referencing(page->_mapcount) and Kernel Space referencing(e.g. DMA).
 * 		
 * 		[?] Why here can invoke the put_page(), does it already called the get_page() ?
 * 
 */
void release_pages(struct page **pages, int nr, bool cold)
{
	int i;
	LIST_HEAD(pages_to_free);
	struct pglist_data *locked_pgdat = NULL;
	struct lruvec *lruvec;
	unsigned long uninitialized_var(flags);
	unsigned int uninitialized_var(lock_batch);

	for (i = 0; i < nr; i++) {
		struct page *page = pages[i];

		/*
		 * Make sure the IRQ-safe lock-holding time does not get
		 * excessive with a continuous string of pages from the
		 * same pgdat. The lock is held only if pgdat != NULL.
		 */
		if (locked_pgdat && ++lock_batch == SWAP_CLUSTER_MAX) {
			spin_unlock_irqrestore(&locked_pgdat->lru_lock, flags);
			locked_pgdat = NULL;
		}

		if (is_huge_zero_page(page))
			continue;

		//#1, decrease and test the _refcount, if no one is referencing it, free it.
		//    [?] just decrease the _refcount, doesn't invoke the __put_page() to free it ?
		page = compound_head(page);
		if (!put_page_testzero(page)) 
			continue;

		if (PageCompound(page)) {
			if (locked_pgdat) {
				spin_unlock_irqrestore(&locked_pgdat->lru_lock, flags);
				locked_pgdat = NULL;
			}
			__put_compound_page(page);
			continue;
		}

		 //#2, remove the pages from it lru list. It may be in active/inactive list.
		 //    And then put it into the pages_to_free list.
		if (PageLRU(page)) { 
			struct pglist_data *pgdat = page_pgdat(page);

			if (pgdat != locked_pgdat) {
				if (locked_pgdat)
					spin_unlock_irqrestore(&locked_pgdat->lru_lock,
									flags);
				lock_batch = 0;
				locked_pgdat = pgdat;
				spin_lock_irqsave(&locked_pgdat->lru_lock, flags);
			}

			lruvec = mem_cgroup_page_lruvec(page, locked_pgdat); // get the pddat for the mem_cgroup
			VM_BUG_ON_PAGE(!PageLRU(page), page);
			__ClearPageLRU(page);
			del_page_from_lru_list(page, lruvec, page_off_lru(page));
		}

		/* #3 Clear Active bit in case of parallel mark_page_accessed */
		__ClearPageActive(page);
		__ClearPageWaiters(page);

		list_add(&page->lru, &pages_to_free);
	}
	if (locked_pgdat)
		spin_unlock_irqrestore(&locked_pgdat->lru_lock, flags);

	// #4, do the action to free the pages.
	//	 1) uncharge from this page's corresponding  cgroup
	//   2) Add the page back to buddy allocator,
	//		  per_cpu_pages list, and then the global free page list.
	mem_cgroup_uncharge_list(&pages_to_free);		// uncharge from cgroup. Or will cause cgroup-OOM error.
	free_hot_cold_page_list(&pages_to_free, cold); // [?] What's the meaning of hot/cold ?
}
EXPORT_SYMBOL(release_pages);

/*
 * The pages which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those pages may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __pagevec_release() will drain those queues here.  __pagevec_lru_add()
 * and __pagevec_lru_add_active() call release_pages() directly to avoid
 * mutual recursion.
 */
void __pagevec_release(struct pagevec *pvec)
{
	lru_add_drain();
	release_pages(pvec->pages, pagevec_count(pvec), pvec->cold);
	pagevec_reinit(pvec);
}
EXPORT_SYMBOL(__pagevec_release);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/* used by __split_huge_page_refcount() */
void lru_add_page_tail(struct page *page, struct page *page_tail,
		       struct lruvec *lruvec, struct list_head *list)
{
	const int file = 0;

	VM_BUG_ON_PAGE(!PageHead(page), page);
	VM_BUG_ON_PAGE(PageCompound(page_tail), page);
	VM_BUG_ON_PAGE(PageLRU(page_tail), page);
	VM_BUG_ON(NR_CPUS != 1 &&
		  !spin_is_locked(&lruvec_pgdat(lruvec)->lru_lock));

	if (!list)
		SetPageLRU(page_tail);

	if (likely(PageLRU(page)))
		list_add_tail(&page_tail->lru, &page->lru);
	else if (list) {
		/* page reclaim is reclaiming a huge page */
		get_page(page_tail);
		list_add_tail(&page_tail->lru, list);
	} else {
		struct list_head *list_head;
		/*
		 * Head page has not yet been counted, as an hpage,
		 * so we must account for each subpage individually.
		 *
		 * Use the standard add function to put page_tail on the list,
		 * but then correct its position so they all end up in order.
		 */
		add_page_to_lru_list(page_tail, lruvec, page_lru(page_tail));
		list_head = page_tail->lru.prev;
		list_move_tail(&page_tail->lru, list_head);
	}

	if (!PageUnevictable(page))
		update_page_reclaim_stat(lruvec, file, PageActive(page_tail));
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

/**
 * Add the "page" into the specific active/inactive lru list, lruvec.
 *  
 */
static void __pagevec_lru_add_fn(struct page *page, struct lruvec *lruvec,
				 void *arg)
{
	int file = page_is_file_cache(page);
	int active = PageActive(page);
	enum lru_list lru = page_lru(page); // types of LRU , anonymous/file backed, active/inacitve

	VM_BUG_ON_PAGE(PageLRU(page), page);

	SetPageLRU(page);
	add_page_to_lru_list(page, lruvec, lru);
	update_page_reclaim_stat(lruvec, file, active);
	trace_mm_lru_insertion(page, lru);
}

/*
 * Add the passed pages to the LRU, then drop the caller's refcount
 * on them.  Reinitialises the caller's pagevec.
 */
void __pagevec_lru_add(struct pagevec *pvec)
{
	pagevec_lru_move_fn(pvec, __pagevec_lru_add_fn, NULL);
}
EXPORT_SYMBOL(__pagevec_lru_add);

/**
 * pagevec_lookup_entries - gang pagecache lookup
 * @pvec:	Where the resulting entries are placed
 * @mapping:	The address_space to search
 * @start:	The starting entry index
 * @nr_entries:	The maximum number of entries
 * @indices:	The cache indices corresponding to the entries in @pvec
 *
 * pagevec_lookup_entries() will search for and return a group of up
 * to @nr_entries pages and shadow entries in the mapping.  All
 * entries are placed in @pvec.  pagevec_lookup_entries() takes a
 * reference against actual pages in @pvec.
 *
 * The search returns a group of mapping-contiguous entries with
 * ascending indexes.  There may be holes in the indices due to
 * not-present entries.
 *
 * pagevec_lookup_entries() returns the number of entries which were
 * found.
 */
unsigned pagevec_lookup_entries(struct pagevec *pvec,
				struct address_space *mapping,
				pgoff_t start, unsigned nr_pages,
				pgoff_t *indices)
{
	pvec->nr = find_get_entries(mapping, start, nr_pages,
				    pvec->pages, indices);
	return pagevec_count(pvec);
}

/**
 * pagevec_remove_exceptionals - pagevec exceptionals pruning
 * @pvec:	The pagevec to prune
 *
 * pagevec_lookup_entries() fills both pages and exceptional radix
 * tree entries into the pagevec.  This function prunes all
 * exceptionals from @pvec without leaving holes, so that it can be
 * passed on to page-only pagevec operations.
 */
void pagevec_remove_exceptionals(struct pagevec *pvec)
{
	int i, j;

	for (i = 0, j = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];
		if (!radix_tree_exceptional_entry(page))
			pvec->pages[j++] = page;
	}
	pvec->nr = j;
}

/**
 * pagevec_lookup - gang pagecache lookup
 * @pvec:	Where the resulting pages are placed
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @nr_pages:	The maximum number of pages
 *
 * pagevec_lookup() will search for and return a group of up to @nr_pages pages
 * in the mapping.  The pages are placed in @pvec.  pagevec_lookup() takes a
 * reference against the pages in @pvec.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 *
 * pagevec_lookup() returns the number of pages which were found.
 */
unsigned pagevec_lookup(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t start, unsigned nr_pages)
{
	pvec->nr = find_get_pages(mapping, start, nr_pages, pvec->pages);
	return pagevec_count(pvec);
}
EXPORT_SYMBOL(pagevec_lookup);

unsigned pagevec_lookup_tag(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t *index, int tag, unsigned nr_pages)
{
	pvec->nr = find_get_pages_tag(mapping, index, tag,
					nr_pages, pvec->pages);
	return pagevec_count(pvec);
}
EXPORT_SYMBOL(pagevec_lookup_tag);

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = totalram_pages >> (20 - PAGE_SHIFT);

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */
}




