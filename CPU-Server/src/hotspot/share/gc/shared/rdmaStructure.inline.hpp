/**
 * 1) All the inline functions should be visible for the compiler, when it compiles a translation unit(or compilation unit).
 * So, these functions need to be defined in a header file and included by the cpp, who needs them. 
 *
 * 2) put the keyword inline before definition is good enough. This will eliminate the multiple definitions problem,
 *    when inlcude to multiple cpp files.
 */

#ifndef SHARE_GC_SHARED_RDMA_STRUCTURE_INLINE
#define SHARE_GC_SHARED_RDMA_STRUCTURE_INLINE



#include "gc/shared/taskqueue.inline.hpp"

// Semeru headers
#include "gc/shared/rdmaStructure.hpp"
#include "gc/shared/rdmaAllocation.inline.hpp"




//
//	Structure -	TaskQueueRDMASuper
//


// Implementation of  TaskQueueRDMASuper<N, F>::Age  cmpxchg
template <class E, unsigned int N, CHeapAllocType Alloc_type>
inline typename TaskQueueRDMASuper<E, N, Alloc_type>::Age TaskQueueRDMASuper<E, N, Alloc_type>::Age::cmpxchg(const Age new_age, const Age old_age) volatile {
  return Atomic::cmpxchg(new_age._data, &_data, old_age._data);
}





//
//	Structure -GenericTaskQueueRDMA
//


template<class E, CHeapAllocType Alloc_type, unsigned int N>
template<class Fn>
inline void GenericTaskQueueRDMA<E, Alloc_type, N>::iterate(Fn fn) {
  uint iters = size();
  uint index = _bottom;
  for (uint i = 0; i < iters; ++i) {
    index = decrement_index(index);
    fn(const_cast<E&>(_elems[index])); // cast away volatility
  }
}



/**
 * Tag : Push a elements into GenericTaskQueueRDMA
 * 
 * if dirty_n_elems < max_elems,   // N -2
 *    push elem into _elem[_bottom]
 *    _bottom++
 * else
 *    // push slow path 
 *    if dirty_n_lemes == max_elems +1 // N-1
 *      push elem into _elem[_bottom]
 *      _bottom++
 *    else
 *      return false
 *    
 *    return false.
 * 
 *  [?]  the elements between _bottom and _age.top() are dirty ??
 * 
 */
template<class E, CHeapAllocType Alloc_type, unsigned int N> inline bool
GenericTaskQueueRDMA<E, Alloc_type, N>::push(E t) {
  uint localBot = _bottom;
  assert(localBot < N, "_bottom out of range.");
  idx_t top = _age.top();         // _age.top ??
  uint dirty_n_elems = dirty_size(localBot, top);       // [?] What's the definition of dirty elements ?
  assert(dirty_n_elems < N, "n_elems out of range.");
  if (dirty_n_elems < max_elems()) {
    // g++ complains if the volatile result of the assignment is
    // unused, so we cast the volatile away.  We cannot cast directly
    // to void, because gcc treats that as not using the result of the
    // assignment.  However, casting to E& means that we trigger an
    // unused-value warning.  So, we cast the E& to void.
    (void) const_cast<E&>(_elems[localBot] = t);
    OrderAccess::release_store(&_bottom, increment_index(localBot));   // _bottom = (++localBot)
    TASKQUEUE_STATS_ONLY(stats.record_push());
    return true;
  } else {
    return push_slow(t, dirty_n_elems);
  }
}




template<class E, CHeapAllocType Alloc_type, unsigned int N>
bool GenericTaskQueueRDMA<E, Alloc_type, N>::push_slow(E t, uint dirty_n_elems) {
  if (dirty_n_elems == N - 1) {
    // Actually means 0, so do the push.
    uint localBot = _bottom;
    // g++ complains if the volatile result of the assignment is
    // unused, so we cast the volatile away.  We cannot cast directly
    // to void, because gcc treats that as not using the result of the
    // assignment.  However, casting to E& means that we trigger an
    // unused-value warning.  So, we cast the E& to void.
    (void)const_cast<E&>(_elems[localBot] = t);
    OrderAccess::release_store(&_bottom, increment_index(localBot));
    TASKQUEUE_STATS_ONLY(stats.record_push());
    return true;
  }
  return false;
}




