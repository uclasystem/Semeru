/**
 * frontswap_rdma is the RDMA component for frontswap path.
 * Its RDMA queues are different from the swap version.
 * 
 * 
 * 1. Global variables
 * 
 * 2. Structure for RDMA connection
 * 	One session for each Memory server.
 * 		
 * 
 * 	2.1 Structure of RDMA session
 * 			1 RDMA queue for Data/Control path
 * 
 * 	2.2	Structure of RDMA queue
 * 			1 cm_id
 * 			1 QP
 * 			1 IB_POLL_DIRECT CQ for control/data path
 * 			
 * 	2.3 There are 3 types of communication here
 * 			a. Data Path. Triggered by swap
 * 					=> all the rdma_queues[]
 * 			b. Control Path. Invoked by User space.
 * 					=> rdma_queue[0]
 * 			c. Exchange meta data with memory server. 2-sided RDMA and only used during initializaiton.
 * 					=> rdma_queue[0]
 * 
 */


// Semeru
#include "frontswap_path.h"
#include <linux/swap_global_struct_mem_layer.h>


//
// Implement the global vatiables here
//

struct rdma_session_context 	rdma_session_global;
int online_cores;	// Control the parallelism 

//debug
u64	rmda_ops_count	= 0;
u64	cq_notify_count	= 0;
u64	cq_get_count	= 0;



// 
// static functions
//




/**
 * Declared in swap_global_struct_bd_layer.h
 * 
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
static pte_t *walk_page_table(struct mm_struct *mm, uint64_t addr){
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








//
// >>>>>>>>>>>>>>>>>>>>>>  Start of handle  TWO-SIDED RDMA message section >>>>>>>>>>>>>>>>>>>>>>
//


/**
 * Received the CQE for the posted recv_wr
 *  
 */
void two_sided_message_done(struct ib_cq *cq, struct ib_wc *wc){
	struct semeru_rdma_queue 	*rdma_queue		=	cq->cq_context;
	int ret = 0;


	// 1) check the wc status
	if (wc->status != IB_WC_SUCCESS) {
		printk(KERN_ERR "%s, cq completion failed with wr_id 0x%llx status %d,  status name %s, opcode %d,\n",__func__,
										wc->wr_id, wc->status, rdma_wc_status_name(wc->status), wc->opcode);
			
		goto out;
	}

	// 2) process the received message
	switch (wc->opcode){

		case IB_WC_RECV:				
			// Recieve 2-sided RDMA recive wr
			printk("%s, Got a WC from CQ, IB_WC_RECV. \n", __func__);

			// Need to do actions based on the received message type.
			ret = handle_recv_wr(rdma_queue, wc);
			if (unlikely(ret)) {
				printk(KERN_ERR "%s, recv wc error: %d\n", __func__, ret);
				goto out;
			}
			break;
		
		case IB_WC_SEND:

			printk("%s, Got a WC from CQ, IB_WC_SEND. 2-sided RDMA post done. \n", __func__);
			break;


		default:
				printk(KERN_ERR "%s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc->opcode);
				goto out;
	} // switch


	//decrease the counter
	atomic_dec(&rdma_queue->rdma_post_counter);

out:
	return;
}





/**
 * Send a RDMA message to remote server.
 * Used for RDMA conenction build.
 * 
 */
int send_message_to_remote(struct rdma_session_context *rdma_session, int rdma_queue_ind , int messge_type  , int chunk_num)
{
	int ret = 0;
	struct ib_recv_wr * recv_bad_wr;
	struct ib_send_wr * send_bad_wr;
	struct semeru_rdma_queue *rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_ind]);
	rdma_session->rdma_send_req.send_buf->type = messge_type;
	rdma_session->rdma_send_req.send_buf->mapped_chunk = chunk_num; 		// 1 Meta , N-1 Data Regions


	// post a 2-sided RDMA recv wr first.
	ret = ib_post_recv(rdma_queue->qp, &rdma_session->rdma_recv_req.rq_wr, &recv_bad_wr);
	if(ret) {
		printk(KERN_ERR "%s, Post 2-sided message to receive data failed.\n", __func__);
		goto err;
	}
	atomic_inc(&rdma_queue->rdma_post_counter);


	#ifdef DEBUG_MODE_BRIEF
	printk("Send a Message to memory server. send_buf->type : %d, %s \n", messge_type, rdma_message_print(messge_type) );
	#endif

	ret = ib_post_send(rdma_queue->qp, &rdma_session->rdma_send_req.sq_wr, &send_bad_wr);
	if (ret) {
		printk(KERN_ERR "%s: BIND_SINGLE MSG send error %d\n", __func__, ret);
		goto err;
	}
	atomic_inc(&rdma_queue->rdma_post_counter);
	// #ifdef DEBUG_MODE_BRIEF
	// else{
	// 	printk("%s: 2-sided RDMA message[%llu] send. \n",__func__,rmda_ops_count++);
	// }
	// #endif

err:
	return ret;	
}





/**
 * Receive a WC, IB_WC_RECV.
 * Read the data from the posted WR.
 * 		For this WR, its associated DMA buffer is rdma_session_context->recv_buf.
 * 
 * Action 
 * 		According to the RDMA message information, rdma_session_context->state, to set some fields.
 * 		FREE_SIZE : set the 
 * 
 * More Explanation
 * 		For the 2-sided RDMA, the receiver even can not responds any message back ?
 * 		The character of 2-sided RDMA communication is just to send a interrupt to receiver's CPU.
 * 
 */
int handle_recv_wr(struct semeru_rdma_queue *rdma_queue, struct ib_wc *wc){
	int ret = 0;
	struct rdma_session_context *rdma_session = rdma_queue->rdma_session;


	if ( wc->byte_len != sizeof(struct message) ) {         // Check the length of received message
		printk(KERN_ERR "%s, Received bogus data, size %d\n", __func__,  wc->byte_len);
		ret = -1;
		goto out;
	}	

	#ifdef DEBUG_MODE_BRIEF
	// Is this check necessary ??
	if (unlikely(rdma_queue->state < CONNECTED) ) {
		printk(KERN_ERR "%s, RDMA is not connected\n", __func__);	
		return -1;
	}
	#endif



	switch(rdma_session->rdma_recv_req.recv_buf->type){

		case AVAILABLE_TO_QUERY:
			#ifdef DEBUG_MODE_BRIEF
			printk( "%s, Received AVAILABLE_TO_QERY, memory server is prepared well. We can qu\n ", __func__);
			#endif

			rdma_queue->state = MEMORY_SERVER_AVAILABLE;

			wake_up_interruptible(&rdma_queue->sem);
			break;

		case FREE_SIZE:
	
			// get the number of Free Regions. 
			printk(KERN_INFO "%s, Received FREE_SIZE, avaible chunk number : %d \n ", __func__,	rdma_session->rdma_recv_req.recv_buf->mapped_chunk );

			rdma_session->remote_chunk_list.chunk_num = rdma_session->rdma_recv_req.recv_buf->mapped_chunk;
			rdma_queue->state = FREE_MEM_RECV;	
			
			ret = init_remote_chunk_list(rdma_session);
			if(unlikely(ret)){
				printk(KERN_ERR "Initialize the remote chunk failed. \n");
			}

			wake_up_interruptible(&rdma_queue->sem);

			break;
		case GOT_CHUNKS:

			// Got memory chunks from remote memory server, do contiguous mapping.
			bind_remote_memory_chunks(rdma_session);

			// Received free chunks from remote memory,
			// Wakeup the waiting main thread and continure.
			rdma_queue->state = RECEIVED_CHUNKS;
			wake_up_interruptible(&rdma_queue->sem);  // Finish main function.

			break;

		default:
			printk(KERN_ERR "%s, Recieved WRONG RDMA message %d \n", __func__, rdma_session->rdma_recv_req.recv_buf->type);
			ret = -1;
			goto out; 	
	}

out:
	return ret;
}


/**
 * Check the available memory size on this session/memory server
 * All the QPs of the session share the same memory, request for any QP is good.
 * 
 */
int semeru_query_available_memory(struct rdma_session_context* rdma_session){
	
	int ret = 0;
	struct semeru_rdma_queue * rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[online_cores-1]);


	//wait untile all the rdma_queue are connected.
	// wait for the last rdma_queue[online_cores-1] get MEMORY_SERVER_AVAILABLE
	wait_event_interruptible( rdma_queue->sem, rdma_queue->state == MEMORY_SERVER_AVAILABLE );
	printk(KERN_INFO "%s, All %d rdma_queues are prepared well. Query its available memory. \n",__func__, online_cores);

	// And then use the rdma_queue[0] for communication.
	// Post a 2-sided RDMA to query memory server's available memory.
	rdma_queue =  &(rdma_session->rdma_queues[0]);
	ret = send_message_to_remote(rdma_session, 0, QUERY, 0); // for QERY, chunk_num = 0
	if(ret) {
		printk(KERN_ERR "%s, Post 2-sided message to remote server failed.\n", __func__);
		goto err;
	}

	// recive 2-sided wr.
	drain_rdma_queue(rdma_queue);


	// Sequence controll
	wait_event_interruptible( rdma_queue->sem, rdma_queue->state == FREE_MEM_RECV );

	printk("%s: Got %d free memory chunks from remote memory server. Request for Chunks \n",
																																	__func__, 
																																	rdma_session->remote_chunk_list.chunk_num);

err:
	return ret;
}



/**
 *  Post a 2-sided request for chunk mapping.
 * 
 * 
 * 
 * More Explanation
 * 	We can only post a cqe notification per time, OR these cqe notifications may overlap and lost.
 * 	So we post the cqe notification in cq_event_handler function.
 * 
 */
int semeru_requset_for_chunk(struct rdma_session_context* rdma_session, int num_chunk){
	int ret = 0;
	struct semeru_rdma_queue * rdma_queue;


	rdma_queue = &(rdma_session->rdma_queues[0]); 	// We can request the registerred memory from any QP of the target memory server.


	if(num_chunk == 0 || rdma_session == NULL){
		printk(KERN_ERR "%s, current memory server has no available memory at all. Exit. \n", __func__);
		goto err;
	}

	// Post a 2-sided RDMA to requst all the available regions from current memory server.
	ret = send_message_to_remote(rdma_session, 0, REQUEST_CHUNKS, num_chunk );
	if(ret) {
		printk(KERN_ERR "%s, Post 2-sided message to remote server failed.\n", __func__);
		goto err;
	}

	drain_rdma_queue(rdma_queue);

	// Sequence controll
	wait_event_interruptible( rdma_queue->sem, rdma_queue->state == RECEIVED_CHUNKS );

	printk(KERN_INFO "%s, Got %d chunks from memory server.\n",__func__, num_chunk);

	err:
	return ret;
}




//
// <<<<<<<<<<<<<<  End of handling TWO-SIDED RDMA message section <<<<<<<<<<<<<<
//








//
// ############# Start of RDMA Communication (CM) event handler ########################
//
// [?] All the RDMA Communication(CM) event is triggered by hardware and mellanox driver.
//		No need to maintain a daemon thread to handle the CM events ?  -> stateless handler 
//


/**
 * The rdma CM event handler function
 * 
 * [?] Seems that this function is triggered when a CM even arrives this device. 
 * 
 * More Explanation
 * 	 CMA Event handler  && cq_event_handler , 2 different functions for CM event and normal RDMA message handling.
 * 
 */
