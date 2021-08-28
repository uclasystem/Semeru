/**
 *  1）Define operations for frontswap
 *  2) Define the RDMA operations for frontswap path
 * 
 */

#ifndef __SEMERU_FRONTSWAP_H
#define __SEMERU_FRONTSWAP_H

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>

// Swap
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/swapfile.h>
#include <linux/swap.h>
#include <linux/frontswap.h>

// For infiniband
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/pci-dma.h>
#include <linux/pci.h> // Use the dma_addr_t defined in types.h as the DMA/BUS address.
#include <linux/inet.h>
#include <linux/lightnvm.h>
#include <linux/sed-opal.h>

// Utilities
#include <linux/log2.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <asm/uaccess.h> // copy data from kernel space to user space
#include <linux/slab.h> // kmem_cache
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/page-flags.h>
#include <linux/smp.h>

// Semeru
#include <linux/swap_global_struct_bd_layer.h>
#include <linux/swap_global_struct_mem_layer.h>

//
// ####################### variable declaration #######################
//

/**
 * Used for message passing control
 * For both CM event, data evetn.
 * RDMA data transfer is desinged in an asynchronous style. 
 */
enum rdma_queue_state {
	IDLE = 1, // 1, Start from 1.
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED, // 5,  updated by IS_cma_event_handler()

	MEMORY_SERVER_AVAILABLE, // 6

	FREE_MEM_RECV, // After query, we know the available regions on memory server.
	RECEIVED_CHUNKS, // get chunks from remote memory server
	RDMA_BUF_ADV, // designed for server
	WAIT_OPS,
	RECV_STOP, // 11

	RECV_EVICT,
	RDMA_WRITE_RUNNING,
	RDMA_READ_RUNNING,
	SEND_DONE,
	RDMA_DONE, // 16

	RDMA_READ_ADV, // updated by IS_cq_event_handler()
	RDMA_WRITE_ADV,
	CM_DISCONNECT,
	ERROR,
	TEST_DONE, // 21, for debug
};

// 2-sided RDMA message type
// Used to communicate with cpu servers.
enum message_type {
	DONE = 1, // Start from 1
	GOT_CHUNKS, // Get the remote_addr/rkey of multiple Chunks
	GOT_SINGLE_CHUNK, // Get the remote_addr/rkey of a single Chunk
	FREE_SIZE, //
	EVICT, // 5

	ACTIVITY, // 6
	STOP, //7, upper SIGNALs are used by server, below SIGNALs are used by client.
	REQUEST_CHUNKS,
	REQUEST_SINGLE_CHUNK, // Send a request to ask for a single chunk.
	QUERY, // 10

	AVAILABLE_TO_QUERY // 11 This memory server is oneline to server.
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
	uint64_t buf[MAX_REGION_NUM]; // Remote addr.
	uint64_t mapped_size[MAX_REGION_NUM]; // Maybe not fully mapped.
	uint32_t rkey[MAX_REGION_NUM]; // remote key
	int mapped_chunk; // Chunk number in current message.

	enum message_type type;
};

// The semeru_rdma_req_sg type
enum rdma_seq_type {
	CONTROL_PATH_MEG, //0
	DATA_PATH_MEG,
	END_TYPE // end flag
};

/**
 * Need to update to Region status.
 */
enum chunk_mapping_state {
	EMPTY, // 0
	MAPPED, // 1, Cached ?
};

enum region_status {
	NEWLY_ALLOCATED, // 0, newly allocated ?
	CACHED, // Partial or Fully cached Regions.
	EVICTED // 2, clearly evicted to Memory Server
};

// Meta Region/chunk: a contiguous chunk of a Region, may be not fully mapped.
// Data Region/chunk: Default mapping size is REGION_SIZE_GB, 4 GB default.
// RDMA mapping size isn't the Java Region size.
//
struct remote_mapping_chunk {
	uint32_t remote_rkey; // RKEY of the remote mapped chunk
	uint64_t remote_addr; // Virtual address of remote mapped chunk
	uint64_t mapped_size; // For some specific Chunk, we may only map a contigunous range.
	enum chunk_mapping_state chunk_state;
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
	uint32_t remote_free_size; // total mapped byte size. Accumulated each remote_mapping_chunk[i]->mapped_size
	uint32_t chunk_num; // length of remote_chunk list
	uint32_t chunk_ptr; // points to first empty chunk.
};

