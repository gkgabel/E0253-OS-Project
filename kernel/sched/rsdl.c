//header files have been simply copied from fair.c
#include <linux/energy_model.h>
#include <linux/mmap_lock.h>
#include <linux/hugetlb_inline.h>
#include <linux/jiffies.h>
#include <linux/mm_api.h>
#include <linux/highmem.h>
#include <linux/spinlock_api.h>
#include <linux/cpumask_api.h>
#include <linux/lockdep_api.h>
#include <linux/softirq.h>
#include <linux/refcount_api.h>
#include <linux/topology.h>
#include <linux/sched/clock.h>
#include <linux/sched/cond_resched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/isolation.h>
#include <linux/sched/nohz.h>

#include <linux/cpuidle.h>
#include <linux/interrupt.h>
#include <linux/mempolicy.h>
#include <linux/mutex_api.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/ratelimit.h>
#include <linux/task_work.h>

#include <asm/switch_to.h>

#include <linux/sched/cond_resched.h>

#include "sched.h"
#include "stats.h"
#include "autogroup.h"

#define prio_to_index(prio) prio-100 //newly defined macroto cnvert static priority to array index
#define alloted_tsk_time 5
#define alloted_level_time 20
const struct sched_class rsdl_sched_class;

void init_rsdl_rq(struct rsdl_rq *rsdl_rq)
{
	rsdl_rq->nr_running = 0;
	rsdl_rq->pri_rotate = 0;
	rsdl_rq->curr = NULL;
	rsdl_rq->priority_time_slice = 20;
	rsdl_rq->active = kmalloc(sizeof(struct ll_array),GFP_KERNEL);
	rsdl_rq->expired = kmalloc(sizeof(struct ll_array),GFP_KERNEL);
	for(int i=0; i<41; i++)
	{
		rsdl_rq->active->pri_array[i] = NULL;
		rsdl_rq->expired->pri_array[i] = NULL;
	}
}

/*static void print_queue(struct rsdl_rq *rsdl)
{
	printk("\nCurrent PID %d \n",rsdl->curr->pid);
	printk("\nActive Queue\n");
	 
	for(int i=0;i<41;i++)
	{
		struct ll_node *ptr = rsdl->active->pri_array[i];
		if(ptr==NULL)continue;
		printk("Priority level:%d\n",i);
		if(ptr)
		{
			printk("%d ",ptr->task->pid);
			ptr = ptr->next;
		}
	}
	printk("\nExpired Queue\n");
	for(int i=0;i<41;i++)
	{
		struct ll_node *ptr = rsdl->expired->pri_array[i];
		if(ptr==NULL)continue;
		printk("Priority level:%d\n",i);
		while(ptr!=NULL)
		{
			printk("%d ",ptr->task->pid);
			ptr = ptr->next;
		}
	}
}
*/
static void __enqueue_task_rsdl(struct rq *rq, struct task_struct *p,int flags, struct ll_array *array)
{

	int tsk_index = prio_to_index(p->prio), ins_index;
	struct rsdl_rq *rsdl_rq = &rq->rsdl;
	struct ll_node *task_node= kmalloc(sizeof(struct ll_node),GFP_KERNEL);
	//printk("enquee------nr_running\n%d",(&rq->rsdl)->nr_running);
	task_node->next = NULL;
	task_node->prev = NULL;
	task_node->task = p;
	ins_index = max(tsk_index,rsdl_rq->pri_rotate);
	rsdl_rq->pri_rotate = ins_index;
	if(array->pri_array[ins_index]==NULL)	
	{
		array->pri_array[ins_index] = task_node;
		task_node->prev = task_node;
		task_node->next = task_node;
	}
	else
	{
		task_node->next = array->pri_array[ins_index];
		task_node->prev = array->pri_array[ins_index]->prev;
		array->pri_array[ins_index]->prev->next = task_node;
		array->pri_array[ins_index]->prev = task_node;
	}
	rsdl_rq->nr_running++;
	//print_queue(rsdl_rq);
	//printk("level now p***l %d \n",rsdl_rq->pri_rotate);
}

static void enqueue_task_rsdl(struct rq *rq, struct task_struct *p,int flags)
{
	//p->st_time_slice = 0; //initializw with 0 schedule ticks //was this the right p[lace to do it] 
	__enqueue_task_rsdl(rq,p,flags, (&rq->rsdl)->active);
}

