/******************************************************************************/
/*		Open-Extension Container Scheduler 			      */
/******************************************************************************/

#include "sched.h"
#include <linux/ktime.h>
#include <linux/slab.h>

/* 
 * A task can run above a per cpu or per container runqueue. 
 * This method returns the runqueue a task runs above.
 */
struct rq* rq_of_task(struct task_struct *p)
{
	struct rq *rq;
	
#ifdef CONFIG_FAIR_GROUP_SCHED
		rq = p->se.cfs_rq->rq;
#else
		rq = task_rq_fair_oxc(p);
#endif
	return rq;
}

/* 
 * If a task is scheduled in a per-container scheduling system,
 * it is called an oxc task.
 */
int is_oxc_task(struct task_struct *p)
{
	struct rq *rq = rq_of_task(p);
	/*
	 * For a per container runqueue,
	 * the 'in_oxc' field is set.
	 */
	return rq->in_oxc == 1;
}

/* 
 * Return the ox container a task runs in.
 * Return NULL if p is not an oxc task.
 */
struct oxc_rq* oxc_rq_of_task(struct task_struct *p)
{
	struct rq *rq = rq_of_task(p);
	
	if( rq->in_oxc != 1)
		return NULL;
	
	return container_of(rq, struct oxc_rq, rq_);
}	

/* 
 * Is the oxc_rq throttled? 
 * When an oxc_rq consumes its budget in a period,
 * it will be removed from the edf tree and the
 * 'oxc_throttled' field will be set.
 */
static inline int oxc_rq_throttled(struct oxc_rq *oxc_rq)
{
	return oxc_rq->oxc_throttled;
}

/* Return the local runqueue in an ox-container. */
static inline struct rq* rq_of_oxc_rq(struct oxc_rq *oxc_rq)
{
	return &oxc_rq->rq_;
}

/* 
 * Is an oxc_rq is currently in the edf tree? 
 * If there are tasks inside the container and 
 * there are budget left, the oxc_rq should be in the
 * edf tree.
 */
static inline int oxc_rq_on_rq(struct oxc_rq *oxc_rq)
{
	return !RB_EMPTY_NODE(&oxc_rq->rb_node);
}

/* Simply compare the number a and b. */
static inline int oxc_time_before(u64 a, u64 b)
{
	return (s64)(a-b) < 0;
}

/* To get the period parameter of an oxc_rq. */
static inline u64 sched_oxc_rq_period(struct oxc_rq *oxc_rq)
{
	return ktime_to_ns(oxc_rq->oxc_period);
}

/* To get the runtime parameter of an oxc_rq. */
static inline u64 sched_oxc_rq_runtime(struct oxc_rq *oxc_rq)
{
	return oxc_rq->oxc_runtime;
}

/* 
 * To compare two oxc_rqs' priority by their deadlines.
 */
static inline int oxc_rq_before(struct oxc_rq *a, struct oxc_rq *b)
{
	return oxc_time_before(a->oxc_deadline, b->oxc_deadline);
}

/* 
 * All oxc_rq in a cpu is ordered by their deadline in an rb-tree.
 * The one with earliest deadline is put the the leftmost node in 
 * the rb-tree.
 */
static inline int oxc_rq_is_leftmost(struct oxc_rq *oxc_rq)
{
	struct rq *rq = oxc_rq->rq;
	
	return rq->oxc_edf_tree.rb_leftmost == &oxc_rq->rb_node;
}

/* 
 * When an oxc task atrrives and the oxc_rq is idle, if the oxc_rq's
 * left budget is larger than currently required service time,
 * then the deadline is updated and budget is recharged.
 * By saying an oxc_rq is idle, we mean there is no task in the
 * oxc_rq and the container is not throttled; of course, at this
 * case, the oxc_rq should be not on the edf tree.
 */
static void oxc_rq_update_deadline(struct oxc_rq *oxc_rq)
{
	struct rq *rq = oxc_rq->rq;
	u64 runtime, period, left, right;
	
	raw_spin_lock(&oxc_rq->oxc_runtime_lock);

	runtime = sched_oxc_rq_runtime(oxc_rq);
	period = sched_oxc_rq_period(oxc_rq);

	/*
	 * To update the deadline if
	 *  (oxc_deadline - now) * (runtime/period) < oxc_runtime - oxc_time
	 */
	if( oxc_time_before(oxc_rq->oxc_deadline, rq->clock))
		goto update;

	WARN_ON_ONCE(oxc_rq->oxc_time > runtime);

	left = period * (runtime - oxc_rq->oxc_time);
	right = (oxc_rq->oxc_deadline - rq->clock) * oxc_rq->oxc_runtime;

	if( left > right) {
update:
		oxc_rq->oxc_deadline = rq->clock + period;
		oxc_rq->oxc_time = 0; 

	}	

	raw_spin_unlock(&oxc_rq->oxc_runtime_lock);
}