/**
 * 1-sided RDMA (read/write) message.
 * Both Semeru Control Path(CP) and Data Path(DP) use this rdma command structu.
 * 
 * Reserve a kcache for semeru_rdma_req_sg allocation.
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
struct semeru_rdma_req_sg {
	struct semeru_rdma_queue *rdma_queue;

	enum rdma_seq_type seq_type;
	struct ib_cqe cqe; // used for completion function
	struct ib_sge sge_list[MAX_REQUEST_SGL]; // scatter-gather entry for 1-sided RDMA read/write WR.
	struct ib_rdma_wr rdma_sq_wr; // wr for 1-sided RDMA write/send.
	struct completion done; // spinlock. caller wait on it.

	// Scatter-Gather
	// scatterlist  - this works like sge_table.
	// [!!] The scatterlist is temporaty data, we can put it in the behind of i/o request and not send them to remote server.
	// points to the physical pages of i/o requset.
	u64 nentry; // number of the segments, usually one pysical page per segment
	struct scatterlist sgl[MAX_REQUEST_SGL]; // store the dma address
};

/**
 * 1-sided RDMA WR for frontswap path.
 * only support sinlge page for now.
 * 
 */
struct fs_rdma_req {
	struct ib_cqe cqe; // CQE complete function
	struct page *page; // [?] Make it support multiple pages
	u64 dma_addr; // dma address for the page
	struct ib_sge sge; // points to local data
	struct ib_rdma_wr rdma_wr; // wr for 1-sided RDMA write/send.

	struct completion done; // spinlock. caller wait on it.
	struct semeru_rdma_queue *rdma_queue; // which rdma_queue is enqueued.
};

struct two_sided_rdma_send {
	struct ib_cqe cqe; // CQE complete function
	struct ib_send_wr sq_wr; // send queue wr
	struct ib_sge send_sgl; // attach the rdma buffer to sg entry. scatter-gather entry number is limitted by IB hardware.
	struct message *send_buf; // the rdma buffer.
	u64 send_dma_addr;

	struct semeru_rdma_queue *rdma_queue; // which rdma_queue is enqueued.
};

struct two_sided_rdma_recv {
	struct ib_cqe cqe; // CQE complete function
	struct ib_recv_wr rq_wr; // receive queue wr
	struct ib_sge recv_sgl; // recv single SGE entry
	struct message *recv_buf;
	u64 recv_dma_addr;

	struct semeru_rdma_queue *rdma_queue; // which rdma_queue is enqueued.
};

/**
 * Build a QP for each core on cpu server. 
 * [?] This design assume we only have one memory server.
 * 			Extern this design later.
 * 
 */
struct semeru_rdma_queue {
	// RDMA client ID, one for per QP.
	struct rdma_cm_id *cm_id; //  ? bind to QP

	// ib events
	struct ib_cq *cq; // Completion queue
	struct ib_qp *qp; // Queue Pair

	enum rdma_queue_state state; // the current status of the QP.
	wait_queue_head_t sem; // semaphore for wait/wakeup
	spinlock_t cq_lock; // used for CQ

	// some function can only be called once, this is the flag to record this.
	// 0 : not freed
	// 1 : client requests for disconnection
	// 255 : One of memory server crashed, start disconnecting from all memory servers.
	uint8_t freed; // are resource freed
	atomic_t rdma_post_counter;

	int q_index; // initialized to disk hardware queue index
	struct rdma_session_context *rdma_session; // Record the RDMA session this queue belongs to.

	// cache for fs_rdma_request. One for each rdma_queue
	struct kmem_cache *fs_rdma_req_cache; // only for fs_rdma_req ?
	struct kmem_cache *rdma_req_sg_cache; // used for rdma request with scatter/gather
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
 * [x] One session per server.
 * 
 */
struct rdma_session_context {
	// 1) RDMA QP/CQ management
	struct semeru_rdma_dev *rdma_dev; // The RDMA device of cpu server.
	struct semeru_rdma_queue *rdma_queues; // point to multiple QP

