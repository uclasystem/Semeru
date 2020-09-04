/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_ALLOCATION_INLINE_HPP
#define SHARE_VM_MEMORY_ALLOCATION_INLINE_HPP

#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "services/memTracker.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"

// Explicit C-heap memory management

#ifndef PRODUCT
// Increments unsigned long value for statistics (not atomic on MP).
inline void inc_stat_counter(volatile julong* dest, julong add_value) {
#if defined(SPARC) || defined(X86)
  // Sparc and X86 have atomic jlong (8 bytes) instructions
  julong value = Atomic::load(dest);
  value += add_value;
  Atomic::store(value, dest);
#else
  // possible word-tearing during load/store
  *dest += add_value;
#endif
}
#endif

template <class E>
size_t MmapArrayAllocator<E>::size_for(size_t length) {
  size_t size = length * sizeof(E);
  int alignment = os::vm_allocation_granularity();
  return align_up(size, alignment);
}

template <class E>
E* MmapArrayAllocator<E>::allocate_or_null(size_t length, MEMFLAGS flags) {
  size_t size = size_for(length);
  int alignment = os::vm_allocation_granularity();

  char* addr = os::reserve_memory(size, NULL, alignment, flags);
  if (addr == NULL) {
    return NULL;
  }

  if (os::commit_memory(addr, size, !ExecMem)) {
    return (E*)addr;
  } else {
    os::release_memory(addr, size);
    return NULL;
  }
}

template <class E>
E* MmapArrayAllocator<E>::allocate(size_t length, MEMFLAGS flags) {
  size_t size = size_for(length);
  int alignment = os::vm_allocation_granularity();

  char* addr = os::reserve_memory(size, NULL, alignment, flags);
  if (addr == NULL) {
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "Allocator (reserve)");
  }

  os::commit_memory_or_exit(addr, size, !ExecMem, "Allocator (commit)");

  return (E*)addr;
}

template <class E>
void MmapArrayAllocator<E>::free(E* addr, size_t length) {
  bool result = os::release_memory((char*)addr, size_for(length));
  assert(result, "Failed to release memory");
}

template <class E>
size_t MallocArrayAllocator<E>::size_for(size_t length) {
  return length * sizeof(E);
}

template <class E>
E* MallocArrayAllocator<E>::allocate(size_t length, MEMFLAGS flags) {
  return (E*)AllocateHeap(size_for(length), flags);
}

template<class E>
void MallocArrayAllocator<E>::free(E* addr) {
  FreeHeap(addr);
}

template <class E>
bool ArrayAllocator<E>::should_use_malloc(size_t length) {
  return MallocArrayAllocator<E>::size_for(length) < ArrayAllocatorMallocLimit;
}

template <class E>
E* ArrayAllocator<E>::allocate_malloc(size_t length, MEMFLAGS flags) {
  return MallocArrayAllocator<E>::allocate(length, flags);
}

template <class E>
E* ArrayAllocator<E>::allocate_mmap(size_t length, MEMFLAGS flags) {
  return MmapArrayAllocator<E>::allocate(length, flags);
}

template <class E>
E* ArrayAllocator<E>::allocate(size_t length, MEMFLAGS flags) {
  if (should_use_malloc(length)) {
    return allocate_malloc(length, flags);
  }

  return allocate_mmap(length, flags);
}

template <class E>
E* ArrayAllocator<E>::reallocate(E* old_addr, size_t old_length, size_t new_length, MEMFLAGS flags) {
  E* new_addr = (new_length > 0)
      ? allocate(new_length, flags)
      : NULL;

  if (new_addr != NULL && old_addr != NULL) {
    memcpy(new_addr, old_addr, MIN2(old_length, new_length) * sizeof(E));
  }

  if (old_addr != NULL) {
    free(old_addr, old_length);
  }

  return new_addr;
}

template<class E>
void ArrayAllocator<E>::free_malloc(E* addr, size_t length) {
  MallocArrayAllocator<E>::free(addr);
}

template<class E>
void ArrayAllocator<E>::free_mmap(E* addr, size_t length) {
  MmapArrayAllocator<E>::free(addr, length);
}

template<class E>
void ArrayAllocator<E>::free(E* addr, size_t length) {
  if (addr != NULL) {
    if (should_use_malloc(length)) {
      free_malloc(addr, length);
    } else {
      free_mmap(addr, length);
    }
  }
}




//
//  Semeru Support
//
//  Added by Chenxi.



