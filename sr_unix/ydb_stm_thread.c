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

#ifdef YDB_USE_POSIX_TIMERS
#include <sys/syscall.h>	/* for "syscall" */
#endif

#include "gtm_semaphore.h"

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

GBLREF	stm_workq	*stmWorkQueue[];
GBLREF	stm_workq	*stmTPWorkQueue;
GBLREF	uint4		dollar_tlevel;
GBLREF	boolean_t	simpleThreadAPI_active;
GBLREF	pthread_t	gtm_main_thread_id;
GBLREF	boolean_t	gtm_main_thread_id_set;
#ifdef YDB_USE_POSIX_TIMERS
GBLREF	pid_t		posix_timer_thread_id;
GBLREF	boolean_t	posix_timer_created;
#endif

STATICFNDCL void ydb_stm_threadq_process(boolean_t *queueChanged);

/* Routine to manage worker thread(s) for the *_st() interface routines (Simple Thread API aka the
 * Simple Thread Method). Note for the time being, only one worker thread is created. In the future,
 * if/when YottaDB becomes fully-threaded, more worker threads may be allowed.
 *
 * Note there is no parameter or return value from this routine currently (both passed as NULL). The
 * routine signature is dictated by this routine being driven by pthread_create().
 */
void *ydb_stm_thread(void *parm)
{
	int		status;
	boolean_t	stop = FALSE, queueChanged;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	TRCTBL_ENTRY(STAPITP_ENTRY, 0, "ydb_stm_thread()", NULL, pthread_self());
	/* Now that we are establishing this main work queue thread, we need to make sure all timers and checks done by
	 * YottaDB *and* user code deal with THIS thread and not some other random thread.
	 */
	simpleThreadAPI_active = TRUE;
	assert(gtm_main_thread_id_set);
	gtm_main_thread_id = pthread_self();
	INITIALIZE_THREAD_MUTEX_IF_NEEDED; /* Initialize thread-mutex variables if not already done */
#	ifdef YDB_USE_POSIX_TIMERS
	assert(0 == posix_timer_created);
	assert(0 == posix_timer_thread_id);
	posix_timer_thread_id = syscall(SYS_gettid);
#	endif
	/* Initialize which queue we are looking for work in */
	TREF(curWorkQHead) = stmWorkQueue[0];			/* Initially pick requests from main work queue */
	assert(NULL != TREF(curWorkQHead));			/* Queue should be setup by now */
	/* Must have mutex locked before we start waiting */
	status = pthread_mutex_lock(&(TREF(curWorkQHead))->mutex);
	if (0 != status)
	{
		SETUP_SYSCALL_ERROR("pthread_mutex_lock(curWorkQHead)", status);
		assertpro(FALSE && YDB_ERR_SYSCALL);			/* No return possible so abandon thread */
	}
	/* Before we wait the first time, veryify nobody snuck something onto the queue by processing anything there */
	do
	{
		queueChanged = FALSE;
		ydb_stm_threadq_process(&queueChanged);
	} while (queueChanged);
	while (!stop)
	{	/* Wait for some work to probably show up */
		status = pthread_cond_wait(&(TREF(curWorkQHead))->cond, &(TREF(curWorkQHead))->mutex);
		if (0 != status)
		{
			SETUP_SYSCALL_ERROR("pthread_cond_wait(curWorkQHead)", status);
			assertpro(FALSE && YDB_ERR_SYSCALL);		/* No return possible so abandon thread */
		}
		do
		{
			queueChanged = FALSE;
			ydb_stm_threadq_process(&queueChanged);	/* Process any entries on the queue */
		} while (queueChanged);
	}
	return NULL;
}

/* Routine to actually process the thread work queue for the Simple Thread API/Method. Note there are two possible queues we
 * would be looking at.
 */
