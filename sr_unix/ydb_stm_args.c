/****************************************************************
 *								*
 * Copyright (c) 2018 YottaDB LLC. and/or its subsidiaries.	*
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_semaphore.h"
#include <errno.h>

#include "libyottadb_int.h"
#include "mdq.h"
#include "gdsroot.h"
#include "gdskill.h"
#include "gdscc.h"
#include "filestruct.h"
#include "jnl.h"
#include "hashtab_int4.h"     /* needed for tp.h */
#include "buddy_list.h"
#include "tp.h"
#include "gtm_multi_thread.h"
#include "caller_id.h"
#include "trace_table.h"

GBLREF	stm_workq	*stmWorkQueue[];		/* Array to hold list of work queues for SimpleThreadAPI */
GBLREF	stm_workq	*stmTPWorkQueue;		/* Alternate queue main worker thread uses when TP is active */

/* Function to take the arguments stored in a callblk and push them onto the work queue for the worker
 * thread(s) (multiple threads is a future project) to execute in the isolated thread.
 *
 * This routine only ever puts work on one of two queues - if not in TP (the descriptor is NULL), then work is put on
 * the main work queue. If we are in TP (descriptor is not NULL), then work is put on the alternate TP work queue regardless
 * of TP level. This works because only one level at a time will ever be active.
 */
intptr_t ydb_stm_args(uint64_t tptoken, stm_que_ent *callblk)
{
	int		status, save_errno;
	intptr_t	retval;
	stm_workq	*queueToUse;
	boolean_t	startThread;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	TRCTBL_ENTRY(STAPITP_ENTRY, 0, "ydb_stm_args", NULL, pthread_self());
	DEBUG_ONLY(callblk->mainqcaller = caller_id());
	/* If have a descriptor block (meaning we are in TP), validate the tptoken value against our tplevel counter. A
	 * failure in validation is an error as well as if we discover no TP transaction has been started. Otherwiswe, we
	 * have a non-TP transaction so use the regular work queue.
	 */
	if (YDB_NOTTP != tptoken)
	{
		if (0 >= TREF(curWorkQHeadIndx))
		{	/* No TP transaction in effect - return error */
			SETUP_GENERIC_ERROR(YDB_ERR_INVTPTRANS);
			return YDB_ERR_INVTPTRANS;
		}
		if (tptoken != stmWorkQueue[TREF(curWorkQHeadIndx)]->tptoken)
		{	/* This is not the correct token for this TP transaction - return error */
			SETUP_GENERIC_ERROR(YDB_ERR_INVTPTRANS);
			return YDB_ERR_INVTPTRANS;
		}
		/* This is legitimately TP mode - We are putting any subsequent work on the alternate queue but using the
		 * same worker thread so don't start a new thread. Instead, just lock the queue.
		 */
		queueToUse = stmTPWorkQueue;
		startThread = FALSE;
	} else
	{	/* Lock the queue header and start the worker thread if not already running */
		queueToUse = stmWorkQueue[0];
		startThread = TRUE;
	}
	assert(NULL != queueToUse);
	TRCTBL_ENTRY(STAPITP_LOCKWORKQ, startThread, queueToUse, callblk, pthread_self());
	LOCK_STM_QHEAD_AND_START_WORK_THREAD(queueToUse, startThread, ydb_stm_thread, TRUE, status);
	if (0 != status)
	{
		assert(FALSE);
		return status;			/* Error already setup */
	}
	/* Place this call block on the thread queue (at the end of the queue) */
	dqrins(&queueToUse->stm_wqhead, que, callblk);
	/* Release CV/mutex lock so worker thread can get the lock and wakeup */
	status = pthread_mutex_unlock(&queueToUse->mutex);
	if (0 != status)
	{
		SETUP_SYSCALL_ERROR("pthread_mutex_unlock()", status);
		assert(FALSE);
		return YDB_ERR_SYSCALL;
	}
	TRCTBL_ENTRY(STAPITP_UNLOCKWORKQ, callblk->calltyp, queueToUse, callblk, pthread_self());
	/* Signal the condition variable something is on the queue awaiting tender ministrations */
	status = pthread_cond_signal(&queueToUse->cond);
	if (0 != status)
	{
		SETUP_SYSCALL_ERROR("pthread_cond_signal()", status);
		assert(FALSE);
		return YDB_ERR_SYSCALL;
	}
	TRCTBL_ENTRY(STAPITP_SEMWAIT, 0, NULL, callblk, pthread_self());
	/* Now wait till a worker thread tells us our request is complete */
	GTM_SEM_WAIT(&callblk->complete, status);
	if (0 != status)
	{
		save_errno = errno;
		SETUP_SYSCALL_ERROR("sem_wait()", save_errno);
		assert(FALSE);
		return YDB_ERR_SYSCALL;
	}
	TRCTBL_ENTRY(STAPITP_REQCOMPLT, callblk->calltyp, callblk->retval, callblk, pthread_self());
	/* Save the return value, queue the now-free call block for later reuse */
	retval = callblk->retval;
	status = ydb_stm_freecallblk(callblk);
	if (0 != status)
	{
		assert(FALSE);
		return status;
	}
	/* Return the return value from the call */
	return retval;
}

