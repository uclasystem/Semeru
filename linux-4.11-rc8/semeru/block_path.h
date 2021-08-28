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
#include <linux/init.h>


// Block layer 
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/swapfile.h>
#include <linux/swap.h>

// For infiniband
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/pci-dma.h>
#include <linux/pci.h>			// Use the dma_addr_t defined in types.h as the DMA/BUS address.
#include <linux/inet.h>
#include <linux/lightnvm.h>
#include <linux/sed-opal.h>

// Utilities 
#include <linux/log2.h>
#include<linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <asm/uaccess.h>   // copy data from kernel space to user space
#include <linux/slab.h>		// kmem_cache

// Semeru
#include <linux/swap_global_struct_bd_layer.h>


// Local block device
#include <trace/events/block.h>
#include <linux/kthread.h>
#include <linux/hdreg.h>



//
// ################################ Macro definition of RDMA ###################################
// 

#define CP_ZERO_MAPPED_PAGE (int)-1  


//
// ################################ Structure definition of RDMA ###################################
//
// Should initialize these RDMA structure before Block Device structures.
//



/**
 * Used for message passing control
 * For both CM event, data evetn.
 * RDMA data transfer is desinged in an asynchronous style. 
 */
enum rdma_queue_state { 
	IDLE = 1,		 // 1, Start from 1.
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,		// 5,  updated by IS_cma_event_handler()

	MEMORY_SERVER_AVAILABLE, // 6

	FREE_MEM_RECV,		// After query, we know the available regions on memory server.
	RECEIVED_CHUNKS,	// get chunks from remote memory server
	RDMA_BUF_ADV,   // designed for server
	WAIT_OPS,
	RECV_STOP,    	// 11

	RECV_EVICT,
	RDMA_WRITE_RUNNING,
	RDMA_READ_RUNNING,
	SEND_DONE,
	RDMA_DONE,     	// 16

	RDMA_READ_ADV,	// updated by IS_cq_event_handler()
	RDMA_WRITE_ADV,
	CM_DISCONNECT,
	ERROR,
	TEST_DONE,		// 21, for debug
};



enum mem_type {
	DMA = 1,
	FASTREG = 2,
	MW = 3,
	MR = 4
};


  // 2-sided RDMA message type
  // Used to communicate with cpu servers.
	enum message_type{
		DONE = 1,					// Start from 1
		GOT_CHUNKS,				// Get the remote_addr/rkey of multiple Chunks
		GOT_SINGLE_CHUNK,	// Get the remote_addr/rkey of a single Chunk
		FREE_SIZE,				//
		EVICT,        		// 5

		ACTIVITY,					// 6
		STOP,							//7, upper SIGNALs are used by server, below SIGNALs are used by client.
		REQUEST_CHUNKS,
		REQUEST_SINGLE_CHUNK,	// Send a request to ask for a single chunk.
		QUERY,         			// 10

		AVAILABLE_TO_QUERY  // 11 This memory server is oneline to server.
	};


/**
 * Semeru CS - Build the RDMA connection to Semeru MS
 * 
 * Two-sided RDMA message structure.
 * We use 2-sieded RDMA communication to exchange information between Client and Server.
 * Both Client and Server have the same message structure. 
 */
struct message {
  	
	// Information of the chunk to be mapped to remote memory server.
	uint64_t buf[MAX_REGION_NUM];					// Remote addr.
	uint64_t mapped_size[MAX_REGION_NUM];	// Maybe not fully mapped. 
  uint32_t rkey[MAX_REGION_NUM];   			// remote key
  int mapped_chunk;											// Chunk number in current message. 

	enum message_type type;
};




/**
 * Need to update to Region status.
 */
enum chunk_mapping_state {
	EMPTY,		// 0
	MAPPED,		// 1, Cached ? 
};


enum region_status{
  NEWLY_ALLOCATED,			// 0, newly allocated ?
	CACHED,			// Partial or Fully cached Regions.
  EVICTED			// 2, clearly evicted to Memory Server
};