int semeru_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret;
	struct semeru_rdma_queue *rdma_queue = cma_id->context;


	#ifdef DEBUG_MODE_BRIEF
	pr_info("cma_event type %d, type_name: %s \n", event->event, rdma_cm_message_print(event->event));
	#endif

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rdma_queue->state = ADDR_RESOLVED;

		#ifdef DEBUG_MODE_BRIEF
		printk("%s,  get RDMA_CM_EVENT_ADDR_RESOLVED. Send RDMA_ROUTE_RESOLVE to Memory server \n",__func__);
		#endif 

		// Go to next step directly, resolve the rdma route.
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "%s,rdma_resolve_route error %d\n", __func__, ret);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:

		#ifdef DEBUG_MODE_BRIEF
		// RDMA route is solved, wake up the main process  to continue.
    	printk("%s : RDMA_CM_EVENT_ROUTE_RESOLVED, wake up rdma_queue->sem\n ",__func__);
		#endif	

		// Sequencial controll 
		rdma_queue->state = ROUTE_RESOLVED;
		wake_up_interruptible(&rdma_queue->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:		// Receive RDMA connection request

    printk("Receive but Not Handle : RDMA_CM_EVENT_CONNECT_REQUEST \n");
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
	  printk("%s, ESTABLISHED, wake up rdma_queue->sem\n", __func__);

		rdma_queue->state = CONNECTED;
    wake_up_interruptible(&rdma_queue->sem);		

		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR "%s, cma event %d, event name %s, error code %d \n", __func__, event->event,
														rdma_cm_message_print(event->event), event->status);
		rdma_queue->state = ERROR;
		wake_up_interruptible(&rdma_queue->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:	//should get error msg from here
		printk( "%s, Receive DISCONNECTED  signal \n",__func__);

		if(rdma_queue->freed){ // 1, during free process.
			// Client request for RDMA disconnection.
			#ifdef DEBUG_MODE_BRIEF
			printk("%s, RDMA disconnect evetn, requested by client. \n",__func__);
			#endif

		}else{					// freed ==0, newly start free process
			// Remote server requests for disconnection.

			// !! DEAD PATH NOW --> CAN NOT FREE CM_ID !!
			// wait for RDMA_CM_EVENT_TIMEWAIT_EXIT ??
			//			TO BE DONE

			#ifdef DEBUG_MODE_BRIEF
			printk("%s, RDMA disconnect evetn, requested by client. \n",__func__);
			#endif
			//do we need to inform the client, the connect is broken ?
			rdma_disconnect(rdma_queue->cm_id);


			// ########### TO BE DONE ###########
			// Disconnect and free the resource of THIS QUEUE.


			// NOT free the resource of the whole session !
			//semeru_disconenct_and_collect_resource(rdma_queue->rdma_session); 
			//octopus_free_block_devicce(rdma_session->rmem_dev_ctrl);	// Free block device resource 
		}
		break;

	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		// After the received DISCONNECTED_EVENT, need to wait the on-the-fly RDMA message
		// https://linux.die.net/man/3/rdma_get_cm_event

		printk("%s, Wait for in-the-fly RDMA message finished. \n",__func__);
		rdma_queue->state = CM_DISCONNECT;

		//Wakeup caller
		wake_up_interruptible(&rdma_queue->sem);

		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:	//this also should be treated as disconnection, and continue disk swap
		printk(KERN_ERR "%s, cma detected device removal!!!!\n", __func__);
		return -1;
		break;

	default:
		printk(KERN_ERR "%s,oof bad type!\n",__func__);
		wake_up_interruptible(&rdma_queue->sem);
		break;
	}

	return ret;
}




// Resolve the destination IB device by the destination IP.
// [?] Need to build some route table ?
// 
static int rdma_resolve_ip_to_ib_device(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue ){

	int ret;
	struct sockaddr_storage sin; 
	struct sockaddr_in *sin4 = (struct sockaddr_in *)&sin;  


	sin4->sin_family = AF_INET;
	memcpy((void *)&(sin4->sin_addr.s_addr), rdma_session->addr, 4);   	// copy 32bits/ 4bytes from cb->addr to sin4->sin_addr.s_addr
	sin4->sin_port = rdma_session->port;                             		// assign cb->port to sin4->sin_port


	ret = rdma_resolve_addr(rdma_queue->cm_id, NULL, (struct sockaddr *)&sin, 2000); // timeout time 2000ms 
	if (ret) {
		printk(KERN_ERR "%s, rdma_resolve_ip_to_ib_device error %d\n", __func__, ret);
		return ret;
	}else{
		printk("rdma_resolve_ip_to_ib_device - rdma_resolve_addr success.\n");
	}
	
	// Wait for the CM events to be finished:  handled by rdma_cm_event_handler()
	// 	1) resolve addr
	//	2) resolve route
	// Come back here and continue:
	//
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state >= ROUTE_RESOLVED);   //[?] Wait on cb->sem ?? Which process will wake up it.
	if (rdma_queue->state != ROUTE_RESOLVED) {
		printk(KERN_ERR  "%s, addr/route resolution did not resolve: state %d\n", __func__, rdma_queue->state);
		return -EINTR;
	}

	printk(KERN_INFO "%s, resolve address and route successfully\n", __func__);
	return ret;
}



/**
 * Build the Queue Pair (QP).
 * 
 */
int semeru_create_qp(struct rdma_session_context *rdma_session, struct semeru_rdma_queue * rdma_queue){
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = rdma_session->send_queue_depth; 
	init_attr.cap.max_recv_wr = rdma_session->recv_queue_depth;  
	//init_attr.cap.max_recv_sge = MAX_REQUEST_SGL;					// enable the scatter
	init_attr.cap.max_recv_sge = 1;		// for the receive, no need to enable S/G.
	init_attr.cap.max_send_sge = MAX_REQUEST_SGL;					// enable the gather 
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;     // Receive a signal when posted wr is done.
	init_attr.qp_type = IB_QPT_RC;                // Queue Pair connect type, Reliable Communication.  [?] Already assign this during create cm_id.

	// Both recv_cq and send_cq use the same cq.
	init_attr.send_cq = rdma_queue->cq;
	init_attr.recv_cq = rdma_queue->cq;

	ret = rdma_create_qp(rdma_queue->cm_id, rdma_session->rdma_dev->pd, &init_attr);
	if (!ret){
		// Record this queue pair.
		rdma_queue->qp = rdma_queue->cm_id->qp;
  	}else{
    	printk(KERN_ERR "%s:  Create QP falied. errno : %d \n", __func__, ret);
  	}

	return ret;
}





// Prepare for building the Connection to remote IB servers. 
// Create Queue Pair : pd, cq, qp, 
int semeru_create_rdma_queue(struct rdma_session_context *rdma_session, int rdma_queue_index ){
	int ret = 0;
	struct rdma_cm_id *cm_id;
	struct semeru_rdma_queue* rdma_queue;
	int comp_vector = 0; // used for IB_POLL_DIRECT


	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_index]);
	cm_id = rdma_queue->cm_id;

	// 1) Build PD.
	// flags of Protection Domain, (ib_pd) : Protect the local OR remote memory region.  [??] the pd is for local or for the remote attached to the cm_id ?
	// Local Read is default.
	// Allocate and initialize for one rdma_session should be good.
	if(rdma_session->rdma_dev == NULL ){
		rdma_session->rdma_dev = kzalloc(sizeof(struct semeru_rdma_dev), GFP_KERNEL);

		rdma_session->rdma_dev->pd = ib_alloc_pd(cm_id->device, IB_ACCESS_LOCAL_WRITE|
                                            		IB_ACCESS_REMOTE_READ|
                                            		IB_ACCESS_REMOTE_WRITE );    // No local read ??  [?] What's the cb->pd used for ?
		rdma_session->rdma_dev->dev = rdma_session->rdma_dev->pd->device;


		if (IS_ERR(rdma_session->rdma_dev->pd)) {
			printk(KERN_ERR "%s, ib_alloc_pd failed\n", __func__);
			goto err;
		}
		printk(KERN_INFO "%s, created pd %p\n", __func__, rdma_session->rdma_dev->pd);

		// Time to reserve RDMA buffer for this session.
		setup_rdma_session_commu_buffer(rdma_session);
	}


	// 2) Build CQ
	rdma_queue->cq = ib_alloc_cq(cm_id->device, rdma_queue, (rdma_session->send_queue_depth + rdma_session->recv_queue_depth), 
																		comp_vector, IB_POLL_DIRECT);
	if (IS_ERR(rdma_queue->cq)) {
		printk(KERN_ERR "%s, ib_create_cq failed\n", __func__);
		ret = PTR_ERR(rdma_queue->cq);
		goto err;
	}
	printk(KERN_INFO "%s, created cq %p\n", __func__, rdma_queue->cq);


	// 3) Build QP.
	ret = semeru_create_qp(rdma_session, rdma_queue);
	if (ret) {
		printk(KERN_ERR  "%s, failed: %d\n", __func__, ret);
		goto err;
	}
	printk(KERN_INFO "%s, created qp %p\n", __func__, rdma_queue->qp);

err:
	return ret;
}





/**
 * Reserve two RDMA wr for receive/send meesages
 * 		rdma_session_context->rq_wr
 * 		rdma_session_context->send_sgl
 * Post these 2 WRs to receive/send controll messages.
 */
void octopus_setup_message_wr(struct rdma_session_context *rdma_session)
{
	// 1) Reserve a wr for 2-sided RDMA recieve wr
	rdma_session->rdma_recv_req.recv_sgl.addr 	= rdma_session->rdma_recv_req.recv_dma_addr;  // sg entry addr    
	rdma_session->rdma_recv_req.recv_sgl.length = sizeof(struct message);				// address of the length
	rdma_session->rdma_recv_req.recv_sgl.lkey = rdma_session->rdma_dev->dev->local_dma_lkey;

	rdma_session->rdma_recv_req.rq_wr.sg_list = &(rdma_session->rdma_recv_req.recv_sgl);
	rdma_session->rdma_recv_req.rq_wr.num_sge = 1;  // ib_sge scatter-gather's number is 1
	rdma_session->rdma_recv_req.cqe.done = two_sided_message_done;
	rdma_session->rdma_recv_req.rq_wr.wr_cqe = &(rdma_session->rdma_recv_req.cqe);


	// 2) Reserve a wr for 2-sided RDMA send wr
	rdma_session->rdma_send_req.send_sgl.addr = rdma_session->rdma_send_req.send_dma_addr;
	rdma_session->rdma_send_req.send_sgl.length = sizeof(struct message);
	rdma_session->rdma_send_req.send_sgl.lkey = rdma_session->rdma_dev->dev->local_dma_lkey;

	rdma_session->rdma_send_req.sq_wr.opcode = IB_WR_SEND;		// ib_send_wr.opcode , passed to wc.
	rdma_session->rdma_send_req.sq_wr.send_flags = IB_SEND_SIGNALED;
	rdma_session->rdma_send_req.sq_wr.sg_list = &rdma_session->rdma_send_req.send_sgl;
	rdma_session->rdma_send_req.sq_wr.num_sge = 1;
	rdma_session->rdma_send_req.cqe.done = two_sided_message_done;
	rdma_session->rdma_send_req.sq_wr.wr_cqe = &(rdma_session->rdma_send_req.cqe);

	return;
}



/**
 * We reserve two WRs for send/receive RDMA messages in a 2-sieded way.
 * 	a. Allocate 2 buffers
 * 		dma_session->recv_buf
 * 		rdma_session->send_buf
 *	b. Bind their DMA/BUS address to  
 * 		rdma_context->recv_sgl
 * 		rdma_context->send_sgl
 * 	c. Bind the ib_sge to send/receive WR
 * 		rdma_context->rq_wr
 * 		rdma_context->sq_wr
 */
