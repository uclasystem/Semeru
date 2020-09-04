/*
 * bvec iterator
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */
#ifndef __LINUX_BVEC_ITER_H
#define __LINUX_BVEC_ITER_H

#include <linux/kernel.h>
#include <linux/bug.h>

/*
 * was unsigned short, but we might as well be ready for > 64kB I/O pages
 * 
 * Tag : record the sector's corresponding physical memory addres information
 * 
 * For a segment, 
 * 	1) its address in corresponding physical memory:
 *		mapped physical page is bio->bi_io_vec[id].bv_page
 *  	start address is bio->bi_io_vec[id].bv_offset
 *		length is 	bio->bi_io_vec[id].bv_len
 * 		
 *		a segment can havve multiple sectors, 512 bytes, and not necessarily 4K alignment. 
 * 
 * 
 * [x] so the problem now is that how to record the sector id ??		
 *  	use bio->bvec_iter to index bio->bio_vec,
 * 		one to one mapping.
 */
struct bio_vec {
	struct page		*bv_page;			// the corresponding physical page address of this sector
	unsigned int	bv_len;				// count in bytes, the size of the bio_vec (vector, segment). bv_len >> 9, the size in sectors. 
	unsigned int	bv_offset;		// the offset for this sector
};


/**
 * Tag : sectors attached to current bio
 * 
 * More Explanation
 * 		[x] how to record multiple sector here ?
 * 		use bio->bvec_iter to index bio->bio_vec,
 * 		one to one mapping.
 * 		
 * [?] how to iterate this field ?
 *  There is only one start address, bi_sector, which assumes the all the sectors are contiguous ?
 * 
 */
struct bvec_iter {
	sector_t				bi_sector;		/* device address in 512 byte sectors */  // [?] only 1 sector address ? assume all the sectors are contiguous ?
	unsigned int		bi_size;			/* residual I/O count */  // sector size, counting in  bytes, are waiting to be processed (read/write).
	unsigned int		bi_idx;				/* current index into bvl_vec */  // into bio->bio_vec, the corresponding physical page.
	unsigned int    bi_bvec_done;	/* number of bytes completed in  current bvec */
};

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

#define bvec_iter_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define bvec_iter_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

static inline void bvec_iter_advance(const struct bio_vec *bv,
				     struct bvec_iter *iter,
				     unsigned bytes)
{
	WARN_ONCE(bytes > iter->bi_size,
		  "Attempted to advance past end of bvec iter\n");

	while (bytes) {
		unsigned iter_len = bvec_iter_len(bv, *iter); 
		unsigned len = min(bytes, iter_len);

		bytes -= len;
		iter->bi_size -= len;
		iter->bi_bvec_done += len;

		if (iter->bi_bvec_done == __bvec_iter_bvec(bv, *iter)->bv_len) {
			iter->bi_bvec_done = 0;
			iter->bi_idx++;
		}
	}
}

#define for_each_bvec(bvl, bio_vec, iter, start)			\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bvl = bvec_iter_bvec((bio_vec), (iter))), 1);	\
	     bvec_iter_advance((bio_vec), &(iter), (bvl).bv_len))

#endif /* __LINUX_BVEC_ITER_H */
