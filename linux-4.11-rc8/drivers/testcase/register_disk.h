/**
 * Register a Remote Block Device under current OS.
 * 
 * The connection is based on RDMA over InfiniBand.
 * 
 */

#ifndef REMOTEMEMPOOL
#define REMOTEMEMPOOL

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h> 

// Block layer 
#include <linux/blk-mq.h>
#include <linux/blkdev.h>

// For infiniband
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>


// Disk hardware information
#define RMEM_PHY_SECT_SIZE					512 	// physical sector seize, used by driver (to disk).
#define RMEM_LOGICAL_SECT_SIZE			4096	// logical sector seize, used by kernel (to i/o).
//#define RMEM_REQUEST_QUEUE_NUM     2  	// for debug, use the online_cores
#define RMEM_QUEUE_DEPTH           	16  	// [?]  1 - (-1U), what's the good value ? 
#define RMEM_QUEUE_MAX_SECT_SIZE		1024 	// The max number of sectors per request, /sys/block/sda/queue/max_hw_sectors_kb is 256
#define DEVICE_NAME_LEN							32


#define RMEM_SIZE_IN_BYTES  (unsigned long)1024*1024*1024*8  // 8GB


static int rbd_major_num;
static int online_cores;



/**
 * Used for storing RDMA connection information 
 * 
 * 
 */
struct rmem_rdma_connection	{

  int tmp;  // empty.

};


// Send with the i/o request to the remote memory server ??
//
struct rmem_rdma_request {
	//struct nvme_request	req;
	struct ib_mr		*mr;
	//struct nvme_rdma_qe	sqe;
	//struct ib_sge		sge[1 + NVME_RDMA_MAX_INLINE_SEGMENTS];
	u32			num_sge;
	int			nents;
	bool			inline_data;
	struct ib_reg_wr	reg_wr;
	struct ib_cqe		reg_cqe;
	//struct nvme_rdma_queue  *queue;
	struct sg_table		sg_table;
	//struct scatterlist	first_sgl[];
};



/**
 * [?] What's the purpose of this queue ??
 * used for RDMA connection
 * 
 */
struct rmem_rdma_queue {
	struct rmem_rdma_connection	      *rmem_conn;				// RDMA session connection 
	struct rmem_device_control	      *rmem_dev_ctrl;  	// pointer to parent, the device driver 
};






/**
 * Block device information
 * This structure is the driver context data. 
 * 
 * 
 * [?] One rbd_device_control should record all the connection_queues ??
 * 
 * [?] Finnaly, we need pass all these fields to  tag_set,  htcx, rdma_connections ?
 * 
 * 
 */
struct rmem_device_control {
	//int			     							fd;   // [?] Why do we need a file handler ??
	int			     							major; /* major number from kernel */
	//struct r_stat64		     stbuf; /* remote file stats*/
	//char			     						file_name[DEVICE_NAME_LEN];       // [?] Do we need a file name ??
	//struct list_head	     		list;           /* next node in list of struct IS_file */    // Why do we need such a list ??
	struct gendisk		    		*disk;        // [?] The disk information, logical/physical sectior size ? 
	struct request_queue	    *queue; 			// The software staging request queue
	struct blk_mq_tag_set	    tag_set;			// Used for information passing. Define blk_mq_ops.(block_device_operations is defined in gendisk.)
	
  //[?] What's  this queue used for ?
  struct rmem_rdma_queue	  *rdma_queues;			//  [?] The rdma connection session ?? one rdma session queue per software staging queue ??
	unsigned int		      		queue_depth;      //[?] How to set these numbers ?
	unsigned int		      		nr_queues;				// [?] pass to blk_mq_tag_set->nr_hw_queues ??
	//int			              		index; /* drive idx */
	char			            		dev_name[DEVICE_NAME_LEN];
	//struct rdma_connection	    **IS_conns;
	//struct config_group	     dev_cg;
	spinlock_t		        		rmem_ctl_lock;					// mutual exclusion
	//enum IS_dev_state	     state;	

	// Below fields are used for debug.
	//
	struct block_device *bdev_raw;			// Points to a local block device. 
	struct bio_list bio_list;
};




/**
 * Store all the information need for Block device, RDMA connection ??
 * 
 */
struct rmem_control {

  // information for the remote block device
  struct rmem_device_control* rmem_deb;

  // information for RDMA


};


// Debug
// Declare some global varibles

static struct rmem_device_control  rmem_dev_ctl_global;



#endif // REMOTEMEMPOOL