/* Let's put an oxc_rq in an edf tree. */
static void __enqueue_oxc_rq(struct oxc_rq *oxc_rq)
{
	/* The per cpu runqueue this oxc_rq points to. */
	struct rq *rq = oxc_rq->rq;
	/* The oxc_edf_tree attached to above per cpu runqueue. */		
	struct oxc_edf_tree *oxc_edf_tree = &rq->oxc_edf_tree;
	
	/* The following are quite mechanical operations. */
	struct rb_node **link = &(oxc_edf_tree->rb_root.rb_node);
	struct rb_node *parent = NULL;
	struct oxc_rq *entry;
	int leftmost = 1;

	BUG_ON(oxc_rq_on_rq(oxc_rq));

	while(*link) {
		parent = *link;
		entry = rb_entry(parent, struct oxc_rq, rb_node);

		if( oxc_rq_before(oxc_rq, entry))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if(leftmost) {
		oxc_edf_tree->rb_leftmost = &oxc_rq->rb_node;
	}
	
	rb_link_node(&oxc_rq->rb_node, parent, link);
        rb_insert_color(&oxc_rq->rb_node, &oxc_edf_tree->rb_root);
}

static void __dequeue_oxc_rq(struct oxc_rq *oxc_rq);

/* 
 * When an oxc task arrives, to update the ox container 
 * information if necessary. 
 */
static void enqueue_oxc_rq(struct oxc_rq *oxc_rq)
{
	int on_rq;

	on_rq = oxc_rq_on_rq(oxc_rq);

	BUG_ON(!oxc_rq->oxc_nr_running);
	BUG_ON(on_rq && oxc_rq_throttled(oxc_rq));

	if( on_rq) {
		/* Already queued properly. */
		return;
	}
	/* We do not put a throttled oxc_rq in the edf tree. */
	if( oxc_rq_throttled(oxc_rq))
		return;
	
	oxc_rq_update_deadline(oxc_rq);
	__enqueue_oxc_rq(oxc_rq);
}
	
/* Move an oxc_rq from an oxc_edf_tree. */
static void __dequeue_oxc_rq(struct oxc_rq *oxc_rq)
{
	struct rq *rq = oxc_rq->rq;
	struct oxc_edf_tree *oxc_edf_tree = &rq->oxc_edf_tree;
	
	BUG_ON(!oxc_rq_on_rq(oxc_rq));

	if( oxc_edf_tree->rb_leftmost == &oxc_rq->rb_node) {
                oxc_edf_tree->rb_leftmost = rb_next(&oxc_rq->rb_node);
        }

        rb_erase(&oxc_rq->rb_node, &oxc_edf_tree->rb_root);
        RB_CLEAR_NODE(&oxc_rq->rb_node);
}

/*
 * When an oxc task leaves an oxc_rq, to check if the container 
 * is empty now.
 */
static void dequeue_oxc_rq(struct oxc_rq *oxc_rq)
{
	int on_rq;

	on_rq = oxc_rq_on_rq(oxc_rq);
	/*
	 * Here we do not expect throttled oxc_rq to be in the edf tree.
	 * Note that when an oxc_rq exceeds its maximum budget,
	 * it is dequeued via sched_oxc_rq_dequeue().
	 */
	BUG_ON(on_rq && oxc_rq_throttled(oxc_rq));
	/* 
	 * If an oxc_rq is not in the edf tree, it should be throttled or 
	 * have no tasks enqueued.
	 */
	BUG_ON(!on_rq && !oxc_rq_throttled(oxc_rq) && !oxc_rq->oxc_nr_running);

	if( on_rq && !oxc_rq->oxc_nr_running) {
		/* Dequeue the oxc_rq if it has no more tasks. */
		__dequeue_oxc_rq(oxc_rq);
		return;
	}
}

/*
 * To explicitly remove an ox container from the edf tree
 * if it is currently in the tree.
 */
static void sched_oxc_rq_dequeue(struct oxc_rq *oxc_rq)
{
	if( oxc_rq_on_rq(oxc_rq))
		__dequeue_oxc_rq(oxc_rq);
}

/*
 * To explicitly add an ox container in the tree.
 */
static void sched_oxc_rq_enqueue(struct oxc_rq *oxc_rq)
{
	if( oxc_rq->oxc_nr_running && !oxc_rq_on_rq(oxc_rq)) {
		__enqueue_oxc_rq(oxc_rq);
		if( oxc_rq_is_leftmost(oxc_rq))
			resched_task(oxc_rq->rq->curr);
	}
}

static void start_oxc_period_timer(struct oxc_rq *oxc_rq);

/* 
 * Is the oxc_rq exceeding its maximum budget? 
 * If it is, move the ox container from its edf tree
 * and start its period timer .
 */
static int sched_oxc_rq_runtime_exceeded(struct oxc_rq *oxc_rq)
{
	u64 runtime = sched_oxc_rq_runtime(oxc_rq);
	u64 period = sched_oxc_rq_period(oxc_rq);
	
	/* 
	 * If the runtime is set as 'RUNTIME_INF',
	 * the ox container can run without throttling.
	 */
	if( runtime == RUNTIME_INF)
		return 0;

	/* 
	 * If the runtime to be larger the the period,
	 * the ox container can run without throttling.
	 */
	if( runtime >=period) 
		return 0;
	
	/* There is still budget left. */
	if( oxc_rq->oxc_time < runtime)
		return 0;
	/* 
	 * The reservation in a period has been exhausted,
	 * to set the throttling label, remove the oxc_rq
	 * from the edf tree and start the recharging timer.
	 */
	else {
		oxc_rq->oxc_throttled = 1;
		oxc_rq->oxc_time = 0;
		sched_oxc_rq_dequeue(oxc_rq);
		start_oxc_period_timer(oxc_rq);

		return 1;
        }

}

/* 
 * If the current task in a cpu is an oxc task, we call its ox container
 * 'current oxc_rq'. This method updates the accumulating running time, i.e. 
 * oxc_time field, of current oxc_rq and call shced_oxc_rq_runtime_exceeded
 * to check if the maximum budget is exceeded.
 */
static void update_curr_oxc_rq(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct oxc_rq *oxc_rq = oxc_rq_of_task(curr);
	u64 delta_exec;
	/*
	 * If current task is not oxc task, simply return.
	 */
	if( !oxc_rq)
		return;

	delta_exec = rq->clock - oxc_rq->oxc_start_time;
	oxc_rq->oxc_start_time = rq->clock;
	if( unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	raw_spin_lock(&oxc_rq->oxc_runtime_lock);

	oxc_rq->oxc_time += delta_exec;
	if( sched_oxc_rq_runtime_exceeded(oxc_rq)) {
		resched_task(curr);
	}

	raw_spin_unlock(&oxc_rq->oxc_runtime_lock);
}

/*
 * To check if a task p can preempt currently executing task;
 * p should be an oxc task.
 */
static inline int
check_preempt_oxc_rq(struct task_struct *curr, struct task_struct *p, int flags)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	struct oxc_rq *oxc_rq_curr = oxc_rq_of_task(curr);
	const struct sched_class *class;

	/* Tasks from a throttled oxc_rq cannot run. */
	if( oxc_rq_throttled(oxc_rq)) {
		if( oxc_rq->oxc_deadline < oxc_rq->rq->clock) {
			WARN_ON(1);
			oxc_rq->oxc_throttled = 0;
			oxc_rq_update_deadline(oxc_rq);
		}
		else return 0;
	}
	/* 
	 * Tasks from a unthrottled oxc_rq always has a higher priority 
	 * than non oxc tasks.
	 */
	if( !oxc_rq_curr)
		return 1;

	/* Both p and current task are in the same oxc_rq. */
	if( oxc_rq_curr == oxc_rq) {
		if( p->sched_class == curr->sched_class) {

		//	raw_spin_lock(&oxc_rq->rq_.lock);

			if( !oxc_rq->rq_.curr) {
			//	raw_spin_unlock(&oxc_rq->rq_.lock);
				return 0; //1;
			}
			else
				curr->sched_class->check_preempt_curr(
							&oxc_rq->rq_, p, flags);

			//raw_spin_unlock(&oxc_rq->rq_.lock);
		}
		else {
			for_each_class(class) {
				if( class == curr->sched_class)
					break;
				if( class == p->sched_class) {
					resched_task(curr);
					break;
				}
			}
		}
	
		return 0;
	}

	/* p and current tasks are oxc tasks from different ox containers. */
	return oxc_rq_before(oxc_rq, oxc_rq_curr);
}
			
/* 
 * Pick the oxc_rq with latest deadline, if there exists,
 * from an rq's oxc_edf_tree.
 */
static struct oxc_rq*
pick_next_oxc_rq(struct rq *rq)
{
	struct rb_node *left = rq->oxc_edf_tree.rb_leftmost;
	struct oxc_rq *oxc_rq;
	/* This is an empty tree, return NULL. */
	if( !left)
		return NULL;
	oxc_rq = rb_entry(left, struct oxc_rq, rb_node);
        oxc_rq->oxc_start_time = rq->clock;

        return oxc_rq;
}

/* 
 * The recharging operation of an oxc_rq:
 *	- reset the oxc_time to 0
 * 	- postpone oxc_period by one period
 * It is called when the ox container's timer is activated.
 */
static bool oxc_rq_recharge(struct oxc_rq *oxc_rq, int overrun)
{
	u64 period = sched_oxc_rq_period(oxc_rq);
	/* If idle is true, there is no need to restart the timer. */
	bool idle = true;
	
	oxc_rq->oxc_time = 0;
	oxc_rq->oxc_deadline += period*overrun; 

	oxc_rq->oxc_throttled = 0;
	sched_oxc_rq_enqueue(oxc_rq);
		
	if( oxc_rq->oxc_nr_running)
		idle = false;
	
	return idle;
}
	
/* To set the expiration time of the ox container's timer. */
static inline void oxc_period_set_expires(struct oxc_rq *oxc_rq)
{
	struct rq *rq = oxc_rq->rq;
	ktime_t now, dline, delta;
	
	/*
	 * Compensate for discrepancies between rq->clock and hrtimer-measured
	 * time, to obtain a better absolute time instant for timer itself.
	 */
	now = hrtimer_cb_get_time(&oxc_rq->oxc_period_timer);
	delta = ktime_sub_ns(now, rq->clock);
	dline = ktime_add_ns(delta, oxc_rq->oxc_deadline);
	hrtimer_set_expires(&oxc_rq->oxc_period_timer, dline);
}

/* 
 * The hook function of the timer in an ox container.
 * It is responsible for recharging the oxc_rq, set next expiration time and
 * restart the timer if necessary.
 */
static enum hrtimer_restart sched_oxc_period_timer(struct hrtimer *hrtimer)
{
	struct oxc_rq *oxc_rq = container_of(hrtimer, 
					struct oxc_rq, oxc_period_timer);
	bool idle = false;
	int overrun;
	for(;;) {
		overrun = hrtimer_forward_now(hrtimer, oxc_rq->oxc_period);
		if( !overrun)
			break;
		raw_spin_lock(&oxc_rq->rq->lock);
		raw_spin_lock(&oxc_rq->oxc_runtime_lock);

		idle = oxc_rq_recharge(oxc_rq, overrun);
	
		raw_spin_unlock(&oxc_rq->oxc_runtime_lock);
		raw_spin_unlock(&oxc_rq->rq->lock);
	}
	
	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

/* To start the timer in an ox container for recharging. */
static void start_oxc_period_timer(struct oxc_rq *oxc_rq)
{
	ktime_t soft, hard;
	unsigned long range;

	/* To set the expiration time first. */
	oxc_period_set_expires(oxc_rq);

	for(;;) {
		if( hrtimer_active(&oxc_rq->oxc_period_timer))
			break;

		soft = hrtimer_get_softexpires(&oxc_rq->oxc_period_timer);
		hard = hrtimer_get_expires(&oxc_rq->oxc_period_timer);
		range = ktime_to_ns(ktime_sub(hard, soft));
		__hrtimer_start_range_ns(&oxc_rq->oxc_period_timer, soft, range,
						HRTIMER_MODE_ABS, 0);
	}
	
}

static inline void inc_oxc_tasks(struct task_struct *p, struct oxc_rq *oxc_rq)
{
	oxc_rq->oxc_nr_running ++;
}

static inline void dec_oxc_tasks(struct task_struct *p, struct oxc_rq *oxc_rq)
{
	oxc_rq->oxc_nr_running --;
}

/* To enqueue an oxc task to its container's local runqueue. */
static void enqueue_task_oxc(struct rq *rq, struct task_struct *p, int flags)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	struct rq *rq_ = rq_of_oxc_rq(oxc_rq);
	update_rq_clock(rq);
	/* Update the local runqueue' clock. */
	update_rq_clock(rq_);
	/*
	 * Enqueue the task into the local runqueue 
	 * by its scheduling class.
	 */
	p->sched_class->enqueue_task(rq_, p, flags);

	//raw_spin_lock(&oxc_rq->oxc_runtime_lock);
	inc_oxc_tasks(p, oxc_rq);
	enqueue_oxc_rq(oxc_rq);
	inc_nr_running(rq);
//	raw_spin_unlock(&oxc_rq->oxc_runtime_lock);
		
}

/* To dequeue an oxc task from its container's local runqueue. */
static void dequeue_task_oxc(struct rq *rq, struct task_struct *p, int flags)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	struct rq *rq_ = rq_of_oxc_rq(oxc_rq);

	//printk("dequeue_task_oxc: pid=%d\n", p->pid);
	update_rq_clock(rq);
	//raw_spin_lock(&rq_->lock);
	/* Update the local runqueue. */
	update_rq_clock(rq_);
	/*
	 * Dequeue the task from the local runqueue 
	 * by its scheduling class.
	 */
	p->sched_class->dequeue_task(rq_, p, flags);

//	raw_spin_lock(&oxc_rq->oxc_runtime_lock);
	dec_oxc_tasks(p, oxc_rq);
	dequeue_oxc_rq(oxc_rq);
	dec_nr_running(rq);
//	raw_spin_unlock(&oxc_rq->oxc_runtime_lock);

	//raw_spin_unlock(&rq_->lock);
}

