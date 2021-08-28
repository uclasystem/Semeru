/**
 * This file contains all the class or structure used by RDMA transferring.
 *  
 */


#ifndef SHARE_GC_SHARED_RDMA_STRUCTURE
#define SHARE_GC_SHARED_RDMA_STRUCTURE

#include "utilities/align.hpp"
#include "utilities/sizes.hpp"
#include "utilities/globalDefinitions.hpp"
#include "gc/shared/rdmaAllocation.hpp"
#include "gc/shared/taskqueue.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/quickSort.hpp"


#define MEM_SERVER_CSET_BUFFER_SIZE		(size_t)(512 - 8) 	




//
// Task queue related structures
//




/**
 * Semeru - Support RDMA data transfer for this structure.
 *  
 * 		1) This object should be allocated at fixed address.
 * 		2) Parts of its fields also allocated at fixe address, just behind the instance.
 * 
 */
template <class E, unsigned int N, CHeapAllocType Alloc_type>
class TaskQueueRDMASuper: public CHeapRDMAObj<E, Alloc_type> {
protected:
  // Internal type for indexing the queue; also used for the tag.
  typedef NOT_LP64(uint16_t) LP64_ONLY(uint32_t) idx_t;

  // The first free element after the last one pushed (mod N).
  volatile uint _bottom;  // [x] points to the first free/available slot.

  enum { MOD_N_MASK = N - 1 };

  // Tag : Used for work stealing. 
  // If other threads steal an elem by pop_global, _age.top++.
  //
  class Age {
  public:
    Age(size_t data = 0)         { _data = data; }
    Age(const Age& age)          { _data = age._data; }
    Age(idx_t top, idx_t tag)    { _fields._top = top; _fields._tag = tag; }

    Age   get()        const volatile { return _data; }
    void  set(Age age) volatile       { _data = age._data; }

    idx_t top()        const volatile { return _fields._top; }
    idx_t tag()        const volatile { return _fields._tag; }

    // Increment top; if it wraps, increment tag also.
    void increment() {
      _fields._top = increment_index(_fields._top);
      if (_fields._top == 0) ++_fields._tag;
    }

    Age cmpxchg(const Age new_age, const Age old_age) volatile;

    bool operator ==(const Age& other) const { return _data == other._data; }

  private:

    // [?] What's the purpose of _top and _tag ?
    //
    struct fields {
      idx_t _top;
      idx_t _tag;
    };

    union {
      size_t _data;
      fields _fields;
    };
  };

  volatile Age _age;  // Used for work streal. _age._top points to the first inserted item.




	// End of fields.


  // These both operate mod N.
  static uint increment_index(uint ind) {
    return (ind + 1) & MOD_N_MASK;
  }
  static uint decrement_index(uint ind) {
    return (ind - 1) & MOD_N_MASK;
  }

  // Returns a number in the range [0..N).  If the result is "N-1", it should be
  // interpreted as 0.
  //
  // [?] dirty_size means the pushed and not handled element size ??
  //
  uint dirty_size(uint bot, uint top) const {
    return (bot - top) & MOD_N_MASK;    // -1 & MOD_N_MASK  = MOD_N_MASK (N-1).
  }

  // Returns the size corresponding to the given "bot" and "top".
  //
  // [?] Same with dirty_size, except for the boundary case. 
  //
  uint size(uint bot, uint top) const {
    uint sz = dirty_size(bot, top);     // (_bottom - _top) & MOD_N_MASK
    // Has the queue "wrapped", so that bottom is less than top?  There's a
    // complicated special case here.  A pair of threads could perform pop_local
    // and pop_global operations concurrently, starting from a state in which
    // _bottom == _top+1.  The pop_local could succeed in decrementing _bottom,
    // and the pop_global in incrementing _top (in which case the pop_global
    // will be awarded the contested queue element.)  The resulting state must
    // be interpreted as an empty queue.  (We only need to worry about one such
    // event: only the queue owner performs pop_local's, and several concurrent
    // threads attempting to perform the pop_global will all perform the same
    // CAS, and only one can succeed.)  Any stealing thread that reads after
    // either the increment or decrement will see an empty queue, and will not
    // join the competitors.  The "sz == -1 || sz == N-1" state will not be
    // modified by concurrent queues, so the owner thread can reset the state to
    // _bottom == top so subsequent pushes will be performed normally.
    return (sz == N - 1) ? 0 : sz;
  }

public:
  TaskQueueRDMASuper() : _bottom(0), _age() {}

