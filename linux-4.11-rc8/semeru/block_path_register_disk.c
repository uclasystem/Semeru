/**
 * Works like a block device driver:
 * 
 * 1) Register a block device under /dev/xxx
 * 
 * 2) Define read/write i/o operation
 * 
 * 3) i/o queue
 * 
 * 4) maintain the RDMA connection to remote server.
 * 
 * 
 * ##########################################
 * The hierarchy :
 * 
 * File system    // [?] What's its purpose ?
 *     V
 *  I/O layer     // handle  the i/o request from file system.
 *     V
 *  Disk dirver   // We are working on this layer.
 * 
 * ###########################################
 * 
 */


// Self defined headers
#include "block_path.h"



//
// Define global variables
//

int rmem_major_num;
struct rmem_device_control      rmem_dev_ctrl_global;

// RMEM_PHY_SECT_SIZE is 512 bytes
// NOT register RDMA Meta Region as SWAP disk.
// The RDMA Meta Region is managed by JVM explicitly.
u64 RMEM_SIZE_IN_PHY_SECT =     ONE_GB / RMEM_PHY_SECT_SIZE * MAX_SWAP_MEM_GB;

#ifdef DEBUG_LATENCY_CLIENT
#define NUM_OF_CORES    16
u32 * cycles_high_before;
u32 * cycles_high_after;
u32 * cycles_low_before;
u32 * cycles_low_after;

u64 * total_cycles;
u64 * last_cycles;

// u32 * cycles_high_before  = kzalloc(sizeof(unsigned int) * NUM_OF_CORES, GFP_KERNEL); 
// u32 * cycles_high_after   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
// u32 * cycles_low_before   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
// u32 * cycles_low_after    = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
#endif

/**
 * 1) Define device i/o operation
 * 
 */

/**
 * Initialize  each dispatch queue.
 * Assign the self-defined driver data to these dispatch queue.
 * 
 * 
 * Parameters
 *    hctx :  
 *        The hardware dispatch queue context.
 * 
 *    data : who assigns this parameter ??
 *        seems the data is the rdd_device_control, the driver controller/context.
 * 
 *    hw_index : 
 *        The index for the dispatch queue.
 *        The mapping between : Dispatch queue[i] --> rmem_rdma_queue[i].
 * 
 * 
 * More Explanation
 *     Each hardware queue coresponds to a rdma_queue.
 * 
 */
static int rmem_init_hctx(struct blk_mq_hw_ctx *hctx, void *data, unsigned int hw_index){

  int ret  = 0;
  struct rmem_device_control  *rmem_dev_ctrl = data;
  struct semeru_rdma_queue      *rdma_queue; 

  #ifdef DEBUG_BD_ONLY
    // Debug Disk driver module

  #else
    rdma_queue  = &(rmem_dev_ctrl->rdma_session->rdma_queues[hw_index]);  // Initialize rdma_queue first.

    // do some initialization for the  rmem_rdma_queues
    rdma_queue->q_index   = hw_index;
    
  #endif
    // Assign rdma_queue to dispatch queue as driver data.
    // Decide the mapping between dispatch queue and RDMA queues 
    hctx->driver_data = rdma_queue;  // Assign the RDMA queue as dispatch queue driver data.


  return ret;
}





/**
 * Got a poped I/O request from dispatch queue, transfer it to 1-sided RDMA message (read/write).
 * 
 * Parameters 
 *    blk_mq_ops->queue_rq is the regsitered function to handle this request.  
 *    blk_mq_queeu_data : I/O request list in this dispatch queue.
 *    blk_mq_hw_ctx   : A dispatch queue context, control one dispatch queue.
 * 
 * More Explanation
 * 
 * For the usage of bio : Documentation/block/biodoc.txt
 * 
 * [?] Which thread is executing this code ?
 *     Kswapd ? or this is a disk driver daemon thread ?
 *     => Guess, should be the kswapd, or the thread doing the direct page reclamation. 
 * 
 */