static void yield_task_oxc(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct oxc_rq *oxc_rq;

	if( is_oxc_task(curr)) {
		oxc_rq = oxc_rq_of_task(curr);
		curr->sched_class->yield_task(rq_of_oxc_rq(oxc_rq));
	}
} 

/* 
 * p is an oxc task and this methos checks if it can preempt 
 * currently executing task in the cpu.
 */
static void 
check_preempt_curr_oxc(struct rq *rq, struct task_struct *p, int flags)
{
	update_rq_clock(rq);
	if( check_preempt_oxc_rq(rq->curr, p, flags)) {
		resched_task(rq->curr);
	}
	return;
}

/* To pick an oxc task to run if there exists; o.w. return NULL. */
static struct task_struct* pick_next_task_oxc(struct rq *rq)
{
	struct oxc_rq *oxc_rq;
	struct rq *rq_;
	struct task_struct *p, *curr;
	const struct sched_class *class;

	update_rq_clock(rq);
	oxc_rq = pick_next_oxc_rq(rq);
	if( !oxc_rq)
		return NULL;

	BUG_ON(!oxc_rq->oxc_nr_running);
	
	rq_ = rq_of_oxc_rq(oxc_rq);

	//raw_spin_lock(&rq_->lock);
	update_rq_clock(rq_);
	curr = rq_->curr;