  // Return true if the TaskQueue contains/does not contain any tasks.
  bool peek()     const { return _bottom != _age.top(); }
  bool is_empty() const { return size() == 0; }

  // Return an estimate of the number of elements in the queue.
  // The "careful" version admits the possibility of pop_local/pop_global
  // races.
  uint size() const {
    return size(_bottom, _age.top());
  }

  uint dirty_size() const {
    return dirty_size(_bottom, _age.top());
  }

  void set_empty() {
    _bottom = 0;
    _age.set(0);
  }

  // Maximum number of elements allowed in the queue.  This is two less
  // than the actual queue size, for somewhat complicated reasons.
  uint max_elems() const { return N - 2; }

  // Total size of queue.
  static const uint total_size() { return N; }

  TASKQUEUE_STATS_ONLY(TaskQueueStats stats;)
};




// Semeru Copy the implementation of the Orginal GenericTaskQueue.
// But changed their allocation policy.
//
//
// GenericTaskQueue implements an ABP, Aurora-Blumofe-Plaxton, double-
// ended-queue (deque), intended for use in work stealing. Queue operations
// are non-blocking.
//
// A queue owner thread performs push() and pop_local() operations on one end
// of the queue, while other threads may steal work using the pop_global()
// method.
//
// The main difference to the original algorithm is that this
// implementation allows wrap-around at the end of its allocated
// storage, which is an array.
//
// The original paper is:
//
// Arora, N. S., Blumofe, R. D., and Plaxton, C. G.
// Thread scheduling for multiprogrammed multiprocessors.
// Theory of Computing Systems 34, 2 (2001), 115-144.
//
// The following paper provides an correctness proof and an
// implementation for weakly ordered memory models including (pseudo-)
// code containing memory barriers for a Chase-Lev deque. Chase-Lev is
// similar to ABP, with the main difference that it allows resizing of the
// underlying storage:
//
// Le, N. M., Pop, A., Cohen A., and Nardell, F. Z.
// Correct and efficient work-stealing for weak memory models
// Proceedings of the 18th ACM SIGPLAN symposium on Principles and
// practice of parallel programming (PPoPP 2013), 69-80
//

template <class E, CHeapAllocType Alloc_type, unsigned int N = TASKQUEUE_SIZE>
class GenericTaskQueueRDMA: public TaskQueueRDMASuper<E, N, Alloc_type> {


protected:
  typedef typename TaskQueueRDMASuper<E, N, Alloc_type>::Age Age;       // [?] What's the Age used for ? Work steal related.
  typedef typename TaskQueueRDMASuper<E, N, Alloc_type>::idx_t idx_t;   // [?] purpose ?

  using TaskQueueRDMASuper<E, N, Alloc_type>::_bottom;            // points to the first available slot.
  using TaskQueueRDMASuper<E, N, Alloc_type>::_age;               // _age._top, points to the first inserted item. Usd by working steal.
  using TaskQueueRDMASuper<E, N, Alloc_type>::increment_index;
  using TaskQueueRDMASuper<E, N, Alloc_type>::decrement_index;
  using TaskQueueRDMASuper<E, N, Alloc_type>::dirty_size;         // [?] Definition of dirty ??

public:
  using TaskQueueRDMASuper<E, N, Alloc_type>::max_elems;      // N-2, 2 slots are reserved for "complicated" reasons.
  using TaskQueueRDMASuper<E, N, Alloc_type>::size;           // _bottom - _top

#if  TASKQUEUE_STATS
  using TaskQueueRDMASuper<E, N, Alloc_type>::stats;
#endif

private:
  // Slow paths for push, pop_local.  (pop_global has no fast path.)
  bool push_slow(E t, uint dirty_n_elems);
  bool pop_local_slow(uint localBot, Age oldAge);

public:
  typedef E element_type;

  // Initializes the queue to empty.
  GenericTaskQueueRDMA();

  void initialize();

  // Push the task "t" on the queue.  Returns "false" iff the queue is full.
  inline bool push(E t);

  // Attempts to claim a task from the "local" end of the queue (the most
  // recently pushed) as long as the number of entries exceeds the threshold.
  // If successful, returns true and sets t to the task; otherwise, returns false
  // (the queue is empty or the number of elements below the threshold).
  inline bool pop_local(volatile E& t, uint threshold = 0);

  // Like pop_local(), but uses the "global" end of the queue (the least
  // recently pushed).
  bool pop_global(volatile E& t);