static int rmem_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd){

  int internal_ret = 0;
  int cpu;
  // Get the corresponding RDMA queue of this dispatch queue.
  // blk_mq_hw_ctx->driver_data  stores the RDMA connection context.
  struct semeru_rdma_queue* rdma_queue = hctx->driver_data;   // get the corresponding rdma_queue
  struct request *rq = bd->rq;
  

  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    u64 seg_num   = rq->nr_phys_segments;  // For swap bio, each segment should be 4K with 4K alignemtn. 
    u64 byte_len  = blk_rq_bytes(rq);       // request->_data_len, the sector size to be read/written.
    // struct bio      *bio_ptr;
    // struct bio_vec  *bv;

    if(rq_data_dir(rq) == WRITE){
      printk("%s: dispatch_queue[%d], get a write request, tag :%d >>>>>  \n", __func__, hctx->queue_num, rq->tag);
    }else{
      printk("%s: dispatch_queue[%d], get a read  request, tag :%d >>>>>  \n", __func__, hctx->queue_num, rq->tag);
    }
    printk("%s: <%llu> segmetns in request, <%u> segments in fist bio , byte length : 0x%llx \n ",__func__, seg_num,rq->bio->bi_phys_segments, byte_len);


    // #ifdef DEBUG_MODE_DETAIL
    //   bio_ptr = rq->bio;
    //   bio_for_each_segment_all(bv, bio_ptr, i) {
    //     struct page *page = bv->bv_page;
    //     printk("%s: handle struct page:0x%llx >> \n", __func__, (u64)page );
    //  }
    // #endif

   // check_segment_address_of_request(rq, "rmem_queue_rq");

  #endif  // end of DEBUG_MODE_BRIEF


  // The connection point between Block Layer with RDMA
  // If no-define DEBUG_BD_ONLY, connect them.
  #ifdef DEBUG_BD_ONLY 

	 cpu = get_cpu();  // disable preempt
    // Debug Disk driver directly

    blk_mq_start_request(rq);

    // Below is only for debug.
    // Finish of the i/o request 
    // [x] Let's just return a NULL data back .
    // Return or NOT is managed by the driver.
    // The content is correct OR not is checked by the applcations.
    // Sometimes, the request is already returned before we reset its request->atomic_flags 
  
    // Insert the debug functions here.

    // 1) Basic information
    check_io_request_basic_info(rq, __func__);


    // 2) Check the number of bio in each Request.
    //    One i/o request can contain multiple bio,
    //    but the acculated pages can't exceed the scatter/gather limitation, 32.
    if(check_number_of_bio_in_request(rq, __func__) > 1){
      printk(KERN_WARNING "%s, Detect more than 1 bio in current i/o request. \n", __func__);
			//blk_mq_end_request(rq,rq->errors);  // This will cause the kernel get stuck ???
		//	internal_ret =-1;
		//	put_cpu(); 
		//	goto err;
    }


    //blk_mq_end_request(rq,rq->errors);
    //printk("%s: End requset->tag : %d <<<<<  \n\n",__func__, rq->tag);

		forward_request_to_local_bd(local_bd.queue, rq);
		put_cpu();  // enable cpu preempt 

  #else
  // The NORMAL path : Connect to RDMA 

    cpu = get_cpu();  // disable cpu preempt
    #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
      printk(KERN_INFO "%s: get cpu %d for hardware queue %d \n",__func__, cpu, hctx->queue_num);
    #endif
  
    // start counting the time
    #ifdef DEBUG_LATENCY_CLIENT
      // Read the rdtsc timestamp and put its value into two 32 bits variables.
      asm volatile("xorl %%eax, %%eax\n\t"
              "CPUID\n\t"
              "RDTSC\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              : "=r"(cycles_high_before[cpu]), "=r"(cycles_low_before[cpu])::"%rax", "%rbx", "%rcx",
                "%rdx");


    #endif

    // Transfer I/O request to 1-sided RDMA messages.
    internal_ret = transfer_requet_to_rdma_message(rdma_queue, rq);
    #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    if(unlikely(internal_ret)){
      printk(KERN_ERR "%s, transfer_requet_to_rdma_message is failed. \n", __func__);
			internal_ret = -1;
      goto err;
    }
    #endif



	  // End the request handling
	  #ifdef DEBUG_LATENCY_CLIENT 


	    asm volatile("RDTSCP\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              "xorl %%eax, %%eax\n\t"
              "CPUID\n\t"
              : "=r"(cycles_high_after[cpu]), 
							"=r"(cycles_low_after[cpu])::"%rax", "%rbx", "%rcx",
                "%rdx");
	

	    uint64_t start, end;
	    start = (((uint64_t)cycles_high_before[cpu] << 32) | cycles_low_before[cpu]);
	    end = (((uint64_t)cycles_high_after[cpu] << 32) | cycles_low_after[cpu]);

      total_cycles[cpu] += end - start;
      if(total_cycles[cpu] - last_cycles[cpu] > 1000*1000*1000*3ul){
        printk(KERN_INFO " cpu %d, spend %llu cycles for enqueue i/o request \n", cpu, total_cycles[cpu]);
        last_cycles[cpu] = total_cycles[cpu];
      }


	  #endif







    put_cpu();  // enable cpu preempt 
    #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
      printk(KERN_INFO "%s: put cpu %d for hardware queue 0x%x \n",__func__, cpu, hctx->queue_num);
    #endif
 

  #endif   // end of no-define DEBUG_BD_ONLY




  // i/o request is handled by disk driver successfully.
  return BLK_MQ_RQ_QUEUE_OK;

#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
err:
#endif
 
  return internal_ret;

}



/**
 * [?] When driver finishes the file page reading, notify some where?
 * 
 */
int rmem_end_io(struct bio * bio, int err){

  // Who assigns the request as bio->bi_private ??
 // struct request * req = (struct request*)ptr_from_uint64(bio->bi_private);

  // notify i/o scheduler?
 // blk_mq_end_request(req, err);

  return err;
}



/**
 * the devicer operations
 * 
 * queue_rq : handle the queued i/o operation
 * map_queues : map the hardware dispatch queue to cpu
 * init_hctx : initialize the hardware dispatch queue ?? 
 * 
 */
static struct blk_mq_ops rmem_mq_ops = {
    .queue_rq       = rmem_queue_rq,
    .map_queues     = blk_mq_map_queues,    // Map staging queues to  hardware dispatch queues via the cpu id.
    .init_hctx      = rmem_init_hctx,
 // .complete       =                       // [?] Do we need to initialize this ?
};



/**
 * Transfer the I/O requset to 1-sided RDMA message.
 * 
 * Parameters
 *    rdma_queue: the corresponding RDMA queue of this dispatch queue.
 *    rq        : the popped requset from dispatch queue.
 * 
 * 
 * More Explanation
 *  [x] Universal Java heap : The swap in/out address, start_addr(sector address), should be equal to the virtual memory address. 
 *      => When a swap out happens, Assign  the same address with the virtual page to the swap out address.
 *         
 * 
 */
