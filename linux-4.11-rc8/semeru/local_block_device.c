/**
 * Write a disk driver to manage a local disk, e.g. /dev/sda1,
 * Let the semeru block manauly forward bio into this disk.
 * This used as a debug device.
 *  
 */


#include "block_path.h"


#define LOGICAL_BLOCK_SIZE 512
#define LOCAL_DISK_NAME "local_bd"
#define LOCAL_DISK_NAME_0 LOCAL_DISK_NAME "0"

#define LOCAL_DISK  "/dev/sdb1"    // the logical partition we use
#define LOCAL_BD_BDEV_MODE (FMODE_READ | FMODE_WRITE | FMODE_EXCL)

#define LOGICAL_BLOCK_SIZE 512
#define KERNEL_SECTOR_SIZE 512
#define MAX_SGL_LEN 32

#define STACKBD_REDIRECT_OFF 0
#define STACKBD_REDIRECT_ON	 1


//
// Global variables
//


struct local_block_device	local_bd;


//
// Functions
//



/**
 * Get the last bio back, end the original i/o request.
 * Only assign this function to the last bio disassembled from the original i/o request. 
 * 
 */
void forwardee_local_bd_end_io(struct bio *bio, int err)
{
	struct request *rq;

#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
	printk("%s, bio 0x%llx end, goto mark request end. \n", __func__, (u64)bio);
#endif

	rq = (struct request *)ptr_from_uint64(bio->bi_private); // Get the original i/o request, before forwarding.

	blk_mq_end_request(rq, err);

#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
	printk("%s: End requset->tag : %d <<<<<  \n\n", __func__, rq->tag);
#endif

#ifdef DEBUG_SWAP_PATH
	check_io_request_basic_info(rq, "forwardee_local_bd_end_io");
	check_bio_basic_info(bio, "forwardee_local_bd_end_io");
#endif
}

/**
 *  Direct add bio to local_bd.
 * Add the bio into local_bd.bio_list and pass the to the daemon thread.
 * This function is only invoked when the local_bd is accessed.
 */
blk_qc_t local_bd_make_request(struct request_queue *q, struct bio *bio){
    spin_lock_irq(&local_bd.lock);
    if (!local_bd.bdev_raw)
    {
        printk("local_bd: Request before bdev_raw is ready, aborting\n");
        goto abort;
    }
    if (!local_bd.is_active)
    {
        printk("local_bd: Device not active yet, aborting\n");
        goto abort;
    }
    bio_list_add(&local_bd.bio_list, bio);
    wake_up(&req_event);  // [?] wake up the daemon thread to pop up a bio from the local_bd.bio_list ?
    spin_unlock_irq(&local_bd.lock);

    return 0;

abort:
    spin_unlock_irq(&local_bd.lock);
    printk("<%p> Abort request\n\n", bio);
    bio_io_error(bio);
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
    return 0;
    #endif
}







/**
 * Disassemble an i/o request into multiple bio and forward them into local_bd's request_queue.
 *  
 * Assign the end_bio function to the last bio.
 * 
 * Parameters
 * 	struct request_queue, the local_bd.queue
 * 	struct request, the original i/o request, enqueued into semeru block_device.
 */
void forward_request_to_local_bd(struct request_queue *q, struct request *req){
    struct bio *bio_copy = NULL;
    struct bio *bio_ptr = req->bio;
    int i;
    //int len = req->nr_phys_segments; // [?] each bio can contain multiple segments ??

    spin_lock_irq(&local_bd.lock);
    if (!local_bd.bdev_raw){
        printk("local_bd: Request before bdev_raw is ready, aborting\n");
        goto abort;
    }
    if (!local_bd.is_active){
        printk("local_bd: Device not active yet, aborting\n");
        goto abort;
    }	

		// Clone each bio of the i/o request.
    for_each_bio(bio_ptr){

			bio_copy = bio_clone(bio_ptr, GFP_ATOMIC);
			bio_list_add(&local_bd.bio_list, bio_copy);
			#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
				printk(KERN_INFO "%s, request 0x%llx, clone  bio[%d] 0x%llx to bio_copy 0x%llx \n", 
													__func__, (uint64_t)req, i, (uint64_t)bio_ptr, (uint64_t)bio_copy );
				if(bio_ptr == NULL){
					printk(KERN_ERR "%s, NULL bio[%d]. \n", __func__, i);
					goto abort;
				}
			#endif



			if(bio_ptr->bi_next ==NULL){
				//[x] The last bio, assign end function.
				bio_copy->bi_end_io = (bio_end_io_t*)forwardee_local_bd_end_io;  // Register the i/o end function.
				bio_copy->bi_private = (void*) uint64_from_ptr(req);   // store the original i/o request's pointer into each bio's reserved space.
				
				#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
					//printk(KERN_INFO "%s, request 0x%llx, clone LAST bio[%d] 0x%llx \n", __func__, (uint64_t)req, i, (uint64_t)bio_ptr );
					check_bio_basic_info(bio_copy, "Last bio_copy");
				#endif

				wake_up(&req_event);
			}// the last bio
		
		}

    spin_unlock_irq(&local_bd.lock);
    return;
abort:
    spin_unlock_irq(&local_bd.lock);
    printk(KERN_ERR "<%p> Abort request\n\n", bio_copy);
    bio_io_error(bio_copy);
}