  // Delete any resource associated with the queue.
  ~GenericTaskQueueRDMA();

  // Apply fn to each element in the task queue.  The queue must not
  // be modified while iterating.
  template<typename Fn> void iterate(Fn fn);

  // Promote to public
  volatile E* _elems;       // [x] The real content, class pointer buffer, of the GenericTaskQueueRDMA. 
  
private:
  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, 0);
  // Element array.
  //volatile E* _elems;       // [x] The real content, buffer, of the GenericTaskQueue. 

  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, sizeof(E*));
  // Queue owner local variables. Not to be accessed by other threads.

  static const uint InvalidQueueId = uint(-1);
  uint _last_stolen_queue_id; // The id of the queue we last stole from

  int _seed; // Current random seed used for selecting a random queue during stealing.

  DEFINE_PAD_MINUS_SIZE(2, DEFAULT_CACHE_LINE_SIZE, sizeof(uint) + sizeof(int));
public:
  int next_random_queue_id();

  void set_last_stolen_queue_id(uint id)     { _last_stolen_queue_id = id; }
  uint last_stolen_queue_id() const          { return _last_stolen_queue_id; }
  bool is_last_stolen_queue_id_valid() const { return _last_stolen_queue_id != InvalidQueueId; }
  void invalidate_last_stolen_queue_id()     { _last_stolen_queue_id = InvalidQueueId; }
};

// The constructor of GenericTaskQueueRDMA
template<class E, CHeapAllocType Alloc_type, unsigned int N>
GenericTaskQueueRDMA<E, Alloc_type, N>::GenericTaskQueueRDMA() : 
_last_stolen_queue_id(InvalidQueueId), 
_seed(17 /* random number */) {
  assert(sizeof(Age) == sizeof(size_t), "Depends on this.");
}








/**
 *  Target Object Queue, same as OverflowTaskQueue. But it's allocated at specific address.
 *  This strcuture is transfered by RDMA.
 *  
 *  [x] The last field of current class is _overflow_stack, not a flexible array.
 * 
 */
template<class E, CHeapAllocType Alloc_type, unsigned int N = TASKQUEUE_SIZE>
class OverflowTargetObjQueue: public GenericTaskQueueRDMA<E, Alloc_type, N>
{
public:
  typedef Stack<E, mtGC>               overflow_t;     // Newly added overflow stack. no size limitation ??
  typedef GenericTaskQueueRDMA<E, Alloc_type, N> taskqueue_t;    // Content is E* _elem, length is N, Mem_type is F.

  // The start address for current OverflowTargetObjQueue instance, is this.
  // GenericTaskQueue->(E*)_elems is the Content for every single GenericTaskQueue<E, F, N> taskqueue_t.
  size_t  _region_index; // Corresponding the HeapRegion->index.

  TASKQUEUE_STATS_ONLY(using taskqueue_t::stats;)

  OverflowTargetObjQueue(); // Constructor.

  // newly defined initialize() function for space allocation.
  // Commit space for the GenericTaskQueueRDMA->(Class E*)_elem, just behind current class instance.
  void initialize(size_t q_index);  

  // Push task t onto the queue or onto the overflow stack.  Return true.
  inline bool push(E t);
  // Try to push task t onto the queue only. Returns true if successful, false otherwise.
  inline bool try_push_to_taskqueue(E t);

  // Attempt to pop from the overflow stack; return true if anything was popped.
  inline bool pop_overflow(E& t);

  inline overflow_t* overflow_stack() { return &_overflow_stack; }

  inline bool taskqueue_empty() const { return taskqueue_t::is_empty(); }
  inline bool overflow_empty()  const { return _overflow_stack.is_empty(); }
  inline bool is_empty()        const {
    return taskqueue_empty() && overflow_empty();
  }

  // Debug, the used slots of generic queue
  inline uint bottom() const {return taskqueue_t::_bottom;}

private:
  overflow_t _overflow_stack;     // The Stack<E,F>
};









/**
 * Limit the instance size within 4KB 
 * 
 * size_t, unsigned long, 8 bytes.
 * 
 * 1) CPU server send data to rewrite the value of _num_regions and _regions_cset[].
 * 		This instance has to be allocated in a fixed memory range, reserved by both CPU server and memory server.
 * 2) Use _num_regions as flag of if new data are written here by CPU server, producer.
 * 		Also use _num_regions to index the content of _region_cset[], decreased by Memory server, consumer.
 * 3) This function is not MT safe. We ausme that only one thread can invoke this function.
 * 4) This structure is designed to support multiple Memory Servers.
 * 
 */