int semeru_setup_buffers(struct rdma_session_context *rdma_session){
	int ret;

	// 1) Allocate some DMA buffers.
	// [x] Seems that any memory can be registered as DMA buffers, if they satisfy the constraints:
	// 1) Corresponding physical memory is allocated. The page table is built. 
	//		If the memory is allocated by user space allocater, malloc, we need to walk  through the page table.
	// 2) The physial memory is pinned, can't be swapt/paged out.
  rdma_session->rdma_recv_req.recv_buf = kzalloc(sizeof(struct message), GFP_KERNEL);  	//[?] Or do we need to allocate DMA memory by get_dma_addr ???
	rdma_session->rdma_send_req.send_buf = kzalloc(sizeof(struct message), GFP_KERNEL);  

	// Get DMA/BUS address for the receive buffer
	rdma_session->rdma_recv_req.recv_dma_addr = ib_dma_map_single(rdma_session->rdma_dev->dev, rdma_session->rdma_recv_req.recv_buf, 
																																	sizeof(struct message), DMA_BIDIRECTIONAL);
	rdma_session->rdma_send_req.send_dma_addr = ib_dma_map_single(rdma_session->rdma_dev->dev, rdma_session->rdma_send_req.send_buf, 
																																	sizeof(struct message), DMA_BIDIRECTIONAL);	

	#ifdef DEBUG_MODE_BRIEF
	printk("%s, Got dma/bus address 0x%llx, for the recv_buf 0x%llx \n", __func__, (unsigned long long)rdma_session->rdma_recv_req.recv_dma_addr, 
																			(unsigned long long)rdma_session->rdma_recv_req.recv_buf);
	printk("%s, Got dma/bus address 0x%llx, for the send_buf 0x%llx \n", __func__, (unsigned long long)rdma_session->rdma_send_req.send_dma_addr, 
																			(unsigned long long)rdma_session->rdma_send_req.send_buf);
	#endif

	

	// 3) Add the allocated (DMA) buffer to reserved WRs
	octopus_setup_message_wr(rdma_session);
	#ifdef DEBUG_MODE_BRIEF
	printk(KERN_INFO "%s, allocated & registered buffers...\n", __func__);
	#endif


	// 4) Initialzation for the Data-Path
	rdma_session->write_tag = kzalloc(sizeof(uint32_t), GFP_KERNEL);
	*(rdma_session->write_tag) = 0; 	// initialize its value to 0. | -- 16 bits, dirty or not --| -- 16bits version --|
	rdma_session->write_tag_dma_addr = ib_dma_map_single(rdma_session->rdma_dev->dev, rdma_session->write_tag, sizeof(uint32_t), DMA_TO_DEVICE);

	//		Initialize the wr here to save some RDMA write issuing time.
	ret = init_write_tag_rdma_command(rdma_session);
	if(unlikely(ret != 0)){
		printk(KERN_ERR "%s, initialize write_tag rdma command failed. \n",__func__);
		goto err;
	}


	#ifdef DEBUG_MODE_BRIEF
	printk(KERN_INFO "%s is done. \n", __func__);
	#endif


err:
	return ret;
}



/**
 * All the PD, QP, CP are setted up, connect to remote IB servers.
 * This will send a CM event to remote IB server && get a CM event response back.
 */
int semeru_connect_remote_memory_server(struct rdma_session_context *rdma_session, int rdma_queue_inx ){
	struct rdma_conn_param conn_param;
	int ret;
	struct semeru_rdma_queue * rdma_queue;
	struct ib_recv_wr *bad_wr;

	
	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_inx]);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;



	// After rdma connection built, memory server will send a 2-sided RDMA message immediately
	// post a recv on cq to wait wc
	// MT safe during the connection process
	atomic_inc(&rdma_queue->rdma_post_counter);
	ret = ib_post_recv(rdma_queue->qp, &rdma_session->rdma_recv_req.rq_wr, &bad_wr); 
	if(ret){
		printk(KERN_ERR "%s: post a 2-sided RDMA message error \n",__func__);
		goto err;
	}	


	ret = rdma_connect(rdma_queue->cm_id, &conn_param);  // RDMA CM event 
	if (ret) {
		printk(KERN_ERR "%s, rdma_connect error %d\n", __func__, ret);
		return ret;
	}
	printk(KERN_INFO "%s, Send RDMA connect request to remote server \n", __func__);



	// Get free memory information from Remote Mmeory Server
	// [X] After building the RDMA connection, server will send its free memory to the client.
	// Post the WR to get this RDMA two-sided message.
	// When the receive WR is finished, cq_event_handler will be triggered.
	
	// a. post a receive wr

	// b. Request a notification (IRQ), if an event arrives on CQ entry.
	// For a IB_POLL_DIRECT cq, we need to poll the cqe manually.

	// After receiving the CONNECTED state, means the RDMA connection is built.
	// Prepare to receive 2-sided RDMA message
	wait_event_interruptible(rdma_queue->sem, rdma_queue->state >= CONNECTED);
	if (rdma_queue->state == ERROR) {
		printk(KERN_ERR "%s, Received ERROR response, state %d\n", __func__, rdma_queue->state);
		return -1;
	}

	// for the posted ib_recv_wr.
	// check available memory
	drain_rdma_queue(rdma_queue);

	printk(KERN_INFO "%s, RDMA connect successful\n", __func__);

err:
	return ret;
}


//
// <<<<<<<<<<<<<<<<<<<<<<<  End of RDMA Communication (CM) event handler <<<<<<<<<<<<<<<<<<<<<<<
//






//
// >>>>>>>>>>>>>>>  Start of ONE-SIDED RDMA message section >>>>>>>>>>>>>>>
//	Stateful,
//		Need to allocate and maintain some resource.
//		i.e. The 1-sided WR.
//		All the reource stored in rmem_rdma_queue, mapped to each dispatch queue.
//		These resources will be invoked by the Disk Driver via the definination of blk_mq_ops->.queue_rq.
//
//  [?] Need a daemon thread ? 
//		Seems no. After getting the chunk remote_addr/rkey, no need to wait any signals.
//		The action is triggered by blk_mq_ops->.queue_rq, which pops i/o requset.
//







/**
 * Initialize the Data-Path, write tag rdma command  
 *  
 * [x] One write_tag wr with one RDMA buffer, can be writting to all the different destinations,Regions.
 * 
 */
int init_write_tag_rdma_command(struct rdma_session_context *rdma_session){
	int ret = 0; // if success, return 0.

/*

	// Allocate 
	// [XX] Definitely not need scatter-gather for the write_tag mechanism.
	//      Because all the 
	rdma_session->write_tag_rdma_cmd = (struct semeru_rdma_req_sg*)kzalloc( sizeof(struct semeru_rdma_req_sg) + 1 * sizeof(struct scatterlist)  \
																																, GFP_KERNEL );
	#ifdef DEBUG_MODE_BRIEF
	if(unlikely(rdma_session->write_tag_rdma_cmd == NULL)){
		printk(KERN_ERR "%s, allocate write_tag_rdma_cmd failed. \n",__func__);
		goto err;
	}
	#endif

	// 1) Do initialization for some fiels
	rdma_session->write_tag_rdma_cmd->io_rq 		= NULL;  // Always null ? seems don't need this field, even this is Data-Path.


	// 2) Local RDMA buffers

	// the vlaue of rdma_session->write_tag needs to be dynamically assigned before sending.

	//	Each 1-sided RDMA write can only write to 1 Region, only reserve 1 write_tag 
	rdma_session->write_tag_rdma_cmd->sge_list[0].addr 		= rdma_session->write_tag_dma_addr;  // source addr
	rdma_session->write_tag_rdma_cmd->sge_list[0].length	=	sizeof(uint32_t);  // Must a atomic write. sizeof(variable) also works
	rdma_session->write_tag_rdma_cmd->sge_list[0].lkey		=	rdma_session->qp->device->local_dma_lkey;
	
	rdma_session->write_tag_rdma_cmd->rdma_sq_wr.wr.sg_list		= rdma_session->write_tag_rdma_cmd->sge_list;  // let wr.sg_list points to the start of the ib_sge array?
	rdma_session->write_tag_rdma_cmd->rdma_sq_wr.wr.num_sge		= 1;											 // 1 RDMA buffer for write_tag


	// 3) Remote RDMA buffers

	// These 2 fields need to be assinged dynamically.
	//rdma_session->write_tag_rdma_cmd->rdma_sq_wr.rkey					= remote_chunk_ptr->remote_rkey;
	//rdma_session->write_tag_rdma_cmd->rdma_sq_wr.remote_addr	= remote_chunk_ptr->remote_addr + offset_within_chunk;

	// The rest fields are assigned statically.
	rdma_session->write_tag_rdma_cmd->rdma_sq_wr.wr.opcode			= IB_WR_RDMA_WRITE;  			//
	rdma_session->write_tag_rdma_cmd->rdma_sq_wr.wr.send_flags 	= IB_SEND_SIGNALED; 	//get a ack when 1-sided RDMA message is sent
	rdma_session->write_tag_rdma_cmd->rdma_sq_wr.wr.wr_id	= (uint64_t)rdma_session->write_tag_rdma_cmd;

	//Debug
	#ifdef DEBUG_MODE_BRIEF
		rdma_session->write_tag_rdma_cmd->message_type	= 2;  // write_tag
		printk(KERN_INFO "%s, initialize the write_tag and write_tag_rdma_command is done. \n",__func__);
	#endif
*/

#ifdef DEBUG_MODE_BRIEF
err:
#endif

	return ret;
}
















//
// <<<<<<<<<<<<<<  End of ONE-SIDED RDMA message section <<<<<<<<<<<<<<
//




//
// >>>>>>>>>>>>>>>  Start of handling chunk management >>>>>>>>>>>>>>>
//
// After building the RDMA connection, build the Client-Chunk Remote-Chunk mapping information.
//



/**
 * Invoke this information after getting the free size of remote memory pool.
 * Initialize the chunk_list based on the chunk size and remote free memory size.
 * 
 *  
 * More Explanation:
 * 	Record the address of the mapped chunk:
 * 		remote_rkey : Used by the client, read/write data here.
 * 		remote_addr : The actual virtual address of the mapped chunk ?
 * 
 */
int init_remote_chunk_list(struct rdma_session_context *rdma_session ){

	int ret = 0;
	uint32_t i;
	
	// 1) initialize chunk related variables
	//		The first Region may not be fullly mapped. Not clear for now.
	//		The 2nd -> rest are fully mapped at REGION_SIZE_GB size.
	rdma_session->remote_chunk_list.chunk_ptr = 0;	// Points to the first empty chunk.
	rdma_session->remote_chunk_list.remote_free_size = 0; // not clear the exactly free size now.
	rdma_session->remote_chunk_list.remote_chunk = (struct remote_mapping_chunk*)kzalloc(  \
																									sizeof(struct remote_mapping_chunk) * rdma_session->remote_chunk_list.chunk_num,\
																									GFP_KERNEL);

	for(i=0; i < rdma_session->remote_chunk_list.chunk_num; i++){
		rdma_session->remote_chunk_list.remote_chunk[i].chunk_state = EMPTY;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_addr = 0x0;
		rdma_session->remote_chunk_list.remote_chunk[i].mapped_size	= 0x0;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_rkey = 0x0;
	}



	return ret;
}




/**
 * Get a chunk mapping 2-sided RDMA message.
 * Bind these chunks to the cient in order.
 * 
 *	1) The information of Chunks to be bound,  is stored in the recv WR associated DMA buffer.
 * Record the address of the mapped chunk:
 * 		remote_rkey : Used by the client, read/write data here.
 * 		remote_addr : The actual virtual address of the mapped chunk
 * 
 *	2) Attach the received chunks to the rdma_session_context->remote_mapping_chunk_list->remote_mapping_chunk[]
 */