// /**
//  * Self defined i/o request building function.
//  * It's purpose is to forward all the submitted bio to another block_device, e.g. /dev/sdc1, direclty.
//  * 
//  * 1) this bio will be added to an exist i/o rquest or build a new i/o request for it in forwardee block_device->make_request_fn,
//  * 
//  * 
//  */
// blk_qc_t local_bd_make_request(struct bio *bio){

//     if (bio == NULL){
//         printk("bio is NULL\n");
//     }
//     #ifdef DEBUG_MODE_DETAIL
//     else{
//         printk(KERN_INFO "%s,sent a bio 0x%llx to local_bd. \n", __func__, (unsigned long long)bio );
//     }
//     #endif


//     if (!local_bd.bdev_raw){
//         printk("local_bd: Request before bdev_raw is ready, aborting\n");
//         goto abort;
//     }

//     if (!local_bd.is_active){
//         printk("local_bd: Device not active yet, aborting\n");
//         goto abort;
//     }
    
    
//     // forward this bio to a already registerred block_device.
	
// 	bio->bi_bdev = local_bd.bdev_raw; // resign the block_device to the bio.

// 	check_bio_basic_info(bio, "local_bd_make_request #1");

// 	// [?] What's this used for ?
// 	trace_block_bio_remap(bdev_get_queue(local_bd.bdev_raw), bio, bio->bi_bdev->bd_dev, bio->bi_iter.bi_sector);

// 	check_bio_basic_info(bio, "local_bd_make_request #2");

// 	// Enqueue this bio into this block_device, bio->bi_bdev, single/multiple_queue.
// 	// Because it's submitted to another block_device's request_queue, invoke the generic_make_request function agian.
// 	generic_make_request(bio);  // Use this bio to create a request.
    
  
//   return 0;
  
// abort:
//     printk("<%p> Abort request\n\n", bio);
//     bio_io_error(bio);
//     #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
//     return 0;
//     #endif
// }





/**
 * Submit the bio to request.
 *  
 */
static void local_bd_io_fn(struct bio *bio)
{
	if (bio == NULL)
    printk("bio is NULL\n");

	bio->bi_bdev = local_bd.bdev_raw;  // Change to another block_device. This block_device is already registerred. 
	trace_block_bio_remap(bdev_get_queue(local_bd.bdev_raw), bio, bio->bi_bdev->bd_dev,	bio->bi_iter.bi_sector);

	generic_make_request(bio);  // Use this bio to create a request.
}



/**
 * Transfer bio to the local Block Device
 *  
 */
static int local_bd_threadfn(void *data){
    struct bio *bio;

    set_user_nice(current, -20);     // The cpu priority for this thread
    while (!kthread_should_stop()){
				// [x] Wait on req_event. 
        wait_event_interruptible(req_event, kthread_should_stop() ||
                !bio_list_empty(&local_bd.bio_list));

        spin_lock_irq(&local_bd.lock);
        if (bio_list_empty(&local_bd.bio_list)){
            spin_unlock_irq(&local_bd.lock);
            continue;
        }

        bio = bio_list_pop(&local_bd.bio_list);  // get a bio from the bio_list.
        spin_unlock_irq(&local_bd.lock);
        local_bd_io_fn(bio);  //invoke the function to build a request from this bio.
    }
    return 0;
}






/**
 * [?] What's the purpose of this open ??
 *  
 */
static struct block_device *local_bd_bdev_open(char dev_path[])
{

    struct block_device *bdev_raw = lookup_bdev(dev_path);

    printk("Opened %s\n", dev_path);
    if (IS_ERR(bdev_raw))
    {
        printk("local_bd: error opening raw device <%lu>\n", PTR_ERR(bdev_raw));
        return NULL;
    }

		// Check the mapping between  block_device <-> bd_dev
    if (!bdget(bdev_raw->bd_dev))
    {
        printk("local_bd: error bdget()\n");
        return NULL;
    }

		// open a block device
		// [?] What's the meaning of "open the block device"
    if (blkdev_get(bdev_raw, LOCAL_BD_BDEV_MODE, &local_bd))
    {
        printk("local_bd: error blkdev_get()\n");
        bdput(bdev_raw);
        return NULL;
    }
    return bdev_raw;
}





/**
 * [?] Use an exist logical partition as our disk.
 *  1) open the logical disk
 *  2) Use its information to initialize the struct local_bd.
 * 
 */
