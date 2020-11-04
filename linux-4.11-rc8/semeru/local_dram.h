/**
 * Used to debug frontswap module.
 *  
 */

#ifndef LOCAL_DRAM
#define LOCAL_DRAM

#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

int semeru_init_local_dram(void);
int semeru_remove_local_dram(void);
int semeru_dram_read(struct page *page, size_t roffset);
int semeru_dram_write(struct page *page, size_t roffset);


#endif // end of LOCAL_DRAM