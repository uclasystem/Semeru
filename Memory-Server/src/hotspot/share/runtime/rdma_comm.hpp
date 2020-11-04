#ifndef RDMA_COMM_H
#define RDMA_COMM_H

// Implementation-defined, search current directories first.
#include "logging/log.hpp"
#include "utilities/ostream.hpp"
#include "runtime/java.hpp"
#include "utilities/globalDefinitions.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/debug.hpp"


// Include standard libraries. Search the configured path first.
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <rdma/rdma_cma.h> 
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/wait.h>


//	Memory server is developed in user space, so use user-space IB API.
//	1)	"rdma/rdma_cma.h" is user space library, which is defined in /usr/include/rdma/rdma_cma.h.
//			"linux/rdma_cm.h" is kernel space library. 
//       cpp -v /dev/null -o /dev/null   print the search path.
//



// Enable the debug information.
//#define DEBUG_RDMA_SERVER 		1


// Used for describing RDMA QP status.
// [?] Does this matter ?
// This stauts is only used in function, handle_cqe(struct ibv_wc *wc). 
#define CQ_QP_BUSY 1
#define CQ_QP_IDLE 0
#define CQ_QP_DOWN 2



#define RDMA_QUEUE_NUM  16  // Larger or equal to the online core of CPU server


/**
 * This memory server support multiple QP for each CPU.
 * 
 */
struct semeru_rdma_queue {
  // RDMA client ID, one for per QP.
	struct rdma_cm_id *cm_id;		//  ? bind to QP

	// ib events 
  struct ibv_cq *cq;			// Completion queue
	struct ibv_qp *qp;			// Queue Pair

	//enum rdma_queue_state state;  // the current status of the QP.
	//wait_queue_head_t 		sem;    // semaphore for wait/wakeup
	uint8_t  	connected;			// some function can only be called once, this is the flag to record this.

	int q_index;
	struct context		*rdma_session;			// Record the RDMA session this queue belongs to.
};


struct semeru_rdma_dev {
  struct ibv_context *ctx;  // Memory server only needs one CQ, use the CQ of rdma_queue[0].
  struct ibv_pd *pd;
};




/**
 * Status of Region
 * 
 *  -1 : Not bind to CPU server.
 */
enum region_status{
  EMPTY,			// 0, No need to scan this region.
	CACHED,
  EVICTED
};

  enum server_states{
    S_WAIT,
    S_BIND,
    S_DONE
  };

  enum send_states{
    SS_INIT,
    SS_MR_SENT, 
    SS_STOP_SENT,
    SS_DONE_SENT
  };

  enum recv_states{
    RS_INIT,
    RS_STOPPED_RECV,
    RS_DONE_RECV
  };

  // 2-sided RDMA message type
  // Used to communicate with cpu servers.
	enum message_type{
		DONE = 1,				      // Start from 1
		SEND_CHUNKS,				  // send the remote_addr/rkey of multiple Chunks. Used for send the extended Regions.
		SEND_SINGLE_CHUNK,		// send the remote_addr/rkey of a single Chunk. Useless now.
		FREE_SIZE,						// Send free size information && the registered whole Java heap to remote CPU server.
		EVICT,        			  // 5

		ACTIVITY,				      // 6 Debug item, used as "End of STW Window "
		STOP,					        // 7, upper SIGNALs are used by server, below SIGNALs are used by client.
		REQUEST_CHUNKS,
		REQUEST_SINGLE_CHUNK,	// Send a request to ask for a single chunk.
		QUERY,         			  // 10
    
    AVAILABLE_TO_QUERY    // This memory server is oneline to server.

	};

/**
 * RDMA command line attached to each RDMA message.
 * 
 */
struct message {
	// Information of the chunk to be mapped to remote memory server.
	uint64_t buf[MAX_REGION_NUM];		      // Remote addr, usd by clinet for RDMA read/write.
  uint64_t mapped_size[MAX_REGION_NUM]; // For a single Region, Maybe not fully mapped
  uint32_t rkey[MAX_REGION_NUM];   	    // remote key
  int mapped_chunk;											// Chunk number in current message. 

  enum message_type type;
};

/**
 *	RDMA conection context.
 *  
 *  [?] Maintain a daemon thread to poll the RDMA message
 * 
 */
struct context {

  struct semeru_rdma_dev * rdma_dev;
  struct semeru_rdma_queue * rdma_queues;


  struct ibv_comp_channel *comp_channel;
  pthread_t cq_poller_thread;  // Deamon thread to handle the 2-sided RDMA messages.