int transfer_requet_to_rdma_message(struct semeru_rdma_queue* rdma_queue, struct request * rq){

  int  ret = 0;

  
  int  write_or_not    = (rq_data_dir(rq) == WRITE);
  u64  start_addr      = blk_rq_pos(rq) << RMEM_LOGICAL_SECT_SHIFT;    // request->_sector. Sector start address, change to bytes.
  u64  bytes_len       = blk_rq_bytes(rq);                             // request->_data_len. The sector of request should be contiguous ?

  struct rdma_session_context  *rmda_session = rdma_queue->rdma_session;
  
  struct remote_mapping_chunk   *remote_chunk_ptr;
  u64  start_chunk_index        = start_addr >> CHUNK_SHIFT;    // REGION_SIZE_GB/chunk in default.
  u64  offset_within_chunk      = start_addr & CHUNK_MASK;     // get the file address offset within chunk.

  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL) 
    u64  end_chunk_index          = (start_addr + bytes_len - 1) >> CHUNK_SHIFT;
    u64 debug_byte_len  = bytes_len;

    // The sector size can be not equal to th physical memory size,
    // e.g. the check in function of blk_bio_segment_split
    //      sometimes, there is hole in physical memory page.
    //      the usage of physical memory may be 8/16 bytes alignment. 
    //check_sector_and_page_size(rq, __func__);


    printk("%s: blk_rq_pos(rq):0x%llx, start_adrr:0x%llx, RMEM_LOGICAL_SECT_SHIFT: 0x%llx, CHUNK_SHIFT : 0x%llx, CHUNK_MASK: 0x%llx \n",
                                                      __func__,(u64)blk_rq_pos(rq),start_addr, (u64)RMEM_LOGICAL_SECT_SHIFT, 
                                                      (u64)CHUNK_SHIFT, (u64)CHUNK_MASK);



  
    // [?] We register all the remote region as RDMA buffer at very fist.
    //     will this cause any problem ?
    if(start_chunk_index!= end_chunk_index){
      ret =-1;
      printk(KERN_ERR "%s, request start_add:0x%llx, byte_len:0x%llx \n",__func__, start_addr,bytes_len);
      printk(KERN_ERR "%s, start_chunk_index[%llu] != end_chunk_index[%llu] \n", __func__,start_chunk_index, end_chunk_index);
    
      bytes_len = 4096;     // Limit it to 1 page.
      printk(KERN_ERR "%s: reset byte_len(0x%llx)  to rq->nr_phys_segments * PAGE_SIZE:0x%llx \n",__func__, debug_byte_len, bytes_len);
      end_chunk_index     = (start_addr + bytes_len -1) >> CHUNK_SHIFT;

      if(start_chunk_index!= end_chunk_index){
         printk(KERN_ERR "%s :After reset byte_len,  start_chunk_index!= end_chunk_index, error.\n",__func__);
         goto err;
      }

      //goto err;
    }

    // 1+8 Regions. The first Region is reserved for meta data.
    // Change all  the memory access to a specific chunk.
     if(start_chunk_index >= MAX_REGION_NUM){
        start_chunk_index =1;
        offset_within_chunk = 0x1000;
        printk(KERN_ERR "%s, error. chunk exceed boundary : %llu. reset to chunk[1] \n", __func__, start_chunk_index);
     }

  #endif





  //Get the remote_chunk information
  // [x] There are 2 kinds of Region
  //     1) First Region is Meta Region. It's managed by JVM explicitly.
  //     2) the rest, Data Regions. Fully mapped. Managed by Swap.
  //     So, we skip the first Region.
  start_chunk_index += 1; 
  remote_chunk_ptr  = &(rmda_session->remote_chunk_list.remote_chunk[start_chunk_index]);

  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
  
  // printk("%s: TRANSFER TO RDMA MSG:  I/O request start_addr : 0x%llx, byte_length 0x%llx  --> \n ",__func__, start_addr, bytes_len);

  printk("%s: TO RDMA MSG[%llu], remote chunk[%llu], chunk_addr 0x%llx, rkey 0x%x offset 0x%llx, byte_len : 0x%llx  \n",  
                                                                __func__ ,
                                                                rmda_ops_count,  
                                                                start_chunk_index,  
                                                                remote_chunk_ptr->remote_addr, 
                                                                remote_chunk_ptr->remote_rkey,    
                                                                offset_within_chunk, 
                                                                bytes_len);
  rmda_ops_count++;
  #endif

 
  // start i/o requset before set it as start.
  // start i/o request timeout counting ?
  blk_mq_start_request(rq);


  // Build the 1-sided RDMA read/write.
  if(write_or_not){
    // post a 1-sided RDMA write, Data-Path
    // Into RDMA section.
    ret = dp_post_rdma_write(rmda_session ,rq, rdma_queue, remote_chunk_ptr,  offset_within_chunk, bytes_len);
    #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    if(unlikely(ret)){
      printk(KERN_ERR "%s, post 1-sided RDMA write failed. \n", __func__);
      goto err;
    }
    #endif

  }else{
    // post a 1-sided RDMA read, Data-Path
    ret = dp_post_rdma_read(rmda_session, rq, rdma_queue, remote_chunk_ptr,  offset_within_chunk, bytes_len);
    #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    if(unlikely(ret)){
      printk(KERN_ERR "%s, post 1-sided RDMA read failed. \n", __func__);
      goto err;
    }
    #endif

  }


  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
  err:
  #endif

  return ret;
}