	for_each_class(class) {
		if( class != &idle_sched_class) {
			p = class->pick_next_task(rq_);
			if( p) {
				rq_->curr = p;
	//			raw_spin_unlock(&rq_->lock);
				return p;
			}
		}
	}
	
//	raw_spin_unlock(&rq_->lock);
	return NULL;
}	

static void put_prev_task_oxc(struct rq* rq, struct task_struct *p)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);

	update_rq_clock(rq);
//	raw_spin_lock(&oxc_rq->rq_.lock);
	if( p->on_rq)
		update_rq_clock(rq_of_oxc_rq(oxc_rq));

	update_curr_oxc_rq(rq);
	
	p->sched_class->put_prev_task(rq_of_oxc_rq(oxc_rq), p);
//	raw_spin_unlock(&oxc_rq->rq_.lock);
}	

static void set_curr_task_oxc(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct oxc_rq *oxc_rq = oxc_rq_of_task(curr);
	
	update_rq_clock(rq);

	oxc_rq->oxc_start_time = rq->clock;
	//raw_spin_lock(&oxc_rq->rq_.lock);

	update_rq_clock(rq_of_oxc_rq(oxc_rq));
	curr->sched_class->set_curr_task(rq);
	//curr->sched_class->set_curr_task(rq_of_oxc_rq(oxc_rq));

//	raw_spin_unlock(&oxc_rq->rq_.lock);
}