static void dequeue_task_rsdl(struct rq *rq, struct task_struct *p,int flags)
{
	int tsk_index = prio_to_index(p->prio);
	struct rsdl_rq *rsdl_rq = &rq->rsdl;
	struct ll_node *task_node = rsdl_rq->active->pri_array[tsk_index];
	//printk("dequeee-------NO OF RUNNING PROCESS = %d\n current pid is=%d\n pid being dequeued=%d\n", rsdl_rq->nr_running,rsdl_rq->curr->pid,p->pid);
	//print_queue(rsdl_rq);
	//printk("TIME SLICE%d\n",(&rq->rsdl)->curr->st_time_slice);
	if(task_node != NULL) //check in the priority level of the task 
	{
		do{
			if(task_node->task->pid == p->pid) //found it
			{
				if(task_node->next==task_node) //is there a single node in this circular list
				{
					rsdl_rq->active->pri_array[tsk_index]= NULL;
					rsdl_rq->nr_running--;
					kfree(task_node);
					return ;	
				}
				//if multiple tasks in circular queue
				task_node->prev->next = task_node->next;
				task_node->next->prev = task_node->prev;
				kfree(task_node);
				rsdl_rq->nr_running--;
				return;
			}
			task_node = task_node->next;
		}while(task_node != rsdl_rq->active->pri_array[tsk_index]);
	}
	/*check in level pri_rotate. Basically just the above logic but
	on a different circular code*/
	task_node = rsdl_rq->active->pri_array[rsdl_rq->pri_rotate];
	if(task_node!=NULL) 
	{
		do{
			if(task_node->task->pid==p->pid) //found it
			{
				if(task_node->next==task_node) //is there a single node in this circular list
				{
					rsdl_rq->active->pri_array[rsdl_rq->pri_rotate]= NULL;
					kfree(task_node);
					rsdl_rq->nr_running--;
					return ;	
				}
				//if multiple tasks in circular queue
				task_node->prev->next = task_node->next;
				task_node->next->prev = task_node->prev;
				kfree(task_node);
				rsdl_rq->nr_running--;
				return;
			}
			task_node = task_node->next;
		}while(task_node != rsdl_rq->active->pri_array[rsdl_rq->pri_rotate]);
	}
	//printk("Error: Trying to dequeue an invalid process or maybe some other error");
}

static void swap_active_expired(struct rq *rq)
{
	struct rsdl_rq *rsdl_rq = &rq->rsdl;
	struct ll_array *temp = rsdl_rq->active;
	/*restructuring the active list and then swapping it */
	int tsk_index = 40; //last index of array
	struct ll_node *task_node = rsdl_rq->active->pri_array[tsk_index];
	//printk("swap_active_ \n");
	while(task_node)
	{
		if(task_node->next==task_node)rsdl_rq->active->pri_array[tsk_index] = NULL;
		else
		{
			task_node->prev->next = task_node->next;
			task_node->next->prev = task_node->prev;
			rsdl_rq->active->pri_array[tsk_index] = task_node->next;
		}
		(&rq->rsdl)->nr_running--;
		__enqueue_task_rsdl(rq,task_node->task,0,temp);
		kfree(task_node);
		task_node = rsdl_rq->active->pri_array[tsk_index];
	}
	rsdl_rq->active =  rsdl_rq->expired;
	rsdl_rq->expired = temp;
}

struct task_struct * pick_next_task_rsdl(struct rq *rq)
{
	struct rsdl_rq *rsdl_rq = &rq->rsdl;
	struct ll_node *task_node;
	if(rsdl_rq->nr_running == 0) return NULL; /////////////////////BIG PROBLEM CHANCE HERE
	//printk("pick next task------- nr_running=%d",rsdl_rq->nr_running);
	
	loop:

	while(rsdl_rq->pri_rotate < 40 && rsdl_rq->active->pri_array[rsdl_rq->pri_rotate]==NULL)
	{
		rsdl_rq->pri_rotate++;
		rsdl_rq->priority_time_slice = alloted_level_time;
	}
	if(rsdl_rq->pri_rotate == 40)
	{
		(&rq->rsdl)->pri_rotate = 0;
		swap_active_expired(rq);
		
		rsdl_rq->priority_time_slice = alloted_level_time;
		//printk("chk pt j\n");
		goto loop; //jai makali
	}
	task_node = rsdl_rq->active->pri_array [rsdl_rq->pri_rotate];
	if(task_node==NULL){
		//printk("chk pt c\n");
		return NULL;
	}
	do
	{
		if(task_node->task->__state == TASK_RUNNING )
		{
			/* Big mistake here. Nowhere has there been an assignment of 
			rq->curr. atleast not in pick_next_task*/
			//rq->curr = task_node->task;
			rsdl_rq->curr = task_node->task;
			return task_node->task;
		}
		task_node = task_node->next;
	}while(task_node != rsdl_rq->active->pri_array [rsdl_rq->pri_rotate]);
	//printk("chk pt h\n");
	//goto loop;
	return NULL;
}