/**
 * #################3
 * 3) Register Local Block Device (Interface)
 * 
 */


/**
 * blk_mq_tag_set stores all i/o operations definition. 
 * 
 *  I/O requets control.
 *      blk_mq_tag_set->ops : Define the i/o operations.  request enqueue (staging queue), dispach queue initialization etc.
 *      blk_mq_tag_set->nr_hw_queues : number of hardware dispatch queue. usually, stage queue = dispatch queue = avaible cores 
 * 
 *      blk_mq_tag_set->driver_data   : points to driver controller.
 * 
 * [?] For the staging queue, set the information within it directly ?
 * 
 * For C:
 *  key word "static" constraint the usage scope of this function to be within this file. 
 * 
 */
static int init_blk_mq_tag_set(struct rmem_device_control* rmem_dev_ctrl){

  struct blk_mq_tag_set* tag_set = &(rmem_dev_ctrl->tag_set);
  int ret = 0;

  if(unlikely(!tag_set)){
    printk(KERN_ERR "%s, init_blk_mq_tag_set : pass a null pointer in. \n", __func__);
    ret = -1;
    goto err;
  }

  tag_set->ops = &rmem_mq_ops;
  tag_set->nr_hw_queues = rmem_dev_ctrl->nr_queues;   // hardware dispatch queue == software staging queue == avaible cores
  tag_set->queue_depth = rmem_dev_ctrl->queue_depth;  // [?] on the fly requet, for each hw queue. Controlled by the availbe request->tag
  tag_set->numa_node  = NUMA_NO_NODE;

  // Reserve RDMA_command space. Get it by blk_mq_rq_to_pdu(struct request*)
  // scatterlist is only used as temporary data structure, no need to send the Remote memory pool.
  tag_set->cmd_size = sizeof(struct rmem_rdma_command) + MAX_REQUEST_SGL * sizeof(struct scatterlist) ;     
  tag_set->flags = BLK_MQ_F_SHOULD_MERGE;                   // [?]merge the bio i/o requets ??
  tag_set->driver_data = rmem_dev_ctrl;                     // The driver controller, the context 


  ret = blk_mq_alloc_tag_set(tag_set);      // Check & correct the value within the blk_mq_tag_set.
  if (unlikely(ret)){
    printk(KERN_ERR "%s, blk_mq_alloc_tag_set error. \n", __func__);
    goto err;
  }
  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    else{
      printk(KERN_INFO "%s, blk_mq_alloc_tag_set is done. \n", __func__);
    }
  #endif

  return ret;

err:
  printk(KERN_ERR "Error in  %s \n",__func__);
  return ret;
}



/**
 * request_queue is the requst queue descriptor/cotrollor, not the real queue.
 * 
 * Real request queue:
 *    blk_mq_ctx->rq_list, the request queue in staging context. 
 *    blk_mq_hw_ctx->dispatch , is the request queue in dispatch context. 
 * 
 * Explanation
 *    blk_mq_ctx is only used by kernel. 
 *    As disk driver, we only need to set the dispatch queue.
 * 
 *    For fast block device, use bio layer directly may get better performance. 
 *    Because for fast divice, it can respond random access very fast.
 *    Enqueue/pop , merge bio/requests etc waste lots of cpu cycles. 
 */
static int init_blk_mq_queue(struct rmem_device_control* rmem_dev_ctrl){

  int ret = 0;
  int page_size;

  rmem_dev_ctrl->queue = blk_mq_init_queue(&rmem_dev_ctrl->tag_set);   // Build the block i/o queue.
  if (unlikely(IS_ERR(rmem_dev_ctrl->queue ))) {
        ret = PTR_ERR(rmem_dev_ctrl->queue );
        printk(KERN_ERR "%s, create the software staging reqeust queue failed. \n", __func__);
        goto err;
  }

  // Do NOT merge mutiple bio into the i/o request.
	//queue_flag_set(QUEUE_FLAG_NOMERGES, rmem_dev_ctrl->queue);

  // request_queue->queuedata is reservered for driver usage.
  // works like : 
  // blk_mq_tag_set->driver_data 
  // gendisk->private_data
  rmem_dev_ctrl->queue->queuedata = rmem_dev_ctrl;
  
  //
  // * set some device queue information
  // i.e. Cat get these information from : /sys/block/sda/queue/*
  // logical block size   : 4KB, The granularity of kernel read/write. From disk buffer to memory ?
  // physical block size  : 512B aligned. The granularity read from hardware disk.  
  // 
  
  
  page_size = PAGE_SIZE;           // alignment to RMEM_SECT_SIZE

  blk_queue_logical_block_size(rmem_dev_ctrl->queue, RMEM_LOGICAL_SECT_SIZE); // logical block size, 4KB. Access granularity generated by Kernel
  blk_queue_physical_block_size(rmem_dev_ctrl->queue, RMEM_PHY_SECT_SIZE);    // physical block size, 512B. Access granularity generated by Drvier (to handware disk)
  sector_div(page_size, RMEM_LOGICAL_SECT_SIZE);                              // page_size /=RMEM_SECT_SIZE
  blk_queue_max_hw_sectors(rmem_dev_ctrl->queue, RMEM_QUEUE_MAX_SECT_SIZE);   // [?] 256kb for current /dev/sda


  return ret;

err:
  printk(KERN_ERR "Error in %s\n",__func__);
  return ret;
}