static void task_tick_oxc(struct rq *rq, struct task_struct *p, int queued)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);

	update_rq_clock(rq);
	update_curr_oxc_rq(rq);
	//raw_spin_lock(&oxc_rq->rq_.lock);
	update_rq_clock(rq_of_oxc_rq(oxc_rq));
	//update_cpu_load_active(rq_of_oxc_rq(oxc_rq));

	p->sched_class->task_tick(rq_of_oxc_rq(oxc_rq), p, queued);
	//raw_spin_unlock(&oxc_rq->rq_.lock);
}

static void 
switched_to_oxc(struct rq *rq, struct task_struct *p)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	
	p->sched_class->switched_to(rq_of_oxc_rq(oxc_rq), p);
}
		
static void prio_changed_oxc(struct rq *rq, struct task_struct *p, int old_prio)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	struct rq *rq_ = rq_of_oxc_rq(oxc_rq);
	
	p->sched_class->prio_changed(rq_, p, old_prio);
}

static unsigned int get_rr_interval_oxc(struct rq *rq, struct task_struct *p)
{
	struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	struct rq *rq_ = rq_of_oxc_rq(oxc_rq);
	
	return p->sched_class->get_rr_interval(rq_, p);
}	

static bool
yield_to_task_oxc(struct rq *rq, struct task_struct *p, bool preempt)
{
        struct oxc_rq *oxc_rq = oxc_rq_of_task(p);
	struct rq *rq_ = rq_of_oxc_rq(oxc_rq);

        return p->sched_class->yield_to_task(rq_, p, preempt);
}

const struct sched_class oxc_sched_class = {
        .enqueue_task           = enqueue_task_oxc,
        .dequeue_task           = dequeue_task_oxc,
        .yield_task             = yield_task_oxc,
        .yield_to_task          = yield_to_task_oxc,

        .check_preempt_curr     = check_preempt_curr_oxc,

        .pick_next_task         = pick_next_task_oxc,
        .put_prev_task          = put_prev_task_oxc,

        .set_curr_task          = set_curr_task_oxc,
        .task_tick              = task_tick_oxc,

        .prio_changed           = prio_changed_oxc,
        .switched_to            = switched_to_oxc,

        .get_rr_interval        = get_rr_interval_oxc,
};



/******************************************************************************/
/*		The following concerns how to export the MBCI core 	      */		
/*		functionality defined above to both developers in kernel      */ 
/*		space and users in user space.				      */
/******************************************************************************/

struct rt_bandwidth def_oxc_bandwidth;

void init_oxc_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime)
{
        rt_b->rt_period = ns_to_ktime(period);
        rt_b->rt_runtime = runtime;

        raw_spin_lock_init(&rt_b->rt_runtime_lock);
}