// Will this function also be inlined ???
// Why have to put it here ?
template<class E, CHeapAllocType Alloc_type, unsigned int N>
bool GenericTaskQueueRDMA<E, Alloc_type, N>::pop_global(volatile E& t) {
  Age oldAge = _age.get();
  // Architectures with weak memory model require a barrier here
  // to guarantee that bottom is not older than age,
  // which is crucial for the correctness of the algorithm.
#if !(defined SPARC || defined IA32 || defined AMD64)
  OrderAccess::fence();
#endif
  uint localBot = OrderAccess::load_acquire(&_bottom);
  uint n_elems = size(localBot, oldAge.top());
  if (n_elems == 0) {
    return false;
  }

  // g++ complains if the volatile result of the assignment is
  // unused, so we cast the volatile away.  We cannot cast directly
  // to void, because gcc treats that as not using the result of the
  // assignment.  However, casting to E& means that we trigger an
  // unused-value warning.  So, we cast the E& to void.
  (void) const_cast<E&>(t = _elems[oldAge.top()]);
  Age newAge(oldAge);
  newAge.increment();
  Age resAge = _age.cmpxchg(newAge, oldAge);

  // Note that using "_bottom" here might fail, since a pop_local might
  // have decremented it.
  assert(dirty_size(localBot, newAge.top()) != N - 1, "sanity");
  return resAge == oldAge;
}





template<class E, CHeapAllocType Alloc_type, unsigned int N> 
inline bool GenericTaskQueueRDMA<E, Alloc_type, N>::pop_local(volatile E& t, uint threshold) {
  uint localBot = _bottom;
  // This value cannot be N-1.  That can only occur as a result of
  // the assignment to bottom in this method.  If it does, this method
  // resets the size to 0 before the next call (which is sequential,
  // since this is pop_local.)
  uint dirty_n_elems = dirty_size(localBot, _age.top());
  assert(dirty_n_elems != N - 1, "Shouldn't be possible...");
  if (dirty_n_elems <= threshold) return false;
  localBot = decrement_index(localBot);
  _bottom = localBot;
  // This is necessary to prevent any read below from being reordered
  // before the store just above.
  OrderAccess::fence();
  // g++ complains if the volatile result of the assignment is
  // unused, so we cast the volatile away.  We cannot cast directly
  // to void, because gcc treats that as not using the result of the
  // assignment.  However, casting to E& means that we trigger an
  // unused-value warning.  So, we cast the E& to void.
  (void) const_cast<E&>(t = _elems[localBot]);
  // This is a second read of "age"; the "size()" above is the first.
  // If there's still at least one element in the queue, based on the
  // "_bottom" and "age" we've read, then there can be no interference with
  // a "pop_global" operation, and we're done.
  idx_t tp = _age.top();    // XXX
  if (size(localBot, tp) > 0) {
    assert(dirty_size(localBot, tp) != N - 1, "sanity");
    TASKQUEUE_STATS_ONLY(stats.record_pop());
    return true;
  } else {
    // Otherwise, the queue contained exactly one element; we take the slow
    // path.
    return pop_local_slow(localBot, _age.get());
  }
}






