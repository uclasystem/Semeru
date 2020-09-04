/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_G1_SEMERU_G1BLOCKOFFSETTABLE_HPP
#define SHARE_VM_GC_G1_SEMERU_G1BLOCKOFFSETTABLE_HPP

#include "gc/g1/g1RegionToSpaceMapper.hpp"
#include "gc/shared/blockOffsetTable.hpp"
#include "memory/memRegion.hpp"
#include "memory/virtualspace.hpp"
#include "utilities/globalDefinitions.hpp"

// Forward declarations
class G1SemeruBlockOffsetTable;
class G1SemeruContiguousSpace;

// Semeru
class SemeruHeapRegion;


// This implementation of "G1SemeruBlockOffsetTable" divides the covered region
// into "N"-word subregions (where "N" = 2^"LogN".  An array with an entry
// for each such subregion indicates how far back one must go to find the
// start of the chunk that includes the first word of the subregion.
//
// Each G1SemeruBlockOffsetTablePart is owned by a G1SemeruContiguousSpace.

class G1SemeruBlockOffsetTable: public CHeapRDMAObj<G1SemeruBlockOffsetTable, NON_ALLOC_TYPE> {
  friend class G1SemeruBlockOffsetTablePart;
  friend class VMStructs;

private:
  // The reserved region covered by the table.
  MemRegion _reserved;

  // Array for keeping offsets for retrieving object start fast given an address.  
  // This should be the real content of the Block Offset Table.
  // each block is 512 bytes, 8 byts alignment, 1 byte is enough to record the address of it.
  u_char* _offset_array;          // byte array keeping backwards offsets

  void check_offset(size_t offset, const char* msg) const {
    assert(offset <= BOTConstants::N_words,
           "%s - offset: " SIZE_FORMAT ", N_words: %u",
           msg, offset, BOTConstants::N_words);
  }

  // Bounds checking accessors:
  // For performance these have to devolve to array accesses in product builds.
  inline u_char offset_array(size_t index) const;

  void set_offset_array_raw(size_t index, u_char offset) {
    _offset_array[index] = offset;
  }

  inline void set_offset_array(size_t index, u_char offset);

  inline void set_offset_array(size_t index, HeapWord* high, HeapWord* low);

  inline void set_offset_array(size_t left, size_t right, u_char offset);

  bool is_card_boundary(HeapWord* p) const;

  void check_index(size_t index, const char* msg) const NOT_DEBUG_RETURN;

public:

  // Return the number of slots needed for the offset array
  // that covers mem_region_words words.
  static size_t compute_size(size_t mem_region_words) {
    size_t number_of_slots = (mem_region_words / BOTConstants::N_words);   // number of slots/blocks:  e.g. 32GB heap.  4G words/(64 word per block)
    size_t block_off_table_bytes = ReservedSpace::allocation_align_size_up(number_of_slots); // page alignment.
    assert(block_off_table_bytes <= BLOCK_OFFSET_TABLE_OFFSET_SIZE_LIMIT, "Exceed size limitations.");

    return block_off_table_bytes;       // 1 byte,u_char, per slot.
  }

  // Returns how many bytes of the heap a single byte of the BOT corresponds to.
  static size_t heap_map_factor() {
    return BOTConstants::N_bytes;     // Same as card, 512 bytes per block.
  }

  // Initialize the Block Offset Table to cover the memory region passed
  // in the heap parameter.
  G1SemeruBlockOffsetTable(MemRegion heap, G1RegionToSpaceMapper* storage);

  // Return the appropriate index into "_offset_array" for "p".
  inline size_t index_for(const void* p) const;
  inline size_t index_for_raw(const void* p) const;

  // Return the address indicating the start of the region corresponding to
  // "index" in "_offset_array".
  inline HeapWord* address_for_index(size_t index) const;
  // Variant of address_for_index that does not check the index for validity.
  inline HeapWord* address_for_index_raw(size_t index) const {
    return _reserved.start() + (index << BOTConstants::LogN_words);
  }
};

class G1SemeruBlockOffsetTablePart {
  friend class G1SemeruBlockOffsetTable;
  friend class VMStructs;
private:
  // allocation boundary at which offset array must be updated
  HeapWord* _next_offset_threshold;
  size_t    _next_offset_index;      // index corresponding to that boundary ? _next_offset_index - 1 is the max_index ?

  // Indicates if an object can span into this G1SemeruBlockOffsetTablePart.
  debug_only(bool _object_can_span;)

  // This is the global BlockOffsetTable. Fixed Address.
  // points to the G1SemeruCollectedHeap->_bot, which covers the whole Java heap.
  // So, there is only one global _bot->_offset_array shared by all the Regions.
  G1SemeruBlockOffsetTable* _bot;

  // The Region/space that owns this subregion.
  // Usually, this is a Region. A contiguous space.
  // [x]This value must be reset after each transfer.
  // Because CPU server and Memory server have different values for it.
  SemeruHeapRegion* _space;

