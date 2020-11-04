/**
 * Semeru CPU Server 
 * 
 * We need to add some extra syscall to current kernel.
 * The files under this directory need to be loaded into kernel during builing.
 * 
 * 1) We need to let the running JVM can send/retrieve its data to Semeru memory server by using the RDMA.
 * 2) The RDMA operations are defined in the kernel module of Semeru. We also need to develop interface to
 * 		let modules redefine these operations.
 * 
 * the logic is :
 * 	1) Define syscall in kernel. The syscall invoke some operations specified by a struct.
 *  2) Write a wrapper function to fill real operations, functions, to this struct.
 *     Make this wrapper as expoted symbol, to let kernel moduls fill operations into it.
 * 	3) Define the RDMA operations in module and fill them into the exported symbol function.
 * 		 When loading the kernel module, it needs to do the filling automatically.
 * 
 */


#include <linux/kernel.h>
#include <asm/uaccess.h>   // copy data from kernel space to user space
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h> 
#include <linux/init.h>
#include <linux/slab.h>

// read/write data from Semeru Memory server, char* start_addr, unsigned long size
// return the RDMA buffer address back
//  
// int : target memory server id
// (write : int -> message type. 0 for data, 1 for signal )
// char _user* 		: start addr
// unsigned long 	: data size
typedef char* (semeru_rdma_read)(int,  char __user * , unsigned long);
typedef char* (semeru_rdma_write)(int, int, char __user * , unsigned long);



struct semeru_rdma_ops{
	semeru_rdma_read* 	rdma_read;
	semeru_rdma_write* 	rdma_write;
};


// a global structure
struct semeru_rdma_ops  rdma_ops_in_kernel;