// pop_local_slow() is done by the owning thread and is trying to
// get the last task in the queue.  It will compete with pop_global()
// that will be used by other threads.  The tag age is incremented
// whenever the queue goes empty which it will do here if this thread
// gets the last task or in pop_global() if the queue wraps (top == 0
// and pop_global() succeeds, see pop_global()).
template<class E, CHeapAllocType Alloc_type, unsigned int N>
bool GenericTaskQueueRDMA<E, Alloc_type, N>::pop_local_slow(uint localBot, Age oldAge) {
  // This queue was observed to contain exactly one element; either this
  // thread will claim it, or a competing "pop_global".  In either case,
  // the queue will be logically empty afterwards.  Create a new Age value
  // that represents the empty queue for the given value of "_bottom".  (We
  // must also increment "tag" because of the case where "bottom == 1",
  // "top == 0".  A pop_global could read the queue element in that case,
  // then have the owner thread do a pop followed by another push.  Without
  // the incrementing of "tag", the pop_global's CAS could succeed,
  // allowing it to believe it has claimed the stale element.)
  Age newAge((idx_t)localBot, oldAge.tag() + 1);
  // Perhaps a competing pop_global has already incremented "top", in which
  // case it wins the element.
  if (localBot == oldAge.top()) {
    // No competing pop_global has yet incremented "top"; we'll try to
    // install new_age, thus claiming the element.
    Age tempAge = _age.cmpxchg(newAge, oldAge);
    if (tempAge == oldAge) {
      // We win.
      assert(dirty_size(localBot, _age.top()) != N - 1, "sanity");
      TASKQUEUE_STATS_ONLY(stats.record_pop_slow());
      return true;
    }
  }
  // We lose; a completing pop_global gets the element.  But the queue is empty
  // and top is greater than bottom.  Fix this representation of the empty queue
  // to become the canonical one.
  _age.set(newAge);
  assert(dirty_size(localBot, _age.top()) != N - 1, "sanity");
  return false;
}






//
// Structure - OverflowTargetObjQueue
// 



/**
 * Semeru - Need to inline this one ??
 *  Who makes this decision ?
 */
template<class E, CHeapAllocType Alloc_type, unsigned int N>
OverflowTargetObjQueue<E, Alloc_type, N>::OverflowTargetObjQueue(){
	//log_debug(semeru,heap)("%s Constructor.",__func__);
}


/**
 * Allocate the array at specific address
 *
 *  [x] This function is used for committing space for  OverflowTargetObjQueue->_elems only.
 *      It should be just behind the instance.
 * 	
 * 	[x] Let Operator new commit all the size for both current instance and the class E* _elems.
 * 			Only initialzie some field value here.
 * 
 *  Parameters:
 *    class E     : Cast the allocated memory to E*. This is C++ instance array. Not pointer array.
 *    MEMFLAGS F  : flag used to trace the memory usage. 
 *             N  : the element number. It's a fixed number. Use the default value, TASKQUEUE_SIZE.
 *                  [x] So, the size for an OverflowTargetObjQueue is align_up(TASKQUEUE_SIZE * sizeof(E), 4KB).
 *
 *  [x] To access template superclass's field, have to use this
 *      https://stackoverflow.com/questions/4010281/accessing-protected-members-of-superclass-in-c-with-templates 
 */ 
template<class E, CHeapAllocType Alloc_type, unsigned int N>
inline void OverflowTargetObjQueue<E, Alloc_type, N>::initialize(size_t q_index) {

	// Have to  assign the region index explicitly in allocation site.
	_region_index = q_index;

	// element length is N.
	// type is class E;
	this->_elems	=	(E*)((char*)this + align_up(sizeof(OverflowTargetObjQueue),PAGE_SIZE));  // 4KB alignment for the _elems

	#ifdef ASSERT
	log_debug(semeru, alloc)("%s, Allocate OverflowTargetObjQueue[%lu] at 0x%lx, content E* _elems 0x%lx \n",__func__,
																																	(size_t)q_index,
																																	(size_t)this,
																																	(size_t)this->_elems);
	#endif
}




template <class E, CHeapAllocType Alloc_type, unsigned int N>
inline bool OverflowTargetObjQueue<E, Alloc_type, N>::push(E t)
{
  if (!taskqueue_t::push(t)) {
    overflow_stack()->push(t);
    TASKQUEUE_STATS_ONLY(stats.record_overflow(overflow_stack()->size()));
  }
  return true;
}


template <class E, CHeapAllocType Alloc_type, unsigned int N>
inline bool OverflowTargetObjQueue<E, Alloc_type, N>::try_push_to_taskqueue(E t) {
  return taskqueue_t::push(t);
}






template <class E, CHeapAllocType Alloc_type, unsigned int N>
inline bool OverflowTargetObjQueue<E, Alloc_type, N>::pop_overflow(E& t)
{
  if (overflow_empty()) return false;
  t = overflow_stack()->pop();
  return true;
}


#endif // SHARE_GC_SHARED_RDMA_STRUCTURE_INLINE