/**
 * Allocate && Set the gendisk information 
 * 
 * 
 * // manual url : https://lwn.net/Articles/25711/
 * 
 * 
 * gendisk->fops : open/close a device ? 
 *        The difference with read/write i/o operation (blk_mq_ops)??
 * 
 * 
 */


static int rmem_dev_open(struct block_device *bd, fmode_t mode)
{

  // What should this function do ?
  // open some hardware disk by path ??

    pr_debug("%s called\n", __func__);
    return 0;
}

static void rmem_dev_release(struct gendisk *gd, fmode_t mode)
{
    pr_debug("%s called\n", __func__);
}

static int rmem_dev_media_changed(struct gendisk *gd)
{
    pr_debug("%s called\n", __func__);
    return 0;
}

static int rmem_dev_revalidate(struct gendisk *gd)
{
    pr_debug("%s called\n", __func__);
    return 0;
}

static int rmem_dev_ioctl(struct block_device *bd, fmode_t mode,
              unsigned cmd, unsigned long arg)
{
    pr_debug("%s called\n", __func__);
    return -ENOTTY;
}

// [?] As a memory pool, how to assign the geometry information ?
// Assign some value like the nvme driver ?
int rmem_getgeo(struct block_device * block_device, struct hd_geometry * geo){
      pr_debug("%s called\n", __func__);
    return -ENOTTY;
}

/**
 * Device operations (for disk tools, user space behavior)
 *    open : open  device for formating ?
 *    release : delete the blockc device.
 * 
 *    getgeo  : get disk geometry. i.e. fdisk need this information.
 * 
 * more :
 *    read/write is for i/o request handle.
 * 
 */
static struct block_device_operations rmem_device_ops = {
  .owner            = THIS_MODULE,
  .open             = rmem_dev_open,
  .release          = rmem_dev_release,
  .media_changed    = rmem_dev_media_changed,
  .revalidate_disk  = rmem_dev_revalidate,
  .ioctl            = rmem_dev_ioctl,
  .getgeo           = rmem_getgeo,
};

/**
 * Allocate & intialize the gendisk.
 * The main controller for Block Device hardare.
 * 
 * [?] Show a device under /dev/
 * alloc_disk_node()  : allocate gendisk handler
 * add_disk()         : add to /dev/
 * 
 * Fields :
 *    gendisk->ops    : define device operation 
 *    gendisk->queue  : points to (staging queue) or (dispatch queue) ? Should be dispatch queue ?
 *    gendisk->private_data : driver controller.
 * 
 * [?] Do we need to set a block_device ??
 * 
 */
int init_gendisk(struct rmem_device_control* rmem_dev_ctrl ){

  int ret = 0;
  sector_t remote_mem_sector_num = RMEM_SIZE_IN_PHY_SECT; // size of the disk. number of physical sector

  rmem_dev_ctrl->disk = alloc_disk_node(1, NUMA_NO_NODE); // minors =1, at most have one partition.
  if(unlikely(!rmem_dev_ctrl->disk)){
    printk("%s: Failed to allocate disk node\n", __func__);
        ret = -ENOMEM;
    goto err;
  }

  rmem_dev_ctrl->disk->major  = rmem_dev_ctrl->major;
  rmem_dev_ctrl->disk->first_minor = 0;                 // The partition id, start from 0.
  rmem_dev_ctrl->disk->fops   = &rmem_device_ops;       // Define device operations
  rmem_dev_ctrl->disk->queue  = rmem_dev_ctrl->queue;   // Assign the hardware dispatch queue
  rmem_dev_ctrl->disk->private_data = rmem_dev_ctrl;    // Driver controller/context. Reserved for disk driver.
  memcpy(rmem_dev_ctrl->disk->disk_name, rmem_dev_ctrl->dev_name, DEVICE_NAME_LEN);

  // RMEM_SIZE_IN_PHY_SECT is just the number of physical sector already.
  //sector_div(remote_mem_sector_num, RMEM_LOGICAL_SECT_SIZE);    // remote_mem_size /=RMEM_SECT_SIZE, return remote_mem_size%RMEM_SECT_SIZE 
  set_capacity(rmem_dev_ctrl->disk, remote_mem_sector_num);     // size is in remote file state->size, add size info into block device
 
 
  // Register Block Device through gendisk(->partition)
  // Device will show under /dev/
  // After call this function, disk is active and prepared well for any i/o request.
  add_disk(rmem_dev_ctrl->disk);

  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
  printk("init_gendisk : initialize disk %s done. \n", rmem_dev_ctrl->disk->disk_name);
  #endif

  return ret;

err:
  printk(KERN_ERR "ERROR in %s \n",__func__);
  del_gendisk(rmem_dev_ctrl->disk);
  return ret;
}

/**
 *  Allocate and initialize the fields of Driver controller/context.
 * 
 */