/* 
 * To set the reservation parameters, oxc_runtime and oxc_period,
 * in an oxc_rq.
 */
void set_oxc_rq_bandwidth(struct oxc_rq *oxc_rq, u64 period, u64 runtime)
{
	raw_spin_lock(&oxc_rq->oxc_runtime_lock);

	oxc_rq->oxc_period = ns_to_ktime(period);
	oxc_rq->oxc_runtime = runtime;
	
	raw_spin_unlock(&oxc_rq->oxc_runtime_lock);
}

/* Initialize the local rq of an oxc_rq. */
static void init_rq_oxc(struct rq *rq, struct rq *per_cpu)
{
        raw_spin_lock_init(&rq->lock);
        rq->nr_running = 0;
        rq->calc_load_active = 0;
        rq->in_oxc = 1;
        init_cfs_rq(&rq->cfs);
        init_rt_rq(&rq->rt, rq);
        rq->rt.rt_runtime = RUNTIME_INF;
        atomic_set(&rq->nr_iowait, 0);
#ifdef CONFIG_SMP
        rq->cpu = per_cpu->cpu;
	rq->sd = NULL;
        rq->rd = NULL;
        rq->cpu_power = SCHED_POWER_SCALE;
        rq->post_schedule = 0;
        rq->active_balance = 0;
        rq->next_balance = jiffies;
        rq->push_cpu = 0;
        rq->online = 0;
        rq->idle_stamp = 0;
        rq->avg_idle = 2*sysctl_sched_migration_cost;

        INIT_LIST_HEAD(&rq->cfs_tasks);

#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
        INIT_LIST_HEAD(&rq->leaf_cfs_rq_list);
#endif
#ifdef CONFIG_RT_GROUP_SCHED
        INIT_LIST_HEAD(&rq->leaf_rt_rq_list);
#endif
	rq->idle = NULL;//per_cpu->idle;
	rq->curr = NULL;//per_cpu->idle;
}

/* To initialize an oxc_rq. */
static void init_oxc_rq(struct oxc_rq *oxc_rq, struct rq *rq)
{
        int  j;

        oxc_rq->oxc_time = 0;
        oxc_rq->oxc_throttled = 0;
        oxc_rq->oxc_deadline = 0;
        oxc_rq->oxc_runtime = 0;
        oxc_rq->oxc_period = def_oxc_bandwidth.rt_period;
        oxc_rq->oxc_start_time = 0;

        raw_spin_lock_init(&oxc_rq->oxc_runtime_lock);

        hrtimer_init(&oxc_rq->oxc_period_timer, CLOCK_MONOTONIC,
                                                        HRTIMER_MODE_REL);
        oxc_rq->oxc_period_timer.function = sched_oxc_period_timer;

        oxc_rq->rq = rq;
        init_rq_oxc(&oxc_rq->rq_, rq);
        oxc_rq->oxc_needs_resync = false;
        RB_CLEAR_NODE(&oxc_rq->rb_node);

        for (j = 0; j < CPU_LOAD_IDX_MAX; j++)
                oxc_rq->rq_.cpu_load[j] = 0;

}

void init_oxc_edf_tree(struct oxc_edf_tree *tree)
{
        tree->rb_root = RB_ROOT;
        tree->rb_leftmost = NULL;
}

int alloc_oxc_sched_group(struct task_group *tg, struct task_group *parent)
{
        int i;

        tg->hyper_oxc_rq = parent->hyper_oxc_rq;

        if( parent->hyper_oxc_rq) {
                for_each_possible_cpu(i) {
#ifdef CONFIG_FAIR_GROUP_SCHED
                        tg->cfs_rq[i]->rq =
                                &parent->hyper_oxc_rq->oxc_rq[i]->rq_;
                        if( !parent->se[i] && tg->se[i])
                                tg->se[i]->cfs_rq =
			              &parent->hyper_oxc_rq->oxc_rq[i]->rq_.cfs;
#endif
#ifdef CONFIG_RT_GROUP_SCHED
                        tg->rt_rq[i]->rq =
                                &parent->hyper_oxc_rq->oxc_rq[i]->rq_;
                        if( !parent->rt_se[i] && tg->rt_se[i])
                                tg->rt_se[i]->rt_rq =
                                       &parent->hyper_oxc_rq->oxc_rq[i]->rq_.rt;
#endif
                }
                tg->oxc_label = 100;
        }
        else
                tg->oxc_label = 0;

        return 1;
}

struct oxc_rq *create_oxc_rq(struct rq *rq)
{
        struct oxc_rq *oxc_rq;

        oxc_rq = kzalloc(sizeof(struct oxc_rq), GFP_KERNEL);
        init_oxc_rq(oxc_rq, rq);

        return oxc_rq;
}