STATICFNDEF void ydb_stm_threadq_process(boolean_t *queueChanged)
{
	stm_que_ent		*callblk;
	int			int_retval, status, save_errno, calltyp;
	void			*voidstar_retval;
	stm_workq		*curTPQLevel;
#	ifndef GTM64
	unsigned long long	tparm;
#	endif
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	TRCTBL_ENTRY(STAPITP_ENTRY, 0, "ydb_stm_threadq_process", TREF(curWorkQHead), pthread_self());
	/* Loop to work till queue is empty */
	while (TRUE)
	{	/* If queue is empty, we should just go right back to sleep */
		if ((TREF(curWorkQHead))->stm_wqhead.que.fl == &(TREF(curWorkQHead))->stm_wqhead)
			break;
		/* Remove the first element (going forward) from the work queue */
		callblk = (TREF(curWorkQHead))->stm_wqhead.que.fl;
		dqdel(callblk, que);		/* Removes our element from the queue */
		/* We don't want to hold the lock during our processing so release it now */
		status = pthread_mutex_unlock(&((TREF(curWorkQHead))->mutex));
		if (0 != status)
		{
			SETUP_SYSCALL_ERROR("pthread_mutex_unlock(&((TREF(curWorkQHead))->mutex))", status);
			callblk->retval = (uintptr_t)YDB_ERR_SYSCALL;
			break;
		}
		TRCTBL_ENTRY(STAPITP_UNLOCKWORKQ, 0, TREF(curWorkQHead), callblk, pthread_self());
		TRCTBL_ENTRY(STAPITP_FUNCDISPATCH, callblk->calltyp, NULL, NULL, pthread_self());
		/* We have our request - dispatch it appropriately */
		calltyp = callblk->calltyp;
		switch (calltyp)
		{	/* This first group are all SimpleThreadAPI critters */
			case LYDB_RTN_CALL_VPLST_FUNC:
				int_retval = ydb_call_variadic_plist_func_s((ydb_vplist_func)callblk->args[0], callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_DATA:
				int_retval = ydb_data_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
							(ydb_buffer_t *)callblk->args[2], (unsigned int *)callblk->args[3]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_DELETE:
				int_retval = ydb_delete_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
							  (ydb_buffer_t *)callblk->args[2], (int)callblk->args[3]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_DELETE_EXCL:
				int_retval = ydb_delete_excl_s((int)callblk->args[0], (ydb_buffer_t *)callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_GET:
				int_retval = ydb_get_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
						       (ydb_buffer_t *)callblk->args[2], (ydb_buffer_t *)callblk->args[3]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_INCR:
				int_retval = ydb_incr_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
							(ydb_buffer_t *)callblk->args[2], (ydb_buffer_t *)callblk->args[3],
							(ydb_buffer_t *)callblk->args[4]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_LOCK_DECR:
				int_retval = ydb_lock_decr_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
							     (ydb_buffer_t *)callblk->args[2]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_LOCK_INCR:
#				ifdef GTM64
				int_retval = ydb_lock_incr_s((unsigned long long)callblk->args[0], (ydb_buffer_t *)callblk->args[1],
							     (int)callblk->args[2], (ydb_buffer_t *)callblk->args[3]);
#				else
#				ifdef BIGENDIAN
				tparm = (((unsigned long long)callblk->args[0]) << 32) | (unsigned long long)callblk->args[1];
#				else
				tparm = (((unsigned long long)callblk->args[1]) << 32) | (unsigned long long)callblk->args[0];
#				endif
				int_retval = ydb_lock_incr_s(tparm, (ydb_buffer_t *)callblk->args[2],
							     (int)callblk->args[3], (ydb_buffer_t *)callblk->args[4]);

#				endif
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_NODE_NEXT:
				int_retval = ydb_node_next_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
							     (ydb_buffer_t *)callblk->args[2], (int *)callblk->args[3],
							     (ydb_buffer_t *)callblk->args[4]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_NODE_PREVIOUS:
				int_retval = ydb_node_previous_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
								 (ydb_buffer_t *)callblk->args[2], (int *)callblk->args[3],
								 (ydb_buffer_t *)callblk->args[4]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_SET:
				int_retval = ydb_set_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
						       (ydb_buffer_t *)callblk->args[2], (ydb_buffer_t *)callblk->args[3]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_STR2ZWR:
				int_retval = ydb_str2zwr_s((ydb_buffer_t *)callblk->args[0], (ydb_buffer_t *)callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_SUBSCRIPT_NEXT:
				int_retval = ydb_subscript_next_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
								  (ydb_buffer_t *)callblk->args[2],
								  (ydb_buffer_t *)callblk->args[3]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_SUBSCRIPT_PREVIOUS:
				int_retval = ydb_subscript_previous_s((ydb_buffer_t *)callblk->args[0], (int)callblk->args[1],
								      (ydb_buffer_t *)callblk->args[2],
								      (ydb_buffer_t *)callblk->args[3]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_TP:
				/* Since TP calls a TP callback routine that may generate more requests, we can't drive it from
				 * here as that would block this queue from processing so this request gets pushed off to a
				 * new TP queue where the callback is made. That routine can then create additional requests for
				 * this thread but puts them on the TP work queue instead. This thread IS blocked for regular
				 * updates but still allows the callback routine to schedule work for this and successive TP
				 * levels by switching to a TP queue until the transaction is complete.
				 */
				*queueChanged = TRUE;
				/* Switch processing from the normal work queue to the TP work queue */
				if (NULL == stmTPWorkQueue)
				{
					stmTPWorkQueue = ydb_stm_init_work_queue();
					stmTPWorkQueue->threadid = stmWorkQueue[0]->threadid;	/* Uses same thread */
				}
				TREF(curWorkQHead) = stmTPWorkQueue;
				/* If not already done, allocate the TP work queue and set it up */
				/* Bump the index to the current TP level to the next one */
				(TREF(curWorkQHeadIndx))++;
				assert(STMWORKQUEUEDIM > TREF(curWorkQHeadIndx));
				curTPQLevel = stmWorkQueue[TREF(curWorkQHeadIndx)];
				/* Make sure new TP level is set up then lock it and start the thread if needed */
				if (NULL == curTPQLevel)
					curTPQLevel = stmWorkQueue[TREF(curWorkQHeadIndx)] = ydb_stm_init_work_queue();
				TRCTBL_ENTRY(STAPITP_LOCKWORKQ, TRUE, curTPQLevel, callblk, pthread_self());
				LOCK_STM_QHEAD_AND_START_WORK_THREAD(curTPQLevel, TRUE, ydb_stm_tpthread, FALSE, status);
				if (0 != status)
				{
					callblk->retval = (uintptr_t)status;
					break;
				}
				/* Place our call block on the TP thread queue */
				DEBUG_ONLY(callblk->tpqcaller = caller_id());
				dqrins(&curTPQLevel->stm_wqhead, que, callblk);
				/* Release CV/mutex lock so worker thread can get the lock and wakeup */
				status = pthread_mutex_unlock(&curTPQLevel->mutex);
				if (0 != status)
				{
					SETUP_SYSCALL_ERROR("pthread_mutex_unlock()", status);
					callblk->retval = (uintptr_t)YDB_ERR_SYSCALL;
					break;
				}
				TRCTBL_ENTRY(STAPITP_UNLOCKWORKQ, 0, NULL, callblk, pthread_self());
				/* Signal the condition variable something is on the queue awaiting tender ministrations */
				TRCTBL_ENTRY(STAPITP_SIGCOND, 0, curTPQLevel, callblk, pthread_self());
				status = pthread_cond_signal(&curTPQLevel->cond);
				if (0 != status)
				{
					SETUP_SYSCALL_ERROR("pthread_cond_signal()", status);
					break;
				}
				break;
			case LYDB_RTN_ZWR2STR:
				int_retval = ydb_zwr2str_s((ydb_buffer_t *)callblk->args[0], (ydb_buffer_t *)callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			/* This group of operations are from threaded utilities */
			case LYDB_RTN_FILE_ID_FREE:
				int_retval = ydb_file_id_free((ydb_fileid_ptr_t)callblk->args[0]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_FILE_IS_IDENTICAL:
				int_retval = ydb_file_is_identical((ydb_fileid_ptr_t)callblk->args[0],
								   (ydb_fileid_ptr_t)callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_FILE_NAME_TO_ID:
				int_retval = ydb_file_name_to_id((ydb_string_t *)callblk->args[0],
								 (ydb_fileid_ptr_t)callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_FREE:
				ydb_free((void *)callblk->args[0]);
				callblk->retval = 0;
				break;
			case LYDB_RTN_MALLOC:
				voidstar_retval = ydb_malloc((size_t)callblk->args[0]);
				callblk->retval = (intptr_t)voidstar_retval;
				break;
			case LYDB_RTN_MESSAGE:
				int_retval = ydb_message((int)callblk->args[0], (ydb_buffer_t *)callblk->args[1]);
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_STDIO_ADJUST:
				int_retval = ydb_stdout_stderr_adjust();
				callblk->retval = (uintptr_t)int_retval;
				break;
			case LYDB_RTN_TIMER_CANCEL:
				ydb_timer_cancel((int)callblk->args[0]);
				callblk->retval = 0;
				break;
			case LYDB_RTN_TIMER_START:
#				ifdef GTM64
				int_retval = ydb_timer_start((int)callblk->args[0], (unsigned long long)callblk->args[1],
							     (ydb_funcptr_retvoid_t)callblk->args[2],
							     (unsigned int)callblk->args[3], (void *)callblk->args[4]);
#				else
#				ifdef BIGENDIAN
				tparm = (((unsigned long long)callblk->args[1]) << 32) | (unsigned long long)callblk->args[2];
#				else
				tparm = (((unsigned long long)callblk->args[2]) << 32) | (unsigned long long)callblk->args[1];
#				endif
				int_retval = ydb_timer_start((int)callblk->args[0], tparm, (ydb_funcptr_retvoid_t)callblk->args[3],
							     (unsigned int)callblk->args[4], (void *)callblk->args[5]);
#				endif
				callblk->retval = (uintptr_t)int_retval;
				break;

			/* This last group of operation(s) are miscellaneous */
			case LYDB_RTN_TPCOMPLT:
				/* A previously initiated TP callblk has completed so we need to switch the work queue back to
				 * the main work queue and signal that to our immediate caller above.
				 */
				assert(stmTPWorkQueue == TREF(curWorkQHead));
				(TREF(curWorkQHeadIndx))--;
				assert(0 == TREF(curWorkQHeadIndx));
				TREF(curWorkQHead) = stmWorkQueue[0];
				*queueChanged = TRUE;
				TRCTBL_ENTRY(STAPITP_TPCOMPLT, TREF(curWorkQHeadIndx), TREF(curWorkQHead), NULL, pthread_self());
				callblk->retval = 0; /* Set request success (relied upon by thread waiting on this request) */
				break;
			default:
				assertpro(FALSE);
		}
		/* The request is complete (except TP - it's just requeued - regrab the lock to check if we are done or not yet */
		TRCTBL_ENTRY(STAPITP_LOCKWORKQ, FALSE, TREF(curWorkQHead), NULL, pthread_self());
		status = pthread_mutex_lock(&((TREF(curWorkQHead))->mutex));
		if (0 != status)
		{
			SETUP_SYSCALL_ERROR("pthread_mutex_lock()", status);
			assertpro(FALSE && YDB_ERR_SYSCALL);		/* No return possible so abandon thread */
		}
		/* If the queue changed due to TP level creation, the request is not done so should not yet be posted. Also,
		 * processing on this queue must halt immediately as we are headed into TP mode so just return.
		 */
		if (*queueChanged && (LYDB_RTN_TP == calltyp))
			return;
		/* Signal to process that we are done with this request */
		TRCTBL_ENTRY(STAPITP_SIGCOND, 0, NULL, callblk, pthread_self());
		GTM_SEM_POST(&callblk->complete, status);
		if (0 != status)
		{
			save_errno = errno;
			SETUP_SYSCALL_ERROR("sem_post()", save_errno);
			assert(FALSE);
			callblk->retval = (uintptr_t)YDB_ERR_SYSCALL;
			/* No return here - just keep going if at all possible */
		}
		/* If the queue head changed because the TP transaction has committed (in full) then we are done with the
		 * current queue for now. Leave it locked until we are ready to process this queue again. We might
		 * change how this works in the future.
		 */
		if (*queueChanged)
			return;
	}
}