void bind_remote_memory_chunks(struct rdma_session_context *rdma_session ){

	int i; 
	uint32_t *chunk_ptr;

	chunk_ptr = &(rdma_session->remote_chunk_list.chunk_ptr);
	// Traverse the receive WR to find all the got chunks.
	for(i = 0; i < MAX_REGION_NUM; i++ ){
		
		#ifdef DEBUG_MODE_BRIEF
		if( *chunk_ptr >= rdma_session->remote_chunk_list.chunk_num){
			printk(KERN_ERR "%s, Get too many chunks. \n", __func__);
			break;
		}
		#endif
		
		if(rdma_session->rdma_recv_req.recv_buf->rkey[i]){
			// Sent chunk, attach to current chunk_list's tail.
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey = rdma_session->rdma_recv_req.recv_buf->rkey[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr = rdma_session->rdma_recv_req.recv_buf->buf[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].mapped_size = rdma_session->rdma_recv_req.recv_buf->mapped_size[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].chunk_state = MAPPED;
			
			rdma_session->remote_chunk_list.remote_free_size += rdma_session->rdma_recv_req.recv_buf->mapped_size[i]; // byte size, 4KB alignment


			#ifdef DEBUG_MODE_BRIEF
				printk(KERN_INFO "Got chunk[%d] : remote_addr : 0x%llx, remote_rkey: 0x%x, mapped_size: 0x%llx \n", i, 
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr,
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey,
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].mapped_size);
			#endif

			(*chunk_ptr)++;
		}

	} // for

	rdma_session->remote_chunk_list.chunk_num	= *chunk_ptr;  // Record the number of received chunks.

}





//
// <<<<<<<<<<<<<<<<<<<<<  End of handling chunk management <<<<<<<<<<<<<<<<<<<<<
//




//
// >>>>>>>>>>>>>>>  Start of  RDMA Control Path >>>>>>>>>>>>>>>
//





/**
 * Semeru CS - Copy the received data from RDMA buffer to destination.
 *  
 * 1) For current design, kernel register the passed in user space as RDMA buffer directly to acheive zero copy.
 * 		a. The caller progress can't quit before the value.
 *    b. The caller progress has to pin the physical memory to not let kernel unmap the physical page from it.
 * 
 * 2）Inform casller progress that the feteched data comes back.
 * 		a. Both caller and responser need to negotiate a specific uint64_t as flag.
 * 		b. Caller reset it to a specific vlaue, e.g. -1 for uint64_t, before send 1-sided RDMA read.
 *    c. The flag value on responser are initiazed to a tag value, e.g. server#1.
 *    d. Caller use a busy waiting loop to check the flag value.
 */
void cp_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc){
	int ret = 0;
  struct semeru_rdma_req_sg 	*rdma_cmd_ptr;
	struct semeru_rdma_queue	*rdma_queue;
  
	// Get rdma_command  attached to wr->wr_id
	// Reuse the rmem_rdam_command instance.
  rdma_cmd_ptr	= container_of(wc->wr_cqe, struct semeru_rdma_req_sg, cqe); 
  if(unlikely(rdma_cmd_ptr == NULL)){
    printk(KERN_ERR "%s, get NULL rmem_rdma_command from wc \n", __func__);
		ret = -1;
		goto out;
  }

	rdma_queue = rdma_cmd_ptr->rdma_queue;
	
	// unmap rdma buffer from device
	// [?] if we keep this mapping ,will it batter for our re-map next time ?
	ib_dma_unmap_sg(rdma_queue->rdma_session->rdma_dev->dev, rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry,	DMA_FROM_DEVICE);

	// Return one wr, decrease the number of outstanding (read) wr.
	ret = atomic_dec_return(&(rdma_queue->rdma_post_counter));
	complete(&rdma_cmd_ptr->done); 

	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO "%s, rdma_queue[%d], rdma_wr[%d] done. <<<< \n", __func__, rdma_queue->q_index, ret + 1);
	#endif

out:
	return;
}


/**
 * Almost same with read done. 
 * 
 */
void cp_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc){
	int ret = 0;
  struct semeru_rdma_req_sg 	*rdma_cmd_ptr;
	struct semeru_rdma_queue	*rdma_queue;
  
	// Get rdma_command  attached to wr->wr_id
	// Reuse the rmem_rdam_command instance.
  rdma_cmd_ptr	= container_of(wc->wr_cqe, struct semeru_rdma_req_sg, cqe); 
  if(unlikely(rdma_cmd_ptr == NULL)){
    printk(KERN_ERR "%s, get NULL rmem_rdma_command from wc \n", __func__);
		ret = -1;
		goto out;
  }

	rdma_queue = rdma_cmd_ptr->rdma_queue;
	


	// unmap rdma buffer from device
	// [?] if we keep this mapping ,will it batter for our re-map next time ?
	ib_dma_unmap_sg(rdma_queue->rdma_session->rdma_dev->dev, rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry,	DMA_TO_DEVICE);

	// Return one wr, decrease the number of outstanding (read) wr.
	ret = atomic_dec_return(&(rdma_queue->rdma_post_counter));
	complete(&rdma_cmd_ptr->done); 

	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO "%s, rdma_queue[%d], rdma_wr[%d] done. <<<< \n", __func__, rdma_queue->q_index, ret+1);
	#endif

out:
	return;
}




/**
 * When we enqueue a write/read wr,
 * the total number can't exceed the send/receive queue depth.
 * Or it will cause QP Out of Memory error.
 * 
 * return :
 *  0 : success;
 *  -1 : error. 
 * 
 * More explanation:
 * There are 2 queues for QP, send/recv queue.
 * 1) Send queue limit the number of outstanding wr.
 *    This limits both the 1-sided/2-sided wr. 
 * 2) For 2-sided RDMA, it also needs to post s recv wr to receive data.
 *    But for Semeru, we reply on 1-sided RDMA wr to write/read data.
 *    The the depth of send queue should be much larger than recv queue.
 * 
 * The depth of these 2 queues are limited by:
 * 	init_attr.cap.max_send_wr
 *	init_attr.cap.max_recv_wr
 * 
 */
int cp_enqueue_send_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue * rdma_queue, struct semeru_rdma_req_sg *rdma_req){
	int ret = 0;
	struct ib_send_wr 	*bad_wr;
	int test;

	rdma_req->rdma_queue = rdma_queue;	// points to the rdma_queue to be enqueued.


	// Post 1-sided RDMA read wr	
	// wait and enqueue wr 
	// Both 1-sided read/write queue depth are RDMA_SEND_QUEUE_DEPTH
		while(1){
			test = atomic_inc_return(&rdma_queue->rdma_post_counter);
			if( test < RDMA_SEND_QUEUE_DEPTH - 16 ){
				//post the 1-sided RDMA write 
				// Use the global RDMA context, rdma_session_global
				ret = ib_post_send(rdma_queue->qp, (struct ib_send_wr*)&rdma_req->rdma_sq_wr, &bad_wr);
				if(unlikely(ret)){
						printk(KERN_ERR "%s, post 1-sided RDMA send wr failed, return value :%d. counter %d \n", __func__, ret, test );
						ret = -1;
						goto err;
				}

				#ifdef DEBUG_MODE_BRIEF
					printk(KERN_INFO "%s, rdma_queue[%d] enqueued rdma_wr[%d] >>>> \n", __func__, rdma_queue->q_index, test );
				#endif
				// Enqueue successfully.
				// exit loop.
				return ret;
			}else{
				// RDMA send queue is full, wait for next turn.
				test = atomic_dec_return(&rdma_queue->rdma_post_counter);
				//schedule(); // release the core for a while.
        // cpu_relax(); // which one is better ?

				// For IB_DIRCT_CQ, poll the cq
				drain_rdma_queue(rdma_queue);
			
			}

		}// end of while, try to enqueue read wr.

err:
	printk(KERN_ERR" Error in %s \n", __func__);
	return -1;
}




/**
 * Semeru CS - Map multiple meta data structure's physical address to rdma scatter-gather.
 * 
 * Find several contiguous virtual pages and register them as RDMA buffer for a S/G WR.
 * The max contiguous pages number is (MAX_REQUEST_SGL -2)*PAGE_SIZE.  2 for safety.
 * 
 * 
 * Build the RDMA buffer of CPU server. 
 * CPU Server : multiple sg entries, their physical/dma address are not contiguous.
 * Memory Server : a contiguous virtual RDMA buffer. 
 * 
 *  Parameters
 * 		*addr_scan_ptr : 
 * 			entry point - points to the start addr to be scanned.
 * 			exit point - points to the end addr of the package page.
 * 
 * 			|- #1 - #2 - #3 - #5 - #6 - .... |
 * 				                   ^
 * 												   *addr_scan_ptr points the end of page #5, start of page #6.
 *                        	 page #5 is the last page in the package. It's mapped.			
 * 
 */
uint64_t meta_data_map_sg(struct rdma_session_context * rdma_session,  struct scatterlist* sgl, 
													char ** addr_scan_ptr, char * end_addr){

	uint64_t entries = 0; // mapped pages
	size_t	package_page_num_limit = (MAX_REQUEST_SGL -2);  // InfiniBand hardware S/G limits, bytes
	pte_t* 	pte_ptr;
	struct page *buf_page;

	// Scan and find several, at most package_len_limit, contiguous pages as RDMA buffer. 
	while(*addr_scan_ptr < end_addr){
		pte_ptr = walk_page_table(current->mm, (uint64_t)(*addr_scan_ptr) );  //[?] Will this walking cause any problem ?

		if(pte_ptr == NULL || !pte_present(*pte_ptr) ){ 
			// 1) not mapped pte, skip it. 
			//    NULL : means never being assigned a page
			//		not present : means being swapped out.
			//
			//    Even if the unmapped physical page is in Swap Cache, it's clean.
			//    All the pages will be written to remote memory server immedialte after being unmapped.
						
			#ifdef DEBUG_MODE_BRIEF
				if(pte_ptr == NULL){
					printk(KERN_WARNING"%s, Virt page 0x%llx  is NOT touched.\n", __func__,  (uint64_t)(*addr_scan_ptr));
				}else if( page_in_swap_cache(*pte_ptr) != NULL){
					printk(KERN_WARNING"%s, Virt page 0x%llx ->  phys page is in Swap Cache.\n", __func__,  (uint64_t)(*addr_scan_ptr));
				}else if(!pte_present(*pte_ptr)){
					// e.g. the page is swapped to swap partition && the swap_cache entry is freed.
					printk(KERN_WARNING"%s, Virt page 0x%llx  is NOT present.\n", __func__,  (uint64_t)(*addr_scan_ptr));
				}else{
					printk(KERN_WARNING"%s, Virt page 0x%llx  is NOT mapped to physical page (unknown mode).\n", __func__,  (uint64_t)(*addr_scan_ptr));
				}
			#endif

			
			// Exit#1, Find a breaking point.
			// Stop building the S/G buffer.
			if(entries != 0){
				goto out; 	// Break RDMA buffer registration 
			}else{

				// Skip the unmapped page at the beginning, update the iterator.
				*addr_scan_ptr += PAGE_SIZE;
				continue;	// goto find the first mapped pte.
			}
		
		} // end of if
				
		// 2) Page Walk the mapped pte.		
		buf_page = pfn_to_page(pte_pfn(*pte_ptr));
		sg_set_page( &(sgl[entries++]), buf_page, PAGE_SIZE, 0); // Assign a page to s/g. entire apge, offset in page is 0x0,.


		#ifdef DEBUG_MODE_BRIEF
			printk(KERN_INFO "%s, Virt page 0x%lx, pte 0x%lx, page 0x%lx, dma_addr 0x%lx \n", 
											__func__, (size_t)*addr_scan_ptr, (size_t)pte_val(*pte_ptr), (size_t)buf_page, (size_t)sgl[entries -1].dma_address );
		#endif


		// Find a mapped page, update the interator pointer
		*addr_scan_ptr += PAGE_SIZE;

		// Exit#2, Find enough contiguous virtual pages.
		if(entries >= package_page_num_limit){
			goto out;
		}

	} // end of for



out:

	return entries;  // number of initialized ib_sge
}