// Meta Region/chunk: a contiguous chunk of a Region, may be not fully mapped.
// Data Region/chunk: Default mapping size is REGION_SIZE_GB, 4 GB default.
// RDMA mapping size isn't the Java Region size.
//
struct remote_mapping_chunk {
	uint32_t 					remote_rkey;		// RKEY of the remote mapped chunk
	uint64_t 					remote_addr;		// Virtual address of remote mapped chunk
	uint64_t					mapped_size;		// For some specific Chunk, we may only map a contigunous range.
	enum chunk_mapping_state 	chunk_state;
};

/**
 * Use the chunk as contiguous File chunks.
 * 
 * For example,
 * 	File address 0x0 to 0x3fff,ffff  is mapped to the first chunk, remote_mapping_chunk_list->remote_mapping_chunk[0].
 * 	When send 1 sided RDMA read/write, the remote address shoudl be remote_mapping_chunk_list->remote_mapping_chunk[0].remote_addr + [0 to 0x3fff,ffff]
 * 
 * For each Semery Memory Server, its mapping size has 2 parts:
 * 		1) Meta Data Region. Maybe not fully mapped.
 * 		2) Data Region. Mpped at REGION_SIZE_GB granularity.
 * 
 */
struct remote_mapping_chunk_list {
	struct remote_mapping_chunk *remote_chunk;		
	uint32_t remote_free_size;			// total mapped byte size. Accumulated each remote_mapping_chunk[i]->mapped_size
	uint32_t chunk_num;							// length of remote_chunk list
	uint32_t chunk_ptr;							// points to first empty chunk. 
};





// This is the kernel level RDMA over IB.  
// Need to use the API defined in kernel headers. 
//

/**
 * Semeru CS - Meta Space layout
 * Store the DMA/physical address of each meta data structure to save some time.
 *
 *
 *
 * 
 */
struct rmem_meta_space_layout{

	char* target_obj_queue_space_buf;		// Can use the  user-space address directly ? OR have to copy data to user space
	char* cpu_server_stw_state_buf;			

	uint64_t 	target_obj_queue_space_dma_addr;		// start address of Target Object Queue space
	uint64_t	cpu_server_stw_state_dma_addr;			// cpu server stw/mutator



};







/**
 * 1-sided RDMA (read/write) message.
 * Both Semeru Control Path(CP) and Data Path(DP) use this rdma command structu.
 * 
 * Reserve a rmem_rdma_command for each I/O requst.
 * Allocate during the building of the I/O request.
 * 		i.e. Have registered RDMA buffer, used to send a wr to the QP.
 * 			[?] Can we optimize here ?? use the data in bio as DMA buffer directly.
 *  	i.e. a. request will reserve size for this request command : requet->cmd_size					
 * 			 	Acccess by : blk_mq_rq_to_pdu(request)
 * 			 b. ib_rdma_wr->ib_send_wr->wr_id also points to this context.
 * 
 * Fields
 * 		ib_sge		: Store the data to be sent to remote memory.
 * 		ib_rdma_wr 	: The WR used for 1-sided RDMA read/write.
 * 		request		: Transfer this requste to 1 sided RDMA message.
 * 						And then resue this request as reponds I/O requseet.
 * 						Because this request has the same request->tag which is used for monitoring if a sync request is finished.
 * 
 * More Explanation
 *	Build a seperate WR for each I/O request and sent them to remote memory pool via  RDMA read/write.
 * 
 */
struct rmem_rdma_command{
 
	struct semeru_rdma_queue * rdma_queue;

	// RDMA wr for 1-sdied RDMA read/write.
	struct ib_rdma_wr 	rdma_sq_wr;			// wr for 1-sided RDMA write/send.
	
	// Related block I/O requset.
	// This is dedicated for data path.
	struct request 			*io_rq;					// Pointer to the I/O requset. [!!] Contain Real Offload [!!]

	struct ib_sge sge_list[MAX_REQUEST_SGL]; 	// scatter-gather entry for 1-sided RDMA read/write WR. 

	#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
		// Debug fields
	// Control-Path, virtual address of source
	// used for rdma read, write back data.
	// The 1-sided RDMA communication is asynchronous. 
	// When the RDMA read finish ack comes back, the caller already exits the syscall.
	// The waken up process is another process which doesn't know the meaning of the user space virtual address.
	// char __user * start_addr; 
		char* kmapped_addr;			// For debug, the kmapped kernel address for the user space address.
		uint64_t bytes_len;			// the length of the user space request.
	