struct hyper_oxc_rq * create_hyper_oxc_rq(cpumask_var_t cpus_allowed)
{
        int i;
        struct oxc_rq *oxc_rq;
        struct hyper_oxc_rq *hir = kzalloc(sizeof(struct hyper_oxc_rq),
                                                                GFP_KERNEL);
        if( !hir)
                goto hir_creation_err;

        if(!alloc_cpumask_var(&hir->cpus_allowed, GFP_KERNEL))
                goto hir_creation_err;

        hir->oxc_rq = kzalloc(sizeof(oxc_rq) * nr_cpu_ids, GFP_KERNEL);
        if( !hir->oxc_rq)
                goto hir_creation_err;

        cpumask_copy(hir->cpus_allowed, cpus_allowed);
        //for_each_cpu(i, cpus_allowed)
        for_each_possible_cpu(i)
                hir->oxc_rq[i] = create_oxc_rq(cpu_rq(i));

        return hir;
hir_creation_err:
        return NULL;
}

int attach_oxc_rq_to_hyper_oxc_rq(struct hyper_oxc_rq *h_oxc_rq, 
						struct oxc_rq *oxc_rq)
{
        int cpu;
#ifdef CONFIG_SMP
        cpu = oxc_rq->rq->cpu;
#else
        cpu = 0;
#endif
        cpumask_set_cpu(cpu, h_oxc_rq->cpus_allowed);
        h_oxc_rq->oxc_rq[cpu] = oxc_rq;

        return 1;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void init_tg_cfs_entry_oxc(struct task_group *tg,
					struct cfs_rq *cfs_rq,
					struct sched_entity *se, int cpu,
					struct sched_entity *parent,
					struct oxc_rq *oxc_rq)
{
	struct rq *rq = rq_of_oxc_rq(oxc_rq);
	init_tg_cfs_entry(tg, cfs_rq, se, cpu, parent);
	tg->cfs_rq[cpu]->rq = rq;
	if( !parent && se)
		se->cfs_rq = &rq->cfs;
} 
#endif

#ifdef CONFIG_RT_GROUP_SCHED
static void init_tg_rt_entry_oxc(struct task_group *tg,
                                struct rt_rq *rt_rq,
                                struct sched_rt_entity *rt_se, int cpu,
                                struct sched_rt_entity *parent,
                                struct oxc_rq *oxc_rq)
{

	struct rq *rq = rq_of_oxc_rq(oxc_rq);
	/* This will bring inconsistency ... */
        init_tg_rt_entry(tg, rt_rq, rt_se, cpu, parent);

        tg->rt_rq[cpu]->rq = rq;
        if( !parent && !rt_se) {
                tg->rt_rq[cpu]->rt_runtime = tg->rt_bandwidth.rt_runtime;
        }