/**
 * Control-Path, build a rdma wr for the RDMA read/write in CP.
 * 
 * Return : The number of pages being sent.
 * 
 * 1) Put the contiguous pages into one RDMA wr by utilizing Scatter/Gather.
 * 		Every time we just put a contiguous range of virtual memory into the S/G buffer.
 * 2) start_addr/end_addr stores the virtual address range to be processed.
 * 
 */
int cp_build_rdma_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_req_sg *rdma_cmd_ptr, enum dma_data_direction dir,
									struct remote_mapping_chunk *	remote_chunk_ptr, char ** addr_scan_ptr,  char* end_addr){

	int mapped_pages = 0;
	int i;
	int dma_entry = 0;
	struct ib_device	*ibdev	=	rdma_session->rdma_dev->dev;  // get the ib_devices

	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO "%s, build wr for range [0x%llx, 0x%llx)\n",
									__func__, (uint64_t)*addr_scan_ptr, (uint64_t)end_addr );
	#endif

	// 1) Register the CPU server's local RDMA buffer.  
	//	  Map the corresponding physical pages to S/G structure.  
	init_completion( &(rdma_cmd_ptr->done) );	
	rdma_cmd_ptr->nentry		= meta_data_map_sg(rdma_session, rdma_cmd_ptr->sgl, addr_scan_ptr, end_addr);
	rdma_cmd_ptr->seq_type 	= CONTROL_PATH_MEG; // means this is CP path data.
	if(unlikely(rdma_cmd_ptr->nentry == 0)){
		// It's ok, the pte are not mapped to any physical pages.
		printk(KERN_INFO "%s, Find zero mapped pages, end at 0x%lx. Skip this wr. \n", __func__,	(size_t)end_addr);
		mapped_pages = 0;
		goto err; // return 0.
	}

	// 2) Register the physical address stored in S/G structure to DMA device.
	//  	Here gets the scatterlist->dma_address.
	//    CPU server RDMA buffer  registration.
	dma_entry	=	ib_dma_map_sg( ibdev, rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry, dir);  // Inform PCI device the dma address of these scatterlist.
	if( unlikely(dma_entry == 0) ){
		printk(KERN_ERR "ERROR in %s Registered 0 entries to rdma scatterlist \n", __func__);
		mapped_pages = 0;
		goto err;
	}
	mapped_pages = dma_entry;  // return the number of pages mapped to RDMA device.




	// 3) Register Remote RDMA buffer to WR.
	// 		The whole remote virtual memory pool is already resigered as RDMA buffer.
	//		Here just fills the information into the rdma_sq_wr.
	rdma_cmd_ptr->rdma_sq_wr.rkey					= remote_chunk_ptr->remote_rkey;
	// Start address of the S/G vector. Universal address space.
	rdma_cmd_ptr->rdma_sq_wr.remote_addr	= remote_chunk_ptr->remote_addr + ( (uint64_t)(*addr_scan_ptr - rdma_cmd_ptr->nentry * PAGE_SIZE) & CHUNK_MASK ); 
	rdma_cmd_ptr->rdma_sq_wr.wr.opcode		= (dir == DMA_TO_DEVICE ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ);
	rdma_cmd_ptr->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED; // 1-sided RDMA message ? both read /write
	
	if(dir == DMA_TO_DEVICE){
		rdma_cmd_ptr->cqe.done = cp_rdma_write_done;
	}else{
		// DMA_FROM_DEVICE
		rdma_cmd_ptr->cqe.done = cp_rdma_read_done;
	}
	rdma_cmd_ptr->rdma_sq_wr.wr.wr_cqe = &(rdma_cmd_ptr->cqe); // completion function


	#ifdef DEBUG_MODE_BRIEF
		if(rdma_cmd_ptr->nentry != dma_entry){
			printk(KERN_ERR"%s, rdma_cmd_ptr->nentry 0x%lx NOT equal to dma_entry 0x%x \n",__func__, (size_t)rdma_cmd_ptr->nentry,  dma_entry);
			mapped_pages = 0; // for current function 0 is error
			goto err;
		}
		
		printk(KERN_INFO"%s,  Mapped %d entries. From CPU server [0x%lx, 0x%lx) to Memory server[0x%lx, 0x%lx) , in current rdma_wr \n",
																				__func__, dma_entry, 
																				(size_t)(*addr_scan_ptr - dma_entry * PAGE_SIZE), (size_t)*addr_scan_ptr, 
																				(size_t)rdma_cmd_ptr->rdma_sq_wr.remote_addr, (size_t)(rdma_cmd_ptr->rdma_sq_wr.remote_addr + dma_entry*PAGE_SIZE)  );



		//print_scatterlist_info(rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry);
	#endif


	// 4) Register Local RDMA Buffer to WR.
	// [Warning] assume all the sectors in this bio is contiguous.
	// Then the remote address is contiguous (virtual space). 
	// Scatter-Gather can only be one to multiple/mutiple to one.
	//
	// build multiple ib_sge
	//struct ib_sge sge_list[dma_entry];
	//
	// [?] not use the scatterlist->page_link at all ?
	for(i=0; i<dma_entry; i++ ){
		rdma_cmd_ptr->sge_list[i].addr 		= sg_dma_address(&(rdma_cmd_ptr->sgl[i]));  // scatterlist->addr
		rdma_cmd_ptr->sge_list[i].length	= PAGE_SIZE; // should be the size for each ib_sge !! not the total size of RDMA S/G !!
		rdma_cmd_ptr->sge_list[i].lkey		=	rdma_session->rdma_dev->dev->local_dma_lkey;
	
		#ifdef DEBUG_MODE_BRIEF
			printk(KERN_INFO "%s, Local RDMA Buffer[%d], ib_sge list addr: 0x%llx, lkey: 0x%llx, len: 0x%llx \n",
																							__func__,
																							i,
																							(uint64_t)rdma_cmd_ptr->sge_list[i].addr,
																							(uint64_t)rdma_cmd_ptr->sge_list[i].lkey,
																							(uint64_t)rdma_cmd_ptr->sge_list[i].length);
		#endif
	
	}

	rdma_cmd_ptr->rdma_sq_wr.wr.next    	= NULL;	// [x] If not set it to NULL, IB treats the pointed address as a ib_rdma_wr too.
	rdma_cmd_ptr->rdma_sq_wr.wr.sg_list		= rdma_cmd_ptr->sge_list;  // let wr.sg_list points to the start of the ib_sge array
	rdma_cmd_ptr->rdma_sq_wr.wr.num_sge		= dma_entry;
		
err:
	return mapped_pages; 
}







/**
 * Semeru CPU Server - Control Path, Post 1-sided rdma_wr 
 * 	Invoked by user space application, e.g. JVM.
 * 
 * Parameters 
 * 	start_addr,  remote virtual memory address, to be read.
 * 	data length , 4KB alignment.
 * 
 * 	[?]For the ideal case, don't use a RDMA buffer. fetch the data back into CS's corresponding virtual address directly.
 * 
 * 
 * More explanation
 * 		
 * 
 */
int semeru_cp_rdma_send(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue, struct semeru_rdma_req_sg *rdma_req_sg,
							char __user * start_addr, uint64_t bytes_len, enum dma_data_direction dir ){

	int ret = 0;
	char* end_addr = start_addr + bytes_len;
	char* addr_scan_ptr = start_addr;  // Points to the current scanned addr

	// 1) Calculate the remote address
	//    The caller has to guarantee the accessd range within one rdma chunk.
  uint64_t  start_chunk_index   	= ((uint64_t)start_addr - SEMERU_START_ADDR) >> CHUNK_SHIFT;    // REGION_SIZE_GB/chunk in default.
  struct remote_mapping_chunk   	*remote_chunk_ptr = &(rdma_session->remote_chunk_list.remote_chunk[start_chunk_index]);

	// Cut the whole data into several packages, limited by the scatter-gather hardware limitations.
	while( addr_scan_ptr < end_addr){

		ret = cp_build_rdma_wr(rdma_session, rdma_req_sg, dir, remote_chunk_ptr, &addr_scan_ptr, end_addr);
		if(unlikely(ret == 0)){
			printk(KERN_WARNING "%s, Build rdma wr in Control-Path failed OR Skip empty pte. Skip enqueue WR \n", __func__);
			complete(&rdma_req_sg->done);	// Not enqueue this rdma_queue, mark it complete here.
			// ret = 0 is good here. But can cause error in the caller.
	  	goto err;  // Skip the WR enqueue.
		}

		// Post 1-sided RDMA read wr	
		// Both read/write queue depth are RDMA_SEND_QUEUE_DEPTH
		ret = cp_enqueue_send_wr(rdma_session, rdma_queue,  rdma_req_sg);
		if(unlikely(ret)){ // -1, non-zero
			printk(KERN_ERR "%s, enque ib_send_wr failed. \n", __func__);
			goto err;
		}
		
		#ifdef DEBUG_MODE_BRIEF
		printk("%s,Post a 1-sided RDMA wr , dir %d , start addr 0x%llx done.\n", __func__, dir,
																					(uint64_t)(addr_scan_ptr - ret*PAGE_SIZE) );
		#endif

		//debug - drain after inserting any rdma_wr
		// drain after queue is full
		//drain_rdma_queue(rdma_queue);

	}// end of for loop, send data.


err:
	return ret;
}










//
// Syscall filling operations
//

/**
 * Semeru CPU Server - Synchronous read. with S/G.
 * 	the kernel space rdma read operation. 
 * 	Register the function into kernel's syscall.
 * 
 */
