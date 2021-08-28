/**
 * Register Frontswap module.
 * Frontswap is a separate module with the old semeru going through the block layer.
 * 
 * Forward the swapped out page go into frontswap path.
 *  
 */




#include "frontswap_path.h"
#include "local_dram.h"
#include "semeru_cpu.h"



//
// ############################ Start of RDMA operation for Fronswap ############################
//

/**
 * Wait for the finish of ALL the outstanding rdma_request
 *  
 * More explanation
 *  Make sure disable preempt before invoking this function ? 
 */
void drain_rdma_queue(struct semeru_rdma_queue *rdma_queue)
{
	unsigned long flags;

	while (atomic_read(&rdma_queue->rdma_post_counter) > 0) {
		spin_lock_irqsave(&rdma_queue->cq_lock, flags);
		//  default, IB_POLL_BATCH is 16. return when cqe reaches min(16, IB_POLL_BATCH) or CQ is empty.
		ib_process_cq_direct(rdma_queue->cq, 16);
		spin_unlock_irqrestore(&rdma_queue->cq_lock, flags); // [?] Is the spin lock necessary ?
		cpu_relax(); // insert PAUSE, good for HT cores
	}

	return;
}

/**
 * Drain all the outstanding messages for a specific memory server.
 * [?] TO BE DONE. Multiple memory server 
 * 
 */
void drain_all_rdma_queue(int target_mem_server)
{
	int i;
	struct rdma_session_context *rdma_session = &rdma_session_global_ptr[target_mem_server];

	for (i = 0; i < online_cores; i++) {
		drain_rdma_queue(&(rdma_session->rdma_queues[i]));
	}
}

/**
 * The function to process rdma write done.
 * 
 */
void fs_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc){
	// get the instance start address of fs_rdma_req, whose filed, fs_rdma_req->cqe is pointed by wc->wr_cqe
	struct fs_rdma_req *rdma_req = container_of(wc->wr_cqe, struct fs_rdma_req, cqe);  
	struct semeru_rdma_queue *rdma_queue = cq->cq_context;
	struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("%s status is not success, it is=%d\n", __func__, wc->status);
	}
	ib_dma_unmap_page(ibdev, rdma_req->dma_addr, PAGE_SIZE, DMA_TO_DEVICE);

	atomic_dec(&rdma_queue->rdma_post_counter); // decrease outstanding rdma request counter
	complete(&rdma_req->done);  // inform caller, write is done. is this necessary for a write?
}



void fs_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc){
	// get the instance start address of fs_rdma_req, whose filed, fs_rdma_req->cqe is pointed by wc->wr_cqe
	struct fs_rdma_req *rdma_req = container_of(wc->wr_cqe, struct fs_rdma_req, cqe);  
	struct semeru_rdma_queue *rdma_queue = cq->cq_context;
	struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("%s status is not success, it is=%d\n", __func__, wc->status);
	}
	ib_dma_unmap_page(ibdev, rdma_req->dma_addr, PAGE_SIZE, DMA_FROM_DEVICE);


	atomic_dec(&rdma_queue->rdma_post_counter); // decrease outstanding rdma request counter
	//SetPageUptodate(rdma_req->page);  // [?] will be invoked in swap_readpage(). no need to do it here ?
	//unlock_page(rdma_req->page);
	complete(&rdma_req->done);  // inform caller, write is done. is this necessary for a write?
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
int fs_enqueue_send_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue,
		       struct fs_rdma_req *rdma_req)
{
	int ret = 0;
	const struct ib_send_wr *bad_wr;
	int test;

	rdma_req->rdma_queue = rdma_queue; // points to the rdma_queue to be enqueued.

