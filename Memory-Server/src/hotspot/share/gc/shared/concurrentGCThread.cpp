/*
 * Copyright (c) 2001, 2016, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "classfile/systemDictionary.hpp"
#include "gc/shared/concurrentGCThread.hpp"
#include "oops/instanceRefKlass.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/os.hpp"

ConcurrentGCThread::ConcurrentGCThread() :
  _should_terminate(false), _has_terminated(false) {
};

/**
 * Tag : 1) Create a pthread based ConcurrentThread.
 *       2) Wake up the created child thread, when it's initialized.     
 *     [?] The first parameter of os::create_thread, pass "this thread" to the function ? What's the purpose ?
 */
void ConcurrentGCThread::create_and_start(ThreadPriority prio) {

    #ifdef ASSERT
      log_debug(gc,thread)("%s,Try to create pthrad based concurrent thread 0x%lx \n", __func__, (size_t)this);
    #endif

  if (os::create_thread(this, os::cgc_thread)) {   // 1) Create a pthread for this JVM ConcurrentThread (handler)
    // XXX: need to set this to low priority       //     The created pthread waits on metex, sync.
    // unless "aggressive mode" set; priority
    // should be just less than that of VMThread.
    os::set_priority(this, prio);

    #ifdef ASSERT
      log_debug(gc,thread)("%s, create pthrad based concurrent thread 0x%lx done \n", __func__, (size_t)this);
    #endif

    if (!_should_terminate) {
      #ifdef ASSERT
        log_debug(gc,thread)("%s, go to wake up concurrent thread 0x%lx \n", __func__, (size_t)this);
      #endif
      os::start_thread(this);                      // 2) Schedule the created pthread to run.
    }
  }
}

void ConcurrentGCThread::initialize_in_thread() {
  this->initialize_named_thread();
  this->set_active_handles(JNIHandleBlock::allocate_block());
  // From this time Thread::current() should be working.
  assert(this == Thread::current(), "just checking");
}

void ConcurrentGCThread::wait_for_universe_init() {
  MutexLockerEx x(CGC_lock, Mutex::_no_safepoint_check_flag);
  while (!is_init_completed() && !_should_terminate) {
    CGC_lock->wait(Mutex::_no_safepoint_check_flag, 1);
  }
}



/**
 * Terminate a Concurrent Thread,
 *  [?] exit it ? or Let it wait on a Mutex ? 
 * 
 * 
 */
void ConcurrentGCThread::terminate() {
  assert(_should_terminate, "Should only be called on terminate request.");
  // Signal that it is terminated
  {
    MutexLockerEx mu(Terminator_lock,
                     Mutex::_no_safepoint_check_flag);
    _has_terminated = true;
    Terminator_lock->notify();
  }
}


/**
 * The main entry of the concurrent marking component. 
 */
void ConcurrentGCThread::run() {

  #ifdef ASSERT
	log_debug(gc,thread)("%s, Execute Concurrent thread 0x%lx 's service.\n",__func__, (size_t)this);
	#endif

  initialize_in_thread();
  wait_for_universe_init();  // Confirm the heap is initialized. Wait on lock CGC_lock  and init->_init_completed.

  run_service();

  terminate();  // If exit from the ConcurrentGCThread's service, terminate this thread ?
}


/**
 * [?] ConcurrentThread can't invoke this function ??
 *  
 *  Because the default paramter of Mutex->wait() is going to "check_the_safepoint" :
 *  bool wait(bool no_safepoint_check = !_no_safepoint_check_flag,
 *           long timeout = 0,
 *           bool as_suspend_equivalent = !_as_suspend_equivalent_flag);
 * 
 *  Only JavaThread can check the safepoint ??
 * 
 */
void ConcurrentGCThread::stop() {
  // it is ok to take late safepoints here, if needed
  {
    MutexLockerEx mu(Terminator_lock);
    assert(!_has_terminated,   "stop should only be called once");
    assert(!_should_terminate, "stop should only be called once");
    _should_terminate = true;
  }

  stop_service();

  {
    MutexLockerEx mu(Terminator_lock);
    while (!_has_terminated) {
      Terminator_lock->wait();     // the default parameter :  safepoint_check, timeout = 0, not_as_suspended_equivalent.
    }
  }
}