char* semeru_cp_rdma_read(int target_mem_server, char __user * start_addr, unsigned long size){

	int ret = 0;
	int cpu;
	char __user * start_addr_aligned;
	char __user * end_addr_aligned;
	unsigned long size_aligned;
	struct semeru_rdma_queue *rdma_queue;
	struct rdma_session_context *rdma_session = &rdma_session_global; // not support multiple Memory server for now
	struct semeru_rdma_req_sg	*rdma_req_sg;

	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO " %s, get start_addr : 0x%lx, size : 0x%lx \n", __func__, (unsigned long)start_addr, size);
	#endif

	// #1 Do page alignmetn,
	// If the sent data small than a page, align up to a page
	// Because we need to register a whole physical page as RDMA buffer.
	start_addr_aligned 	= (char*)((unsigned long)start_addr & PAGE_MASK); // align_down
	end_addr_aligned	= (char*)(((unsigned long)start_addr + size + PAGE_SIZE -1) & PAGE_MASK); // align_up
	size_aligned	= (unsigned long)(end_addr_aligned - start_addr_aligned);

	cpu = get_cpu(); // disable core preempt

	rdma_queue = &(rdma_session->rdma_queues[cpu]);
  rdma_req_sg = (struct semeru_rdma_req_sg*)kmem_cache_alloc(rdma_queue->rdma_req_sg_cache, GFP_ATOMIC);
  if(unlikely(rdma_req_sg == NULL)){
    pr_err("%s, get reserved rdma_req_sg failed. \n", __func__);
    ret = -1;
    goto out;
  }
	//reset_semeru_rdma_req_sg(rdma_req_sg);
	memset(rdma_req_sg, 0, sizeof(struct semeru_rdma_req_sg));


	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO "%s, rdma_queue[%d],	aligned start_addr : 0x%lx, aligned size : 0x%lx \n", 
											__func__,	rdma_queue->q_index, (unsigned long)start_addr_aligned, size_aligned);
	#endif


	// 1) build and enqueue the 1-sided rdma_wr
	ret = semeru_cp_rdma_send(rdma_session, rdma_queue, rdma_req_sg, start_addr_aligned, size_aligned, DMA_FROM_DEVICE);
	if(unlikely(ret != 0)){
		printk(KERN_ERR "%s, cp_post_rdma_read failed. \n", __func__);
		start_addr = NULL;
		goto out;
	}

	put_cpu(); // enable core preemtp


	// 2) Wait for the completion.
	// There is no physical page. 
	// So, we just drain the corresponding queue
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ
	
	ret = wait_for_completion_timeout(&(rdma_req_sg->done), msecs_to_jiffies(5)); // 5ms at most. The waiting is un-interrupptible
  if(unlikely( ret == 0)){
    pr_err("%s, rdma_queue[%d] wait for rdma_req_sg timeout for 5ms.\n",__func__, rdma_queue->q_index);
    ret = -1;
    goto out;
  }
	ret = 0; // reset return value to 0.

out :
	return start_addr;

}



/**
 * Semeru Control Path - Synchronous write
 * Write data to remote memory pool.
 * 
 * Parameters:
 * target_mem_server : which server to send the data.
 * write_type: 0 for data; 1(non-zero) for signal.
 * 	[x] Lots of flag/signals are written by this function.
 * 		We need to guarantee the signal is the last message on the QP.
 * 		For example, when we issue an CSet to Memory server, we need to confirm all the other packages are already done.
 * 
 * 
 * 
 * If succeed, 
 * 		return the start_addr.
 * else
 * 		return NULL.
 * 
 */
char* semeru_cp_rdma_write(int target_mem_server, int write_type , char __user * start_addr, unsigned long size){
	
	int ret = 0;
	int cpu;
	char __user * start_addr_aligned;
	char __user * end_addr_aligned;
	unsigned long size_aligned;
	struct semeru_rdma_queue *rdma_queue;
	struct rdma_session_context *rdma_session = &rdma_session_global; // not support multiple Memory server for now
	struct semeru_rdma_req_sg	*rdma_req_sg;

	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO " %s, write_type 0x%x, start_addr : 0x%lx, size : 0x%lx \n",__func__, write_type, (unsigned long)start_addr, size);
	#endif

	// #1 Do page alignmetn,
	// If the sent data small than a page, align up to a page
	// Because we need to register a whole physical page as RDMA buffer.
	start_addr_aligned 	= (char*)((unsigned long)start_addr & PAGE_MASK); // align_down
	end_addr_aligned	= (char*)(((unsigned long)start_addr + size + PAGE_SIZE -1) & PAGE_MASK); // align_up
	size_aligned	= (unsigned long)(end_addr_aligned - start_addr_aligned);

	cpu = get_cpu(); // disable core preempt

	rdma_queue = &(rdma_session->rdma_queues[cpu]);
  rdma_req_sg = (struct semeru_rdma_req_sg*)kmem_cache_alloc(rdma_queue->rdma_req_sg_cache, GFP_ATOMIC);
  if(unlikely(rdma_req_sg == NULL)){
    pr_err("%s, get reserved rdma_req_sg failed. \n", __func__);
    ret = -1;
    goto out;
  }
	//reset_semeru_rdma_req_sg(rdma_req_sg);
	memset(rdma_req_sg, 0, sizeof(struct semeru_rdma_req_sg));


	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO "%s, rdma_queue[%d], aligned start_addr : 0x%lx, aligned size : 0x%lx \n", 
																__func__, rdma_queue->q_index, (unsigned long)start_addr_aligned, size_aligned);
	#endif

	// 1) Drain all the outstanding requests for a signal write
	if(write_type){  // no-zero
		drain_all_rdma_queue(target_mem_server);
	}

	// 2) build and enqueue the 1-sided rdma_wr
	// [x] Get the rdma_session_context *rdma_session
	// [?] How to confirm the rdma_session_global is fully initialized ?
	ret = semeru_cp_rdma_send(&rdma_session_global, rdma_queue, rdma_req_sg, start_addr_aligned, size_aligned, DMA_TO_DEVICE);
	if(unlikely(ret != 0)){
		printk(KERN_ERR "%s, cp_post_rdma_read failed. \n", __func__);
		start_addr = NULL;
		goto out;
	}

	put_cpu(); // enable core preemtp


	// 1) Wait for the completion.
	// There is no physical page. 
	// So, we just drain the corresponding queue
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ
	
	ret = wait_for_completion_timeout(&(rdma_req_sg->done), msecs_to_jiffies(5)); // 5ms at most. The waiting is un-interrupptible
  if(unlikely( ret == 0)){
    pr_err("%s, rdma_queue[%d] wait for rdma_req_sg timeout for 5ms.\n",__func__, rdma_queue->q_index);
    ret = -1;
    goto out;
  }
	ret = 0; // reset return value to 0.

out :
	return start_addr;

}





/**
 * Reset all the fields 
 */
void reset_semeru_rdma_req_sg(struct semeru_rdma_req_sg* rmem_rdma_cmd_ptr){

	rmem_rdma_cmd_ptr->seq_type = END_TYPE; 
	memset(rmem_rdma_cmd_ptr->sge_list, 0,  MAX_REQUEST_SGL*sizeof(rmem_rdma_cmd_ptr->sge_list));  // works for stack array

	rmem_rdma_cmd_ptr->nentry = 0;
	memset(rmem_rdma_cmd_ptr->sgl, 0, MAX_REQUEST_SGL*sizeof(struct scatterlist)); // heap array
}






/**
 * Semeru CS - RDMA Control-Path initialization
 *  
 * For CP, the source and destination have the same virtual address.
 * e.g.	
 * 		rdma_read(start_addr#01, len)
 * 		src : start_addr#01, len
 * 		dst : start_addr#01, len
 * 		
 * [?] Calculate the corresponding DMA address dynamically ? 
 * 		 Or calculate the corresponding DMA address for meta space at initialization time and time them. 
 * 		 Then we can use these address to build the wr directly. 
 * 
 */
int init_rdma_control_path(struct rdma_session_context *rdma_session){
	int ret = 0;

	/*
	// 2) Allocate and initiate the rdma wr
	//    [x] the rdma_session->cp_rmem_rdma_read/write_cmd->rdma_sq_wr is reused for all the  control-path.
	//				so, assign the addr and length dynamically.
	//        [?] Will this cause problem ?? e.g. the previous write is not finished, and next write begin using this semeru_rdma_req_sg ??
	//
	//    [x] The Control-Path may transfer some big structure, whose physical pages may not contiguous.
	//				Here still needs scatter-gather.
	rdma_session->cp_rmem_rdma_read_cmd = (struct semeru_rdma_req_sg*)kzalloc( sizeof(struct semeru_rdma_req_sg) +  \
																																MAX_REQUEST_SGL*sizeof(struct scatterlist), GFP_KERNEL);
	if(unlikely(rdma_session->cp_rmem_rdma_read_cmd == NULL)){
		printk(KERN_ERR "%s, allocate  cp_rmem_rdma_read_cmd failed.\n",__func__);
		ret = -1;
		goto out;
	}
	reset_semeru_rdma_req_sg(rdma_session->cp_rmem_rdma_read_cmd);

	rdma_session->cp_rmem_rdma_write_cmd = (struct semeru_rdma_req_sg*)kzalloc( sizeof(struct semeru_rdma_req_sg) +  \
																																MAX_REQUEST_SGL*sizeof(struct scatterlist), GFP_KERNEL);
	if(unlikely(rdma_session->cp_rmem_rdma_write_cmd == NULL)){
		printk(KERN_ERR "%s, allocate  cp_rmem_rdma_write_cmd failed.\n",__func__);
		ret = -1;
		goto out;
	}
	reset_semeru_rdma_req_sg(rdma_session->cp_rmem_rdma_write_cmd);


	
	#ifdef DEBUG_MODE_BRIEF
		printk(KERN_INFO "%s, initialize meta space structure done, with rdma_session_context:0x%llx \n",
																											__func__,
																											(uint64_t)rdma_session);

		printk(KERN_INFO "%s, allocate & initialized rdma_session->cp_rmem_rdma_read_cmd: 0x%llx \n", __func__,
																																												(uint64_t)rdma_session->cp_rmem_rdma_read_cmd);

		printk(KERN_INFO "%s, allocate & initialized rdma_session->cp_rmem_rdma_write_cmd: 0x%llx \n", __func__,
																																												(uint64_t)rdma_session->cp_rmem_rdma_write_cmd);
		
	#endif 

*/


out:
	return ret;
}








/**
 * Register module defined functions into kernel.
 *  
 */
void init_kernel_semeru_rdma_ops(void){
	
	#ifndef DEBUG_BD_ONLY // For block device only mode, no need to register the RDMA function
		struct semeru_rdma_ops module_rdma_ops;					 // temporary var
		module_rdma_ops.rdma_read 	= &semeru_cp_rdma_read;   // the address of function is fixed.
		module_rdma_ops.rdma_write 	= &semeru_cp_rdma_write;

		rdma_ops_wrapper(&module_rdma_ops);							 // exported kernel call
	#endif

	return;
}


/**
 * Safely removed the registered rdma operations.
 *  
 */
void reset_kernel_semeru_rdma_ops(void){
	
	#ifndef DEBUG_BD_ONLY
		struct semeru_rdma_ops module_rdma_ops;		// temporary var
		module_rdma_ops.rdma_read 	= NULL;   		// reset to NULL
		module_rdma_ops.rdma_write 	= NULL;

		rdma_ops_wrapper(&module_rdma_ops);				// exported kernel call
	#endif
	
	return;
}


//
// <<<<<<<<<<<<<<<<<<<<<  End of RDMA Control Path <<<<<<<<<<<<<<<<<<<<<
//



//
// >>>>>>>>>>>>>>>  Start of RDMA intialization >>>>>>>>>>>>>>>
//

/**
 * Init the rdma sessions for each memory server.
 * 	One rdma_session_context for each memory server
 *  	one QP for each core (on cpu server)
 * 
 */
int init_rdma_sessions(struct rdma_session_context *rdma_session){
	int ret = 0; 
	char *ip;

	// [Debug] Only support 1 memory server now.

	if(NUM_OF_MEMORY_SERVER != 1){
		printk(KERN_ERR "%s, debug mode for frontswap path. Only support 1 memory server.\n",__func__);
	}

	ip = mem_server_ip;

	// 1) RDMA queue information
  // The number of outstanding wr the QP's send queue and recv queue.
	// For Semeru, the on-the-fly RDMA request should be more than on-the-fly i/o request.
	// Semeru use 1-sided RDMA to transfer data.
	// Only 2-sided RDMA needs to post the recv wr.
	rdma_session->rdma_queues = kzalloc(sizeof(struct semeru_rdma_queue) * online_cores, GFP_KERNEL);
  rdma_session->send_queue_depth = RDMA_SEND_QUEUE_DEPTH + 1;
	rdma_session->recv_queue_depth = RDMA_RECV_QUEUE_DEPTH + 1;


	// 2) Setup socket information
	// Debug : for test, write the ip:port as 10.0.0.2:9400
  rdma_session->port = htons((uint16_t)mem_server_port);  				// transffer to big endian
  ret= in4_pton(ip, strlen(ip), rdma_session->addr, -1, NULL);   // char* to ipv4 address
  if(ret == 0){  		// kernel 4.11.0 , success 1; failed 0.
		printk(KERN_ERR"Assign ip %s to  rdma_session->addr : %s failed.\n",ip, rdma_session->addr );
		goto err;
	}
	rdma_session->addr_type = AF_INET;  //ipv4


err:
	return ret;
}