	// Post 1-sided RDMA read wr
	// wait and enqueue wr
	// Both 1-sided read/write queue depth are RDMA_SEND_QUEUE_DEPTH
	while (1) {
		test = atomic_inc_return(&rdma_queue->rdma_post_counter);
		if (test < RDMA_SEND_QUEUE_DEPTH - 16) {
			//post the 1-sided RDMA write
			// Use the global RDMA context, rdma_session_global
			ret = ib_post_send(rdma_queue->qp, (struct ib_send_wr *)&rdma_req->rdma_wr, &bad_wr);
			if (unlikely(ret)) {
				printk(KERN_ERR "%s, post 1-sided RDMA send wr failed, return value :%d. counter %d \n",
				       __func__, ret, test);
				ret = -1;
				goto err;
			}

			// Enqueue successfully.
			// exit loop.
			return ret;
		} else {
			// RDMA send queue is full, wait for next turn.
			test = atomic_dec_return(&rdma_queue->rdma_post_counter);
			//schedule(); // release the core for a while.
			// cpu_relax(); // which one is better ?

			// IB_DIRECT_CQ, poll cqe directly
			drain_rdma_queue(rdma_queue);
		}

	} // end of while, try to enqueue read wr.

err:
	printk(KERN_ERR " Error in %s \n", __func__);
	return -1;
}

/**
 * Build a rdma_wr for frontswap data path.
 *  
 */
int dp_build_fs_rdma_wr(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue, struct fs_rdma_req *rdma_req,
		struct remote_mapping_chunk  *remote_chunk_ptr, size_t offset_within_chunk, struct page *page, enum dma_data_direction dir){

	int ret = 0;
	struct ib_device * dev = rdma_session->rdma_dev->dev;


	//debug
	//printk(KERN_INFO"%s, build wr for remote_mapping_chunk 0x%lx, offset_within_chunk 0x%lx \n", __func__, (size_t)remote_chunk_ptr->remote_addr, offset_within_chunk);

	// 1) Map a single page as RDMA buffer
	rdma_req->page = page;
	init_completion( &(rdma_req->done) );

	rdma_req->dma_addr = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if ( unlikely(ib_dma_mapping_error(dev, rdma_req->dma_addr)) ){
		pr_err("%s, ib_dma_mapping_error\n",__func__);
		ret = -ENOMEM;
		kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, rdma_req->dma_addr, PAGE_SIZE, dir); // Map the dma address to IB deivce.
	
	if(dir == DMA_TO_DEVICE){
		rdma_req->cqe.done = fs_rdma_write_done; // rdma cqe process function
	}else{
		rdma_req->cqe.done = fs_rdma_read_done;
	}

	// 2) Initialize the rdma_wr
	// 2.1 local addr
	rdma_req->sge.addr    = rdma_req->dma_addr;
	rdma_req->sge.length  = PAGE_SIZE;
	rdma_req->sge.lkey    = rdma_session->rdma_dev->pd->local_dma_lkey;

	// 2.2 initia rdma_wr for remote addr
	rdma_req->rdma_wr.wr.next    = NULL;
	rdma_req->rdma_wr.wr.wr_cqe  = &rdma_req->cqe;  // assing completion handler. prepare for container_of()
	rdma_req->rdma_wr.wr.sg_list = &(rdma_req->sge);
	rdma_req->rdma_wr.wr.num_sge = 1; // single page.  [?] how to support mutiple pages ?
	rdma_req->rdma_wr.wr.opcode  = (dir == DMA_TO_DEVICE ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ);
	rdma_req->rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_req->rdma_wr.remote_addr =  remote_chunk_ptr->remote_addr + offset_within_chunk; 
	rdma_req->rdma_wr.rkey = remote_chunk_ptr->remote_rkey;


	//debug
	#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
	if(dir == DMA_FROM_DEVICE){
		printk(KERN_INFO "%s, read data from remote 0x%lx, size 0x%lx \n", __func__, (size_t)rdma_req->rdma_wr.remote_addr, (size_t)PAGE_SIZE);
	}
	#endif

out:
	return ret;
}






/**
 * Enqueue a page into RDMA queue.
 *  
 */
int semeru_fs_rdma_send(struct rdma_session_context *rdma_session, struct semeru_rdma_queue *rdma_queue, struct fs_rdma_req *rdma_req, 
		struct remote_mapping_chunk *remote_chunk_ptr, size_t offset_within_chunk, struct page *page, enum dma_data_direction dir ){

	int ret = 0;

	// initialize the rdma_req
	ret = dp_build_fs_rdma_wr(rdma_session, rdma_queue, rdma_req, remote_chunk_ptr, offset_within_chunk, page, dir);
	if(unlikely(ret)){
		pr_err("%s, Build rdma_wr failed.\n",__func__);
		goto out;
	}

	// enqueue the rdma_req
	ret = fs_enqueue_send_wr(rdma_session, rdma_queue, rdma_req);
	if(unlikely(ret)){
		pr_err("%s, enqueue rdma_wr failed.\n",__func__);
		goto out;
	}

out:
	return ret;
}








