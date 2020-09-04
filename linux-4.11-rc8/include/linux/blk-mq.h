#ifndef BLK_MQ_H
#define BLK_MQ_H

#include <linux/blkdev.h>
#include <linux/sbitmap.h>
#include <linux/srcu.h>

struct blk_mq_tags;
struct blk_flush_queue;

/**
 * Tag : annotation, debug
 * 
 * This struct is provided in /linux/include, can be used by modules development.
 * staging queue : struct blk_mq_ctx{} is defined in linux/block/blk-mq.h. We can't use it for kernel module development ?
 * 
 * record the hardware dispatch queue information,
 * one blk_mq_hw_ctx per dispatch queue.
 * 
 * [?] What's the usage for poll ?
 * 
 * 
 * [!] Both staging queue and hardware dispatch queue use the same structure, request_queue, to manage its queue list.
 * 		blk_mq_hw_ctx-request_queue
 * 		blk_mq_ctx->request_queue
 * 
 * 		gendisk->request_queue
 * 
 * 		requst->request_queue
 * 
 * 		[?] Are these request_queue the same one ???
 * 
 */
struct blk_mq_hw_ctx {
	struct {
		spinlock_t		lock;
		struct list_head	dispatch;   // The link-list for hardware dispatch queue, the requests are added to this list. 
		unsigned long		state;				/* BLK_MQ_S_* flags */   // [?] What's for ?
	} ____cacheline_aligned_in_smp;

	struct work_struct	run_work;
	cpumask_var_t		cpumask;
	int			next_cpu;
	int			next_cpu_batch;

	unsigned long		flags;		/* BLK_MQ_F_* flags */

	void			*sched_data;
	struct request_queue	*queue;		//[!] The requset_queue controller/descriptor. 
	struct blk_flush_queue	*fq;

	void							*driver_data;

	struct sbitmap		ctx_map;

	struct blk_mq_ctx	**ctxs;
	unsigned int			nr_ctx;			// [?] record the staging queue number?

	wait_queue_t			dispatch_wait;
	atomic_t					wait_index;       // request->tag runs out, some process is waiting here.

	struct blk_mq_tags	*tags;           // used for scheduling ?
	struct blk_mq_tags	*sched_tags;     // [?] What's this used for ?

	struct srcu_struct	queue_rq_srcu;

	unsigned long		queued;							// queued request conter
	unsigned long		run;
#define BLK_MQ_MAX_DISPATCH_ORDER	7
	unsigned long		dispatched[BLK_MQ_MAX_DISPATCH_ORDER];   // [?] What's this for ?

	unsigned int		numa_node;
	unsigned int		queue_num;				// dispatch queue context id, start from 0.

	atomic_t				nr_active;

	struct delayed_work	delayed_run_work;
	struct delayed_work	delay_work;

	struct hlist_node	cpuhp_dead;
	struct kobject		kobj;

	unsigned long		poll_considered;
	unsigned long		poll_invoked;
	unsigned long		poll_success;
};

/**
 * The core structure to describe the disk devices information 
 * [?] This structure is only used for hardware dispatch queue ??
 * 
 * 
 * blk_mq_ops   : driver defined operations, defined in blk_mq_ops. 
 * nr_hw_queues : The hardware dispatch queues. Assigned by device driver, usually equal to number of available cores. 
 * queue_depth	: The max number of on-the-fly i/o request. limited by the request available tag.
 * 								[?] What will happen if the i/o request queue depth > rdma requets ? 
 * 
 * numa_node		: NUMA_NO_NODE, doesn't care numa 
 * 
 * timeout 			: waiting for the i/o request responds ??
 * 
 * driver_data 	: points to the Driver controller variable, usually stores all the device inforamtion. 
 * 
 * blk_mq_tags	: [?] one blk_mq_tag is enough ??
 * 
 * 
 */
struct blk_mq_tag_set {
	unsigned int		*mq_map;				// Record the mapping of staing queue <-> dispatch queue (context), index is the cpu_id : htcx = q->queue_hw_ctx[q->mq_map[cpu]];
	const struct blk_mq_ops	*ops; 	// Let kernel module fill self desinged functions to its function pointer.
	unsigned int		nr_hw_queues;  
	unsigned int		queue_depth;	/* max hw supported */
	unsigned int		reserved_tags;
	unsigned int		cmd_size;			/* per-request extra data */  // Usually, a driver defined data will be attached to the end of each request.
	int							numa_node;
	unsigned int		timeout;
	unsigned int		flags;			/* BLK_MQ_F_* */
	void						*driver_data;	 	// disk driver controller/context, i.e. the struct nullb{} in null_block.

	struct blk_mq_tags	**tags;

	struct mutex				tag_list_lock;
	struct list_head		tag_list;
};


/**
 * Tag : annotation & debug
 * 
 * [?] The data sent from upper system, file system or usr ?
 * It contains a request queue  
 * Dirver build this  list based on hardware dispatch queue : blk_mq_hw_ctx->dispatch 
 * 
 */