/**
 * Reserve some rdma_wr, registerred buffer for fast communiction. 
 * 	e.g. 2-sided communication, meta data communication.
 */
int setup_rdma_session_commu_buffer(struct rdma_session_context * rdma_session){

	int ret = 0;

	if(rdma_session->rdma_dev == NULL){
		printk(KERN_ERR "%s, rdma_session->rdma_dev is NULL. too early to regiseter RDMA buffer.\n", __func__);
		goto err;
	}


	// 1) Reserve some buffer for 2-sided RDMA communication
  ret = semeru_setup_buffers(rdma_session);
	if(unlikely(ret)){
		printk(KERN_ERR "%s, Bind DMA buffer error\n", __func__);
		goto err;
	}


	// 2) Control-Path
	ret = init_rdma_control_path(rdma_session);
	if(unlikely(ret)){
		printk(KERN_ERR "%s, initialize 1-sided RDMA buffers error. \n",__func__);
		goto err;
	}

err:
	return ret;
}




/**
 * Build and Connect all the QP for each Session/memory server.
 *  
 */
int semeru_init_rdma_queue(struct rdma_session_context *rdma_session,  int cpu ){
  int ret = 0;
	struct semeru_rdma_queue * rdma_queue = &(rdma_session->rdma_queues[cpu]);


  rdma_queue->rdma_session = rdma_session;
	rdma_queue->q_index = cpu;
  rdma_queue->cm_id = rdma_create_id(&init_net, semeru_rdma_cm_event_handler, rdma_queue, RDMA_PS_TCP, IB_QPT_RC);
  if (IS_ERR(rdma_queue->cm_id)) {
    printk(KERN_ERR "failed to create cm id: %ld\n", PTR_ERR(rdma_queue->cm_id));
    ret = -ENODEV;
		goto err;
  }

  rdma_queue->state = IDLE;
  init_waitqueue_head(&rdma_queue->sem); // Initialize the semaphore.
	spin_lock_init( &(rdma_queue->cq_lock) );		// initialize spin lock
	atomic_set(&(rdma_queue->rdma_post_counter), 0); // Initialize the counter to 0
	rdma_queue->fs_rdma_req_cache = kmem_cache_create("fs_rdma_req_cache", sizeof(struct fs_rdma_req), 0,
                      SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);
	if(unlikely(rdma_queue->fs_rdma_req_cache == NULL)){
		printk(KERN_ERR "%s, allocate rdma_queue->fs_rdma_req_cache failed.\n", __func__);
		ret = -1;
		goto err;
	}

	rdma_queue->rdma_req_sg_cache = kmem_cache_create("rdma_req_sg_cache", sizeof(struct semeru_rdma_req_sg), 0,
                      SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);
	if(unlikely(rdma_queue->rdma_req_sg_cache == NULL)){
		printk(KERN_ERR "%s, allocate rdma_queue->rdma_req_sg_cache failed.\n", __func__);
		ret = -1;
		goto err;
	}


	//2) Resolve address(ip:port) and route to destination IB. 
  ret = rdma_resolve_ip_to_ib_device(rdma_session, rdma_queue);
	if (unlikely(ret)){
		printk (KERN_ERR "%s, bind socket error (addr or route resolve error)\n", __func__);
		return ret;
   	}
 

  return ret;

err:
  rdma_destroy_id(rdma_queue->cm_id);
  return ret;
}






/**
 * Build the RDMA connection to remote memory server.
 * 	
 * Parameters
 * 		rdma_session, RDMA controller/context.
 * 			
 * 
 * More Exlanation:
 * 		[?] This function is too big, it's better to cut it into several pieces.
 * 
 */
int rdma_session_connect(struct rdma_session_context *rdma_session){

	int ret;
	int i;

	// 1) Build and connect all the QPs of this session
	//
	for(i=0; i<online_cores; i++){

		// crete cm_id and other fields e.g. ip
		ret = semeru_init_rdma_queue(rdma_session, i);
		if(unlikely(ret)){
			printk(KERN_ERR "%s,init rdma queue [%d] failed.\n",__func__, i);
		}

		// Create device PD, QP CP
  	ret = semeru_create_rdma_queue(rdma_session, i);
  	if(unlikely(ret)){
			printk(KERN_ERR "%s, Create rdma queues failed. \n", __func__);
  	}

		// Connect to memory server 
		ret = semeru_connect_remote_memory_server(rdma_session, i);
		if(ret){
			printk(KERN_ERR "%s: Connect to remote server error \n", __func__);
			goto err;
		}

		printk(KERN_INFO "%s, RDMA queue[%d] Connect to remote server successfully \n", __func__, i);			
	}




	// 
	// 2) Get the memory pool from memory server.
	//

	// 2.1 Send a request to query available memory for this rdma_session
	//     All the QPs within one session/memory server share the same avaialble buffer
	ret = semeru_query_available_memory(rdma_session);
	if(unlikely(ret)){
		printk("%s, request for chunk failed.\n", __func__);
		goto err;
	}


	// 2.2  Request free memory from Semeru Memory Server
	// 1st Region is the Meta Region,
	// Next are serveral Data Regions.
	ret = semeru_requset_for_chunk(rdma_session, rdma_session->remote_chunk_list.chunk_num);
	if(unlikely(ret)){
		printk("%s, request for chunk failed.\n", __func__);
		goto err;
	}



	// FINISHED.

	// [!!] Only reach here afeter got STOP_ACK signal from remote memory server.
	// Sequence controll - FINISH.
	// Be carefull, this will lead to all the local variables collected.

	// [?] Can we wait here with function : semeru_disconenct_and_collect_resource together?
	//  NO ! https://stackoverflow.com/questions/16163932/multiple-threads-can-wait-on-a-semaphore-at-same-time
	// Use  differeent semapore signal.
	//wait_event_interruptible(rdma_session->sem, rdma_session->state == CM_DISCONNECT);

	//wait_event_interruptible( rdma_session->sem, rdma_session->state == TEST_DONE );


	printk("%s,Exit the main() function with built RDMA conenction rdma_session_context:0x%llx .\n", 
																																											__func__,
																																											(uint64_t)rdma_session);

	return ret;

err:
	printk(KERN_ERR "ERROR in %s \n", __func__);
	return ret;
}




/**
 * >>>>>>>>>>>>>>> Start of Resource Free Functions >>>>>>>>>>>>>>>
 * 
 * For kernel space, there is no concept of multi-processes. 
 * There is only multilple kernel threads which share the same kernel virtual memory address.
 * So it's the thread's resposibility to free the allocated memory in the kernel heap.
 * 
 * 
 * 
 */



/**
 * Free the RDMA buffers.
 * 
 * 	1) 2-sided DMA buffer
 * 	2) mapped remote chunks.
 * 
 */
void semeru_free_buffers(struct rdma_session_context *rdma_session) {

	// Free the DMA buffer for 2-sided RDMA messages
	if(rdma_session == NULL)
		return;

	// Free 1-sided dma buffers
	if(rdma_session->rdma_recv_req.recv_buf != NULL)
		kfree(rdma_session->rdma_recv_req.recv_buf);
	if(rdma_session->rdma_send_req.send_buf != NULL)
		kfree(rdma_session->rdma_send_req.send_buf);



	// Free the remote chunk management,
	if(rdma_session->remote_chunk_list.remote_chunk != NULL)
		kfree(rdma_session->remote_chunk_list.remote_chunk);

	#ifdef DEBUG_MODE_BRIEF
		printk("%s, Free RDMA buffers done. \n",__func__);
	#endif

}


/**
 * Free InfiniBand related structures.
 * 
 * rdma_cm_id : the main structure to maintain the IB.
 * 		
 * 
 * 
 */
void semeru_free_rdma_structure(struct rdma_session_context *rdma_session){

	struct semeru_rdma_queue * rdma_queue;
	int i;

	if (rdma_session == NULL)
		return;

	// Free each QP
	for(i=0; i<online_cores; i++){

		rdma_queue = &(rdma_session->rdma_queues[i]);

		if(rdma_queue->cm_id != NULL){
			rdma_destroy_id(rdma_queue->cm_id);

			#ifdef DEBUG_MODE_BRIEF
			printk("%s, free rdma_queue[%d] rdma_cm_id done. \n",__func__, i);
			#endif
		}

		if(rdma_queue->qp != NULL){
			ib_destroy_qp(rdma_queue->qp);

			#ifdef DEBUG_MODE_BRIEF
			printk("%s, free rdma_queue[%d] ib_qp  done. \n",__func__, i);
			#endif
		}

		// 
		// Both send_cq/recb_cq should be freed in ib_destroy_qp() ?
		//
		if(rdma_queue->cq != NULL){
		ib_destroy_cq(rdma_queue->cq);

			#ifdef DEBUG_MODE_BRIEF
			printk("%s, free rdma_queue[%d] ib_cq  done. \n",__func__, i);
			#endif
		}

	}// end of free RDMA QP

	// Before invoke this function, free all the resource binded to pd.
	if(rdma_session->rdma_dev->pd != NULL){
		ib_dealloc_pd(rdma_session->rdma_dev->pd); 

		#ifdef DEBUG_MODE_BRIEF
		printk("%s, Free device PD  done. \n",__func__);
		#endif
	}

	#ifdef DEBUG_MODE_BRIEF
	printk("%s, Free RDMA structures,cm_id,qp,cq,pd done. \n",__func__);
	#endif

}


/**
 * The main entry of resource free.
 * 
 * [x] 2 call site.
 * 		1) Called by client, at the end of function, octopus_rdma_client_cleanup_module().
 * 		2) Triggered by DISCONNECT CM event, in semeru_rdma_cm_event_handler()
 * 
 */
int semeru_disconenct_and_collect_resource(struct rdma_session_context *rdma_session){

	int ret = 0;
	int i;
	struct semeru_rdma_queue * rdma_queue;


	// 1) Disconnect each QP
	for(i=0; i< online_cores; i++){
		rdma_queue = &(rdma_session->rdma_queues[i]);

		if(unlikely(rdma_queue->freed != 0)){
			// already called by some thread,
			// just return and wait.
			printk(KERN_WARNING "%s, rdma_queue[%d] already freed. \n",__func__, i);
			continue;
		}
		rdma_queue->freed++;

		// The RDMA connection maybe already disconnected.
		if(rdma_queue->state != CM_DISCONNECT){
			ret = rdma_disconnect(rdma_queue->cm_id);
			if(ret){
				printk(KERN_ERR "%s, RDMA disconnect failed. \n",__func__);
				goto err;
			}

			// wait the ack of RDMA disconnected successfully
			wait_event_interruptible(rdma_queue->sem, rdma_queue->state == CM_DISCONNECT); 
		}

		#ifdef DEBUG_MODE_BRIEF
			printk("%s, RDMA queue[%d] disconnected, start to free resoutce. \n", __func__, i);
		#endif
	}




	// 2) Free resouces
	semeru_free_buffers(rdma_session);
	semeru_free_rdma_structure(rdma_session);

	// If not allocated by kzalloc, no need to free it.
	//kfree(rdma_session);  // Free the RDMA context.
	

	// DEBUG -- 
	// Exit the main function first.
	// wake_up_interruptible will cause context switch, 
	// Just skip the code below this invocation.
	//rdma_session_global.state = TEST_DONE;
	//wake_up_interruptible(&(rdma_session_global.sem));


	#ifdef DEBUG_MODE_BRIEF
	printk("%s, RDMA memory resouce freed. \n", __func__);
	#endif

err:
	return ret;
}