//
// ############################ Start of Fronswap operations definition ############################
//

/**
 * @brief Get the memory server id. 
 * 	The memory server id is decided by the chunk index.
 * 	The mapping is defined in include/linux/swap_global_struct.h
 * 
 * 	Warning : alreays reserve the first chunk in each memory server.
 * 
 * @return int , the id of memory server.
 */
static inline int get_memory_server_id(size_t chunk_index)
{
	return (int)(chunk_index/DATA_REGION_PER_MEM_SERVER);
}


/**
 * @brief Translate the CPU serve virtual address to memory server virtual address.
 * 	1) Reserve the first region for each memory server as meta region. 
 * 	Never write any data to meta region via data path.
 * 	2) This function is only used by both Data path and Control path.
 * 	3) This function is only used for data region area.
 * 
 * Examples of Memory region
 * 	// | -- Meta Region -- | -- Data Regsons --|
 * 
 * 	There are 2 translation path
 * 	1) via the swap_entry_t, 
 * 	CPU server virtual addr -> swap_entry_t => memory server virtual addr.
 * 	Data on both meta regions and data regions can be swapped out to memory servers.
 * 	In theory, no need to reserve the meta Regions in memory server.
 * 	We skip the meta regions here only to prevent writing irrelevant signals to crash the memory servers.
 * 	
 * 	2) via the  swap_remmaping table,
 * 	CPU server virtual addr -> swap_entry_t -> CPU server_virtual addr => memory server virtual addr.
 * 	Because Only data on data regions can be swapped out. 
 * 	The  swap_remmaping table count start from the first page in Data region.
 * 	So, we also need to add the RDMA_META_REGION_NUM back when calcualting the region index.
 * 
 * @param mem_addr 
 * @param swap_entry_offset 
 */
void translate_to_mem_server_addr(struct mem_server_addr * mem_addr, pgoff_t swap_entry_offset)
{
	
	// page offset, compared start of Data Region
	// The real virtual address is RDMA_DATA_SPACE_START_ADDR + start_addr.
#ifdef ENABLE_SWP_ENTRY_VIRT_REMAPPING
	// byte address offset to the RDMA_DATA_SPACE_START_ADDR 
	size_t start_addr = retrieve_swap_remmaping_virt_addr_via_offset(swap_entry_offset) << PAGE_SHIFT; 
#else
	// For the default kernel, no need to do the swp_offset -> virt translation
	size_t start_addr = swap_entry_offset << PAGE_SHIFT;
#endif

	size_t start_chunk_index = start_addr >> CHUNK_SHIFT; // absolute data chunk index
	size_t offset_within_chunk = start_addr & CHUNK_MASK;

	//debug
	//pr_warn("%s, for swap_entry 0x%lx , start_addr 0x%lx, start_chunk_index %lu, offset 0x%lx \n",
	//	__func__, swap_entry_offset, start_addr, start_chunk_index, offset_within_chunk);

	// Calculate the target memory server
	mem_addr->mem_server_id = get_memory_server_id(start_chunk_index);
	// calculate chunk index within the memory server
	mem_addr->mem_server_chunk_index = start_chunk_index - (mem_addr->mem_server_id * DATA_REGION_PER_MEM_SERVER);
	// skip the meta regions for both translation paths.
	mem_addr->mem_server_chunk_index += RDMA_META_REGION_NUM;
	mem_addr->mem_server_offset_within_chunk = offset_within_chunk;

}

/**
 * Synchronously write data to memory server.
 *  
 *  1.swap out is single pages in default. 
 *    [?]  Can we make it support multiple pages swapout ?
 * 
 *  2. This is a synchronous swapping out. Return only when pages is written out.
 *     This is the assumption of frontswap store operation.
 * 
 * Parameters
 *  type : used to select the swap device ?
 *  page_offset : swp_offset(entry). the offset for a page in the swap partition/device.
 *  page : the handler of page.
 * 
 * 
 * return
 *  0 : success
 *  non-zero : failed.
 * 
 */
