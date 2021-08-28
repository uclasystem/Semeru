/**
 * The main entry of Semeru CPU server module
 * Semeru has 2 different paths,
 *  a. Block layer path : the swap data is compacted into i/o request, and then forwarded to RDMA path 
 *  b. Frontswap path : the swap data is forwarded to RDMA path as physical page directly. Bypass the block layer.
 * 
 * Use the macro, SEMERU_FRONTSWAP_PATH,  to control the path. * Need to update Makefile manually *
 * 
 * 
 * 
 */


#include "semeru_cpu.h"



MODULE_AUTHOR("Semeru, Chenxi Wang");
MODULE_DESCRIPTION("RMEM, remote memory paging over RDMA");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");



// ##########  Global variables ##########

atomic_t rdma_read_to_mem_server[NUM_OF_MEMORY_SERVER];
atomic_t rdma_write_to_mem_server[NUM_OF_MEMORY_SERVER];

char *mem_server_ip[] = { "10.0.0.2", "10.0.0.14" };
uint16_t mem_server_port = 9400;



// ########### Profiling functions ##############


void reset_rdma_message_info(void){
	int mem_server_id;

	for(mem_server_id = 0; mem_server_id < NUM_OF_MEMORY_SERVER; mem_server_id++){
		atomic_set(&rdma_read_to_mem_server[mem_server_id],0);
		atomic_set(&rdma_write_to_mem_server[mem_server_id],0);
	}
}

// Multiple thread safe.
void rdma_read_from_mem_server_inc(int mem_server_id){
	atomic_inc(&rdma_read_to_mem_server[mem_server_id]);
}

void rdma_write_to_mem_server_inc(int mem_server_id){
	atomic_inc(&rdma_write_to_mem_server[mem_server_id]);
}

void periodically_print_info(const char *message)
{
	int mem_server_id;

	for (mem_server_id = 0; mem_server_id < NUM_OF_MEMORY_SERVER; mem_server_id++) {
		if (atomic_read(&rdma_read_to_mem_server[mem_server_id]) &&
		    atomic_read(&rdma_read_to_mem_server[mem_server_id]) % PRINT_LIMIT == 0) {
			pr_warn("%s, Memory server[%d] , read %d , write %d (may overflow)", message, mem_server_id,
				atomic_read(&rdma_read_to_mem_server[mem_server_id]),
				atomic_read(&rdma_write_to_mem_server[mem_server_id]));
		}
	}
}

//############# End of Profiling functions ##########



// invoked by insmod 
int __init semeru_cpu_init(void){

  int ret = 0;

  #ifdef SEMERU_FRONTSWAP_PATH

    ret = semeru_fs_rdma_client_init();
    if(unlikely(ret)){
      printk(KERN_ERR "%s, semeru_fs_rdma_client_init failed. \n",__func__);
      goto out;
    }

  #else
    printk(KERN_ERR "%s, TO BE DONE.\n",__func__);

  #endif


out:
	return ret;
}


// invoked by rmmod 
void __exit semeru_cpu_exit(void)
{
  		
	printk(" Prepare to remove the Semeru CPU Server module.\n");

  #ifdef SEMERU_FRONTSWAP_PATH
    semeru_fs_rdma_client_exit();
  #else
    printk(KERN_ERR "%s, TO BE DONE.\n",__func__);
  #endif
	

	printk(" Remove  Semeru CPU Server module DONE. \n");

	return;
}

module_init(semeru_cpu_init);
module_exit(semeru_cpu_exit);

