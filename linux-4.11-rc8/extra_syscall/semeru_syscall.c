/**
 * Semeru CPU server 
 * 
 * 
 */

// self defined. The syscall should be declared in syscalls.h
#include "semeru_syscall.h"

// Develop syscall for this section
#include "linux/swap_global_struct_mem_layer.h"




/**
 * the wrapper, let module fill its defined functions into the structure.
 * And then these module defined functions can be used by kernel.
 */
int rdma_ops_wrapper(struct semeru_rdma_ops* module_defined_rdma_ops){
	if(module_defined_rdma_ops == NULL){
		printk("semeru_rdma_read is NULL. Can't execute it. \n");
	}else{
		printk("Fill the  rdma_ops_in_kernel with module_defined_rdma_ops, 0x%lx. \n", (unsigned long)module_defined_rdma_ops);
		
		rdma_ops_in_kernel.rdma_read = module_defined_rdma_ops->rdma_read;
		rdma_ops_in_kernel.rdma_write = module_defined_rdma_ops->rdma_write;
	}

	return 0;
}
EXPORT_SYMBOL(rdma_ops_wrapper);



/**  
 * The implementation of syscall
 * 
 * asmlinkage ? 
 * 		skip the first parameter ?
 * 		and put all the parameters in stack ?
 * 
 * 
 * Parameters:
 * 		type 1, 1-sided rdma read;  
 *    type 2, 1-sided rdma data write;
 * 		type 3, 1-sided rdma signal write; Flush all the outstanding messages before issue signal.
 * 		target_server : the id of memory server
 * 		start_addr,
 * 		size, 		4KB alignment
 * 
 * 
 */
asmlinkage int sys_do_semeru_rdma_ops(int type, int target_server,  char __user * start_addr, unsigned long size){
	char* ret;
	int write_type;

	#ifdef DEBUG_MODE_BRIEF
	printk("Enter %s. with type 0x%x \n", __func__, type);
	#endif

	if(type == 1){
		// rdma read
		if(rdma_ops_in_kernel.rdma_read != NULL){
			rdma_ops_in_kernel.rdma_read(target_server, start_addr,size);
		}else{
			printk("rdma_ops_in_kernel.rdma_read is NULL. Can't execute it. \n");
		}
	}else if(type == 2){
		// rdma data write 
		if( rdma_ops_in_kernel.rdma_write != NULL){
			
			#ifdef DEBUG_MODE_BRIEF
			printk("rdma_ops_in_kernel.rdma_write is 0x%llx. \n",(uint64_t)rdma_ops_in_kernel.rdma_write );
			#endif
			write_type = 0x0; // data write 
			ret = rdma_ops_in_kernel.rdma_write(target_server, write_type, start_addr,size);
			if(unlikely(ret == NULL) ){
				printk(KERN_ERR "%s, rdma write [0x%lx, 0x%lx) failed. ", __func__, (unsigned long)start_addr, (unsigned long)(start_addr + size) );
				return -1;
			}
		}else{
			printk("rdma_ops_in_kernel.rdma_write is NULL. Can't execute it. \n");
		}

	}else if(type == 3){
		// rdma signal write 
		if( rdma_ops_in_kernel.rdma_write != NULL){
			
			#ifdef DEBUG_MODE_BRIEF
			printk("rdma_ops_in_kernel.rdma_write is 0x%llx. \n",(uint64_t)rdma_ops_in_kernel.rdma_write );
			#endif
			write_type = 0x1; // signal write 
			ret = rdma_ops_in_kernel.rdma_write(target_server, write_type, start_addr,size);
			if(unlikely(ret == NULL) ){
				printk(KERN_ERR "%s, rdma write [0x%lx, 0x%lx) failed. ", __func__, (unsigned long)start_addr, (unsigned long)(start_addr + size) );
				return -1;
			}
	
		}else{
			printk("rdma_ops_in_kernel.rdma_write is NULL. Can't execute it. \n");
		}

	}else{
		// wrong types
		printk("%s, wrong type. \n", __func__);
	}

	return 0;
}


//
// Functions for swap ratio monitor
//


/**
 * Semeru CPU, reset array initial value to 0.
 * 
 * 	return 0 , succ,
 * 				-1 , error. 
 * 
 */