static void check_preempt_rsdl (struct rq *rq, struct task_struct *p, int wake_flags)
{
	/*do this to check if a newly woken up task can be given cpu. Wrong.
	This was a big mistake. Above statement is wrong. In RSDL an incoming
	higher priority task cannot preempt a running higher priorty task. 
	Now is this even true? I guess new task comes and adds itself to the 
	te back of currently running list. So I am keeping it empty */
}

static struct task_struct *pick_task_rsdl(struct rq *rq)
{
	return NULL;
}

static bool yield_to_task_rsdl(struct rq *rq, struct task_struct *p)
{
	return 0;
}

static void yield_task_rsdl(struct rq *rq)
{
	/*
	Be very careful here. Something does not look good here
	Big mistake. i have done everywhere a time slice increase of 5. But in case of 
	IO wait if a process is simply 
	 One question remains. Is yield only called by user and
	never by kernel itself
	What it will do is to put the curr task at the end of its 
	queue in active queue. but is it the the right way to do it?*/
	struct ll_array *active = (&rq->rsdl)->active;
	if((&rq->rsdl)->nr_running == 1) return;
	if(active->pri_array[(&rq->rsdl)->pri_rotate] == active->pri_array[(&rq->rsdl)->pri_rotate]-> next)
	{
		//should I also call resched task here?????????????
		(&rq->rsdl)->pri_rotate++;
		(&rq->rsdl)->priority_time_slice = alloted_level_time;
	}
	else
	{
		active->pri_array[(&rq->rsdl)->pri_rotate] = active->pri_array[(&rq->rsdl)->pri_rotate]->next;
	}
	
}

static void put_prev_task_rsdl(struct rq *rq, struct task_struct *prev)
{
	//simply does accounting for previous task
}

static void set_next_task_rsdl(struct rq *rq, struct task_struct *p, bool first)
{
 //based on my knowledge it is called in __sched_setscheduler to to net the next task to be executed 
 //so here i update rq->curr
	(&rq->rsdl)->curr = p;
	p->st_time_slice = alloted_tsk_time;
}

static int
balance_rsdl(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return 0;
}

static int
select_task_rq_rsdl(struct task_struct *p, int prev_cpu, int wake_flags)
{
	return 0;
}

static void migrate_task_rq_rsdl(struct task_struct *p, int new_cpu)
{

}

static void rq_online_rsdl(struct rq *rq)
{

}

static void rq_offline_rsdl(struct rq *rq)
{

}

static void task_dead_rsdl(struct task_struct *p)
{
	
}