int init_rmem_device_control(char* dev_name, struct rmem_device_control* rmem_dev_ctrl){
  
  int ret = 0;


  if(rmem_dev_ctrl == NULL){
    ret = -1;
    printk(KERN_ERR "%s, Allocate struct rmem_device_control failed \n", __func__);
    goto err;
  }
 

  if(dev_name != NULL ){
    //Use assigned name
    int len = strlen(dev_name) >= DEVICE_NAME_LEN ? DEVICE_NAME_LEN : strlen(dev_name);
    memcpy(rmem_dev_ctrl->dev_name, dev_name, len);   // string copy or memory copy ?
  }else{
    strcpy(rmem_dev_ctrl->dev_name,"rmempool");       // rmem_dev_ctrl->dev_name = "xx" is wrong. Try to change the value(address) of a char pointer.
  }

  rmem_dev_ctrl->major = rmem_major_num;

  rmem_dev_ctrl->queue_depth  = RMEM_QUEUE_DEPTH; // 
  rmem_dev_ctrl->nr_queues    = online_cores;     // staging queue = dispatch queue = avaible cores.

  rmem_dev_ctrl->freed       = 0;                // Falg, if the block resouce is already freed.
  //
  // blk_mq_tag_set, request_queue, rmem_rdma_queue are waiting to be initialized later. 
  //

err:
  return ret;
}




/**
 * The main function to create the block device. 
 * 
 * Parameters:
 *    device name :
 *    driver context : self-defiend structure. 
 *          [?] who assigns the value of rmem_dev_ctrl ??
 * 
 * Explanation
 *    1) Build and initialize the driver context.
 *          [?] What fileds need to be defined and initialized ?
 *    2) Define i/o operation
 *    3) Allocate and initialize the i/o staging & dispatch queues
 *    4) Active the block device : add it tho kernel bd list.
 * 
 * return 0 if success.
 */
int RMEM_create_device(char* dev_name, struct rmem_device_control* rmem_dev_ctrl ){

  int ret = 0;  
  //unsigned long page_size; 

  //
  //  1) Initiaze the fields of rmem_device_control structure 
  //
  if(rmem_dev_ctrl == NULL ){    
    printk(KERN_ERR "Can not pass a null pointer to initialize. \n");
    ret = -1;
    goto err;
  }

  ret = init_rmem_device_control( dev_name, rmem_dev_ctrl);
  if(ret){
    printk(KERN_ERR "Intialize rmem_device_control error \n");
    goto err;
  }
  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    else{
      printk(KERN_INFO "%s,init_rmem_device_control done. \n", __func__);
    }
  #endif

  // 2) Intialize thte blk_mq_tag_set
  ret = init_blk_mq_tag_set(rmem_dev_ctrl);
  if(ret){
    printk(KERN_ERR "Allocate blk_mq_tag_set failed. \n");
   goto err;
  }
  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    else{
      printk(KERN_INFO "%s,init_blk_mq_tag_set done. \n", __func__);
    }
  #endif


  //
  // 3) allocate & intialize the software staging queue
  //
  ret = init_blk_mq_queue(rmem_dev_ctrl);
  if(ret){
    printk("init_blk_mq_queue failed. \n");
    goto err;
  }
  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    else{
      printk(KERN_INFO "%s,init_blk_mq_queue done. \n", __func__);
    }
  #endif
  
  //
  // 4) initiate the gendisk (device information) & add the device into kernel list (/dev/) -- Active device..
  //
  ret = init_gendisk(rmem_dev_ctrl);
  if(ret){
    pr_err("Init_gendisk failed \n");
    goto err;
  }

  #if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
    printk("RMEM_create_device done. \n");
  #endif

  return ret;

err:
  printk(KERN_ERR "Error in %s \n", __func__);
  return ret;
}




/**
 * The entry of disk driver.
 * Invoked by RDMA part after the RDMA connection is built.
 * 
 * Can NOT be static, this funtion is invoked in rdma_client.c.
 * 
 * More Explanation
 *    1) online_cores is initialized in rdma part.
 */
 int  rmem_init_disk_driver(struct rmem_device_control *rmem_dev_ctl){

  int ret = 0;
    
  #ifdef  DEBUG_MODE_BRIEF 
  printk("%s,Load kernel module : register remote block device. \n", __func__);
  #endif

	// 1) Build the local block_device first.
  #ifdef DEBUG_BD_ONLY 
  // Init a local block device for debugging.
  ret = init_local_block_device();
  if(unlikely(ret != 0)){
    printk(KERN_ERR "%s, Intialize local block device error. \n", __func__);
    goto err;
  }
  #endif


	// 2) Build the Semeru Block_device

  #ifdef DEBUG_LATENCY_CLIENT
  cycles_high_before  = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL); 
  cycles_high_after   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
  cycles_low_before   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
  cycles_low_after    = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);

  total_cycles =  kzalloc(sizeof(u64) * NUM_OF_CORES, GFP_KERNEL);;
  last_cycles =  kzalloc(sizeof(u64) * NUM_OF_CORES, GFP_KERNEL);;
  #endif

  // Become useless since 4.9, maybe removed latter.
  // Get a major number, list the divice under /proc/devices
  // i.e.
  // <major_number> <device_name>
  // 252 rmempool
  //
  rmem_major_num = register_blkdev(0, "rmempool");
  if (unlikely(rmem_major_num < 0)){
    printk(KERN_ERR "%s, register_blkdev failed. \n",__func__);
    ret = -1;
        goto err;
  }

  // number of staging and dispatch queues, equal to available cores
  // Kernel blocking layer assume that the number of software staging queue and avialable cores are same. 
  // Usualy, we set the number of dispatch queues same with staging queues.
  //online_cores = num_online_cpus(); // Initialized in rdma part.
  
  #ifdef  DEBUG_MODE_BRIEF 
  printk("%s, Got block device major number: %d \n", __func__, rmem_major_num);
  printk("%s, online cores : %d \n", __func__, online_cores);
  #endif

  //debug
  // create the block device here
  // Create block information within functions
  
  ret = RMEM_create_device(NULL, rmem_dev_ctl);  
  if(unlikely(ret)){
    printk(KERN_ERR "%s, Crate block device error.\n", __func__);
    goto err;
  }

  return ret;