	// For infiniband connection rdma_cm operation
	int mem_server_id;
	// The first region is reserved for meta data. data region start from 1.
	int data_region_start_id; 
	int data_region_num;

	uint16_t port; /* dst port in NBO */
	u8 addr[16]; /* dst addr in NBO */
	uint8_t addr_type; /* ADDR_FAMILY - IPv4/V6 */
	int send_queue_depth; // Send queue depth. Both 1-sided/2-sided RDMA wr is limited by this number.
	int recv_queue_depth; // Receive Queue depth. 2-sided RDMA need to post a recv wr.

	//
	// 2) 2-sided RDMA section.
	//		This section is used for RDMA connection and basic information exchange with remote memory server.
	struct two_sided_rdma_recv rdma_recv_req;
	struct two_sided_rdma_send rdma_send_req;

	// 3) For 1-sided RDMA read/write
	// I/O request --> 1 sided RDMA message.
	// rmem_rdma_queue store these reserved 1-sided RDMA wr information.

	// 3.1) Data-Path, used for swaping mechanism

	// 3.2) Control-Path, used for user-space invocation.  --> integrated into rdma_queue
	//struct semeru_rdma_req_sg				*cp_rmem_rdma_read_cmd;
	//struct semeru_rdma_req_sg				*cp_rmem_rdma_write_cmd;
	// Move into the rdma_queue
	//atomic_t rdma_post_counter; // Control the outstanding wr number. Less than RDMA_SEND_QUEUE_DEPTH.

	// 4) manage the CHUNK mapping.
	struct remote_mapping_chunk_list remote_chunk_list;

	// Keep a rdma buffer for flag byte specially
	// Fill these information into a 1-sided ib_rdma_wr
	// Write the value 1 to the corresponding Region's flag .
	// The flag is 4 bytes and allocated in native space.
	uint32_t *write_tag;
	uint64_t write_tag_dma_addr; // corresponding dma address, just the physical address.
	struct semeru_rdma_req_sg *write_tag_rdma_cmd;
};


//
// ###################### address translation related #######################
//

/**
 * @brief Describe the data address in
 * 	the memory servers.
 */
struct mem_server_addr{
	int mem_server_id;
	size_t mem_server_chunk_index;
	size_t mem_server_offset_within_chunk;
};





//
// ###################### function declaration #######################
//

// functions for RDMA connection



int semeru_fs_rdma_client_init(void);
void semeru_fs_rdma_client_exit(void);
int init_rdma_sessions(struct rdma_session_context **rdma_session_global_ptr_addr);
int init_rdma_session(struct rdma_session_context *rdma_session );
int rdma_sessions_connect(struct rdma_session_context *rdma_session_global_ptr);
int rdma_session_connect(struct rdma_session_context *rdma_session);
int semeru_init_rdma_queue(struct rdma_session_context *rdma_session, int cpu);
int semeru_create_rdma_queue(struct rdma_session_context *rdma_session, int rdma_queue_index);
int semeru_connect_remote_memory_server(struct rdma_session_context *rdma_session, int rdma_queue_inx);
int semeru_query_available_memory(struct rdma_session_context *rdma_session);
int setup_rdma_session_commu_buffer(struct rdma_session_context *rdma_session);
int semeru_setup_buffers(struct rdma_session_context *rdma_session);

int init_remote_chunk_list(struct rdma_session_context *rdma_session);
void bind_remote_memory_chunks(struct rdma_session_context *rdma_session);


int semeru_disconnect_mem_servers(struct rdma_session_context *rdma_session_global);
int semeru_disconenct_and_collect_resource(struct rdma_session_context *rdma_session);
void semeru_free_buffers(struct rdma_session_context *rdma_session);
void semeru_free_rdma_structure(struct rdma_session_context *rdma_session);

// functions for 2-sided RDMA.
void two_sided_message_done(struct ib_cq *cq, struct ib_wc *wc);
int handle_recv_wr(struct semeru_rdma_queue *rdma_queue, struct ib_wc *wc);
int send_message_to_remote(struct rdma_session_context *rdma_session, int rdma_queue_ind, int messge_type,
			   int chunk_num);

// functions for 1-sided RDMA
int init_write_tag_rdma_command(struct rdma_session_context *rdma_session);

// functions for frontswap
int semeru_init_frontswap(void);
void semeru_exit_frontswap(void);
void translate_to_mem_server_addr(struct mem_server_addr * mem_addr, pgoff_t swap_entry_offset);
int semeru_frontswap_store(unsigned type, pgoff_t page_offset, struct page *page);
int semeru_frontswap_load(unsigned type, pgoff_t page_offset, struct page *page);

int semeru_fs_rdma_send(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue,
			struct fs_rdma_req *rdma_req, struct remote_mapping_chunk *remote_chunk_ptr,
			size_t offset_within_chunk, struct page *page, enum dma_data_direction dir);

int dp_build_fs_rdma_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue,
			struct fs_rdma_req *rdma_req, struct remote_mapping_chunk *remote_chunk_ptr,
			size_t offset_within_chunk, struct page *page, enum dma_data_direction dir);