class received_memory_server_cset : public CHeapRDMAObj<received_memory_server_cset>{

private :
	// First field, identify if CPU server pushed new Regions here.
	volatile size_t 	_num_regions[NUM_OF_MEMORY_SERVER];


public :
	// [x] a flexible array, points the memory just behind this instance.
	// The size of current instance should be limited within 4K, 
	// The array size should be limited by MEM_SERVER_CSET_BUFFER_SIZE.
  // 	within 4KB, support 64GB per memory server. e.g. 512M per Region
	volatile uint	_region_cset[NUM_OF_MEMORY_SERVER][128];    

  //
  // Do NOT declare any variables behind _regions_cset[][] !!
  //
	
	received_memory_server_cset();
	
	volatile size_t*	num_received_regions(size_t mem_id)	    {	return &_num_regions[mem_id];	}
  uint              num_of_enqueued_regions(size_t mem_id)  { return _num_regions[mem_id]; }

	// Allocate the _region_cset just behind this instance ?
	// void initialize(){
	// 	_num_regions = 0;
	// }



	void reset(){	
    int i;
    for(i=0; i<NUM_OF_MEMORY_SERVER; i++){
      _num_regions[i] = 0;	
    }
  }

  void reset_cset_for_target_mem(size_t mem_id){
    _num_regions[mem_id]  = 0;
  }

	//
	// This function isn't MT safe.
	// 
	int	pop(size_t mem_id)	{
		if(_num_regions[mem_id] >= 1)
			return _region_cset[mem_id][--_num_regions[mem_id]];	
		else
			return -1; 
	}

    //mhr: modify
  uint get(size_t mem_id, size_t i) {
    return _region_cset[mem_id][i];
  }

  //
  // !! Fix Here !!
  //  1) Assume 2 memory servers
  //  2) Assume each JVM region is 512 MB
  void add( uint region_id) {
    // Region#0 is meta data region. Data Region is #1 to #8. REGION_SIZE_GB * ONE_GB, e.g. 4GB, per Region.
    // For JVM,  region size is GrainBytes, e.g. 512MB. 
    uint region_boundary_id =  (MEMORY_SERVER_1_REGION_START_ID - MEMORY_SERVER_0_REGION_START_ID) * ((REGION_SIZE_GB * ONE_GB)/(512*ONE_MB));
    int mem_id;

    if( region_id < region_boundary_id){
      mem_id = 0;
    }else{
      mem_id = 1;
    }

    _region_cset[mem_id][_num_regions[mem_id]++] = region_id;
	}


};


/**
 * Memory server need to know the current state of CPU srever to make its own decesion.
 * For example, if the CPU server is in STW GC now, memory server switch to Remark and Compact cm_scanned Regions.
 * Or memory server keeps concurrently tracing the freshly evicted Regions.
 *  
 * 	These flags are only writable by CPU server.
 *  Memory server only read the value of them.
 *  These varialbes are valotile, every time Memory server needs to read the value from memory.
 * 
 * Size limitations, 1 page,4KB
 * 
 */
class flags_of_cpu_server_state : public CHeapRDMAObj<flags_of_cpu_server_state>{
	//private :
  public:

    // CPU server states
    //
    volatile bool _is_cpu_server_in_stw;
    volatile bool _cpu_server_data_sent;


	public :
		flags_of_cpu_server_state();

    //mhr: modify
    //mhr: new
    inline void	set_cpu_server_in_stw()			{	_is_cpu_server_in_stw = true;		}
		inline void set_cpu_server_in_mutator()	{	_is_cpu_server_in_stw = false;	}

    inline volatile bool is_cpu_server_in_stw()	{	return _is_cpu_server_in_stw;	}

};


/**
 *  Semeru
 *  For CPU server, this is read only class.
 *  4K Bytes.
 */
class flags_of_mem_server_state : public CHeapRDMAObj<flags_of_mem_server_state>{
	//private :
  public:

    // Memory server states
    //

    // CPU server needs to keep reading the data until this value changed to false.
    volatile bool _mem_server_wait_on_data_exchange;

    volatile bool _is_mem_server_in_compact;

    // Thread same structure
    // Add a Region into the queue ONLY when its compaction is finished.
    uint _compacted_regions[128];  // assume max regions num is 128.  512 Bytes.
    volatile size_t _compacted_region_length;