static int local_bd_start(char dev_path[])
{
    unsigned max_sectors;
    unsigned int page_sec = PAGE_SIZE;

	
		// Use the exist logical partition as our block device
		// Open the device.
    if (!(local_bd.bdev_raw = local_bd_bdev_open(dev_path)))
        return -EFAULT;

    /* Set up our internal device */
    local_bd.capacity = get_capacity(local_bd.bdev_raw->bd_disk);
    printk("local_bd: Device real capacity: %llu\n", (long long unsigned int) local_bd.capacity);
    set_capacity(local_bd.gd, local_bd.capacity);

    sector_div(page_sec, KERNEL_SECTOR_SIZE);
    max_sectors = page_sec * MAX_SGL_LEN;
    blk_queue_max_hw_sectors(local_bd.queue, max_sectors);
    printk("local_bd: Max sectors: %u\n", max_sectors);

		// Main a seperate thread to generate request for the local BD
		// Of the generic_make_request can cause problem.
    local_bd.thread = kthread_create(local_bd_threadfn, NULL,
           local_bd.gd->disk_name);

    if (IS_ERR(local_bd.thread))
    {
        printk("local_bd: error kthread_create <%lu>\n",
               PTR_ERR(local_bd.thread));
        goto error_after_bdev;
    }
    printk("local_bd: done initializing successfully\n");
    local_bd.is_active = 1;
    atomic_set(&local_bd.redirect_done, STACKBD_REDIRECT_OFF);   // [?] meaning of this ??
    wake_up_process(local_bd.thread);   // Wake up the thread


    return 0;
error_after_bdev:
    blkdev_put(local_bd.bdev_raw, LOCAL_BD_BDEV_MODE);
    bdput(local_bd.bdev_raw);
    return -EFAULT;
}



/**
 * Partition basic information ?
 *  
 */
int local_bd_getgeo(struct block_device * block_device, struct hd_geometry * geo)
{
        long size;
        /* We have no real geometry, of course, so make something up. */
        size = local_bd.capacity * (LOGICAL_BLOCK_SIZE / KERNEL_SECTOR_SIZE);
        geo->cylinders = (size & ~0x3f) >> 6;
        geo->heads = 4;
        geo->sectors = 16;
        geo->start = 0;
        return 0;
}


/**
 * Self defined block operation ?
 *  
 */
static struct block_device_operations local_bd_ops = {
    .owner       = THIS_MODULE,
    .getgeo      = local_bd_getgeo,
};







/**
 * Main entry : init a disk driver.
 *  
 */
int init_local_block_device(void){

	int err = 0;
	int major_num =0;


	//
	// For the local backup disk, local_bd.
	// Only set the gendisk information, use the its default deriver. 
	//
	/* Set up our internal device */
    spin_lock_init(&local_bd.lock);
    /* blk_alloc_queue() instead of blk_init_queue() so it won't set up the
     * queue for requests.
     */
    if (!(local_bd.queue = blk_alloc_queue(GFP_KERNEL))) // single queue
    {
        printk("local_bd: alloc_queue failed\n");
        return -EFAULT;
    }
    blk_queue_make_request(local_bd.queue, local_bd_make_request);  // register the i/o request building function.
    blk_queue_logical_block_size(local_bd.queue, LOGICAL_BLOCK_SIZE);
    /* Get registered */
    if ((major_num = register_blkdev(major_num, LOCAL_DISK_NAME)) < 0)
    {
        printk("local_bd: unable to get major number\n");
        err=-EFAULT;
        goto error_after_alloc_queue;
    }else{
        printk(KERN_INFO "%s, get major_num %d \n", __func__, major_num);
    }



    /* Gendisk structure, Register the disk/partition information to kernel. */
    if (!(local_bd.gd = alloc_disk(16))){  
    	goto error_after_redister_blkdev; 
    	err=-EFAULT;
    }
    local_bd.gd->major = major_num;
    local_bd.gd->first_minor = 0;
    local_bd.gd->fops = &local_bd_ops;
    local_bd.gd->private_data = &local_bd;  //reserved pointer,  points to itsefl 
    strcpy(local_bd.gd->disk_name, LOCAL_DISK_NAME_0);
    local_bd.gd->queue = local_bd.queue;

		// Add the local backup device
    add_disk(local_bd.gd);
    

		// 2) Register & start a daemon thread for this device?
    if (local_bd_start(LOCAL_DISK) < 0){
        printk("Kernel call returned: %m");
        err= -1;
    }

		printk(KERN_INFO "%s,local_bd: init done \n\n", __func__);
    goto out;

error_after_redister_blkdev:
    unregister_blkdev(major_num, LOCAL_DISK_NAME); 
error_after_alloc_queue:
    blk_cleanup_queue(local_bd.queue);   
    printk("stackbd queue cleaned up\n");

out:
	return err;

}




//
// Resource free
//

void un_register_local_disk(void){
    int major_num = local_bd.gd->major;

    if (local_bd.is_active)
    {
        //kthread_stop(local_bd.thread);
        blkdev_put(local_bd.bdev_raw, LOCAL_BD_BDEV_MODE);
        bdput(local_bd. bdev_raw);

        local_bd.is_active = 0;
    }
    del_gendisk(local_bd.gd);
    put_disk(local_bd.gd);
    unregister_blkdev(major_num, LOCAL_DISK_NAME);
    blk_cleanup_queue(local_bd.queue);

    printk( KERN_INFO "%s, local_bd: exit done. \n", __func__);
}