int semeru_frontswap_store(unsigned type, pgoff_t swap_entry_offset, struct page *page)
{
	int ret = 0;
	int cpu;
	struct fs_rdma_req *rdma_req;
	struct semeru_rdma_queue *rdma_queue;
	struct rdma_session_context *rdma_session;
	struct remote_mapping_chunk *remote_chunk_ptr;
	struct mem_server_addr mem_addr;

	//debug - before translation
	//pr_warn("%s, store page 0x%lx, swap_entry 0x%lx \n", __func__, (size_t)page,  swap_entry_offset);

	// 1) Translate swap index to memory server address
	// page offset, compared start of Data Region
	translate_to_mem_server_addr(&mem_addr, swap_entry_offset);

	// debug - after translation
	//pr_warn("%s, for swap_entry 0x%lx mem_server_id %d, chunk index %lu, offset 0x%lx \n", 
	//	__func__, swap_entry_offset, mem_addr.mem_server_id,  mem_addr.mem_server_chunk_index, mem_addr.mem_server_offset_within_chunk);

	
#ifdef RDMA_MESSAGE_PROFILING
	rdma_write_to_mem_server_inc(mem_addr.mem_server_id);	
	periodically_print_info("RDMA store");

	//debug
	//pr_warn("%s, page swap_entry 0x%lx , memory_server[%d], chunk_index 0x%lx, offset 0x%lx",
	//	__func__, swap_entry_offset, mem_addr.mem_server_id, mem_addr.mem_server_chunk_index, mem_addr.mem_server_offset_within_chunk );

#endif

#ifdef DEBUG_FRONTSWAP_ONLY
	// 1) Local dram path
	ret = semeru_dram_write(page, swap_entry_offset << PAGE_SHIFT); // only return after copying is done.
	if (unlikely(ret)) {
		pr_err("could not read page remotely\n");
		goto out;
	}

#else
	// 2) RDMA path
	rdma_session = &rdma_session_global_ptr[mem_addr.mem_server_id];
	
	cpu = get_cpu(); // disable preempt
	//cpu = smp_processor_id(); // if already disabled the preempt in caller, use this one

	// 2.1 get the rdma queue and remote chunk
	rdma_queue = &(rdma_session->rdma_queues[cpu]);
	rdma_req = (struct fs_rdma_req *)kmem_cache_alloc(rdma_queue->fs_rdma_req_cache, GFP_ATOMIC);
	if (unlikely(rdma_req == NULL)) {
		pr_err("%s, get reserved fs_rdma_req failed. \n", __func__);
		ret = -1;
		goto out;
	}

	remote_chunk_ptr = &(rdma_session->remote_chunk_list.remote_chunk[mem_addr.mem_server_chunk_index]);

	// TO BE DONE
	// Warning : The data in Meta Region can be swapped out.
	// We keep some useless data in the Meta Region.
	// Swap out them to memory server can save the CPU server local cache.
	
	//debug - swap into meta region
	// if( start_addr + SEMERU_START_ADDR <  RDMA_DATA_SPACE_START_ADDR ){
	//   printk(KERN_INFO"%s, meta region start_addr 0x%lx, offset_within_chunk 0x%lx. start_chunk_index 0x%lx, remote_chunk_ptr 0x%lx\n",
	//         __func__, start_addr + SEMERU_START_ADDR, offset_within_chunk, start_chunk_index, (size_t)remote_chunk_ptr );
	// }
	// if( start_addr + SEMERU_START_ADDR <  SEMERU_START_ADDR + ALIVE_BITMAP_SIZE  ){ // start_addr is an offset.
	//   printk(KERN_ERR"%s, alive_bitmap zone, start_addr 0x%lx, offset_within_chunk 0x%lx. start_chunk_index 0x%lx, remote_chunk_ptr 0x%lx\n",
	//         __func__, start_addr + SEMERU_START_ADDR, offset_within_chunk, start_chunk_index, (size_t)remote_chunk_ptr );
	// }

	// end of debug

	// 2.2 enqueue RDMA request
	ret = semeru_fs_rdma_send(rdma_session, rdma_queue, rdma_req, remote_chunk_ptr, mem_addr.mem_server_offset_within_chunk, page,
				  DMA_TO_DEVICE);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n", __func__);
		goto out;
	}

