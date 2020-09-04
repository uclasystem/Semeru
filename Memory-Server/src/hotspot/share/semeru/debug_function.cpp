#include "semeru/debug_function.h"





//
// ************** RDMA **************
//



void print_region_write_check_flag(size_t granulairty){
	size_t dirty_tag;
	size_t version_tag;
	volatile uint32_t *start = (uint32_t*)(SEMERU_START_ADDR + FLAGS_OF_CPU_WRITE_CHECK_OFFSET);
	volatile uint32_t *end = start + FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT/granulairty;
	size_t count = 0;

	log_debug(semeru, rdma)("%s, Check Write_Tag :", __func__);

	assert(sizeof(uint32_t)  == granulairty, "granularity is wrong.");

	for(; start != end; start+= sizeof(uint32_t)){
		dirty_tag = (size_t)( (*start & DIRTY_TAG_MASK) >> DIRTY_TAG_OFFSET );
		version_tag	=	(size_t)( (*start & VERSION_MASK) >> VERSION_TAG_OFFSET );

		if(dirty_tag | version_tag){
			printf(" Region[0x%lx], write_tag 0x%lx, val 0x%lx   dirty_tag : 0x%lx, version_tag 0x%lx \n", 
												count, (uint64_t)start, (size_t)(*start),  dirty_tag, version_tag);
		}

		count++;
	}

	log_debug(semeru, rdma)("%s, Check Write_Tag END. \n", __func__);
}