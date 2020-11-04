/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/frontswap.h>
#include <linux/blkdev.h>
#include <linux/uio.h>
#include <asm/pgtable.h>

//Semeru
#include <linux/swap_global_struct_bd_layer.h>


/**
 * 
 * Use the swap_cache to initiate the bio.
 * 	All the sector addr, swp_entry_t, and physical memory information are stored in "struct page". 
 *  
 */
static struct bio *get_swap_bio(gfp_t gfp_flags,
				struct page *page, bio_end_io_t end_io)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, 1);  // every time allocate a new bio for the pag ?
	if (bio) {
		bio->bi_iter.bi_sector = map_swap_page(page, &bio->bi_bdev);  // basicly, sector addr is derived from page->private.
		bio->bi_iter.bi_sector <<= PAGE_SHIFT - 9;
		bio->bi_end_io = end_io;

		bio_add_page(bio, page, PAGE_SIZE, 0);   // For swap, the length is always PAGE_SIZE, offset is 0.
		BUG_ON(bio->bi_iter.bi_size != PAGE_SIZE);
	}
	return bio;
}

/**
 * Semeru CPU 
 * Use the swap_cache to initiate the bio.
 * 	All the sector addr, swp_entry_t, and physical memory information are stored in "struct page". 
 *  
 * Use virtual address to replace the sector address.
 * 
 * 
 * Parameter :
 * 	gfp_flags :	
 * 	page : the physical page to be swapped out.
 *  end_io : the function to process the end bio  ?
 * 
 * 
 * More explanation.
 * 
 * 	[?] Why does here assign the got sector address to bio->bi_iter.bi_sector directly ?
 * 		  Because this is the first sector added to this bio ?
 * 
 */
static struct bio *semeru_get_swap_bio(gfp_t gfp_flags,
				struct page *page, bio_end_io_t end_io)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, 1);  // Allocate a new bio , 1 bio_vec in it.
	if (bio) {

		#ifdef ENABLE_SWP_ENTRY_VIRT_REMAPPING
				bio->bi_iter.bi_sector = semeru_map_swap_page(page, &bio->bi_bdev);  // get page's mapping sector addr.
		#else
				bio->bi_iter.bi_sector = map_swap_page(page, &bio->bi_bdev);  // basicly, sector addr is derived from page->private.
		#endif
		bio->bi_iter.bi_sector <<= PAGE_SHIFT - 9;  // change page index, 4KB, to sector index, 512 bytes.
		bio->bi_end_io = end_io;

		bio_add_page(bio, page, PAGE_SIZE, 0);   // For swap, the length is always PAGE_SIZE, offset is 0.
		BUG_ON(bio->bi_iter.bi_size != PAGE_SIZE);
	}
	return bio;
}






void end_swap_bio_write(struct bio *bio)
{
	struct page *page = bio->bi_io_vec[0].bv_page;

	if (bio->bi_error) {
		SetPageError(page);
		/*
		 * We failed to write the page out to swap-space.
		 * Re-dirty the page in order to avoid it being reclaimed.
		 * Also print a dire warning that things will go BAD (tm)
		 * very quickly.
		 *
		 * Also clear PG_reclaim to avoid rotate_reclaimable_page()
		 */
		set_page_dirty(page);
		pr_alert("Write-error on swap-device (%u:%u:%llu)\n",
			 imajor(bio->bi_bdev->bd_inode),
			 iminor(bio->bi_bdev->bd_inode),
			 (unsigned long long)bio->bi_iter.bi_sector);
		ClearPageReclaim(page);
	}
	end_page_writeback(page);
	bio_put(bio);
}