err:
  printk(KERN_ERR "ERROR in %s \n", __func__);
    return ret;
}




/**
 * 
 * Blcok Resource free
 * 
 */

// This function can only be called once.
//  And the rmem_dev_ctrl context is initialized.
int octopus_free_block_devicce(struct rmem_device_control * rmem_dev_ctrl ){
  int ret =0;

  if( rmem_dev_ctrl != NULL && rmem_dev_ctrl->freed != 0){
    //already called 
    return ret;
  }

  rmem_dev_ctrl->freed  = 1;

  if(rmem_dev_ctrl!=NULL){


    if(likely(rmem_dev_ctrl->disk != NULL)){
      del_gendisk(rmem_dev_ctrl->disk);
      printk("%s, free gendisk done. \n",__func__);
    }
    
    if(likely(rmem_dev_ctrl->queue != NULL)){
      blk_cleanup_queue(rmem_dev_ctrl->queue);
      printk("%s, free and close dispatch queue. \n",__func__);
    }

    blk_mq_free_tag_set(&(rmem_dev_ctrl->tag_set));
    printk("%s, free tag_set done. \n",__func__);

    if(likely(rmem_dev_ctrl->disk != NULL)){
        put_disk(rmem_dev_ctrl->disk);
      printk("%s, put_disk gendisk done. \n",__func__);
    }

    unregister_blkdev(rmem_dev_ctrl->major, rmem_dev_ctrl->dev_name);

    #ifdef DEBUG_BD_ONLY
      un_register_local_disk();
    #endif 

    printk("%s, Free Block Device, %s, DONE. \n",__func__, rmem_dev_ctrl->dev_name);

  }else{
      printk("%s, rmem_dev_ctrl is NULL, nothing to free \n",__func__);
  }

  return ret;
}



// module function
// static void RMEM_cleanup_module(void){

//     unregister_blkdev(rmem_major_num, "rmempool");

// }




//
// RDMA part invoke the device registration funtion.
//

//module_init(RMEM_init_module);
//module_exit(RMEM_cleanup_module);



/**
 * Debug function 
 */


/**
 * I/O Request Check
 *  Check the number of BIOs in each I/O request.
 * 
 *  Return value : number of bio in current i/o request.
 *  
 */ 
uint32_t check_number_of_bio_in_request(struct request *io_rq, const char* message){
  uint32_t bio_count = 0;
  struct bio *bio_ptr = io_rq->bio;

  for_each_bio(bio_ptr){
    printk(KERN_INFO "%s, for bio[%u] 0x%llx, sector addr : 0x%lx , sectors size : 0x%x bytes\n", 
                                    message, bio_count, (u64)bio_ptr, bio_ptr->bi_iter.bi_sector << RMEM_LOGICAL_SECT_SHIFT, bio_ptr->bi_iter.bi_size );
  
    bio_count++;
  }

  return bio_count;
}


void check_io_request_basic_info(struct request *rq, const char* message){
  int  write_or_not    = (rq_data_dir(rq) == WRITE);
  u64  start_addr      = blk_rq_pos(rq) << RMEM_LOGICAL_SECT_SHIFT;    // request->_sector. Sector start address, change to bytes.
  u64  bytes_len       = blk_rq_bytes(rq);      

  printk(KERN_INFO "%s: For current i/o Requet, tag %d ,  Write ? %d. start_adrr:0x%llx, size 0x%llx, RMEM_LOGICAL_SECT_SHIFT: 0x%llx \n",
                message, rq->tag, write_or_not, start_addr, (uint64_t)bytes_len, (u64)RMEM_LOGICAL_SECT_SHIFT );

  // Transfer to virtual addr index
  #ifdef ENABLE_SWP_ENTRY_VIRT_REMAPPING
    printk(KERN_INFO "%s, remote start addr 0x%llx, size 0x%llx \n", __func__, start_addr + RDMA_DATA_SPACE_START_ADDR, bytes_len);
  #endif

}







/**
 * BIO check
 * 
 *  Check #1, confirm all the segments are contiguous. 
 *  Because we need to use the scatter/gather characteristics of InfiniBand.
 *  Scatter/Gather are multiple to one mapping.
 * 
 *  Only check the bio built during swap procedure. 
 *  In this scenario, the size of each segment should be (bv_len) 4K, start at 0 (bv_offset == 0).
 * 
 * 
 *  More Explanation
 *  echo bio has two components
 *    1) physical pages;
 *    2) segments 
 * 
 *  For read bio, load data from segments to physical pages.
 *  For write bio, store data from physical pages to segments.  
 *  In our design, the address of segmetns are same as the virtual address of the physical pages. 
 * 
 * 
 */