struct blk_mq_queue_data {
	struct request *rq;						// some driver_cmd data should be attached at the end of this request ?
	struct list_head *list;				//[?] The list structure should be put at the head of blk_mq_queue_data ?
	bool last;
};

typedef int (queue_rq_fn)(struct blk_mq_hw_ctx *, const struct blk_mq_queue_data *);
typedef enum blk_eh_timer_return (timeout_fn)(struct request *, bool);
typedef int (init_hctx_fn)(struct blk_mq_hw_ctx *, void *, unsigned int);
typedef void (exit_hctx_fn)(struct blk_mq_hw_ctx *, unsigned int);
typedef int (init_request_fn)(void *, struct request *, unsigned int,
		unsigned int, unsigned int);
typedef void (exit_request_fn)(void *, struct request *, unsigned int,
		unsigned int);
typedef int (reinit_request_fn)(void *, struct request *);

typedef void (busy_iter_fn)(struct blk_mq_hw_ctx *, struct request *, void *,
		bool);
typedef void (busy_tag_iter_fn)(struct request *, void *, bool);
typedef int (poll_fn)(struct blk_mq_hw_ctx *, unsigned int);
typedef int (map_queues_fn)(struct blk_mq_tag_set *set);



/**
 * Tag : annotation, debug
 * 
 * Self defined I/O operation for hardware dispatch queue.
 * the fields are function pointer.
 * Kernel module can assign defined functions to these function pointer.
 * 
 * 
 */
struct blk_mq_ops {
	/*
	 * Queue request
	 */
	queue_rq_fn		*queue_rq;   // [?] The driver defined operations for this queue ??  every reqeust should have different operation ?

	/*
	 * Called on request timeout
	 */
	timeout_fn		*timeout;

	/*
	 * Called to poll for completion of a specific tag.
	 */
	poll_fn				*poll;

	softirq_done_fn		*complete;

	/*
	 * Called when the block layer side of a hardware queue has been
	 * set up, allowing the driver to allocate/init matching structures.
	 * Ditto for exit/teardown.
	 */
	init_hctx_fn		*init_hctx;
	exit_hctx_fn		*exit_hctx;

	/*
	 * Called for every command allocated by the block layer to allow
	 * the driver to set up driver specific data.
	 *
	 * Tag greater than or equal to queue_depth is for setting up
	 * flush request.
	 *
	 * Ditto for exit/teardown.
	 */
	init_request_fn		*init_request;			// Driver defined request init, driver attach a driver_cmd to the request.
	exit_request_fn		*exit_request;
	reinit_request_fn	*reinit_request;

	map_queues_fn		*map_queues;    // The mapping between cpu_id and dispatch queue. Staging queue <--> cpu_id <--> dispatch queue.
};

/**
 *  Tag : annotation, debug
 * 
 * return message after queue an i/o requets ?
 * 		BLK_MQ_RQ_QUEUE_OK
 * 		BLK_MQ_RQ_QUEUE_BUSY
 * 		BLK_MQ_RQ_QUEUE_ERROR
 * 
 * 
 * i/o requets control ?
 * 		BLK_MQ_F_SHOULD_MERGE
 * 
 */
enum {
	BLK_MQ_RQ_QUEUE_OK	= 0,	/* queued fine */
	BLK_MQ_RQ_QUEUE_BUSY	= 1,	/* requeue IO for later */
	BLK_MQ_RQ_QUEUE_ERROR	= 2,	/* end IO with error */

	BLK_MQ_F_SHOULD_MERGE	= 1 << 0,
	BLK_MQ_F_TAG_SHARED	= 1 << 1,
	BLK_MQ_F_SG_MERGE	= 1 << 2,
	BLK_MQ_F_DEFER_ISSUE	= 1 << 4,
	BLK_MQ_F_BLOCKING	= 1 << 5,
	BLK_MQ_F_NO_SCHED	= 1 << 6,
	BLK_MQ_F_ALLOC_POLICY_START_BIT = 8,
	BLK_MQ_F_ALLOC_POLICY_BITS = 1,

	BLK_MQ_S_STOPPED	= 0,
	BLK_MQ_S_TAG_ACTIVE	= 1,
	BLK_MQ_S_SCHED_RESTART	= 2,
	BLK_MQ_S_TAG_WAITING	= 3,

	BLK_MQ_MAX_DEPTH	= 10240,

	BLK_MQ_CPU_WORK_BATCH	= 8,
};
#define BLK_MQ_FLAG_TO_ALLOC_POLICY(flags) \
	((flags >> BLK_MQ_F_ALLOC_POLICY_START_BIT) & \
		((1 << BLK_MQ_F_ALLOC_POLICY_BITS) - 1))