#ifdef DEBUG_MODE_DETAIL
	//pr_info("%s,  rdma_queue[%d] store page 0x%lx, virt addr 0x%lx, swp_offset 0x%lx >>>>> \n",
	//                    __func__, rdma_queue->q_index, (size_t)page, (size_t)(RDMA_DATA_SPACE_START_ADDR + start_addr), (size_t)swap_entry_offset );

	// Enable swap-out of Meta Region
	pr_info("%s,  rdma_queue[%d] store page 0x%lx, virt addr 0x%lx, swp_offset 0x%lx >>>>> \n", __func__,
		rdma_queue->q_index, (size_t)page, (size_t)(SEMERU_START_ADDR + start_addr), (size_t)swap_entry_offset);
#endif

	put_cpu(); // enable preeempt.

	// 2.3 wait for write is done.

	// busy wait on the rdma_queue[cpu].
	// This is not exclusive.
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ

	// 3) wait for the finish of current fs_rdma_req
	//  [??] uninterruptible is good. drain_rdma_queue() already processed all the outstanding rdma requests
	// 5ms at most. The waiting is un-interrupptible
	ret = wait_for_completion_timeout(&(rdma_req->done), msecs_to_jiffies(5)); 
	if (unlikely(ret == 0)) {
		pr_err("%s, wait for rdma_req timeout for 5ms.\n", __func__);
		ret = -1;
		goto out;
	}

	kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req); // safe to free
	ret = 0; // reset to 0 for succss.

#ifdef DEBUG_MODE_DETAIL
	pr_info("%s, rdma_queue[%d] store page 0x%lx, virt addr 0x%lx DONE <<<<< \n", __func__, rdma_queue->q_index,
		(size_t)page, start_addr);
#endif

#endif

out:
	return ret;
}

/**
 * Synchronously read data from memory server.
 * 
 * 
 * return:
 *  0 : success
 *  non-zero : failed.
 */
int semeru_frontswap_load(unsigned type, pgoff_t swap_entry_offset, struct page *page)
{
	int ret = 0;
	int cpu;
	struct fs_rdma_req *rdma_req;
	struct semeru_rdma_queue *rdma_queue;
	//struct rdma_session_context *rdma_session = &rdma_session_global_ptr[0];
	struct rdma_session_context *rdma_session;

	struct remote_mapping_chunk *remote_chunk_ptr;
	struct mem_server_addr mem_addr;

	// 1) Translate swap index to memory server address
	translate_to_mem_server_addr(&mem_addr, swap_entry_offset);

#ifdef RDMA_MESSAGE_PROFILING
	rdma_read_from_mem_server_inc(mem_addr.mem_server_id);	
	periodically_print_info("RDMA load");

	//debug
	//pr_warn("%s, page swap_entry 0x%lx , memory_server[%d], chunk_index 0x%lx, offset 0x%lx",
	//	__func__, swap_entry_offset, mem_addr.mem_server_id, mem_addr.mem_server_chunk_index, mem_addr.mem_server_offset_within_chunk );

#endif

#ifdef DEBUG_FRONTSWAP_ONLY
	ret = semeru_dram_read(page, swap_entry_offset << PAGE_SHIFT);
	if (unlikely(ret)) {
		pr_err("could not read page remotely\n");
		goto out;
	}

#else
	// 2) RDMA path
	rdma_session = &rdma_session_global_ptr[mem_addr.mem_server_id];

	cpu = get_cpu(); // disable preempt

	// 2.1 get the rdma queue and remote chunk
	rdma_queue = &(rdma_session->rdma_queues[cpu]);
	rdma_req = (struct fs_rdma_req *)kmem_cache_alloc(rdma_queue->fs_rdma_req_cache, GFP_ATOMIC);
	if (unlikely(rdma_req == NULL)) {
		pr_err("%s, get reserved fs_rdma_req failed. \n", __func__);
		ret = -1;
		goto out;
	}

	// TO BE DONE
	// Warning : The data in Meta Region can NOT be swapped out right now.
	// We keep some useless data in the Meta Region.
	// Swap out them to memory server can save the CPU server local cache.
	remote_chunk_ptr = &(rdma_session->remote_chunk_list.remote_chunk[mem_addr.mem_server_chunk_index]);

	// 2.2 enqueue RDMA request
	ret = semeru_fs_rdma_send(rdma_session, rdma_queue, rdma_req, remote_chunk_ptr,
				  mem_addr.mem_server_offset_within_chunk, page, DMA_FROM_DEVICE);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n", __func__);
		goto out;
	}