/* Function to create a callblk with no parameters */
intptr_t ydb_stm_args0(uint64_t tptoken, uintptr_t calltyp)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	return ydb_stm_args(tptoken, callblk);
}

/* Function to create a callblk with 1 parameter */
intptr_t ydb_stm_args1(uint64_t tptoken, uintptr_t calltyp, uintptr_t p1)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	callblk->args[0] = p1;
	return ydb_stm_args(tptoken, callblk);
}

/* Function to create a callblk with 2 parameters */
intptr_t ydb_stm_args2(uint64_t tptoken, uintptr_t calltyp, uintptr_t p1, uintptr_t p2)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	callblk->args[0] = p1;
	callblk->args[1] = p2;
	return ydb_stm_args(tptoken, callblk);
}

/* Function to create a callblk with 3 parameters */
intptr_t ydb_stm_args3(uint64_t tptoken, uintptr_t calltyp, uintptr_t p1, uintptr_t p2, uintptr_t p3)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	callblk->args[0] = p1;
	callblk->args[1] = p2;
	callblk->args[2] = p3;
	return ydb_stm_args(tptoken, callblk);
}

/* Function to create a callblk with 4 parameters */
intptr_t ydb_stm_args4(uint64_t tptoken, uintptr_t calltyp, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	callblk->args[0] = p1;
	callblk->args[1] = p2;
	callblk->args[2] = p3;
	callblk->args[3] = p4;
	return ydb_stm_args(tptoken, callblk);
}

/* Function to create a callblk with 5 parameters */
intptr_t ydb_stm_args5(uint64_t tptoken, uintptr_t calltyp, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	callblk->args[0] = p1;
	callblk->args[1] = p2;
	callblk->args[2] = p3;
	callblk->args[3] = p4;
	callblk->args[4] = p5;
	return ydb_stm_args(tptoken, callblk);
}

#ifndef GTM64
/* Function to create a callblk with 6 parameters needed in 32 bit mode only */
intptr_t ydb_stm_args6(uint64_t tptoken, uintptr_t calltyp, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5,
		       uintptr_t p6)
{
	stm_que_ent	*callblk;

	/* Grab a call block (parameter block/queue entry) */
	callblk = ydb_stm_getcallblk();
	if (-1 == (intptr_t)callblk)
		return YDB_ERR_SYSCALL;		/* All errors from ydb_stm_getcallblk() are of this type */
	callblk->calltyp = calltyp;
	callblk->args[0] = p1;
	callblk->args[1] = p2;
	callblk->args[2] = p3;
	callblk->args[3] = p4;
	callblk->args[4] = p5;
	callblk->args[5] = p6;
	return ydb_stm_args(tptoken, callblk);
}
#endif
