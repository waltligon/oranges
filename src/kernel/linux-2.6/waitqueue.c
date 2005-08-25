/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup pvfs2linux
 *
 *  In-kernel waitqueue operations.
 */

#include "pvfs2-kernel.h"

extern struct list_head pvfs2_request_list;
extern spinlock_t pvfs2_request_list_lock;
extern struct qhash_table *htable_ops_in_progress;
extern int debug;

static inline void clean_up_interrupted_operation(
    pvfs2_kernel_op_t * op)
{
    /*
      handle interrupted cases depending on what state we were in when
      the interruption is detected.  there is a coarse grained lock
      across the operation.

      NOTE: be sure not to reverse lock ordering by locking an op lock
      while holding the request_list lock.  Here, we first lock the op
      and then lock the appropriate list.
    */
    spin_lock(&op->lock);
    switch (op->op_state)
    {
	case PVFS2_VFS_STATE_WAITING:
	    /*
              upcall hasn't been read; remove op from upcall request
              list.
            */
	    remove_op_from_request_list(op);
	    pvfs2_print("Interrupted: Removed op from request_list\n");
	    break;
	case PVFS2_VFS_STATE_INPROGR:
	    /* op must be removed from the in progress htable */
	    remove_op_from_htable_ops_in_progress(op);
	    pvfs2_print("Interrupted: Removed op from "
			"htable_ops_in_progress\n");
	    break;
	case PVFS2_VFS_STATE_SERVICED:
	    /*
              can this happen? even if it does, I think we're ok with
              doing nothing since no cleanup is necessary
	     */
	    break;
    }
    spin_unlock(&op->lock);
}

/** sleeps on waitqueue waiting for matching downcall for some amount of
 *    time and then wakes up.
 *
 *  \post when this call returns to the caller, the specified op will no
 *        longer be on any list or htable.
 *
 *  \return values and op status changes:
 *
 *  \retval PVFS2_WAIT_ERROR an error occurred; op status unknown
 *  \retval PVFS2_WAIT_SUCCESS success; everything ok.  the op state will
 *          be marked as serviced
 *  \retval PVFS2_WAIT_TIMEOUT_REACHED timeout reached (before downcall
 *          recv'd) the caller has the choice of either requeueing the op
 *          or failing the operation when this occurs.  the op observes no
 *          state change.
 *  \retval PVFS2_WAIT_SIGNAL_RECVD sleep interrupted (signal recv'd) the
 *          op observes no state change.
*/
int wait_for_matching_downcall(pvfs2_kernel_op_t * op)
{
    int ret = PVFS2_WAIT_ERROR;
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
	    ret = PVFS2_WAIT_SUCCESS;
	    break;
	}
	spin_unlock(&op->lock);

	if (!signal_pending(current))
	{
	    if (!schedule_timeout
		(MSECS_TO_JIFFIES(1000 * MAX_SERVICE_WAIT_IN_SECONDS)))
	    {
                pvfs2_print("*** operation timed out (tag %Ld)\n",
                            Ld(op->tag));
                clean_up_interrupted_operation(op);
		ret = PVFS2_WAIT_TIMEOUT_REACHED;
		break;
	    }
	    continue;
	}

        pvfs2_print("*** operation interrupted by a signal (tag %Ld)\n",
                    Ld(op->tag));
        clean_up_interrupted_operation(op);
        ret = PVFS2_WAIT_SIGNAL_RECVD;
        break;
    }

    set_current_state(TASK_RUNNING);

    spin_lock(&op->lock);
    remove_wait_queue(&op->waitq, &wait_entry);
    spin_unlock(&op->lock);

    return ret;
}

/** similar to wait_for_matching_downcall(), but used in the special case
 *  of I/O cancellations.
 *
 *  \note we need a special wait function because if this is called we already
 *        know that a signal is pending in current and need to service the
 *        cancellation upcall anyway.  the only way to exit this is to either
 *        timeout or have the cancellation be serviced properly.
*/
int wait_for_cancellation_downcall(pvfs2_kernel_op_t * op)
{
    int ret = PVFS2_WAIT_ERROR;
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
	    ret = PVFS2_WAIT_SUCCESS;
	    break;
	}
	spin_unlock(&op->lock);

        if (!schedule_timeout
            (MSECS_TO_JIFFIES(1000 * MAX_SERVICE_WAIT_IN_SECONDS)))
        {
            pvfs2_print("*** operation timed out\n");
            clean_up_interrupted_operation(op);
            ret = PVFS2_WAIT_TIMEOUT_REACHED;
            break;
        }
    }

    set_current_state(TASK_RUNNING);

    spin_lock(&op->lock);
    remove_wait_queue(&op->waitq, &wait_entry);
    spin_unlock(&op->lock);

    return ret;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