static void task_tick_rsdl(struct rq *rq, struct task_struct *curr, int queued)
{
	//printk("tick() tock\n");
	/*if((&rq->rsdl)->pri_rotate==41)
	{
		swap_active_expired(rq);
		(&rq->rsdl)->pri_rotate = 0;
	}*/
	//printk("tick() ck\n");
	(&rq->rsdl)->priority_time_slice = max(0,(&rq->rsdl)->priority_time_slice-1);
	curr->st_time_slice = max(0,curr->st_time_slice-1);	
	//if( (&rq->rsdl)->nr_running < 2) return;
	//printk("tick() t\n");
	//printk("pid in tick() %d", curr->pid);
	if(curr->st_time_slice == 0 || (&rq->rsdl)->priority_time_slice == 0)
	{
		//printk("level number %d",(&rq->rsdl)->pri_rotate);
		//print_queue(&rq->rsdl);

		if(curr->st_time_slice==0)
		{
			//printk("time slicw for pid expure curr_level=%d\n",(&rq->rsdl)->pri_rotate);
			printk("cp 1\n");
			if((&rq->rsdl)->active->pri_array[(&rq->rsdl)->pri_rotate]==NULL){
				//printk("cp kururururu %d  curr pid =%d\n",(&rq->rsdl)->pri_rotate,curr->pid);
				
				//return;
			}
			(&rq->rsdl)->active->pri_array[(&rq->rsdl)->pri_rotate]=(&rq->rsdl)->active->pri_array[(&rq->rsdl)->pri_rotate]->next;
			if((&rq->rsdl)->active->pri_array[(&rq->rsdl)->pri_rotate]->task->st_time_slice==0)
			{	
				//printk("cp 2\n");
				//curr=(&rq->rsdl)->active->pri_array[(&rq->rsdl)->pri_rotate]->task;
				(&rq->rsdl)->active->pri_array[(&rq->rsdl)->pri_rotate]->task->st_time_slice = alloted_tsk_time;
			}
			//printk("cp 3\n");
		}
		else if((&rq->rsdl)->priority_time_slice == 0)
		{
			int curr_level = (&rq->rsdl)->pri_rotate;
			struct ll_node *curr_active = (&rq->rsdl)->active->pri_array[curr_level], *next_active = (&rq->rsdl)->active->pri_array[curr_level+1];
			//printk("time slicw for queue expure\n");
			(&rq->rsdl)->pri_rotate++;
			//printk("cp 1\n");
			(&rq->rsdl)->priority_time_slice = alloted_level_time;
			//printk("cp 2\n");
			if(curr_active!=NULL)
			{
				//printk("cp 2.5\n");
				if(next_active==NULL)
				{
					//printk("cp 2.75\n");
					//next_active = curr_active;
					//curr_active = NULL;
					(&rq->rsdl)->active->pri_array[curr_level+1]= curr_active;
					(&rq->rsdl)->active->pri_array[curr_level]=NULL;
				}
				else
				{
					
					struct ll_node * temp = curr_active->prev ;
					//printk("cp 2.8\n");
					next_active->prev->next = curr_active;
					next_active->prev = curr_active->prev;
					curr_active->prev->next = next_active;
					curr_active->prev = temp;
					(&rq->rsdl)->active->pri_array[curr_level] = NULL;
				}
			}
		}
		//printk("cp 3\n");
		resched_curr(rq);
		return;
	}
	//printk("tick() n");
}

static void task_fork_rsdl(struct task_struct *p)
{

}

static void
prio_changed_rsdl(struct rq *rq, struct task_struct *p, int oldprio)
{
	if(!task_on_rq_queued(p))return ;
	if(task_current(rq,p))
	{
		//careful here
		//if priority decreases for running then reschedule. Should I reschedule or let it run?
		if (oldprio != p->prio)
			resched_curr(rq);
	}
	//if not runnig then no need to worry about resched. it should be simply enqueued.
}

static void switched_from_rsdl(struct rq *rq, struct task_struct *p)
{
	
}

static void switched_to_rsdl(struct rq *rq, struct task_struct *p)
{
	//what happens when a task from some other rq is changes its policy its policy to rsdl
	/*big mistake here.Be careful. should i preempt a currrenly running
	task for this. No I don't think so. It will simply be enqueued and
	no attempt is made to preempt the currenty running task*/
	/* I think when a new task changes its policy we should set its 
	default time slice to 5 sched ticks*/
	p->st_time_slice = alloted_tsk_time;
}

static unsigned int get_rr_interval_rsdl(struct rq *rq, struct task_struct *task)
{
	return 0;
}

static void update_curr_rsdl(struct rq *rq)
{

}


DEFINE_SCHED_CLASS(rsdl) = {
    .enqueue_task		= enqueue_task_rsdl, //changed
	.dequeue_task		= dequeue_task_rsdl, //changed
	.pick_next_task		= pick_next_task_rsdl, //changed

	.yield_task		= yield_task_rsdl,  
	.yield_to_task		= yield_to_task_rsdl,

	.check_preempt_curr	= check_preempt_rsdl,
	
	.put_prev_task		= put_prev_task_rsdl, //changed
	.set_next_task          = set_next_task_rsdl,
#ifdef CONFIG_SMP
	.balance		= balance_rsdl,
	.pick_task		= pick_task_rsdl,
	.select_task_rq		= select_task_rq_rsdl,
	.migrate_task_rq	= migrate_task_rq_rsdl,

	.rq_online		= rq_online_rsdl,
	.rq_offline		= rq_offline_rsdl,

	.task_dead		= task_dead_rsdl,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif
    .task_tick	= task_tick_rsdl, //changed
	.task_fork		= task_fork_rsdl,

	.prio_changed		= prio_changed_rsdl, //be careful with this function. 
	.switched_from		= switched_from_rsdl,
	.switched_to		= switched_to_rsdl, //be careful. 
 
	.get_rr_interval	= get_rr_interval_rsdl,

	.update_curr		= update_curr_rsdl,


#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

__init void init_sched_rsdl_class(void)
{

}