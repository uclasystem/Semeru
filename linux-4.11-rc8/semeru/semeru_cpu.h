/**
 * The main header of Semeru cpu header.
 * It contains 2 separate parts for now:
 * 1) the remote memory pool go through swap-block layer-RDMA
 * 		inlcude files: 
 * 		block_layer_rdma.h
 *		block_layer_rdma.c
 * 		regsiter_disk.c
 * 
 *  2) Remote memory go through frontswap-RDMA
 * 		include files:
 * 		frontswap_path.h
 * 		frontswap_ops.c
 * 		frontswap_rdma.c
 * 
 */

#ifndef SEMERU_CPU
#define SEMERU_CPU

#include <linux/swap_global_struct_bd_layer.h>

#ifdef SEMERU_FRONTSWAP_PATH
	#include "frontswap_path.h"
#else
	#include "block_path.h"
#endif
 

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h> 
#include <linux/init.h>

// ############# Control Path #####################

#define control_path_fixed_qp 0


// ############# Multiple Memory Configurations #############

// moved to include/linux/swap_global_struct.h
// This information can be used by JDK


// Decelare the ip of each memory servers.
// !! defined in semeru_cpu.c
extern char *mem_server_ip[];
extern uint16_t mem_server_port;





// ############# profiling counter #############


//#define RDMA_MESSAGE_PROFILING 1

extern atomic_t rdma_read_to_mem_server[];
extern atomic_t rdma_write_to_mem_server[];

void reset_rdma_message_info(void);
void rdma_read_from_mem_server_inc(int mem_server_id);
void rdma_write_to_mem_server_inc(int mem_server_id);

#define PRINT_LIMIT (1024 * 256)
void periodically_print_info(const char* message);



// ###############  Debug macros ####################

//#define DEBUG_MODE_BRIEF 1


#endif // End of SEMERU_CPU