	// Data-Path
	// 0 : (Default) read data page
	// 1 : write data page
	// 2 : write tag
	// 3 : read
	//
	// Control-Path
	// 10 : read
	// 11 : write
		int message_type;

	#endif


	// Scatter-Gather 
	// scatterlist  - this works like sge_table.
	// [!!] The scatterlist is temporaty data, we can put it in the behind of i/o request and not send them to remote server.
	// points to the physical pages of i/o requset. 
	u64  								nentry;		// number of the segments, usually one pysical page per segment
	struct scatterlist	sgl[];		// Just keep a pointer. The memory is reserved behind the i/o request. 
	// scatterlist should be at the end of this structure : Variable/Flexible array in structure.
	// Then it can points to reserved space in i/o requst  after sizeof(struct rmem_rdma_command).
};



/**
 * Build a QP for each core on cpu server. 
 * [?] This design assume we only have one memory server.
 * 			Extern this design later.
 * 
 */
struct semeru_rdma_queue {
  // RDMA client ID, one for per QP.
	struct rdma_cm_id *cm_id;		//  ? bind to QP

	// ib events 
  struct ib_cq *cq;			// Completion queue
	struct ib_qp *qp;			// Queue Pair

	enum rdma_queue_state state;  	// the current status of the QP.
	wait_queue_head_t 		sem;    	// semaphore for wait/wakeup
	spinlock_t 						cq_lock;	// used for CQ
	uint8_t  		freed;			// some function can only be called once, this is the flag to record this.
	atomic_t 		rdma_post_counter;


	int q_index;					// initialized to disk hardware queue index
	struct rdma_session_context		*rdma_session;			// Record the RDMA session this queue belongs to.

	// cache for fs_rdma_request. One for each rdma_queue
	struct kmem_cache *fs_rdma_req_cache;

};


/**
 * The rdma device.
 * It's shared by multiple QP and CQ. 
 * One RDMA device per server.
 * 
 */
struct semeru_rdma_dev {
  struct ib_device *dev;
  struct ib_pd *pd;
};



/**
 * Mange the RDMA connection to a remote server. 
 * Every Remote Memory Server has a dedicated rdma_session_context as controller.
 * 	
 * More explanation 
 * [?] One session per server. Extend here later.
 * 
 */
struct rdma_session_context {

	// 1) RDMA QP/CQ management
	struct semeru_rdma_dev *rdma_dev; // The RDMA device of cpu server.
	struct semeru_rdma_queue * rdma_queues;	// point to multiple QP 

  // For infiniband connection rdma_cm operation
  uint16_t 	port;			/* dst port in NBO */
	u8 				addr[16];		/* dst addr in NBO */
  uint8_t 	addr_type;		/* ADDR_FAMILY - IPv4/V6 */
  int 			send_queue_depth;		// Send queue depth. Both 1-sided/2-sided RDMA wr is limited by this number.
	int 			recv_queue_depth;		// Receive Queue depth. 2-sided RDMA need to post a recv wr.


	//
  // 2) 2-sided RDMA section. 
  //		This section is used for RDMA connection and basic information exchange with remote memory server.

  // DMA Receive buffer
  struct ib_recv_wr 	rq_wr;			// receive queue wr
	struct ib_sge 			recv_sgl;		// recv single SGE entry
  struct message			*recv_buf;
  //dma_addr_t 			recv_dma_addr;	// It's better to check the IB DMA address limitations, 32 or 64
	u64									recv_dma_addr;	
	//DECLARE_PCI_UNMAP_ADDR(recv_mapping)	// Use MACRO to define a DMA fields. It will do architrecture check first. same some fields.


  // DMA send buffer
	// ib_send_wr, used for posting a two-sided RDMA message 
  struct ib_send_wr 	sq_wr;			// send queue wr
	struct ib_sge 			send_sgl;		// attach the rdma buffer to sg entry. scatter-gather entry number is limitted by IB hardware.
	struct message			*send_buf;	// the rdma buffer.
	//dma_addr_t 			send_dma_addr;
	u64 								send_dma_addr;
	//DECLARE_PCI_UNMAP_ADDR(send_mapping)



