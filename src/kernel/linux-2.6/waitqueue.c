/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include "pvfs2-kernel.h"

extern struct list_head pvfs2_request_list;
extern spinlock_t pvfs2_request_list_lock;
extern struct qhash_table *htable_ops_in_progress;
extern struct qhash_table *htable_ops_invalidated;

/* FIXME: For testing, this is small; make bigger */
#define MAX_SERVICE_WAIT_IN_SECONDS       60

/*
  sleeps on waitqueue waiting for matching downcall
  for some amount of time and then wakes up.

  return values and op status changes:

  -1 - an error occurred
  ??? - op is moved to htable_ops_invalidated
   0 - success; everything ok
     - the passed in op will no longer be on any list or htable
   1 - timeout reached (before downcall recv'd)
   ??? - op is moved to htable_ops_invalidated
   2 - sleep interrupted (signal recv'd)
   ??? - op is moved to htable_ops_invalidted
*/
int wait_for_matching_downcall(
    pvfs2_kernel_op_t * op)
{
    int ret = -1;
    DECLARE_WAITQUEUE(wait_entry, current);

    spin_lock(&op->lock);
    add_wait_queue(&op->waitq, &wait_entry);
    spin_unlock(&op->lock);

    while (1)
    {
	set_current_state(TASK_INTERRUPTIBLE);

	spin_lock(&op->lock);
	if (op->op_state == PVFS2_VFS_STATE_SERVICED)
	{
	    spin_unlock(&op->lock);
	    ret = 0;
	    break;
	}
	spin_unlock(&op->lock);

	if (!signal_pending(current))
	{
	    if (!schedule_timeout
		(MSECS_TO_JIFFIES(1000 * MAX_SERVICE_WAIT_IN_SECONDS)))
	    {
		ret = 1;
		break;
	    }
	    continue;
	}

	pvfs2_print("FIXME: interrupted while waiting for downcall!!\n");
	/*
	   handle interrupted cases depending on what state
	   we were in when the interruption is detected.
	   there is a coarse grained lock across the operation (for now).

	   NOTE: be sure not to reverse lock ordering by locking
	   an op lock while holding the request_list lock
	 */
	spin_lock(&op->lock);
	switch (op->op_state)
	{
	case PVFS2_VFS_STATE_INVALID:
	    panic("pvfs2 op has invalid state; kernel panic");
	    break;
	case PVFS2_VFS_STATE_WAITING:
	    /*
	       upcall hasn't been read; remove
	       op from upcall request list
	     */
	    remove_op_from_request_list(op);
	    pvfs2_print("Interrupted: Removed op from request_list\n");
/*                 invalidate_op(op, 0); */
	    break;
	case PVFS2_VFS_STATE_INPROGR:
	    /*
	       op must be removed from the in progress htable
	       and inserted into the invalidated op htable.
	     */
	    remove_op_from_htable_ops_in_progress(op);
	    pvfs2_print("Interrupted: Removed op from "
			"htable_ops_in_progress\n");
/*                 invalidate_op(op, 0); */
	    break;
	case PVFS2_VFS_STATE_SERVICED:
	    /*
	       can this happen?
	     */
	    break;
	case PVFS2_VFS_STATE_DEAD:
	    break;
	}
	spin_unlock(&op->lock);

	ret = 2;
	break;
    }
    set_current_state(TASK_RUNNING);

    spin_lock(&op->lock);
    remove_wait_queue(&op->waitq, &wait_entry);
    spin_unlock(&op->lock);

    return ret;
}

/* int wait_for_matching_downcall(pvfs2_kernel_op_t *op) */
/* { */
/*     int ret = -1; */
/*     DEFINE_WAIT(wait_entry); */
/*     int op_state = PVFS2_VFS_STATE_INVALID; */

/*     while (op_state != PVFS2_VFS_STATE_SERVICED) */
/*     { */
/*         prepare_to_wait(&op->waitq,&wait_entry,TASK_INTERRUPTIBLE); */

/*         spin_lock(&op->lock); */
/*         op_state = op->op_state; */
/*         spin_unlock(&op->lock); */

/*         if (op_state != PVFS2_VFS_STATE_SERVICED) */
/*         { */
/*             schedule(); */
/*         } */
/*         finish_wait(&op->waitq, &wait_entry); */
/*     } */
/*     return ret; */
/* } */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