void check_segment_address_of_request(struct request *io_rq, const char* message){
  struct bio        *bio_ptr = io_rq->bio;
  struct bio_vec    bv;     // iterator for segment in current bio.
	struct bvec_iter  iter;   // points to current 
  int count;
  //u64 sector_size_in_pg;
  //u64 segment_addr;

	printk(KERN_INFO "%s, start. Chekc for i/o request 0x%llx \n", message, (u64)io_rq );

  for_each_bio(bio_ptr){
		printk(KERN_INFO "	for bio 0x%llx, sector addr : 0x%lx , sectors size : 0x%x bytes\n", 
                                       (u64)bio_ptr, bio_ptr->bi_iter.bi_sector, bio_ptr->bi_iter.bi_size );

    count = 0;
    bio_for_each_segment(bv, bio_ptr, iter) {
      // As swap bio, each segment should be 4K alignment (offset = 0) with 4K size.
      // 1 segment <-> 1 physical page.
      if(bv.bv_offset!= 0 || bv.bv_len!= 4096){
        printk(KERN_INFO "	non-page alignment segment[%d]  struct page: 0x%llx, offset: 0x%x, length: 0x%x \n", 
                                         count++, (u64)bv.bv_page, bv.bv_offset, bv.bv_len );
      }


      // !! map_swap_page isn't a external symbol, need to modify it.
      // sector_size_in_pg = (u64)map_swap_page(bv.bv_page, &bio_ptr->bi_bdev);
      // sector_size_in_pg <<= PAGE_SHIFT - 9;
      // // As swap cache, each physical page should record the swp_entry_t which is equal to segment addr.
      // printk(KERN_INFO "%s, check sector addr from page->private : 0x%llx \n",
      //                                   message, sector_size_in_pg);
    }  // segment


  } // bio

  printk(KERN_INFO " %s:  done\n\n", message);
}




void check_bio_basic_info(struct bio *bio_ptr, const char* message){
	printk(KERN_INFO "%s, start. Chekc for BIO 0x%llx \n", message, (u64)bio_ptr );
	
	printk(KERN_INFO "	for bio 0x%llx, sector addr: 0x%lx, virt_addr 0x%llx , sectors size : 0x%x bytes\n", 
                                       (u64)bio_ptr, bio_ptr->bi_iter.bi_sector, (u64)(bio_ptr->bi_iter.bi_sector<<9)  , bio_ptr->bi_iter.bi_size );

  printk(KERN_INFO " %s:  done\n\n", message);

}








/**
 * Check if the sector size and physical memory buffer size are equal.
 *  
 * request->_data_len // sector size
 * 
 * accumulate the physical memory size used by all the segments, bio->bv_vec.
 * 
 */
bool check_sector_and_page_size(struct request *io_rq, const char* message){
  u64 request_sector_size_in_byte   = 0;
  u64 sector_size_in_byte           = 0;
  u64 physical_memory_size_in_bytes = 0;
  u64 segment_num                   = io_rq->nr_phys_segments;
  struct bio        *bio_ptr = io_rq->bio;
  struct bio_vec    bv;     // iterator for segment in current bio.
	struct bvec_iter  iter;   // points to current 
  struct page       *page_ptr = NULL;
  bool ret  = true;


  request_sector_size_in_byte = io_rq->__data_len;  // get from bio_sector size


  //print the attached message
  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , Start\n", __func__, message);


  for_each_bio(bio_ptr){

    sector_size_in_byte += bio_ptr->bi_iter.bi_size;
    bio_for_each_segment(bv, bio_ptr, iter) {
      
      // assume the segments use physical memory in some order, ascending or descending. 
      if(bv.bv_page != page_ptr){
        physical_memory_size_in_bytes += 4096;
        page_ptr = bv.bv_page;
      }
    } // segment
  } // bio

  if(request_sector_size_in_byte != sector_size_in_byte){
    printk(KERN_ERR "%s, Error : request_sector_size_in_byte(0x%llx)  != sector_size_in_byte(0x%llx) \n ", 
                                      __func__,request_sector_size_in_byte, sector_size_in_byte );
    ret = false;
  }

  if(sector_size_in_byte!= physical_memory_size_in_bytes){
     printk(KERN_ERR "%s: Error, sector_size_in_byte(0x%llx) != physical_memory_size_in_bytes(0x%llx)  \n ", 
                                      __func__, sector_size_in_byte, physical_memory_size_in_bytes );
    ret = false;
  }

  if(ret == false)
    goto err;

  printk(KERN_INFO "%s, [0x%llx] segments, requset->_data_len 0x%llx, sector_size_of_bio 0x%llx, physical memory 0x%llx. \n\n", 
                                        __func__, segment_num, request_sector_size_in_byte, sector_size_in_byte,  physical_memory_size_in_bytes );
 
  if(message != NULL)
     printk(KERN_INFO "\n  %s invoked in %s , End\n", __func__, message);
 
  return ret;

err:
  return ret;
}




/**
 * Print the physical page address  attached to each io request.
 * 
 * => Print information 
 * 
 */
void print_io_request_physical_pages(struct request *io_rq, const char* message){

  struct bio        *bio_ptr = io_rq->bio;
  struct bio_vec    bv;     // iterate the struct page attached to the bio.
	struct bvec_iter  iter;
  u64 phys_addr;

  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , Start\n", __func__, message);
  

  for_each_bio(bio_ptr)
		bio_for_each_segment(bv, bio_ptr, iter) {
      struct page *page = bv.bv_page;   
      phys_addr = page_to_phys(page);
      printk(KERN_INFO "%s: handle struct page: 0x%llx , physical address : 0x%llx \n", 
                          __func__,  (u64)page, (u64)phys_addr );
    }

  printk(KERN_INFO " %s, request->tag %d. \n\n", __func__, io_rq->tag );

  if(message != NULL)
    printk(KERN_INFO "\n  %s invoked in %s , End\n", __func__, message);
  
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