  // Semeru 
  // Covered block offset table start addr and length in char.
  // Used to synchronize between CPU and Memory server.
  u_char* _offset_array_part;
  size_t  _offset_array_part_length;


  // Sets the entries
  // corresponding to the cards starting at "start" and ending at "end"
  // to point back to the card before "start": the interval [start, end)
  // is right-open.
  void set_remainder_to_point_to_start(HeapWord* start, HeapWord* end);
  // Same as above, except that the args here are a card _index_ interval
  // that is closed: [start_index, end_index]
  void set_remainder_to_point_to_start_incl(size_t start, size_t end);

  // Zero out the entry for _bottom (offset will be zero). Does not check for availability of the
  // memory first.
  void zero_bottom_entry_raw();
  // Variant of initialize_threshold that does not check for availability of the
  // memory first.
  HeapWord* initialize_threshold_raw();

  inline size_t block_size(const HeapWord* p) const;

  // Returns the address of a block whose start is at most "addr".
  // If "has_max_index" is true, "assumes "max_index" is the last valid one
  // in the array.
  inline HeapWord* block_at_or_preceding(const void* addr,
                                         bool has_max_index,
                                         size_t max_index) const;

  // "q" is a block boundary that is <= "addr"; "n" is the address of the
  // next block (or the end of the space.)  Return the address of the
  // beginning of the block that contains "addr".  Does so without side
  // effects (see, e.g., spec of  block_start.)
  inline HeapWord* forward_to_block_containing_addr_const(HeapWord* q, HeapWord* n,
                                                          const void* addr) const;

  // "q" is a block boundary that is <= "addr"; return the address of the
  // beginning of the block that contains "addr".  May have side effects
  // on "this", by updating imprecise entries.
  inline HeapWord* forward_to_block_containing_addr(HeapWord* q,
                                                    const void* addr);

  // "q" is a block boundary that is <= "addr"; "n" is the address of the
  // next block (or the end of the space.)  Return the address of the
  // beginning of the block that contains "addr".  May have side effects
  // on "this", by updating imprecise entries.
  HeapWord* forward_to_block_containing_addr_slow(HeapWord* q,
                                                  HeapWord* n,
                                                  const void* addr);

  // Requires that "*threshold_" be the first array entry boundary at or
  // above "blk_start", and that "*index_" be the corresponding array
  // index.  If the block starts at or crosses "*threshold_", records
  // "blk_start" as the appropriate block start for the array index
  // starting at "*threshold_", and for any other indices crossed by the
  // block.  Updates "*threshold_" and "*index_" to correspond to the first
  // index after the block end.
  void alloc_block_work(HeapWord** threshold_, size_t* index_,
                        HeapWord* blk_start, HeapWord* blk_end);

  void check_all_cards(size_t left_card, size_t right_card) const;

public:
  //  The elements of the array are initialized to zero.
  G1SemeruBlockOffsetTablePart(G1SemeruBlockOffsetTable* array, SemeruHeapRegion* gsp);

  void  initialize_array_offset_par();

  void verify() const;

  // Returns the address of the start of the block containing "addr", or
  // else "null" if it is covered by no block.  (May have side effects,
  // namely updating of shared array entries that "point" too far
  // backwards.  This can occur, for example, when lab allocation is used
  // in a space covered by the table.)
  inline HeapWord* block_start(const void* addr);
  // Same as above, but does not have any of the possible side effects
  // discussed above.
  inline HeapWord* block_start_const(const void* addr) const;

  // Initialize the threshold to reflect the first boundary after the
  // bottom of the covered region.
  HeapWord* initialize_threshold();

  void reset_bot() {
    zero_bottom_entry_raw();
    initialize_threshold_raw();
  }

  // Return the next threshold, the point at which the table should be
  // updated.
  HeapWord* threshold() const { return _next_offset_threshold; }

  // These must be guaranteed to work properly (i.e., do nothing)
  // when "blk_start" ("blk" for second version) is "NULL".  In this
  // implementation, that's true because NULL is represented as 0, and thus
  // never exceeds the "_next_offset_threshold".
  void alloc_block(HeapWord* blk_start, HeapWord* blk_end) {
    if (blk_end > _next_offset_threshold) {
      alloc_block_work(&_next_offset_threshold, &_next_offset_index, blk_start, blk_end);
    }
  }
  void alloc_block(HeapWord* blk, size_t size) {
    alloc_block(blk, blk+size);
  }

  void set_for_starts_humongous(HeapWord* obj_top, size_t fill_size);
  void set_object_can_span(bool can_span) NOT_DEBUG_RETURN;

  void print_on(outputStream* out) PRODUCT_RETURN;



  // Semeru
  void  initialize_array_offset_par(G1SemeruBlockOffsetTable* array, SemeruHeapRegion* coverd_region);
  void reset_fields_after_transfer(SemeruHeapRegion* covered_region);
};

#endif // SHARE_VM_GC_G1_G1BLOCKOFFSETTABLE_HPP