        if( !parent && rt_se)
                rt_se->rt_rq = &rq->rt;
}
#endif

/* 
 * To associate an existing cgroup to an hyper_oxc_rq :
 * - empty tasks inside the cgroup and its descendant cgroups;
 * - redirect the cgroup and its descendant cgroups to local runqueues
 *   of hyper_oxc_rq;
 * - direct tasks tasks in these cgroups to local runqueues and 
 *   re enqueue them.
 */
static void remove( struct task_struct *p, struct cgroup_scanner *scan)
{
        int *cpu = scan->data;
	int on_rq;
#ifdef CONFIG_SMP
        if( task_thread_info(p)->cpu != *cpu)
                return;
#endif
	
	on_rq = p->on_rq; 
	
	if( on_rq) {
		raw_spin_lock(&cpu_rq(*cpu)->lock);
		dequeue_task(cpu_rq(*cpu), p, 0);
		raw_spin_unlock(&cpu_rq(*cpu)->lock);
	}
}

static void add( struct task_struct *tsk, struct cgroup_scanner *scan)
{
        int *cpu = scan->data;

#ifdef CONFIG_SMP
        if( task_thread_info(tsk)->cpu != *cpu)
                return;
#endif
        set_task_rq(tsk, *cpu);
	raw_spin_lock(&cpu_rq(*cpu)->lock);
        enqueue_task(cpu_rq(*cpu), tsk, 0);
	raw_spin_unlock(&cpu_rq(*cpu)->lock);
}

static void empty_cgroup_oxc(struct cgroup *cg)
{
	int i;
        struct cgroup *child, *parent;
        struct cgroup_scanner scan;
	struct task_group *tg;

	for_each_possible_cpu(i) {
//		raw_spin_lock(&cpu_rq(i)->lock);
		scan.cg = cg;
                scan.test_task = NULL;
                scan.process_task = remove;
                scan.heap = NULL;
                scan.data = &i;

                cgroup_scan_tasks(&scan);

                /* dequeue tasks in sub cgroups  */
                parent = cg;
eco1:		
	list_for_each_entry_rcu(child, &parent->children, sibling) {
			tg = cgroup_tg(child);

			if( tg->hyper_oxc_rq)
				continue; 
			
			scan.cg = child;
			cgroup_scan_tasks(&scan);
		
			parent = child;
			goto eco1;
eco2:
			continue;
		}

		if( parent == cg)
			goto eco3;
		child = parent;
		parent = parent->parent;
		goto eco2;
eco3:
//		raw_spin_unlock(&cpu_rq(i)->lock);
		continue;
	}
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void reset_fair_group_stats(struct cgroup *cg) 
{
	struct task_group *tg = cgroup_tg(cg);
	int i;
	struct task_group *parent, *child;

	for_each_possible_cpu(i) {
//		raw_spin_lock(&cpu_rq(i)->lock);
/*
 * Before re enqueue tasks in each decsendant cgroups
 * we first reset some time related parameters for a cfs task 
 * group.
 */
                parent = tg;
rtg1:
                list_for_each_entry_rcu(child, &parent->children, siblings) {

                        if( child->hyper_oxc_rq)
                                continue;
#if 0
                        child->se[i]->on_rq                     = 0;
                        child->se[i]->exec_start                = 0;
                        child->se[i]->sum_exec_runtime          = 0;
                        child->se[i]->prev_sum_exec_runtime     = 0;
                        child->se[i]->nr_migrations             = 0;
#endif
                        child->se[i]->vruntime                  = 0;

			init_cfs_rq(child->cfs_rq[i]);
                        parent = child;
                        goto rtg1;
rtg2:
                        continue;
                }

                if( parent == tg)
                        goto rtg3;

                child = parent;
                parent = parent->parent;
                goto rtg2;
rtg3:
//		raw_spin_unlock(&cpu_rq(i)->lock);
		continue;
	}
}
#endif
	
static void redirect_rq(struct hyper_oxc_rq *hyper_oxc_rq, struct cgroup *cg)
{
	int i;
        struct task_group *child, *parent;
        struct cgroup_scanner scan;
	struct task_group *tg = cgroup_tg(cg);
	
	
      for_each_possible_cpu(i) {
//		raw_spin_lock(&cpu_rq(i)->lock);
#ifdef CONFIG_FAIR_GROUP_SCHED
                init_tg_cfs_entry_oxc(tg, &hyper_oxc_rq->oxc_rq[i]->rq_.cfs,
                                                NULL, i, NULL,
                                                hyper_oxc_rq->oxc_rq[i]);
#endif
#ifdef CONFIG_RT_GROUP_SCHED
                init_tg_rt_entry_oxc(tg, &hyper_oxc_rq->oxc_rq[i]->rq_.rt,
                                                NULL, i, NULL,
                                                hyper_oxc_rq->oxc_rq[i]);
#endif
		scan.cg = cg;
                scan.test_task = NULL;
                scan.process_task = add;
                scan.heap = NULL;
                scan.data = &i;
                cgroup_scan_tasks(&scan);
//		raw_spin_unlock(&cpu_rq(i)->lock);
	}

	for_each_possible_cpu(i) {
//		raw_spin_lock(&cpu_rq(i)->lock);
		parent = tg;
rr1:
                list_for_each_entry_rcu(child, &parent->children, siblings) {

                        if( child->hyper_oxc_rq
					&& child->hyper_oxc_rq != hyper_oxc_rq)
                                continue;
#ifdef CONFIG_FAIR_GROUP_SCHED
                        init_tg_cfs_entry_oxc(child, child->cfs_rq[i],
                                        child->se[i], i, child->parent->se[i],
                                        hyper_oxc_rq->oxc_rq[i]);
#endif

#ifdef CONFIG_RT_GROUP_SCHED
                        init_tg_rt_entry_oxc(child, child->rt_rq[i],
                                        child->rt_se[i], i,
                                        child->parent->rt_se[i],
                                        hyper_oxc_rq->oxc_rq[i]);
#endif

                        child->hyper_oxc_rq = hyper_oxc_rq;
                        child->oxc_label = 100;

                        scan.cg = child->css.cgroup;
                        scan.process_task = add;
                        scan.data = &i;
                        cgroup_scan_tasks(&scan);
                        parent = child;
                        goto rr1;
rr2:
                        continue;
                }

                if( parent == tg)
                        goto rr3;

                child = parent;
                parent = parent->parent;
                goto rr2;
rr3:
//		raw_spin_unlock(&cpu_rq(i)->lock);
                continue;
        }
}

void attach_cgroup_to_hyper_oxc_rq(struct hyper_oxc_rq *hyper_oxc_rq, 
							struct cgroup *cg)
{
	struct task_group *tg;

//	local_irq_disable();
//	lock_kernel();
	empty_cgroup_oxc(cg);

	tg = cgroup_tg(cg);	
	tg->parent = NULL;
	tg->oxc_label = 1;
	tg->hyper_oxc_rq = hyper_oxc_rq;
	
#ifdef CONFIG_FAIR_GROUP_SCHED
	reset_fair_group_stats(cg);
#endif	
	redirect_rq(hyper_oxc_rq, cg);
//	unlock_kernel();
//	local_irq_enable();
}
	