  // 2) Reserve wr for 2-sided RDMA communications
  //    
  struct message *recv_msg;				// RDMA commandline attached to each RDMA request.
	struct ibv_mr *recv_mr;       	// Need to register recv_msg as RDMA MR, then RDMA device can read/write it.

  struct message *send_msg;
  struct ibv_mr *send_mr;


  // 3) Used for 1-sided RDMA communications
  //
  struct rdma_mem_pool* mem_pool;  // Manage the whole heap and Region, RDMA_MR information.
  int connected;    // global connection state, if any QP is connected to CPU server.

	// In our design, the memory pool on Memory server will be exited also.
	// Can't see benefits for reusing the Memory pool ? It also causes privacy problems. 
	//
  server_states server_state;

};


/**
 * Describe the memory pool
 *  Start address;
 * 	Size;
 * 
 * 	Start address of each Region.
 * 	The RDMA MR descripor for each Region. 
 * 
 * More explanation
 * 		Registering RDMA buffer at Region status is good for dynamic scaling. 
 * 		The Java heap can expand at Region granularity.
 * 
 */
struct rdma_mem_pool{
	char*	  Java_heap_start;									// Start address of Java heap.
	int		  region_num; 											// Number of Regions. Regions size is defined by Macro : CHUNK_SIZE_GB * ONE_GB.

	struct ibv_mr*  Java_heap_mr[MAX_FREE_MEM_GB];	// Register whole Java heap as RDMA buffer.
  char*	  region_list[MAX_FREE_MEM_GB];       		// Start address of each Region. region_list[0] == Java_start.
  size_t  region_mapped_size[MAX_FREE_MEM_GB];    // The byte size of the corresponding Region. Count at bytes.
  int		  cache_status[MAX_FREE_MEM_GB];					// -1 NOT bind with CPU server. Or check the value of region_status.
};


/**
 * Used for passing multiple parameters
 */
struct rdma_main_thread_args {
	char*		heap_start;
  size_t	heap_size;
};




/**
 * Define tools
 * 
 */

void die(const char *reason);


#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
        (unsigned int)ntohl(((int)(x >> 32))))

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)	// ERROR if NON-NULL.
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)  // ERROR if NULL





/**
 * Declare functions
 */
void* Build_rdma_to_cpu_server(void* _args );
int 	on_cm_event(struct rdma_cm_event *event);
int 	on_connect_request(struct rdma_cm_id *id);
int 	rdma_connected(struct semeru_rdma_queue* rdma_queue);
int 	on_disconnect(struct semeru_rdma_queue * rdma_queue);

void  build_connection(struct semeru_rdma_queue* rdma_queue);
void  build_params(struct rdma_conn_param *params);
void  get_device_info(struct semeru_rdma_queue * rdma_queue);
void  build_qp_attr(struct semeru_rdma_queue * rdma_queue, struct ibv_qp_init_attr *qp_attr);
void  handle_cqe(struct ibv_wc *wc);

void  inform_memory_pool_available(struct semeru_rdma_queue * rdma_queue);
void  send_free_mem_size(struct semeru_rdma_queue* rdma_queue);
void  send_regions(struct semeru_rdma_queue* rdma_queue);
void  send_message(struct semeru_rdma_queue * rdma_queue);

void 	destroy_connection(struct context * rdma_session);
void*	poll_cq(void *ctx);
void 	post_receives(struct semeru_rdma_queue * rdma_queue);

void 	init_memory_pool(char* heap_start, size_t heap_size, struct context * rdma_ctx );
void 	register_rdma_comm_buffer(struct context *rdma_ctx);


/**
 * Global variables
 *
 * More Explanations
 * 
 *  "static" : For both C and C++, using "static" before a global variable will limit its usage scope, the defined .cpp file. 
 *             For example,  "static struct context * global_rdma_ctx" in header, if multiple .cpp include this header,
 *             each .cpp has a local copy of variable, struct context* global_rdma_ctx. There is no conflict at all.
 * 
 *             For "struct context * global_rdma_ctx" in header, and multiple .cpp include this header,
 *             then each .cpp has a global variable, struct context *global_rdma_ctx, with global usage scope.
 *             This will cause "Multiple definitions issue". We need to use "extern".
 *              
 *              Warning : static in Class, funtion means a "single version" and "duration" variable with the same lifetime of the program. 
 * 
 * "extern" : linkage. Only one instance of this global variable and it's defined in some source file.
 * 
 */
extern struct context *global_rdma_ctx;					// The RDMA controll context.
extern int rdma_queue_count;
//extern struct rdma_mem_pool* global_mem_pool;

extern int errno ;

#endif