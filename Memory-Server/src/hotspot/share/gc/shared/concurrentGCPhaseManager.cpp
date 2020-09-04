/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/concurrentGCPhaseManager.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"

#define assert_ConcurrentGC_thread() \
  assert(Thread::current()->is_ConcurrentGC_thread(), "precondition")

#define assert_not_enter_unconstrained(phase) \
  assert((phase) != UNCONSTRAINED_PHASE, "Cannot enter \"unconstrained\" phase")

#define assert_manager_is_tos(manager, stack, kind)  \
  assert((manager) == (stack)->_top, kind " manager is not top of stack")

ConcurrentGCPhaseManager::Stack::Stack() :
  _requested_phase(UNCONSTRAINED_PHASE),
  _top(NULL)
{ }


/**
 * Tag : Build a new ConcurrentGCPhaseManager.  
 * 
 * 
 * the Stack is shared between different ConcurrentGCPhaseManager ?
 *  
 */
ConcurrentGCPhaseManager::ConcurrentGCPhaseManager(int phase, Stack* stack) :
  _phase(phase),    /* Phase name */
  _active(true),    /* Actice this phase manager */
  _prev(NULL),
  _stack(stack)     /* Use the Stack assigned in Parameter*/
{
  assert_ConcurrentGC_thread();
  assert_not_enter_unconstrained(phase);
  assert(stack != NULL, "precondition");
  MonitorLockerEx ml(CGCPhaseManager_lock, Mutex::_no_safepoint_check_flag);
  if (stack->_top != NULL) {
    assert(stack->_top->_active, "precondition");
    _prev = stack->_top;
  }
  stack->_top = this;   // push the newly created PhaseManager into Stack.
  ml.notify_all();      // Notify the waiting threads before release the lock.
}

ConcurrentGCPhaseManager::~ConcurrentGCPhaseManager() {
  assert_ConcurrentGC_thread();
  MonitorLockerEx ml(CGCPhaseManager_lock, Mutex::_no_safepoint_check_flag);
  assert_manager_is_tos(this, _stack, "This");
  wait_when_requested_impl();
  _stack->_top = _prev;    // Pop the PhaseManager.
  ml.notify_all();
}


/**
 * [?] What's the meaninf of is_requested ??
 *  Current PhaseManager is requested by some Java Thread ? 
 * 
 *  => All the pushed PhaseManager on Stack is _active;
 *     && _stack->_requested is this PhaseManager, same phase title, 
 *     return true.
 * 
 */
bool ConcurrentGCPhaseManager::is_requested() const {
  assert_ConcurrentGC_thread();
  MonitorLockerEx ml(CGCPhaseManager_lock, Mutex::_no_safepoint_check_flag);
  assert_manager_is_tos(this, _stack, "This");
  return _active && (_stack->_requested_phase == _phase);
}


/**
 * [?] If current phase is active and requested (by some Java Thread), wait for its finishing.
 *  
 * For the ConcurrentGCPhaseManager:
 * 
 * 
 * [?] if 
 *  _active == true &&
 *  _stack->_requested_phase == ConcurrentGCPhaseManager->_phase
 * 
 *  wait on CGCPhaseManager_lock ?  Wait for what ??
 * 
 * 
 * return value,  waited or not ?
 * 
 */
bool ConcurrentGCPhaseManager::wait_when_requested_impl() const {
  assert_ConcurrentGC_thread();
  assert_lock_strong(CGCPhaseManager_lock);
  bool waited = false;
  while (_active && (_stack->_requested_phase == _phase)) {
    waited = true;
    CGCPhaseManager_lock->wait(Mutex::_no_safepoint_check_flag);
  }
  return waited;
}

bool ConcurrentGCPhaseManager::wait_when_requested() const {
  assert_ConcurrentGC_thread();
  MonitorLockerEx ml(CGCPhaseManager_lock, Mutex::_no_safepoint_check_flag);
  assert_manager_is_tos(this, _stack, "This");
  return wait_when_requested_impl();
}


/**
 * Tag : Set current Phase for the ConcurrentGCPhaseManager.
 *  
 * [?] set_phase modify a existing PhaseManager->_phase.
 *    Why not create a new one ?? 
 *    What's the purpose ?
 * 
 * 
 * [?] Purpose of the CGCPhaseManager_lock ?
 *    => The lock is used to synchronize Java Thread and Concurrent Thread ?
 * 
 * 
 *  
 */
void ConcurrentGCPhaseManager::set_phase(int phase, bool force) {
  assert_ConcurrentGC_thread();
  assert_not_enter_unconstrained(phase);
  MonitorLockerEx ml(CGCPhaseManager_lock, Mutex::_no_safepoint_check_flag);
  assert_manager_is_tos(this, _stack, "This");
  if (!force) wait_when_requested_impl();     // [?] Finish the requested Phase before switch to new phase.
  _phase = phase;
  ml.notify_all();  // Notify the concurrent thread waiting on CGCPhaseManager_lock to run.
}

void ConcurrentGCPhaseManager::deactivate() {
  assert_ConcurrentGC_thread();
  MonitorLockerEx ml(CGCPhaseManager_lock, Mutex::_no_safepoint_check_flag);
  assert_manager_is_tos(this, _stack, "This");
  _active = false;
  ml.notify_all();
}


/**
 * Tag : Java Thread is waiting for the execution of some phase.
 *       So, we should confrim  the finish of the requested phase.
 *  
 * 
 */
bool ConcurrentGCPhaseManager::wait_for_phase(int phase, Stack* stack) {
  assert(Thread::current()->is_Java_thread(), "precondition");
  assert(stack != NULL, "precondition");
  MonitorLockerEx ml(CGCPhaseManager_lock);
  // Update request and notify service of change.
  if (stack->_requested_phase != phase) {
    stack->_requested_phase = phase;      // Tell PhaseManager, we are requesting this phase ?
    ml.notify_all();
  }

  if (phase == UNCONSTRAINED_PHASE) {
    return true;
  }

  // Wait until phase or IDLE is active.
  while (true) {
    bool idle = false;
    for (ConcurrentGCPhaseManager* manager = stack->_top;
         manager != NULL;
         manager = manager->_prev) {
      if (manager->_phase == phase) {
        return true;            // phase is active.  // ? Just find this PhaseManager, why think it's active ??
      } else if (manager->_phase == IDLE_PHASE) {
        idle = true;            // Note idle active, continue search for phase.
      }
    }
    if (idle) {
      return false;             // idle is active and phase is not.
    } else {
      ml.wait();                // Wait for phase change.
    }
  }
}