#define BLK_ALLOC_POLICY_TO_MQ_FLAG(policy) \
	((policy & ((1 << BLK_MQ_F_ALLOC_POLICY_BITS) - 1)) \
		<< BLK_MQ_F_ALLOC_POLICY_START_BIT)

struct request_queue *blk_mq_init_queue(struct blk_mq_tag_set *);
struct request_queue *blk_mq_init_allocated_queue(struct blk_mq_tag_set *set,
						  struct request_queue *q);
int blk_mq_register_dev(struct device *, struct request_queue *);
void blk_mq_unregister_dev(struct device *, struct request_queue *);

int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set);
void blk_mq_free_tag_set(struct blk_mq_tag_set *set);

void blk_mq_flush_plug_list(struct blk_plug *plug, bool from_schedule);

void blk_mq_free_request(struct request *rq);
bool blk_mq_can_queue(struct blk_mq_hw_ctx *);

/**
 * Used for blk_mq_alloc_data->flag  
 * 
 * [?] Seems Block Layer stores lots of request and reuse them ?
 * 
 */
enum {
	BLK_MQ_REQ_NOWAIT	= (1 << 0), /* return when out of requests */
	BLK_MQ_REQ_RESERVED	= (1 << 1), /* allocate from reserved pool */
	BLK_MQ_REQ_INTERNAL	= (1 << 2), /* allocate internal/sched tag */
};

struct request *blk_mq_alloc_request(struct request_queue *q, int rw,
		unsigned int flags);
struct request *blk_mq_alloc_request_hctx(struct request_queue *q, int op,
		unsigned int flags, unsigned int hctx_idx);
struct request *blk_mq_tag_to_rq(struct blk_mq_tags *tags, unsigned int tag);

enum {
	BLK_MQ_UNIQUE_TAG_BITS = 16,
	BLK_MQ_UNIQUE_TAG_MASK = (1 << BLK_MQ_UNIQUE_TAG_BITS) - 1,
};

u32 blk_mq_unique_tag(struct request *rq);

static inline u16 blk_mq_unique_tag_to_hwq(u32 unique_tag)
{
	return unique_tag >> BLK_MQ_UNIQUE_TAG_BITS;
}

static inline u16 blk_mq_unique_tag_to_tag(u32 unique_tag)
{
	return unique_tag & BLK_MQ_UNIQUE_TAG_MASK;
}


int blk_mq_request_started(struct request *rq);
void blk_mq_start_request(struct request *rq);
void blk_mq_end_request(struct request *rq, int error);
void __blk_mq_end_request(struct request *rq, int error);

void blk_mq_requeue_request(struct request *rq, bool kick_requeue_list);
void blk_mq_add_to_requeue_list(struct request *rq, bool at_head,
				bool kick_requeue_list);
void blk_mq_kick_requeue_list(struct request_queue *q);
void blk_mq_delay_kick_requeue_list(struct request_queue *q, unsigned long msecs);
void blk_mq_abort_requeue_list(struct request_queue *q);
void blk_mq_complete_request(struct request *rq, int error);

bool blk_mq_queue_stopped(struct request_queue *q);
void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx);
void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx);
void blk_mq_stop_hw_queues(struct request_queue *q);
void blk_mq_start_hw_queues(struct request_queue *q);
void blk_mq_start_stopped_hw_queue(struct blk_mq_hw_ctx *hctx, bool async);
void blk_mq_start_stopped_hw_queues(struct request_queue *q, bool async);
void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs);
void blk_mq_run_hw_queues(struct request_queue *q, bool async);
void blk_mq_delay_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs);
void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *tagset,
		busy_tag_iter_fn *fn, void *priv);
void blk_mq_freeze_queue(struct request_queue *q);
void blk_mq_unfreeze_queue(struct request_queue *q);
void blk_mq_freeze_queue_start(struct request_queue *q);
void blk_mq_freeze_queue_wait(struct request_queue *q);
int blk_mq_freeze_queue_wait_timeout(struct request_queue *q,
				     unsigned long timeout);
int blk_mq_reinit_tagset(struct blk_mq_tag_set *set);

int blk_mq_map_queues(struct blk_mq_tag_set *set);
void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues);

/*
 * Driver command data is immediately after the request. So subtract request
 * size to get back to the original request, add request size to get the PDU.
 */
static inline struct request *blk_mq_rq_from_pdu(void *pdu)
{
	return pdu - sizeof(struct request);
}
static inline void *blk_mq_rq_to_pdu(struct request *rq)
{
	return rq + 1;
}

#define queue_for_each_hw_ctx(q, hctx, i)				\
	for ((i) = 0; (i) < (q)->nr_hw_queues &&			\
	     ({ hctx = (q)->queue_hw_ctx[i]; 1; }); (i)++)

#define hctx_for_each_ctx(hctx, ctx, i)					\
	for ((i) = 0; (i) < (hctx)->nr_ctx &&				\
	     ({ ctx = (hctx)->ctxs[(i)]; 1; }); (i)++)

#endif