asmlinkage int sys_swap_stat_reset_and_check(u64 start_vaddr, u64 bytes_len){
	u32 i;

	//printk(KERN_INFO"%s, reset swap out monitor information. \n", __func__);

	// 1) reset on-demand swapin counter.
	reset_swap_info();
	printk(KERN_INFO"%s, ater reset, on_demand_swapin_number 0x%x \n",__func__,  get_on_demand_swapin_number());


	// 2) the [buff, buff+ bytes_len) must fall into the cover of the array.
	// 
	#ifdef DEBUG_SERVER_HOME
	if(within_range(start_vaddr)){
		
		for(i=0; i<SWAP_OUT_MONITOR_ARRAY_LEN; i++ ){
			//jvm_region_swap_out_counter[i] = 0;
			atomic_set(&jvm_region_swap_out_counter[i], 0);
		}

		return 0;
	}// end of if.
	#else
	if( (u64)start_vaddr >= SWAP_OUT_MONITOR_VADDR_START  &&  bytes_len <=  (u64)(SWAP_OUT_MONITOR_ARRAY_LEN *(1<<SWAP_OUT_MONITOR_UNIT_LEN_LOG)) ){
		
		for(i=0; i<SWAP_OUT_MONITOR_ARRAY_LEN; i++ ){
			//jvm_region_swap_out_counter[i] = 0;
			atomic_set(&jvm_region_swap_out_counter[i], 0);
		}

		printk(KERN_INFO"%s, Region monitoring, reset jvm_region_swap_out_counter[] to 0 \n",__func__);

		return 0;
	}// end of if.
	#endif


  printk(KERN_ERR "%s, [0x%llx, 0x%llx) exceed the swap out array range [0x%llx, 0x%llx),  ", __func__, 
																													(u64)start_vaddr, 
																													(u64)(start_vaddr+bytes_len),
																													(u64)SWAP_OUT_MONITOR_VADDR_START,  
																													(u64)(SWAP_OUT_MONITOR_VADDR_START+ SWAP_OUT_MONITOR_ARRAY_LEN *(1<<SWAP_OUT_MONITOR_UNIT_LEN_LOG) ));
	return -1;
}


/**
 * Semeru CPU : get the swapped out pages number.
 * Because we can't use any FPU in kernel, return the number of swapped pages. 
 * 
 * Warning : For a not-full Region. Using swapped-out pages to calculate cached-paged ratio is not that accurate.
 * 
 */
asmlinkage u64 sys_num_of_swap_out_pages(u64 start_vaddr, u64 bytes_len){
	return swap_out_pages_for_range(start_vaddr, start_vaddr + bytes_len);
}



/**
 * Semeru CPU, get the on-demand swapin number.
 * 						For each on-demand swapin operation, it may load multiple pages into Swap Cache.
 * Syscall id, 337
 *  
 * Warning : Some pages are prefetched into CPU DRAM. However, we can't count them accurately right now.
 * 
 */
asmlinkage int sys_num_of_on_demand_swapin(void){

	printk(KERN_INFO"%s, on-demand swapin page number : %d \n", __func__, get_on_demand_swapin_number());
	printk(KERN_INFO"%s, hit on swap cache page number : %d \n\n", __func__, get_hit_on_swap_cache_number());
	
	return get_on_demand_swapin_number();
}



/**
 * Copy data to between user space and kernel space.
 *  
 */
asmlinkage int sys_test_syscall(char* __user *buff, uint64_t len){
	int ret = 0;
	uint64_t i;


	char* kernel_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	
	for(i=0; i<PAGE_SIZE/sizeof(uint64_t); i++ ){
		((uint64_t*)kernel_buf)[i] = 1;
	}

	printk(KERN_INFO "%s, Before copy_from_user, first uint64_t kernel_buf value: 0x%llx \n", 
																																					__func__,
																												 									*((uint64_t*)kernel_buf));

	//1) copy data from user space to kernel space	
	if(len > PAGE_SIZE){
		len = PAGE_SIZE;
	}
	
	// void *to, const void __user *from, unsigned long n
  ret = copy_from_user(kernel_buf, buff, len);
	if(ret!=0){
		printk(KERN_ERR "%s, %d bytes are not copied in copy_from_user. \n", __func__, ret);
		goto err;
	}

	printk(KERN_INFO "%s, After copy_from_user, first uint64_t kernel_buf value: 0x%llx \n", 
																																					__func__,
																												 									*((uint64_t*)kernel_buf));


	// 2) Copy data to user space
	// rseet the value.
	for(i=0; i<PAGE_SIZE/sizeof(uint64_t); i++ ){
		((uint64_t*)kernel_buf)[i] = 1;
	}

	// void __user *to, const void *from, unsigned long n
	ret = copy_to_user(buff, kernel_buf, len);
	if(ret!=0){
		printk(KERN_ERR "%s, %d bytes are not copied in copy_to_user. \n", __func__, ret);
		goto err;
	}

err:
	return ret;
}