	// 3) For 1-sided RDMA read/write
	// I/O request --> 1 sided RDMA message.
	// rmem_rdma_queue store these reserved 1-sided RDMA wr information.
	
	// 3.1) Data-Path, used for swaping mechanism


	// 3.2) Control-Path, used for user-space invocation.
	struct rmem_rdma_command				*cp_rmem_rdma_read_cmd;
	struct rmem_rdma_command				*cp_rmem_rdma_write_cmd;
	// Move into the rdma_queue
	//atomic_t rdma_post_counter; // Control the outstanding wr number. Less than RDMA_SEND_QUEUE_DEPTH.


	struct rmem_meta_space_layout 	semeru_meta_space;

	// 4) manage the CHUNK mapping.
	struct remote_mapping_chunk_list remote_chunk_list;


	// Keep a rdma buffer for flag byte specially
	// Fill these information into a 1-sided ib_rdma_wr
	// Write the value 1 to the corresponding Region's flag .
	// The flag is 4 bytes and allocated in native space.
	uint32_t* write_tag;
	uint64_t	write_tag_dma_addr;  			 // corresponding dma address, just the physical address.
	struct rmem_rdma_command *write_tag_rdma_cmd;


	// 5) The block deivce
	// Points to the Disk Driver Controller/Context.
	// RDMA part is initialized first, 
	// then invoke "RMEM_init_disk_driver(void)" to get the Disk Driver controller.
	//
	struct rmem_device_control		*rmem_dev_ctrl;

};









//
// ================================= Block Device Part =============================
//








/**
 * Block device information
 * This structure is the driver context data.  
 * 		i.e. blk_mq_tag_set->driver_data
 * 
 * 
 * [?] One rbd_device_control should record all the connection_queues ??
 * 
 * [?] Finnaly, we need pass all these fields to  tag_set,  htcx, rdma_connections ?
 * 
 * 
 */
struct rmem_device_control {
	//int			     		fd;   // [?] Why do we need a file handler ??
	int			     						major; /* major number from kernel */
	//struct r_stat64		     stbuf; /* remote file stats*/
	//char			     						file_name[DEVICE_NAME_LEN];       // [?] Do we need a file name ??
	//struct list_head	     		list;        /* next node in list of struct IS_file */    // Why do we need such a list ? only one Swap Partition.
	struct gendisk		    	*disk;			// The disk information, logical/physical sectior size ? 
	struct request_queue	  *queue; 		// Queue controller/context, controll both the staging queue and dispatch queue.
	struct blk_mq_tag_set	  tag_set;		// Used for information passing. Define blk_mq_ops.(block_device_operations is defined in gendisk.)
	
	unsigned int		      	queue_depth;    // Number of the on-the-fly request for each dispatch queue. 
	unsigned int		      	nr_queues;		// Number of dispatch queue, pass to blk_mq_tag_set->nr_hw_queues ??
	//int			            	index; 							/* drive idx */
	char			            	dev_name[DEVICE_NAME_LEN];
	//struct config_group	     dev_cg;
	spinlock_t		        	rmem_ctl_lock;					// mutual exclusion
	//enum IS_dev_state	     state;	

	//
	//struct bio_list bio_list;

	uint8_t  					freed; 			// initial is 0, Block resource is freed or not. 

	//
	// RDMA related information
	//
	//[?] What's  this queue used for ?
  	//struct rmem_rdma_queue	  	*rdma_queues;			//  [?] The rdma connection session ?? one rdma session queue per software staging queue ??
	struct rdma_session_context	*rdma_session;				// RDMA connection context, one per Remote Memory Server.


	// Below fields are used for debug.
	struct block_device *bdev_raw;			// Points to a local block device. 


};





/**
 * Block device controller/context
 * Manage an exist logical partition. 
 * Used for semeru disk driver debuging.
 */
struct local_block_device {
    sector_t capacity; 
    struct gendisk *gd;
    spinlock_t lock;
    struct bio_list bio_list;  	// store the waiting processed bio.
    struct task_struct *thread;	// a seperate task_struct, a seperate task_struct->bio_list.
    int is_active;
    struct block_device *bdev_raw;
    struct request_queue *queue;        //[?] multiple queue ??
    atomic_t redirect_done;
};



static DECLARE_WAIT_QUEUE_HEAD(req_event);