int fs_enqueue_send_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue,
		       struct fs_rdma_req *rdma_req);
void fs_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc);
void fs_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc);

void drain_rdma_queue(struct semeru_rdma_queue *rdma_queue);
void drain_all_rdma_queue(int target_mem_server);

//
// control path

char *semeru_cp_rdma_read(int target_mem_server, char __user *start_addr, unsigned long size);
char *semeru_cp_rdma_write(int target_mem_server, int write_type, char __user *start_addr, unsigned long size);

int semeru_cp_rdma_send(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue,
			struct semeru_rdma_req_sg *rdma_req_sg, char __user *start_addr, uint64_t bytes_len,
			enum dma_data_direction dir);
int cp_build_rdma_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_req_sg *rdma_cmd_ptr,
		     enum dma_data_direction dir, struct remote_mapping_chunk *remote_chunk_ptr, char **addr_scan_ptr,
		     char *end_addr);
uint64_t meta_data_map_sg(struct rdma_session_context *rdma_session, struct scatterlist *sgl, char **addr_scan_ptr,
			  char *end_addr);
int cp_enqueue_send_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue,
		       struct semeru_rdma_req_sg *rdma_req);
void cp_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc);
void cp_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc);
void reset_semeru_rdma_req_sg(struct semeru_rdma_req_sg *rmem_rdma_cmd_ptr);
// Get the pte_t value  of the user space virtual address.
// For the kernel space virtual address, allocated by kmalloc or kzalloc, user the virt_to_phys is good.
// 2) The scope for static functions in c is limited to the .cpp including it.
// If we put the static function declaration in header, each .cpp file include it needs to implement its own walk_page_table.

// the strucute assigned to kernel.
struct semeru_rdma_ops {
	char *(*rdma_read)(
		int, char __user *,
		unsigned long); // a function pointer, to  return value char*,  parameter(char*, unsigned long)
	char *(*rdma_write)(int, int, char __user *,
			    unsigned long); // (2nd int -> message type. 0 for data, 1 for signal )
};

// a exported_symbol, defined in kernel.
// its parameter should be a function pointer, can we just declare it as void* ??
extern int rdma_ops_wrapper(void *);

//
// Assign module defined function to kernel
//

void init_kernel_semeru_rdma_ops(void);
void reset_kernel_semeru_rdma_ops(void);

//
// Debug functions
//
char *rdma_message_print(int message_id);
char *rdma_session_context_state_print(int id);
char *rdma_cm_message_print(int cm_message_id);
char *rdma_wc_status_name(int wc_status_id);

void print_scatterlist_info(struct scatterlist *sl_ptr, int nents);

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
// [!!] Unify the RDMA context and Disk Driver context global var [!!]
extern struct rdma_session_context * rdma_session_global_ptr; 

extern int rmem_major_num;
extern int online_cores; // Both dispatch queues, rdma_queues equal to this online_cores.

//debug
extern u64 rmda_ops_count;
extern u64 cq_notify_count;
extern u64 cq_get_count;

#ifdef DEBUG_LATENCY_CLIENT
extern u32 *cycles_high_before;
extern u32 *cycles_high_after;
extern u32 *cycles_low_before;
extern u32 *cycles_low_after;

extern u64 *total_cycles;
extern u64 *last_cycles;
#endif

#endif // SEMERU_FRONTSWAP