	public :
		flags_of_mem_server_state();


    inline volatile bool is_mem_server_in_compact()  { return _is_mem_server_in_compact; }
    inline volatile size_t mem_server_compcated_region_length() { return _compacted_region_length;  }

    // Add a claimed Region index.
    // MT safe.
    inline void add_claimed_region(uint region_index){
      size_t available_slot;

      do{
        available_slot = _compacted_region_length;
      }while( Atomic::cmpxchg(available_slot+1, &_compacted_region_length, available_slot ) != available_slot );

      _compacted_regions[available_slot] = region_index;
    }



    /**
     * It's ok to set these flags multiple times.
     *  
     */
    void set_all_flags_to_start_mode(){
      // sync#1

      // sync #2 finished
      _mem_server_wait_on_data_exchange = false;

      // Sync #3, all done
      _is_mem_server_in_compact = true;

    }


    /**
     * It's ok to set these flags multiple times.
     *  
     */
    void set_all_flags_to_end_mode(){
      // Sync #1 read the STW window ended.
      // if possible stop claimed Region.
      // OR
      // Finish current compaction.

      // sync #2 finished
      _mem_server_wait_on_data_exchange = true;

      // Sync #3, all done
      _is_mem_server_in_compact = false;

    }

};





/**
 * 1-Sided RDMA write flag,
 *  with flexbile array.
 * 
 * Reverse 4 bytes for each Region,
 * Assume there are at most 1024 Regions. (The instance can NOT cost space)
 * Reserve 4KB.  Region[index]->write_check_flag = Region_index x 4 bytes.
 *  
 */
class flags_of_rdma_write_check : public CHeapRDMAObj<flags_of_rdma_write_check>{
private :

public :

  // Real content, the flexible array.
  // [x] Do we need to declare the base as volatile also
  //  => Any variable pointed by volatile pointers are treated as volatile variables. 
  volatile uint32_t one_sided_rdma_write_check_flags_base[]; 

  // functions
  
  // Constructor
  flags_of_rdma_write_check( char* start, size_t byte_size , size_t granularity){
    // reset memory value to 0.
    uint32_t *ptr = (uint32_t*)start;

    assert(sizeof(uint32_t) == granularity, "wrong granularity. ");

    memset(ptr, 0, byte_size/granularity);
    
  }


  // return a uint32_t for a Region.
	inline volatile uint32_t* region_write_check_flag(size_t index)	{	return ( volatile uint32_t*)(one_sided_rdma_write_check_flags_base + index) ;	}

};








/**
 * Record the new address for the objects in Target Object Queue.
 *  <key : old addr, value : new addr >
 * 
 *  a. Can't distinguish where is the source for the Target Object Queue
 *     So, have to broadcast the HashQueue to all the other servers. 
 * 
 * b. The object array type, is struct ElemPair.
 * 
 * [?] Can we merge this queue with Target Object Queue to save some space ??
 * 
 */
// struct ElemPair{
//     uint from;     // 8 bytes
//     uint nex;
//   };


// class HashQueue : public CHeapRDMAObj<struct ElemPair, CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE> {

// public:

//   static int compare_elempair(const ElemPair a, const ElemPair b) {
//     if ((unsigned long long)a.from > (unsigned long long)b.from) {
//       return 1;
//     } else if ((unsigned long long)a.from == (unsigned long long)b.from) {
//       return 0;
//     } else {
//       return -1;
//     }
//   }

// //private:

//   size_t _key_tot;
//   size_t _tot;
//   size_t _num;
//   volatile size_t _length;
//   size_t _region_index;
//   HeapWord* _base;
//   bool _marked_from_root;
//   int _age;

//   Mutex _m;

//   ElemPair* _queue; // Flexible array, must be the last field. length limitation : CROSS_REGION_REF_UPDATE_Q_LEN.

// public:
//   HashQueue():_m(Mutex::leaf, FormatBuffer<128>("HashQueue"), true, Monitor::_safepoint_check_never){

//   }

//   ~HashQueue(){clear();}

//   void reset() {
//     //print_info();

//     //memset(_queue, 0xff, (CROSS_REGION_REF_UPDATE_Q_LEN_SQRT+1) * sizeof(ElemPair));
    