/**
 * ########## Function declaration ##########
 * 
 * static : static function in C means that this function is only callable in this file.
 * 
 */



int 	rdma_session_connect(struct rdma_session_context *rdma_session_ptr);
int 	octopus_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event);

void 	semeru_cq_event_handler(struct ib_cq * cq, void *rdma_session_context);
int 	handle_recv_wr(struct semeru_rdma_queue *rdma_session, struct ib_wc *wc);
int 	send_message_to_remote(struct rdma_session_context *rdma_session, int rmda_queue_ind, int messge_type  , int size_gb);
void 	map_single_remote_memory_chunk(struct rdma_session_context *rdma_session);
int 	init_rdma_sessions(struct rdma_session_context *rdma_session);
int 	setup_rdma_session_commu_buffer(struct rdma_session_context * rdma_session);
//
// 1-sided RDMA message
//

// Control the number of outstanding wr.
int enqueue_send_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue * rdma_queue, struct rmem_rdma_command *rdma_cmd_ptr);

// Data-Path 
int 	dp_build_rdma_wr( struct rmem_rdma_command *rdma_cmd_ptr, struct request * io_rq, 
									struct remote_mapping_chunk *	remote_chunk_ptr , uint64_t offse_within_chunk, uint64_t len);

int		dp_post_rdma_write(struct rdma_session_context *rdma_session, struct request* io_rq, struct semeru_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len );

int 	dp_post_rdma_read(struct rdma_session_context *rdma_session, struct request* io_rq, struct semeru_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len );

int 	init_write_tag_rdma_command(struct rdma_session_context *rdma_session);
int dp_build_flag_byte_write(struct rdma_session_context *rdma_session,	struct remote_mapping_chunk* remote_chunk_ptr);


// Control-Path

int cp_build_rdma_wr(struct rdma_session_context *rdma_session, struct rmem_rdma_command *rdma_cmd_ptr, bool write_or_not,
									struct remote_mapping_chunk *	remote_chunk_ptr,  char ** addr_scan_ptr,  char* end_addr);
int cp_post_rdma_read(struct rdma_session_context *rdma_session, char __user * start_addr, uint64_t bytes_len );
int cp_post_rdma_write(struct rdma_session_context *rdma_session, char __user * start_addr, uint64_t bytes_len );

// Data-Path and Control-Path use the same receive functions.
int rdma_write_done(struct ib_wc *wc);
int rdma_read_done(struct ib_wc *wc);

int dp_rdma_read_done(struct rmem_rdma_command 	*rdma_cmd_ptr);
int cp_rdma_read_done(struct rmem_rdma_command 	*rdma_cmd_ptr);

int dp_rdma_write_done(struct rmem_rdma_command 	*rdma_cmd_ptr);
int cp_rdma_write_done(struct rmem_rdma_command 	*rdma_cmd_ptr);

uint64_t meta_data_map_sg(struct rdma_session_context *rdma_session,  struct scatterlist* sgl, 
													char  ** addr_scan_ptr, char* end_addr);


//inline void free_a_rdma_cmd_to_rdma_q(struct rmem_rdma_command* rdma_cmd_ptr);
//struct rmem_rdma_command* get_a_free_rdma_cmd_from_rdma_q(struct rmem_rdma_queue* rmda_q_ptr);


// Chunk management
int 	init_remote_chunk_list(struct rdma_session_context *rdma_session );
void 	bind_remote_memory_chunks(struct rdma_session_context *rdma_session );



// Transfer Block I/O to RDMA message
int 	transfer_requet_to_rdma_message(struct semeru_rdma_queue* rdma_queue, struct request * rq);
void 	copy_data_to_rdma_buf(struct request *io_rq, struct rmem_rdma_command *rdma_ptr);


//
// Block Device functions
//
int   rmem_init_disk_driver(struct rmem_device_control *rmem_dev_ctl);
int 	RMEM_create_device(char* dev_name, struct rmem_device_control* rmem_dev_ctrl );

//
// Resource free
//
void 	semeru_free_buffers(struct rdma_session_context *rdma_session);
void 	semeru_free_rdma_structure(struct rdma_session_context *rdma_session);
int 	semeru_disconenct_and_collect_resource(struct rdma_session_context *rdma_session);