static void swap_slot_free_notify(struct page *page)
{
	struct swap_info_struct *sis;
	struct gendisk *disk;

	/*
	 * There is no guarantee that the page is in swap cache - the software
	 * suspend code (at least) uses end_swap_bio_read() against a non-
	 * swapcache page.  So we must check PG_swapcache before proceeding with
	 * this optimization.
	 */
	if (unlikely(!PageSwapCache(page)))
		return;

	sis = page_swap_info(page);
	if (!(sis->flags & SWP_BLKDEV))
		return;

	/*
	 * The swap subsystem performs lazy swap slot freeing,
	 * expecting that the page will be swapped out again.
	 * So we can avoid an unnecessary write if the page
	 * isn't redirtied.
	 * This is good for real swap storage because we can
	 * reduce unnecessary I/O and enhance wear-leveling
	 * if an SSD is used as the as swap device.
	 * But if in-memory swap device (eg zram) is used,
	 * this causes a duplicated copy between uncompressed
	 * data in VM-owned memory and compressed data in
	 * zram-owned memory.  So let's free zram-owned memory
	 * and make the VM-owned decompressed page *dirty*,
	 * so the page should be swapped out somewhere again if
	 * we again wish to reclaim it.
	 */
	disk = sis->bdev->bd_disk;
	if (disk->fops->swap_slot_free_notify) {
		swp_entry_t entry;
		unsigned long offset;

		entry.val = page_private(page);
		offset = swp_offset(entry);

		SetPageDirty(page);
		disk->fops->swap_slot_free_notify(sis->bdev,
				offset);
	}
}

static void end_swap_bio_read(struct bio *bio)
{
	struct page *page = bio->bi_io_vec[0].bv_page;

	if (bio->bi_error) {
		SetPageError(page);
		ClearPageUptodate(page);
		pr_alert("Read-error on swap-device (%u:%u:%llu)\n",
			 imajor(bio->bi_bdev->bd_inode),
			 iminor(bio->bi_bdev->bd_inode),
			 (unsigned long long)bio->bi_iter.bi_sector);
		goto out;
	}

	SetPageUptodate(page);
	swap_slot_free_notify(page);
out:
	unlock_page(page);
	bio_put(bio);
}

int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent list.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		cond_resched();

		first_block = bmap(inode, probe_block);
		if (first_block == 0)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = bmap(inode, probe_block + block_in_page);
			if (block == 0)
				goto bad_bmap;
			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
	sis->highest_bit = page_no - 1;
out:
	return ret;