//     memset(_queue, 0xff, CROSS_REGION_REF_UPDATE_Q_LEN * sizeof(ElemPair));
//     // for(int i = 0; i < CROSS_REGION_REF_UPDATE_Q_LEN_SQRT+2; i ++) {
//     //   _queue[i].from = 0xffffffff;
//     //   _queue[i].nex = 0;
//     // }
//     _length = _key_tot + 1;
//     _num = 0;
//     _marked_from_root=false;
//     _age = -1;
//   }

//   void print_info() {
//       tty->print("Region 0x%lx's queue: 0x%lx, len: 0x%lx, ", _region_index, (size_t)_queue, ((CROSS_REGION_REF_UPDATE_Q_LEN_SQRT+1) * sizeof(ElemPair)));

//     tty->print("keytot: 0x%lx, _tot: 0x%lx, num: 0x%lx \n", _key_tot, _tot, _num);
//   }

//   // invoke the initialization function explicitly 
//   void initialize(size_t region_index, HeapWord* bottom) {
//     _tot = CROSS_REGION_REF_UPDATE_Q_LEN;
//     _key_tot = CROSS_REGION_REF_UPDATE_Q_LEN_SQRT;
//     _length = _key_tot + 1; // bump pointer
//     _num = 0;
//     _region_index = region_index;
//     _base = bottom;
//     _queue  = (ElemPair*)((char*)this + align_up(sizeof(HashQueue),PAGE_SIZE)); // page alignment, can we save this space ?
//     _marked_from_root=false;
//     _age = -1;


//     memset(_queue, 0xff, CROSS_REGION_REF_UPDATE_Q_LEN * sizeof(ElemPair));

//     tty->print("Initialize region 0x%lx's queue: 0x%lx, len: 0x%lx, ", _region_index, (size_t)_queue, ((CROSS_REGION_REF_UPDATE_Q_LEN_SQRT+1) * sizeof(ElemPair)));

//     tty->print("keytot: 0x%lx, _tot: 0x%lx, num: 0x%lx \n", _key_tot, _tot, _num);

//     log_debug(semeru,alloc)("%s, Cross region refernce update queue, 0x%lx,  _queue 0x%lx , length 0x%lx", __func__, (size_t)this, (size_t)_queue, (size_t)_tot);
//   }

//   void clear() {
//     if(_queue != NULL){
//       ArrayAllocator<ElemPair>::free(_queue, _tot);
//       _queue = NULL;
//       _tot = _length = _key_tot = 0;
//       _num = 0;
//       _marked_from_root=false;
//       _age = -1;
//     }
//   }

//   void insert(uint index, uint x) {
//     //MutexLockerEx z(&_m, Mutex::_no_safepoint_check_flag);
//     if(_length >= _tot) {
//       return;
//     }
//     if(_queue[index].from == 0xffffffff) {

//       if(Atomic::cmpxchg(x, &_queue[index].from, 0xffffffff ) == 0xffffffff ){
//         //_queue[index].nex = 0;
//         _num ++; //not important
//         return;
//       }
//     }
//     //while(_queue[index].nex != 0 && _queue[index].from != x) {
//     while(_queue[index].nex != 0xffffffff && _queue[index].from != x) {
//       //tty->print("1!: %u, %u\n",index,  _queue[index].nex);
//       index = _queue[index].nex;
//     }
//     if(_queue[index].from != x) {
//       size_t new_index;
//       do{
//         new_index = _length;
//         //tty->print("2!: %lu, %lu\n",new_index,  _length);
//       }while( Atomic::cmpxchg((new_index+1), &_length, new_index ) != new_index );

//       //new_index = new_index + 1;
//       if(new_index >= _tot) {
//         return;
//       }
//       do {
        
//         while(_queue[index].nex != 0xffffffff) {
//           //tty->print("3!: %u, %u\n",index,  _queue[index].nex);
//           index = _queue[index].nex;
//         }
//         //tty->print("4!: %lu, %u\n",new_index,  _queue[index].nex);
//       } while(Atomic::cmpxchg((uint)new_index, &_queue[index].nex, 0xffffffff ) != 0xffffffff);
//       _queue[new_index].from = x;
//       //_length++;
//       _num++; //not important
//     }
//   }

//   void push(oop x, oop y) {
//     if(_marked_from_root) {
//       return;
//     }

//     if(_length == _tot) {
//       return;
//     }

//     size_t k = (size_t)((HeapWord*)x - _base);
//     uint hash_k = ((k>>1) % _key_tot * (HASH_MUL))  % _key_tot + 1;
//     insert(hash_k, k);
    