int 	octopus_free_block_devicce(struct rmem_device_control * rmem_dev_ctl );



//
// Debug functions
//
char* rdma_message_print(int message_id);
char* rdma_session_context_state_print(int id);
char* rdma_cm_message_print(int cm_message_id);
char* rdma_wc_status_name(int wc_status_id);


void check_bio_basic_info(struct bio *bio_ptr, const char* message);
void check_io_request_basic_info(struct request *rq, const char* message);
uint32_t check_number_of_bio_in_request(struct request *io_rq, const char* message);
void 	print_io_request_physical_pages(struct request *io_rq, const char* message);
void 	print_scatterlist_info(struct scatterlist* sl_ptr , int nents );
void 	check_segment_address_of_request(struct request *io_rq, const char* message);
bool 	check_sector_and_page_size(struct request *io_rq, const char* message);

/** 
 * ########## Declare some global varibles ##########
 * 
 * There are 2 parts, RDMA parts and Disk Driver parts.
 * Some functions are stateless and they are only used for allocate and initialize the variables.
 * So, we need to keep some global variables and not not exit the Main function.
 * 
 * 	1). Data allocated by kzalloc, kmalloc etc. will not be freed by the kernel.  
 *  	1.1) If we don't want to use any data, we need to free them mannually.	
 * 		1.2) Fileds allocated by kzalloc and initiazed in the stateless functions will stay available.
 * 
 *  2) Do NOT define global variables in header, only declare extern variables in header and implement them in .c file.
 */

// Initialize in main().
// One rdma_session_context per memory server connected by IB.
extern struct rdma_session_context 	rdma_session_global;   // [!!] Unify the RDMA context and Disk Driver context global var [!!]
extern struct rmem_device_control  	rmem_dev_ctrl_global;
extern struct local_block_device		local_bd;


extern int rmem_major_num;
extern int online_cores;		// Both dispatch queues, rdma_queues equal to this online_cores.


//debug
extern u64	rmda_ops_count;
extern u64	cq_notify_count;
extern u64	cq_get_count;


#ifdef DEBUG_LATENCY_CLIENT
extern u32 * cycles_high_before; 
extern u32 * cycles_high_after;
extern u32 * cycles_low_before;
extern u32 * cycles_low_after;

extern u64 * total_cycles;
extern u64 * last_cycles;
#endif





//
// Control-Path RDMA communication functions
//

int 	init_rdma_control_path(struct rdma_session_context *rdma_session);
void	reset_rmem_rdma_cmd(struct rmem_rdma_command* rmem_rdma_cmd_ptr);






//
// Local block device control
//
#define uint64_from_ptr(p)    (uint64_t)(uintptr_t)(p)
#define ptr_from_uint64(p)    (void *)(unsigned long)(p)

int init_local_block_device(void);
void un_register_local_disk(void);
blk_qc_t local_bd_make_request(struct request_queue *q, struct bio *bio);
void forward_request_to_local_bd(struct request_queue *q, struct request *req);
void forwardee_local_bd_end_io(struct bio *bio, int err);


//
// Syscall 
//

int syscall_hello(int num);


char* semeru_rdma_read(int target_mem_server, char __user * start_addr, unsigned long size);
char* semeru_rdma_write(int target_mem_server, char __user * start_addr, unsigned long size);

// Get the pte_t value  of the user space virtual address.
// For the kernel space virtual address, allocated by kmalloc or kzalloc, user the virt_to_phys is good.
// 2) The scope for static functions in c is limited to the .cpp including it.
// If we put the static function declaration in header, each .cpp file include it needs to implement its own walk_page_table. 


// the strucute assigned to kernel.
struct semeru_rdma_ops{
	char* (*rdma_read)(int, char __user *, unsigned long);   // a function pointer, to  return value char*,  parameter(char*, unsigned long)
	char* (*rdma_write)(int, char __user *, unsigned long);
};



// a exported_symbol, defined in kernel.
// its parameter should be a function pointer, can we just declare it as void* ??
extern int rdma_ops_wrapper(void*);  


//
// Assign module defined function to kernel  
//

void init_kernel_semeru_rdma_ops(void);
void reset_kernel_semeru_rdma_ops(void);


extern int errno ;

#endif // REMOTEMEMPOOL


