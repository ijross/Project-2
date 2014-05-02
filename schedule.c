/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */


#include <time.h>
#include <stdlib.h>
#include <stdio.h>
PRIVATE timer_t sched_timer;
PRIVATE unsigned balance_timeout;
PRIVATE int totalLot;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

FORWARD _PROTOTYPE( int schedule_process, (struct schedproc * rmp)	);
FORWARD _PROTOTYPE( void balance_queues, (struct timer *tp)		);


#define DEFAULT_USER_TIME_SLICE 200
#define DEFAULT_TICKET_NUMBER 20
#define LOSER_QUEUE  15
#define WINNER_QUEUE 14
#define BLOCK_QUEUE  13
#define MAX_TICKETS 100
#define MIN_TICKETS 0
/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

PUBLIC int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;
	
    	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
        	printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
            		m_ptr->m_source);
        	return EBADEPT;
    	}

    	rmp = &schedproc[proc_nr_n];
    
    
    	if (rmp->priority == WINNER_QUEUE || rmp->priority == BLOCK_QUEUE) {
        	rmp->ticket_number /= 2;
        	rmp->priority = LOSER_QUEUE; 
    	} 
	
	/* Process no in the lottery queue get their priority lowered by one*/
	if (rmp->priority < 12) {
		rmp->priority += 1; /* lower priority */
	}
		
	if ((rv = schedule_process(rmp)) != OK) {
		return rv;
	}

	do_lottery();

	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
PUBLIC int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;
	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	rmp->flags = 0; /*&= ~IN_USE;*/


	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
PUBLIC int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n, nice;
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
			!= OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint      = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent        = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority  = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	
	/* add ticket_number and win_about */
	rmp->ticket_number = DEFAULT_TICKET_NUMBER;
	rmp->win_amount    = 0;

	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		/*rmp->priority   = rmp->max_priority;*/
		rmp->priority   = LOSER_QUEUE;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		totalLot = 0;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		/*rmp->priority = schedproc[parent_nr_n].priority;*/
		rmp->priority   = LOSER_QUEUE;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	if ((rv = schedule_process(rmp)) != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;
	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
PUBLIC int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
    	int ticket_num = m_ptr->SCHEDULING_MAXPRIO;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;
        unsigned new_ticket_num, old_num;
	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}
    
    	/******SCHEDULING_MAXPRIO is the number passed by nice() *****/
	rmp = &schedproc[proc_nr_n];
      /*	new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}*/


	/*****Convert nice number of ticket number**** */
	ticket_num += 20;
	
	printf("IN nice newtickets = %d\n", ticket_num); 
	new_ticket_num = (unsigned)(ticket_num);
	if(new_ticket_num >= MAX_TICKETS){
	    return EINVAL;
	}

	/* Store old values, in case we need to roll back the changes 
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;*/
	old_num = rmp->ticket_number;

	/* Update the proc entry and reschedule the process */
	rmp->max_priority = rmp->priority = new_q;
	rmp->ticket_number = new_ticket_num;

	if ((rv = schedule_process(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
		rmp->ticket_number = old_num;
	}
        

	return rv;
}


/*===========================================================================*
 *				do_lottery					     *
 *===========================================================================*/
PUBLIC int do_lottery(void)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr;
	int ticket_number = 0;
	int lottery_num;

	totalLot += 1;

    	/* Picks a lottery number*/
	lottery_num = randTick(ticket_count());
    
	 /* Checks for any process in the winning queue and moves it to the BLOCK_QUEUE*/
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority == WINNER_QUEUE) {
				rmp->priority = BLOCK_QUEUE;
                		rmp->ticket_number *= 2;
				rv = schedule_process(rmp);
			}
		}
	}

    	/* Finds the process with the wining lottery number and moves it to the WINNER_QUEUE */
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority == LOSER_QUEUE) {	
				ticket_number += rmp->ticket_number;
				if(lottery_num <= ticket_number) {
					rmp->priority = WINNER_QUEUE;
					rmp->win_amount += 1;
					rv = schedule_process(rmp);
					break;
				}
			}		
		}
	}
	
	return rv;
}

/*===========================================================================*
 *				do_printWinner					     *
 *===========================================================================*/

PUBLIC void do_printWinner(void) {
	struct schedproc *rmp;
	int proc_nr;
	

/*	printf("Lottery info: \n");
		for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
			if (rmp->flags & IN_USE && rmp->win_amount > 0) {
				printf("pid: %d, win_amount: %d/%d = %d | rmp->ticket_number: %d, queue: %d\n", 
										rmp->endpoint, rmp->win_amount,
										totalLot, (rmp->win_amount/totalLot),
										rmp->ticket_number,rmp->priority);
			}
		}
	printf("Lottery info end\n");*/
}


/*===========================================================================*
 *				queue_count					     *
 *===========================================================================*/
PUBLIC int queue_count(void)
{
	struct schedproc *rmp;
	int proc_nr;
	unsigned queue_count = 0;
    	/* Counts the number of process in the WINNER_QUEUE */
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority == WINNER_QUEUE) {		
				 queue_count++;
			}
		}
	}
	return queue_count;
}

/*===========================================================================*
 *				ticket_count					     *
 *===========================================================================*/
PUBLIC int ticket_count(void)
{
	struct schedproc *rmp;
	int proc_nr;
	int ticket_count = 0;
	
    	/* Count the number of tickets in the system */
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority == LOSER_QUEUE) {		
				 ticket_count += rmp->ticket_number;
			}
		}
	}
	return ticket_count;
}

/*===========================================================================*
 *				randTick					     *
 *===========================================================================*/
PUBLIC int randTick(int totalTicks){
	int ticket = 0;
	if (totalTicks == 0) {
		return 0;
   	}
	srand((unsigned) time(0));
	ticket = rand() % (totalTicks) +1;
	return ticket;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
PRIVATE int schedule_process(struct schedproc * rmp)
{
	int rv;
    	printf("Winner has %d tickets\n", rmp->ticket_number);
	if ((rv = sys_schedule(rmp->endpoint, rmp->priority,
			rmp->time_slice)) != OK) {
		printf("SCHED: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, rv);
	}

	return rv;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

PUBLIC void init_scheduling(void)
{
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
PRIVATE void balance_queues(struct timer *tp)
{
	struct schedproc *rmp;
	int proc_nr;
	int rv;
	
	/* Balance queues not in the lottery queues */
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if ((rmp->priority > rmp->max_priority) &&
			    (rmp->priority != BLOCK_QUEUE) &&  
		            (rmp->priority != LOSER_QUEUE) && 
                	    (rmp->priority != WINNER_QUEUE)) {		
				
				rmp->priority -= 1; /* increase priority */
				schedule_process(rmp);
			}
		}
	}

	/* Check if there is a process in the WINNER_QUEUE
           if not pick a new winner from the lottery */
	if(queue_count() == 0) {
		do_lottery();
	}
	

	do_printWinner();

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}