#ifdef DEBUG_MODE_DETAIL
	//pr_info("%s, rdma_queue[%d]  load page 0x%lx, virt addr 0x%lx, swp_offset 0x%lx  >>>>> \n",
	//                  __func__, rdma_queue->q_index, (size_t)page, (size_t)(RDMA_DATA_SPACE_START_ADDR + start_addr), (size_t)swap_entry_offset);

	// enable swap out of Meta Region
	pr_info("%s, rdma_queue[%d]  load page 0x%lx, virt addr 0x%lx, swp_offset 0x%lx  >>>>> \n", __func__,
		rdma_queue->q_index, (size_t)page, (size_t)(SEMERU_START_ADDR + start_addr), (size_t)swap_entry_offset);
#endif

	put_cpu(); // enable preeempt.

	// 2.3 wait for write is done.

	// busy wait on the rdma_queue[cpu].
	// This is not exclusive.
	drain_rdma_queue(rdma_queue); // poll the corresponding RDMA CQ

	// 3) wait for the finish of current fs_rdma_req
	//  [??] uninterruptible is good. drain_rdma_queue() already processed all the outstanding rdma requests
	// 5ms at most. The waiting is un-interrupptible
	ret = wait_for_completion_timeout(&(rdma_req->done), msecs_to_jiffies(5));
	if (unlikely(ret == 0)) {
		pr_err("%s, rdma_queue[%d] wait for rdma_req timeout for 5ms.\n", __func__, rdma_queue->q_index);
		ret = -1;
		goto out;
	}

	kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req); // safe to free
	ret = 0; // reset to 0 for succss.

#ifdef DEBUG_MODE_DETAIL
	pr_info("%s, rdma_queue[%d] load page 0x%lx, virt addr 0x%lx DONE <<<<< \n", __func__, rdma_queue->q_index,
		(size_t)page, start_addr);
#endif

#endif

out:
	return ret;
}

static void semeru_invalidate_page(unsigned type, pgoff_t offset)
{
#ifdef DEBUG_MODE_DETAIL
	pr_info("%s, remove page_virt addr 0x%lx\n", __func__, offset << PAGE_OFFSET);
#endif

	return;
}

/**
 * Remove the stale pages
 * 
 */
static void semeru_invalidate_area(unsigned type)
{
	#ifdef DEBUG_MODE_DETAIL
		pr_warn("%s, remove the pages of area 0x%x ?\n", __func__, type);
	#endif
	return;
}

/**
 * @brief : Do swap partition related initialization.
 * 
 * @param type : swap partition type.
 */
static void semeru_frontswap_init(unsigned type)
{
	pr_info("semeru_init end\n");
}

static struct frontswap_ops semeru_frontswap_ops = {
	.init   = semeru_frontswap_init,
	.store  = semeru_frontswap_store,
	.load   = semeru_frontswap_load,
	.invalidate_page = semeru_invalidate_page, // Invoked to clear a page of memory pool? 
	.invalidate_area = semeru_invalidate_area,   

};


int semeru_init_frontswap(void){
	frontswap_register_ops(&semeru_frontswap_ops); // will enable the frontswap path

	#ifdef DEBUG_FRONTSWAP_ONLY
		semeru_init_local_dram();
	#endif


	pr_info("frontswap module loaded\n");
	return 0;
}


void semeru_exit_frontswap(void){
	#ifdef DEBUG_FRONTSWAP_ONLY
		semeru_remove_local_dram();
	#endif

	// * TO BE DONE * - Have to reboot after rmmod 
	// We need remove the frontswap path from kernel. How to do this ??
	// 1) dec frontswap_enabled_key
	// 2) Also need to unplug the swap device from kernel.

	pr_info("unloading frontswap module\n");
	pr_info("1) decrease frontswap_enabled_key to 0. \n");
	pr_info("2) Remove all registered frontswap_ops from the link list.\n");

	frontswap_deregister_ops();
}