/**
 * <<<<<<<<<<<<<<<<<<<<< End of  Resource Free Functions <<<<<<<<<<<<<<<<<<<<<
 */




//
// Kernel Module registration functions.
//



// invoked by insmod 
int  semeru_fs_rdma_client_init(void){

	int ret = 0;
	//printk("Do nothing for now. \n");
	printk(KERN_INFO "%s, start \n",__func__);

	// Initialize the RDMA control path, provided by the RDMA driver.
	init_kernel_semeru_rdma_ops();

	// online cores decide the parallelism. e.g. number of QP, CP etc.
	online_cores = num_online_cpus();
	printk(KERN_INFO "%s, online_cores : %d (Can't exceed the slots on Memory server) \n", __func__, online_cores);

	// init the rdma session to memory server
	ret = init_rdma_sessions(&rdma_session_global);

	// Build both the RDMA and Disk driver
	ret = rdma_session_connect(&rdma_session_global);
	if(unlikely(ret)){
		printk(KERN_ERR "%s, rdma_session_connect failed. \n", __func__);
		goto out;
	}


	// Enable the frontswap path
	ret = semeru_init_frontswap();
	if(unlikely(ret)){
		printk(KERN_ERR "%s, Enable frontswap path failed. \n", __func__);
		goto out;
	}


out:
	return ret;
}


// invoked by rmmod 
void  semeru_fs_rdma_client_exit(void){
  
	int ret = 0;
	
	// 1) rest control path
	reset_kernel_semeru_rdma_ops();

	// 2) disconect rdma connction
	ret = semeru_disconenct_and_collect_resource(&rdma_session_global);
	if(unlikely(ret)){
		printk(KERN_ERR "%s,  failed.\n",  __func__);
	}


	// 3) disconnect fontswap path
	semeru_exit_frontswap();

	printk(KERN_INFO "%s done.\n",__func__);

	return;
}






/**
 * >>>>>>>>>>>>>>> Start of Debug functions >>>>>>>>>>>>>>>
 *
 */


//
// Print the RDMA Communication Message 
//
char* rdma_cm_message_print(int cm_message_id){
	char* message_type_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes

	switch(cm_message_id){
		case 0:
			strcpy(message_type_name,  "RDMA_CM_EVENT_ADDR_RESOLVED");
			break;
		case 1:
			strcpy(message_type_name, "RDMA_CM_EVENT_ADDR_ERROR");
			break;
		case 2:
			strcpy(message_type_name, "RDMA_CM_EVENT_ROUTE_RESOLVED");
			break;
		case 3:
			strcpy(message_type_name, "RDMA_CM_EVENT_ROUTE_ERROR");
			break;
		case 4:
			strcpy(message_type_name, "RDMA_CM_EVENT_CONNECT_REQUEST");
			break;
		case 5:
			strcpy(message_type_name, "RDMA_CM_EVENT_CONNECT_RESPONSE");
			break;
		case 6:
			strcpy(message_type_name, "RDMA_CM_EVENT_CONNECT_ERROR");
			break;
		case 7:
			strcpy(message_type_name, "RDMA_CM_EVENT_UNREACHABLE");
			break;
		case 8:
			strcpy(message_type_name, "RDMA_CM_EVENT_REJECTED");
			break;
		case 9:
			strcpy(message_type_name, "RDMA_CM_EVENT_ESTABLISHED");
			break;
		case 10:
			strcpy(message_type_name, "RDMA_CM_EVENT_DISCONNECTED");
			break;
		case 11:
			strcpy(message_type_name, "RDMA_CM_EVENT_DEVICE_REMOVAL");
			break;
		case 12:
			strcpy(message_type_name, "RDMA_CM_EVENT_MULTICAST_JOIN");
			break;
		case 13:
			strcpy(message_type_name, "RDMA_CM_EVENT_MULTICAST_ERROR");
			break;
		case 14:
			strcpy(message_type_name, "RDMA_CM_EVENT_ADDR_CHANGE");
			break;
		case 15:
			strcpy(message_type_name, "RDMA_CM_EVENT_TIMEWAIT_EXIT");
			break;
		default:
			strcpy(message_type_name, "ERROR Message Type");
			break;
	}

	return message_type_name;
}


/**
 * wc.status name
 * 
 */
char* rdma_wc_status_name(int wc_status_id){
	char* message_type_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes

	switch(wc_status_id){
		case 0:
			strcpy(message_type_name, "IB_WC_SUCCESS");
			break;
		case 1:
			strcpy(message_type_name, "IB_WC_LOC_LEN_ERR");
			break;
		case 2:
			strcpy(message_type_name, "IB_WC_LOC_QP_OP_ERR");
			break;
		case 3:
			strcpy(message_type_name, "IB_WC_LOC_EEC_OP_ERR");
			break;
		case 4:
			strcpy(message_type_name, "IB_WC_LOC_PROT_ERR");
			break;
		case 5:
			strcpy(message_type_name, "IB_WC_WR_FLUSH_ERR");
			break;
		case 6:
			strcpy(message_type_name, "IB_WC_MW_BIND_ERR");
			break;
		case 7:
			strcpy(message_type_name, "IB_WC_BAD_RESP_ERR");
			break;
		case 8:
			strcpy(message_type_name, "IB_WC_LOC_ACCESS_ERR");
			break;
		case 9:
			strcpy(message_type_name, "IB_WC_REM_INV_REQ_ERR");
			break;
		case 10:
			strcpy(message_type_name, "IB_WC_REM_ACCESS_ERR");
			break;
		case 11:
			strcpy(message_type_name, "IB_WC_REM_OP_ERR");
			break;
		case 12:
			strcpy(message_type_name, "IB_WC_RETRY_EXC_ERR");
			break;
		case 13:
			strcpy(message_type_name, "IB_WC_RNR_RETRY_EXC_ERR");
			break;
		case 14:
			strcpy(message_type_name, "IB_WC_LOC_RDD_VIOL_ERR");
			break;
		case 15:
			strcpy(message_type_name, "IB_WC_REM_INV_RD_REQ_ERR");
			break;
		case 16:
			strcpy(message_type_name, "IB_WC_REM_ABORT_ERR");
			break;
		case 17:
			strcpy(message_type_name, "IB_WC_INV_EECN_ERR");
			break;
		case 18:
			strcpy(message_type_name, "IB_WC_INV_EEC_STATE_ERR");
			break;
		case 19:
			strcpy(message_type_name, "IB_WC_FATAL_ERR");
			break;
		case 20:
			strcpy(message_type_name, "IB_WC_RESP_TIMEOUT_ERR");
			break;
		case 21:
			strcpy(message_type_name, "IB_WC_GENERAL_ERR");
			break;
		default:
			strcpy(message_type_name, "ERROR Message Type");
			break;
	}

	return message_type_name;
}



/**
 * The message type name, used for 2-sided RDMA communication.
 */
char* rdma_message_print(int message_id){
	char* message_type_name;
	message_type_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes

	switch(message_id){

		case 1:
			strcpy(message_type_name, "DONE");
			break;

		case 2:
			strcpy(message_type_name, "GOT_CHUNKS");
			break;

		case 3:
			strcpy(message_type_name, "GOT_SINGLE_CHUNK");
			break;

		case 4:
			strcpy(message_type_name, "FREE_SIZE");
			break;

		case 5:
			strcpy(message_type_name, "EVICT");
			break;

		case 6:
			strcpy(message_type_name, "ACTIVITY");
			break;

		case 7:
			strcpy(message_type_name, "STOP");
			break;

		case 8:
			strcpy(message_type_name, "REQUEST_CHUNKS");
			break;

		case 9:
			strcpy(message_type_name, "REQUEST_SINGLE_CHUNK");
			break;

		case 10:
			strcpy(message_type_name, "QUERY");
			break;

		case 11 :
			strcpy(message_type_name, "AVAILABLE_TO_QUERY");
			break;

		default:
			strcpy(message_type_name, "ERROR Message Type");
			break;
	}

	return message_type_name;
}



// Print the string of rdma_session_context state.
// void rdma_session_context_state_print(int id){
char* rdma_session_context_state_print(int id){

	char* rdma_seesion_state_name;
	rdma_seesion_state_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes.

	switch (id){

		case 1 :
			strcpy(rdma_seesion_state_name, "IDLE");
			break;
		case 2 :
			strcpy(rdma_seesion_state_name, "CONNECT_REQUEST");
			break;
		case 3 :
			strcpy(rdma_seesion_state_name, "ADDR_RESOLVED");
			break;
		case 4 :
			strcpy(rdma_seesion_state_name, "ROUTE_RESOLVED");
			break;
		case 5 :
			strcpy(rdma_seesion_state_name, "CONNECTED");
			break;
		case 6 :
			strcpy(rdma_seesion_state_name, "FREE_MEM_RECV");
			break;
		case 7 :
			strcpy(rdma_seesion_state_name, "RECEIVED_CHUNKS");
			break;
		case 8 :
			strcpy(rdma_seesion_state_name, "RDMA_BUF_ADV");
			break;
		case 9 :
			strcpy(rdma_seesion_state_name, "WAIT_OPS");
			break;
		case 10 :
			strcpy(rdma_seesion_state_name, "RECV_STOP");
			break;
		case 11 :
			strcpy(rdma_seesion_state_name, "RECV_EVICT");
			break;
		case 12 :
			strcpy(rdma_seesion_state_name, "RDMA_WRITE_RUNNING");
			break;
		case 13 :
			strcpy(rdma_seesion_state_name, "RDMA_READ_RUNNING");
			break;
		case 14 :
			strcpy(rdma_seesion_state_name, "SEND_DONE");
			break;
		case 15 :
			strcpy(rdma_seesion_state_name, "RDMA_DONE");
			break;
		case 16 :
			strcpy(rdma_seesion_state_name, "RDMA_READ_ADV");
			break;
		case 17 :
			strcpy(rdma_seesion_state_name, "RDMA_WRITE_ADV");
			break;
		case 18 :
			strcpy(rdma_seesion_state_name, "CM_DISCONNECT");
			break;
		case 19 :
			strcpy(rdma_seesion_state_name, "ERROR");
			break;

		default :
			strcpy(rdma_seesion_state_name, "Un-defined state.");
			break;
	}

	return rdma_seesion_state_name;
}



/**
 * print the information of this scatterlist
 */
void  print_scatterlist_info(struct scatterlist* sl_ptr , int nents ){

  int i;

  printk(KERN_INFO "\n %s, %d entries , Start\n", __func__, nents);

  for(i=0; i< nents; i++){
   // if(sl_ptr[i] != NULL){   // for array[N], the item can't be NULL.
      printk(KERN_INFO "%s, \n page_link(struct page*) : 0x%lx \n  offset : 0x%x bytes\n  length : 0x%x bytes \n dma_addr : 0x%llx \n ",
                        __func__, sl_ptr[i].page_link, sl_ptr[i].offset, sl_ptr[i].length, sl_ptr[i].dma_address );
  //  }
  }

  printk(KERN_INFO " %s End\n\n", __func__);

}


/**
 * <<<<<<<<<<<<<<<<<<<<< End of Debug Functions <<<<<<<<<<<<<<<<<<<<<
 */