template <class E>
E* MmapArrayAllocator<E>::allocate_at(size_t length, MEMFLAGS flags, char* requested_addr) {
  size_t size = size_for(length);
  int alignment = os::vm_allocation_granularity();

  //1) reserve space at requested address.
  char* addr = os::semeru_attempt_reserve_memory_at(size, requested_addr, alignment, -1);      // Reserve space 
  if (addr == NULL || addr != requested_addr ) {
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "Allocator (reserve)");
  }

  // 2) Commit the reserved space immediately.
  //    why here is !ExecMem ?
  os::commit_memory_or_exit(addr, size, !ExecMem, "Allocator (commit)");  // Commit the space.

  return (E*)addr;
}


// Commit the space at already reserved space directly.
template <class E>
E* MmapArrayAllocator<E>::commit_at(size_t length, MEMFLAGS flags, char* requested_addr) {
  size_t size = size_for(length);

  // why here is !ExecMem ?
  os::commit_memory_or_exit(requested_addr, size, !ExecMem, "Allocator (commit)");  // Commit the space.

  return (E*)requested_addr;
}










template <class E>
bool SemeruArrayAllocator<E>::should_use_malloc(size_t length) {
  //return MallocArrayAllocator<E>::size_for(length) < ArrayAllocatorMallocLimit;

  // All the Semeru allocation use fixed start address.
  return false;
}

template <class E>
E* SemeruArrayAllocator<E>::allocate_malloc(size_t length, MEMFLAGS flags) {
  return MallocArrayAllocator<E>::allocate(length, flags);
}


template <class E>
E* SemeruArrayAllocator<E>::allocate_mmap(size_t length, MEMFLAGS flags) {
  return MmapArrayAllocator<E>::allocate(length, flags);
}

/**
 * Semeru
 * 1) Reserve space from OS
 * 2) Commit the reserved space. 
 */
template <class E>
E* SemeruArrayAllocator<E>::allocate_mmap_at(size_t length, MEMFLAGS flags, char* requested_addr) {
  return MmapArrayAllocator<E>::allocate_at(length, flags, requested_addr);
}

/**
 * Semeru
 * Commit the already reserved space directly. 
 */
// template <class E>
// E* SemeruArrayAllocator<E>::commit_at(size_t length, MEMFLAGS flags, char* requested_addr) {
//   return MmapArrayAllocator<E>::commit_at(length, flags, requested_addr);
// }


template <class E>
E* SemeruArrayAllocator<E>::allocate(size_t length, MEMFLAGS flags) {
  if (should_use_malloc(length)) {
    // Never use malloc.
    guarantee(false, "Never use malloc for Semeru RDMA structure allocation. \n");

    return allocate_malloc(length, flags);
  }

  return allocate_mmap(length, flags);
}

/**
 * This is used for building the Java heap. It's Multiple-Thread safe.
 *  1) Reserve space from OS
 *  2) Commit space on the reserved space.
 */
template <class E>
E* SemeruArrayAllocator<E>::allocate_target_oop_q(size_t length, MEMFLAGS flags, char* requested_addr) {
  // if (should_use_malloc(length)) {
  //   // Never use malloc.
  //   guarantee(false, "Never use malloc for Semeru RDMA structure allocation. \n");

  //   return allocate_malloc(length, flags);
  // }

  return allocate_mmap_at(length, flags, requested_addr);
}

/**
 * Commit space on the already reserved space. 
 */
template <class E>
E* SemeruArrayAllocator<E>::commit_target_oop_q(size_t length, MEMFLAGS flags, char* requested_addr) {

  return MmapArrayAllocator<E>::commit_at(length, flags, requested_addr);
}


template <class E>
E* SemeruArrayAllocator<E>::reallocate(E* old_addr, size_t old_length, size_t new_length, MEMFLAGS flags) {
  E* new_addr = (new_length > 0)
      ? allocate(new_length, flags)
      : NULL;

  if (new_addr != NULL && old_addr != NULL) {
    memcpy(new_addr, old_addr, MIN2(old_length, new_length) * sizeof(E));
  }

  if (old_addr != NULL) {
    free(old_addr, old_length);
  }

  return new_addr;
}

template<class E>
void SemeruArrayAllocator<E>::free_malloc(E* addr, size_t length) {
  MallocArrayAllocator<E>::free(addr);
}

template<class E>
void SemeruArrayAllocator<E>::free_mmap(E* addr, size_t length) {
  MmapArrayAllocator<E>::free(addr, length);
}

template<class E>
void SemeruArrayAllocator<E>::free(E* addr, size_t length) {
  if (addr != NULL) {
    if (should_use_malloc(length)) {
      free_malloc(addr, length);
    } else {
      free_mmap(addr, length);
    }
  }
}







#endif // SHARE_VM_MEMORY_ALLOCATION_INLINE_HPP