//     //mhr: debug
//     if(_length%0x40000==0) {
//       printf("move_to_current_len: 0x%lx ", _length);
//     }

//     if(_length > _tot) {
//       _length = _tot;
//       //assert(false, "Not OK!");
//     }

//   }

//   /**
//    * Get the item by key.
//    *  
//    */
//   oop get(oop x){
//     size_t k = (size_t)(HeapWord*)x;
//     uint hash_k = ((k>>1) % _key_tot * (HASH_MUL))  % _key_tot + 1;
//     while(_queue[hash_k].nex != 0 && k != (size_t)_queue[hash_k].from) {
//       hash_k = _queue[hash_k].nex;
//     }
//     if(k == (size_t)_queue[hash_k].from) {
//       return (oop)(_base+_queue[hash_k].from);
//     }
//     else{
//       //log_trace(semeru,mem_compact)("Waring : can't find item for key 0x%lx", (size_t)x );
//       return (oop)(HeapWord*)MAX_SIZE_T;  // No this key, return unsigned long max.
//     }
//   }

//   bool is_empty() {
//     return (_num == 0);
//   }

//   inline size_t    length(){  return _length;  }
//   inline ElemPair* retrieve_item(size_t index) { 
//     assert(index < _length, "Exceed the stored length.");
//     return &_queue[index]; 
//   }
  
// };


struct ElemPair{
    uint from;     // 8 bytes
};


class HashQueue : public CHeapRDMAObj<struct ElemPair, CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE> {

public:
//private:

  size_t _key_tot;
  size_t _tot;
  volatile size_t _length;
  size_t _region_index;
  HeapWord* _base;
  bool _marked_from_root;
  int _age;
  //G1CollectedHeap* g1h;
  size_t bitmap_st;
  size_t* g1hbitmap;

  ElemPair* _queue; // Flexible array, must be the last field. length limitation : CROSS_REGION_REF_UPDATE_Q_LEN.

public:
  HashQueue(size_t* bmap):g1hbitmap(bmap){

  }

  ~HashQueue(){clear();}

  void reset() {
    memset(g1hbitmap + bitmap_st, 0, 536870912ULL/8/64 * sizeof(size_t));
    _length = 0;
    _marked_from_root=false;
    _age = -1;
  }

  // invoke the initialization function explicitly 
  void initialize(size_t region_index, HeapWord* bottom) {
    /*
    _tot = CROSS_REGION_REF_UPDATE_Q_LEN;
    _length = 0; // bump pointer
    _region_index = region_index;
    _base = bottom;
    _queue  = (ElemPair*)((char*)this + align_up(sizeof(HashQueue),PAGE_SIZE)); // page alignment, can we save this space ?
    _marked_from_root=false;
    _age = -1;
    
    bitmap_st = (_region_index * 536870912ULL)/8/64;
    memset(g1hbitmap + bitmap_st, 0, 536870912ULL/8/64 * sizeof(size_t));
    log_debug(semeru,alloc)("%s, Cross region refernce update queue, 0x%lx,  _queue 0x%lx , length 0x%lx", __func__, (size_t)this, (size_t)_queue, (size_t)_tot);
  */
  }

  void clear() {
    if(_queue != NULL){
      ArrayAllocator<ElemPair>::free(_queue, _tot);
      _queue = NULL;
      _tot = _length = 0;
      _marked_from_root=false;
      _age = -1;
    }
  }

  size_t* getbyte(size_t x) {
    return g1hbitmap + bitmap_st + (x/64);
  }

  

  void push(oop x, oop y) {
    return;
    if(_marked_from_root) {
      return;
    }
    if(_length >= _tot) {

      size_t k = (size_t)((HeapWord*)x - _base);
      size_t* bytee = getbyte(k);
      size_t mask = 1ULL << (k%64);
      size_t old_val = *bytee;
      *bytee = (old_val|mask);

      return;
    }
    size_t k = (size_t)((HeapWord*)x - _base);
    
    size_t* bytee = getbyte(k);
    size_t mask = 1ULL << (k%64);
    size_t old_val = *bytee;
    // while((old_val&mask)==0){
    //   if(Atomic::cmpxchg((old_val|mask), bytee, old_val ) != old_val){
    //     old_val = *bytee;
    //     continue;
    //   }
    //   else {
    //     size_t new_index;
    //     do{
    //       new_index = _length;
    //     }while( Atomic::cmpxchg((new_index+1), &_length, new_index ) != new_index );
    //     if(new_index < _tot){
    //       _queue[new_index].from = (uint)k;
    //     }
    //     break;
    //   }
    // }
    if((old_val&mask)==0){
      *bytee = (old_val|mask);
      size_t new_index;
      do{
        new_index = _length;
      }while( Atomic::cmpxchg((new_index+1), &_length, new_index ) != new_index );
      if(new_index < _tot){
        _queue[new_index].from = (uint)k;
      }
    }
    


    //mhr: debug
    if(_length%0x40000==0) {
      tty->print("move_to_current_len: 0x%lx ", _length);
    }

    if(_length > _tot) {
      _length = _tot;
      //assert(false, "Not OK!");
    }
  }

