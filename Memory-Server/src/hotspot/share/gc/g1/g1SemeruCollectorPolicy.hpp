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

#ifndef SHARE_VM_GC_G1_SEMERU_G1COLLECTORPOLICY_HPP
#define SHARE_VM_GC_G1_SEMERU_G1COLLECTORPOLICY_HPP

#include "gc/shared/collectorPolicy.hpp"

// G1CollectorPolicy is primarily used during initialization and to expose the
// functionality of the CollectorPolicy interface to the rest of the VM.

class G1YoungGenSizer;

class G1SemeruCollectorPolicy: public CollectorPolicy {
protected:
  // override the virutal functions
  void initialize_alignments();
  void initialize_flags();    
  void initialize_size_info();

  // Define these parameters in G1SemeruCollectorPolicy
  size_t  _semeru_max_heap_byte_size;
  size_t  _semeru_initial_heap_byte_size;
  size_t  _semeru_heap_alignment;      // Override the CollectorPolicy->_heap_alignment



  //
  // Abandoned paramters from super class : CollectorPolicy
  //  Override them and assign a , but never use them. 
  size_t _initial_heap_byte_size;
  size_t _max_heap_byte_size;
  //size_t _min_heap_byte_size; // Still use this one.

  // size_t _space_alignment;   // Still use this one.
  size_t _heap_alignment;       // [?] Used for Java heap ?

public:
  G1SemeruCollectorPolicy();

  // non-virtual override the 
  void initialize_all() {
    initialize_alignments();
    initialize_flags();
    initialize_size_info();
  }

  // Override the non-virutal functions of base class.
  // These non-virtual function call will be determined in C++/C compilation time.
  // which means that the version of non-virtual function is determined by the class variable, not the real object instance.
  size_t heap_reserved_size_bytes()         { return _semeru_max_heap_byte_size;   }
  size_t semeru_memory_pool_alignment()     { return _semeru_heap_alignment;  }
  
  // size_t space_alignment()        { return _space_alignment; }
  size_t heap_alignment()         { return _semeru_heap_alignment;  }

  size_t initial_heap_byte_size() { return _semeru_initial_heap_byte_size; }
  size_t max_heap_byte_size()     { return _semeru_max_heap_byte_size; }
  size_t min_heap_byte_size()     { return _min_heap_byte_size; }               // Use the CollectorPolicy default min heap size.

  bool is_hetero_heap()     const {  return false;  }

  void reset_the_abandoned_super_class_fields();

  #ifdef ASSERT

    void assert_flags();

  #endif

};



#endif // SHARE_VM_GC_G1_SEMERU_G1COLLECTORPOLICY_HPP
