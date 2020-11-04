/**
 * The main header of Semeru cpu header.
 * It contains 2 separate parts for now:
 * 	1) the remote memory pool go through swap-block layer-RDMA
 * 		inlcude files: 
 * 		block_layer_rdma.h
 *		block_layer_rdma.c
 * 		regsiter_disk.c
 * 
 *  2) Remote memory go through frontswap-RDMA
 * 		include files:
 * 		frontswap.h
 * 		frontswap.c
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








#endif // End of SEMERU_CPU