  /**
   * Get the item by key.
   *  
   */
  oop get(oop x){
    return x;
  }

  bool is_empty() {
    return (_length == 0);
  }

  inline size_t    length(){  return _length;  }
  inline ElemPair* retrieve_item(size_t index) { 
    assert(index < _length, "Exceed the stored length.");
    return &_queue[index]; 
  }
};






class BitQueue : public CHeapRDMAObj<size_t, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE> {

public:
// 1) meta fields. Must within 4K
  size_t _region_index;
  HeapWord* _base;
  bool _marked_from_root;
  int _age;
  size_t _heap_words; //words


  // 2) the real content
  size_t* _target_bitmap;

public:
  BitQueue(size_t heap_words):_heap_words(heap_words){

  }

  ~BitQueue(){clear();}

  void reset() {
    memset(_target_bitmap, 0, _heap_words/64 * sizeof(size_t));
    _marked_from_root=false;
    _age = -1;
  }

  // invoke the initialization function explicitly 
  void initialize(size_t region_index, HeapWord* bottom) {
    _region_index = region_index;
    _base = bottom;
    _marked_from_root=false;
    _age = -1;
    _target_bitmap  = (size_t*)((char*)this + align_up(sizeof(BitQueue),PAGE_SIZE));
    tty->print("target_bitmap: 0x%lx\n", (size_t)_target_bitmap);
    memset(_target_bitmap, 0, _heap_words/64*sizeof(size_t));
    log_debug(semeru,alloc)("%s, Cross region refernce target queue, 0x%lx,  _target_bitmap 0x%lx , length 0x%lx", __func__, (size_t)this, (size_t)_target_bitmap, (size_t)_heap_words/64);
  }

  void clear() {
    _marked_from_root=false;
    _age = -1;
  }

  size_t* getbyte(size_t x) {
    return _target_bitmap + (x/64);
  }

  void push(oop x) {
    if(_marked_from_root) {
      return;
    }
    size_t k = (size_t)((HeapWord*)x - _base);
    
    size_t* bytee = getbyte(k);

    k %= 64;
    // if((k&1) != 0) {
    //   tty->print("Error here! oop is not even! %lu\n", (size_t)(HeapWord*)x);
    //   ShouldNotReachHere();
    // }

    // k >>= 1;
    size_t old_val, new_val;
    do{
      old_val = *bytee;
      new_val = old_val|(1ULL << k);
    }while( Atomic::cmpxchg(new_val, bytee, old_val) != old_val );
    
  }
};












/**
 * Semeru Memory Server
 *  
 *  This class is used to pad gap between two RDMA structure.
 * 
 */
class rdma_padding : public CHeapRDMAObj<rdma_padding, NON_ALLOC_TYPE>{
public:
  size_t num_of_item;
  char* content[];
};




/**
 * Semeru 
 *  
 *  CPU Server  - Producer 
 *     CPU server builds the TargetObjQueue from 3 roots. And send the TargetQueue to Memory sever at the end of each CPU server GC.
 *     First, from thread stack variables. This is done during CPU server GC.
 *     Second, Cross-Region references recoreded by the Post Write Barrier ?
 *     Third, the SATB buffer queue, recoreded by the Pre Write Barrier.
 *  
 *  Memory Server - Consumer 
 *     Receive the TargetObjQueue and use them as the scavenge roots.
 * 
 */
 typedef OverflowTargetObjQueue<StarTask, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>        TargetObjQueue;     // Override the typedef of OopTaskQueue
 //typedef GenericTaskQueueSet<TargetObjQueue, mtGC>     TargetObjQueueSet;  // Assign to a global ?





#endif // SHARE_GC_SHARED_RDMA_STRUCTURE