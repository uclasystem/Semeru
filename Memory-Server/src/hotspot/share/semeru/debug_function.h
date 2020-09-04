/**
 * Debug functions used for semeru development
 * 
 * 
 */ 

// Basic headers
#include "stdint.h"

// openjdk headers 
#include "utilities/globalDefinitions.hpp"
#include "logging/log.hpp"




//
// ****************** RDMA *********************
// 

/**
 * Check and print the all Region's write-check-flag value.
 *  
 */
void print_region_write_check_flag(size_t granulairty);