bad_bmap:
	pr_err("swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
int swap_writepage(struct page *page, struct writeback_control *wbc)
{
	int ret = 0;

	if (try_to_free_swap(page)) {		// #1 Check if this page is already swapped out, we can free the swap entry and page directly.
		unlock_page(page);
		goto out;
	}

	if (frontswap_store(page) == 0) {	// #2, Fast swap path. Synchronous swap out to a ultra fast device.
		set_page_writeback(page);
		unlock_page(page);
		end_page_writeback(page);
		goto out;
	}

	ret = __swap_writepage(page, wbc, end_swap_bio_write);  // #3, do the swap action. Build a bio and write the page to Block Device.
out:
	return ret;
}

/**
 * Calculate the corresponding sector address of this physical page,
 * it's recoreded in page->private.
 * 
 * [?] What's the connection between bio sector address and the swap_entry_t ?
 * 
 * 	Always assume sector size if 512 bytes.
 */
static sector_t swap_page_sector(struct page *page)
{
	return (sector_t)__page_file_index(page) << (PAGE_SHIFT - 9);  // 12(PAGE_SHIFT) - 9(SECTOR_SHIFT)
}


/**
 * Semeru CPU - retrive swp_entry_t's remapping virt_addr
 *  
 * Assumption
 * 	1) The disk still uses sector to index it data. which is 512 bytes/sector
 * 
 */
static sector_t swap_page_virt_addr_sector(struct page *page, bool debug_mode)
{
	return (sector_t)__virt_page_index(page, debug_mode) << (PAGE_SHIFT - 9);  // 12(PAGE_SHIFT) - 9(SECTOR_SHIFT)
}




/**
 * Swap out a physical page to swap partition.
 *  
 */
int __swap_writepage(struct page *page, struct writeback_control *wbc,
		bio_end_io_t end_write_func)
{
	struct bio *bio;
	int ret;
	struct swap_info_struct *sis = page_swap_info(page);

	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	if (sis->flags & SWP_FILE) {
		struct kiocb kiocb;
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;
		struct bio_vec bv = {
			.bv_page = page,
			.bv_len  = PAGE_SIZE,
			.bv_offset = 0
		};
		struct iov_iter from;

		iov_iter_bvec(&from, ITER_BVEC | WRITE, &bv, 1, PAGE_SIZE);
		init_sync_kiocb(&kiocb, swap_file);
		kiocb.ki_pos = page_file_offset(page);

		set_page_writeback(page);
		unlock_page(page);
		ret = mapping->a_ops->direct_IO(&kiocb, &from);
		if (ret == PAGE_SIZE) {
			count_vm_event(PSWPOUT);
			ret = 0;
		} else {
			/*
			 * In the case of swap-over-nfs, this can be a
			 * temporary failure if the system has limited
			 * memory for allocating transmit buffers.
			 * Mark the page dirty and avoid
			 * rotate_reclaimable_page but rate-limit the
			 * messages but do not flag PageError like
			 * the normal direct-to-bio case as it could
			 * be temporary.
			 */
			set_page_dirty(page);
			ClearPageReclaim(page);
			pr_err_ratelimited("Write error on dio swapfile (%llu)\n",
					   page_file_offset(page));
		}
		end_page_writeback(page);
		return ret;
	}

	//ret = bdev_write_page(sis->bdev, swap_page_sector(page), page, wbc); // Get the device information to be paged out.
	// Semeru CPU
	// Path #1
	ret = bdev_write_page(sis->bdev, swap_page_virt_addr_sector(page, 0), page, wbc);
	if (!ret) {
		count_vm_event(PSWPOUT);
		return 0;
	}

	// Semeru CPU 
	// Path #2
	ret = 0;  // return 0, if submit the bio successfully.
	//bio = get_swap_bio(GFP_NOIO, page, end_write_func);
	bio = semeru_get_swap_bio(GFP_NOIO, page, end_write_func);
	
	if (bio == NULL) {
		set_page_dirty(page);  // [?] swap out failed ?
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	bio->bi_opf = REQ_OP_WRITE | wbc_to_write_flags(wbc);
	count_vm_event(PSWPOUT);
	set_page_writeback(page);  // start marking page as PG_writeback.
	unlock_page(page);
	submit_bio(bio);		// [?] Submit to where ??
out:
	return ret;
}


/**
 * Do the action of swapin
 * 
 * From swp_entry_t --> physical page,
 * 	value of swp_entry_t is recored in page->private. 
 * 
 *  
 */
int swap_readpage(struct page *page)
{
	struct bio *bio;
	int ret = 0;
	struct swap_info_struct *sis = page_swap_info(page);

	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageUptodate(page), page);
	if (frontswap_load(page) == 0) {
		SetPageUptodate(page);
		unlock_page(page);
		goto out;
	}

	// #1 
	// if the swap partition is a file
	// 
	if (sis->flags & SWP_FILE) {  
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;

		ret = mapping->a_ops->readpage(swap_file, page);
		if (!ret)
			count_vm_event(PSWPIN);
		return ret;
	}

	// #2 
	// swap data from a block device.
	// [?] why this can null ??
	//ret = bdev_read_page(sis->bdev, swap_page_sector(page), page);
	// Smeeru CPU
	ret = bdev_read_page(sis->bdev, swap_page_virt_addr_sector(page, 0), page);  // return the recorded virt_addr or swp_offet
	if (!ret) {
		if (trylock_page(page)) {
			swap_slot_free_notify(page);
			unlock_page(page);
		}

		count_vm_event(PSWPIN);
		return 0;
	}

	// Clear the swp_entry to virt_addr_page entry.



	// #3,
	// Build a bio for this swp_entry_t and send it to block layer.
	//
	ret = 0;
	//bio = get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);  // Original, build the bio for this swap operation.
	bio = semeru_get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);  // build the bio for this swap operation.

	if (bio == NULL) {
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	count_vm_event(PSWPIN);
	submit_bio(bio);
out:
	return ret;
}

int swap_set_page_dirty(struct page *page)
{
	struct swap_info_struct *sis = page_swap_info(page);

	if (sis->flags & SWP_FILE) {
		struct address_space *mapping = sis->swap_file->f_mapping;

		VM_BUG_ON_PAGE(!PageSwapCache(page), page);
		return mapping->a_ops->set_page_dirty(page);
	} else {
		return __set_page_dirty_no_writeback(page);
	}
}
