/*
 * pg_self_query.c
 *		Extract information about query state from current backend
 *
 * Copyright (c) 2016-2016, Postgres Professional
 *
 *	  contrib/pg_self_query/pg_self_query.c
 * IDENTIFICATION
 */

//#include "pg_self_query.h"
#include <postgres.h>
#include "commands/explain.h"
#include "nodes/pg_list.h"
#include "storage/shm_mq.h"

#include "utils/builtins.h"
#include "utils/memutils.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "executor/execParallel.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/shm_toc.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include <time.h>

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

//#define	PG_SQ_MODULE_KEY	0xCA94B109
//#define	PG_SELF_QUERY_KEY	0

//#define MIN_TIMEOUT   5000

#define TIMINIG_OFF_WARNING 1
#define BUFFERS_OFF_WARNING 2

#define TEXT_CSTR_CMP(text, cstr) \
	(memcmp(VARDATA(text), (cstr), VARSIZE(text) - VARHDRSZ))

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
//static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

void		_PG_init(void);
void		_PG_fini(void);

/* hooks defined in this module */
static void qs_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void qs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
						   uint64 count, bool execute_once);
static void qs_ExecutorFinish(QueryDesc *queryDesc);

/* Global variables */
List 					*QueryDescStack = NIL;
//static ProcSignalReason QueryStatePollReason = INVALID_PROCSIGNAL;
//static ProcSignalReason WorkerPollReason = INVALID_PROCSIGNAL;
//static bool 			module_initialized = false;
static const char		*be_state_str[] = {						/* BackendState -> string repr */
							"undefined",						 /* STATE_UNDEFINED */
							"idle",								/* STATE_IDLE */
							"active",							/* STATE_RUNNING */
							"idle in transaction",				/* STATE_IDLEINTRANSACTION */
							"fastpath function call",			/* STATE_FASTPATH */
							"idle in transaction (aborted)",	/* STATE_IDLEINTRANSACTION_ABORTED */
							"disabled",							/* STATE_DISABLED */
						};

// typedef struct
// {
// 	slock_t	 mutex;		/* protect concurrent access to `userid` */
// 	Oid		 userid;
// 	Latch	*caller;
// } RemoteUserIdResult;

/*
 * Result status on query state request from asked backend
 */
typedef enum
{
	QUERY_NOT_RUNNING,		/* Backend doesn't execute any query */
	STAT_DISABLED,			/* Collection of execution statistics is disabled */
	QS_RETURNED				/* Backend succesfully returned its query state */
} PG_QS_RequestResult;

/*
 *	Format of transmited data through message queue
 */
typedef struct
{
	int		length;							/* size of message record, for sanity check */
	PGPROC	*proc;
	PG_QS_RequestResult	result_code;
	int		warnings;						/* bitmap of warnings */
	int		stack_depth;
	char	stack[FLEXIBLE_ARRAY_MEMBER];	/* sequencially laid out stack frames in form of text records */
} shm_mq_msg;

//static void SendBgWorkerPids(void);
//static List *GetRemoteBackendWorkers(PGPROC *proc);
static List *GetRemoteBackendQueryStates(PGPROC *leader,
										 List *pworkers);

/* Shared memory variables */
//shm_toc			*toc = NULL;
//RemoteUserIdResult *counterpart_userid = NULL;
//shm_mq 			*mq = NULL;

/*
 * Estimate amount of shared memory needed.
 */
// static Size
// pg_qs_shmem_size()
// {
// 	shm_toc_estimator	e;
// 	Size				size;
// 	int					nkeys;

// 	shm_toc_initialize_estimator(&e);

// 	nkeys = 3;

// 	shm_toc_estimate_chunk(&e, sizeof(RemoteUserIdResult));
// 	shm_toc_estimate_chunk(&e, (Size) QUEUE_SIZE);

// 	shm_toc_estimate_keys(&e, nkeys);
// 	size = shm_toc_estimate(&e);

// 	return size;
// }

/*
 * Distribute shared memory.
 */
// static void
// pg_qs_shmem_startup(void)
// {
// 	bool	found;
// 	Size	shmem_size = pg_qs_shmem_size();
// 	void	*shmem;
// 	int		num_toc = 0;

// 	shmem = ShmemInitStruct("pg_self_query", shmem_size, &found);
// 	if (!found)
// 	{
// 		toc = shm_toc_create(PG_SQ_MODULE_KEY, shmem, shmem_size);

// 		counterpart_userid = shm_toc_allocate(toc, sizeof(RemoteUserIdResult));
// 		shm_toc_insert(toc, num_toc++, counterpart_userid);
// 		SpinLockInit(&counterpart_userid->mutex);

// 		mq = shm_toc_allocate(toc, QUEUE_SIZE);
// 		shm_toc_insert(toc, num_toc++, mq);
// 	}
// 	else
// 	{
// 		toc = shm_toc_attach(PG_SQ_MODULE_KEY, shmem);
// 		counterpart_userid = shm_toc_lookup(toc, num_toc++, false);
// 		mq = shm_toc_lookup(toc, num_toc++, false);
// 	}

// 	if (prev_shmem_startup_hook)
// 		prev_shmem_startup_hook();

// 	module_initialized = true;
// }

/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in qs_shmem_startup().
	 */
	//RequestAddinShmemSpace(pg_qs_shmem_size());

	/* Register interrupt on custom signal of polling query state */
	//QueryStatePollReason = RegisterCustomProcSignalHandler(SendQueryState);
	//WorkerPollReason = RegisterCustomProcSignalHandler(SendBgWorkerPids);
	//if (QueryStatePollReason == INVALID_PROCSIGNAL
	//	|| WorkerPollReason == INVALID_PROCSIGNAL)
	//{
	//	ereport(WARNING, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
	//				errmsg("pg_self_query isn't loaded: insufficient custom ProcSignal slots")));
	//	return;
	//}

	EmitWarningsOnPlaceholders("pg_self_query");

	/* Install hooks */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = qs_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = qs_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = qs_ExecutorFinish;
	//prev_shmem_startup_hook = shmem_startup_hook;
	//shmem_startup_hook = pg_qs_shmem_startup;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	//module_initialized = false;

	/* clear global state */
	list_free(QueryDescStack);
	//AssignCustomProcSignalHandler(QueryStatePollReason, NULL);

	/* Uninstall hooks. */
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	//shmem_startup_hook = prev_shmem_startup_hook;
}

/*
 * ExecutorStart hook:
 * 		set up flags to store runtime statistics,
 * 		push current query description in global stack
 */
static void
qs_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
		/* Enable per-node instrumentation */
		if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		{
			queryDesc->instrument_options |= INSTRUMENT_ROWS;
		}

		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);

}

/*
 * ExecutorRun:
 * 		Catch any fatal signals
 */
static void
qs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
			   bool execute_once)
{
	QueryDescStack = lcons(queryDesc, QueryDescStack);
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		QueryDescStack = list_delete_first(QueryDescStack);
	}
	PG_CATCH();
	{
		QueryDescStack = list_delete_first(QueryDescStack);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish:
 * 		Catch any fatal signals
 */
static void
qs_ExecutorFinish(QueryDesc *queryDesc)
{
	QueryDescStack = lcons(queryDesc, QueryDescStack);
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		QueryDescStack = list_delete_first(QueryDescStack);

	}
	PG_CATCH();
	{
		QueryDescStack = list_delete_first(QueryDescStack);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Find PgBackendStatus entry
 */
static PgBackendStatus *
search_be_status(int pid)
{
	int beid;

	if (pid <= 0)
		return NULL;

	for (beid = 1; beid <= pgstat_fetch_stat_numbackends(); beid++)
	{
		PgBackendStatus *be_status = pgstat_fetch_stat_beentry(beid);

		if (be_status && be_status->st_procpid == pid)
			return be_status;
	}

	return NULL;
}

/*
 * Init userlock
 */
// static void
// init_lock_tag(LOCKTAG *tag, uint32 key)
// {
// 	tag->locktag_field1 = PG_SQ_MODULE_KEY;
// 	tag->locktag_field2 = key;
// 	tag->locktag_field3 = 0;
// 	tag->locktag_field4 = 0;
// 	tag->locktag_type = LOCKTAG_USERLOCK;
// 	tag->locktag_lockmethodid = USER_LOCKMETHOD;
// }

/*
 * Structure of stack frame of fucntion call which transfers through message queue
 */
typedef struct
{
	text	*query;
} stack_frame;

/*
 * Structure of stack frame of fucntion call which resulted from analyze of query state
 */
typedef struct
{
	const char	*query;
} stack_frame_2;

/*
 *	Convert serialized stack frame into stack_frame record
 *		Increment '*src' pointer to the next serialized stack frame
 */
static stack_frame *
deserialize_stack_frame(char **src)
{
	stack_frame *result = palloc(sizeof(stack_frame));
	text		*query = (text *) *src;

	result->query = palloc(VARSIZE(query));
	memcpy(result->query, query, VARSIZE(query));

	*src = (char *) query + INTALIGN(VARSIZE(query));
	return result;
}

/*
 * Convert serialized stack frames into List of stack_frame records
 */
static List *
deserialize_stack(char *src, int stack_depth)
{
	List 	*result = NIL;
	char	*curr_ptr = src;
	int		i;

	for (i = 0; i < stack_depth; i++)
	{
		stack_frame	*frame = deserialize_stack_frame(&curr_ptr);
		//if (i == (stack_depth - 3))
		//{
			result = lappend(result, frame);
		//}
	}

	return result;
}

/*
 * Implementation of pg_self_query function
 */
PG_FUNCTION_INFO_V1(pg_self_query);
Datum
pg_self_query(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		PGPROC 		*proc;
		ListCell 	*frame_cursor;
		int			 frame_index;
		List		*stack;
	} proc_state;

	/* multicall context type */
	typedef struct
	{
		ListCell	*proc_cursor;
		List		*procs;
	} pg_qs_fctx;

	FuncCallContext	*funcctx;
	MemoryContext	oldcontext;
	pg_qs_fctx		*fctx;
#define		N_ATTRS  2
	pid_t			pid = MyProcPid;

	if (SRF_IS_FIRSTCALL())
	{
		//LOCKTAG			 tag;
		PGPROC			*proc;
		//Oid				 counterpart_user_id;
		shm_mq_msg		*msg;
		List			*bg_worker_procs = NIL;
		List			*msgs;
	
		// if (!module_initialized)
		// 	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		// 					errmsg("pg_self_query wasn't initialized yet")));
							
		proc = BackendPidGetProc(pid);

		if (!proc || proc->backendId == InvalidBackendId)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("backend with pid=%d not found", pid)));

		/*
		 * init and acquire lock so that any other concurrent calls of this fuction
		 * can not occupy shared queue for transfering query state
		 */
	
		//init_lock_tag(&tag, PG_SELF_QUERY_KEY);
		//LockAcquire(&tag, ExclusiveLock, false, false);

		//bg_worker_procs = GetRemoteBackendWorkers(proc);
		elog(INFO, "SRF_IS_FIRSTCALL 4");
		msgs = GetRemoteBackendQueryStates(proc,
										   bg_worker_procs);
		elog(INFO, "SRF_IS_FIRSTCALL 5");
		
		funcctx = SRF_FIRSTCALL_INIT();
		
		if (list_length(msgs) == 0)
		{
			elog(WARNING, "backend does not reply");
			//LockRelease(&tag, ExclusiveLock, false);
			SRF_RETURN_DONE(funcctx);
		}

		msg = (shm_mq_msg *) linitial(msgs);

		switch (msg->result_code)
		{
			case QUERY_NOT_RUNNING:
				{
					PgBackendStatus	*be_status = search_be_status(pid);

					if (be_status)
						elog(INFO, "state of backend is %s",
								be_state_str[be_status->st_state - STATE_UNDEFINED]);
					else
						elog(INFO, "backend is not running query");

					//LockRelease(&tag, ExclusiveLock, false);
					//elog(INFO, "SRF_IS_FIRSTCALL 8");
					SRF_RETURN_DONE(funcctx);
				}
			case STAT_DISABLED:
				elog(INFO, "query execution statistics disabled");
				//LockRelease(&tag, ExclusiveLock, false);
				//elog(INFO, "SRF_IS_FIRSTCALL 9");
				SRF_RETURN_DONE(funcctx);
			case QS_RETURNED:
				{
					TupleDesc	tupdesc;
					ListCell	*i;
					int64		max_calls = 0;

					/* print warnings if exist */
					if (msg->warnings & TIMINIG_OFF_WARNING)
						ereport(WARNING, (errcode(ERRCODE_WARNING),
										errmsg("timing statistics disabled")));
					if (msg->warnings & BUFFERS_OFF_WARNING)
						ereport(WARNING, (errcode(ERRCODE_WARNING),
										errmsg("buffers statistics disabled")));

					oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

					/* save stack of calls and current cursor in multicall context */
					fctx = (pg_qs_fctx *) palloc(sizeof(pg_qs_fctx));
					fctx->procs = NIL;
					foreach(i, msgs)
					{
						List 		*qs_stack;
						shm_mq_msg	*msg = (shm_mq_msg *) lfirst(i);
						proc_state	*p_state = (proc_state *) palloc(sizeof(proc_state));

						if (msg->result_code != QS_RETURNED)
							continue;

						AssertState(msg->result_code == QS_RETURNED);

						qs_stack = deserialize_stack(msg->stack, msg->stack_depth);

						p_state->proc = msg->proc;
						p_state->stack = qs_stack;
						p_state->frame_index = 0;
						p_state->frame_cursor = list_head(qs_stack);

						fctx->procs = lappend(fctx->procs, p_state);

						//elog(INFO, "max_calls-> %s", max_calls_str[max_calls]);
						max_calls += list_length(qs_stack);
						//elog(INFO, "max_calls-> %s", max_calls_str[max_calls]);
					}
					fctx->proc_cursor = list_head(fctx->procs);

					funcctx->user_fctx = fctx;
					funcctx->max_calls = max_calls;

					/* Make tuple descriptor */
					tupdesc = CreateTemplateTupleDesc(N_ATTRS, false);
					TupleDescInitEntry(tupdesc, (AttrNumber) 1, "frame_number", INT4OID, -1, 0);
					TupleDescInitEntry(tupdesc, (AttrNumber) 2, "query_text", TEXTOID, -1, 0);
					funcctx->tuple_desc = BlessTupleDesc(tupdesc);

					//LockRelease(&tag, ExclusiveLock, false);
					MemoryContextSwitchTo(oldcontext);
					//elog(INFO, "SRF_IS_FIRSTCALL 10");
				}
				break;
		}
		//elog(INFO, "SRF_IS_FIRSTCALL END");
	}

	/* restore function multicall context */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;
	//(INFO, "SRF_PERCALL_SETUP 1");
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		HeapTuple 	 tuple;
		Datum		 values[N_ATTRS];
		bool		 nulls[N_ATTRS];
		proc_state	*p_state = (proc_state *) lfirst(fctx->proc_cursor);
		stack_frame	*frame = (stack_frame *) lfirst(p_state->frame_cursor);

		/* Make and return next tuple to caller */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum(p_state->frame_index);
		values[1] = PointerGetDatum(frame->query);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		/* increment cursor */
		p_state->frame_cursor = lnext(p_state->frame_cursor);
		p_state->frame_index++;

		if (p_state->frame_cursor == NULL)
			fctx->proc_cursor = lnext(fctx->proc_cursor);
		//(INFO, "SRF_PERCALL_SETUP 2 NEXT");
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		//elog(INFO, "SRF_PERCALL_SETUP 2 DONE");
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Receive a message from a shared message queue until timeout is exceeded.
 *
 * Parameter `*nbytes` is set to the message length and *data to point to the
 * message payload. If timeout is exceeded SHM_MQ_WOULD_BLOCK is returned.
 */
// static shm_mq_result
// shm_mq_receive_with_timeout(shm_mq_handle *mqh,
// 							Size *nbytesp,
// 							void **datap,
// 							long timeout)
// {
// 	int 		rc = 0;
// 	long 		delay = timeout;

// 	for (;;)
// 	{
// 		instr_time	start_time;
// 		instr_time	cur_time;
// 		shm_mq_result mq_receive_result;

// 		INSTR_TIME_SET_CURRENT(start_time);

// 		mq_receive_result = shm_mq_receive(mqh, nbytesp, datap, true);
// 		if (mq_receive_result != SHM_MQ_WOULD_BLOCK)
// 			return mq_receive_result;
// 		if (rc & WL_TIMEOUT || delay <= 0)
// 			return SHM_MQ_WOULD_BLOCK;

// 		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT, delay,
// 					   PG_WAIT_EXTENSION);

// 		INSTR_TIME_SET_CURRENT(cur_time);
// 		INSTR_TIME_SUBTRACT(cur_time, start_time);

// 		delay = timeout - (long) INSTR_TIME_GET_MILLISEC(cur_time);
		
// 		CHECK_FOR_INTERRUPTS();
// 		ResetLatch(MyLatch);
// 	}
// }

/*
 * Extract to *result pids of all parallel workers running from leader process
 * that executes plan tree whose state root is `node`.
 */
// static bool
// extract_running_bgworkers(PlanState *node, List **result)
// {
// 	if (node == NULL)
// 		return false;

// 	if (IsA(node, GatherState))
// 	{
// 		GatherState *gather_node = (GatherState *) node;
// 		int 		i;

// 		if (gather_node->pei)
// 		{
// 			for (i = 0; i < gather_node->pei->pcxt->nworkers_launched; i++)
// 			{
// 				pid_t 					 pid;
// 				BackgroundWorkerHandle 	*bgwh;
// 				BgwHandleStatus 		 status;

// 				bgwh = gather_node->pei->pcxt->worker[i].bgwhandle;
// 				if (!bgwh)
// 					continue;

// 				status = GetBackgroundWorkerPid(bgwh, &pid);
// 				if (status == BGWH_STARTED)
// 					*result = lcons_int(pid, *result);
// 			}
// 		}
// 	}
// 	return planstate_tree_walker(node, extract_running_bgworkers, (void *) result);
// }

// typedef struct
// {
// 	int		number;
// 	pid_t	pids[FLEXIBLE_ARRAY_MEMBER];
// } BgWorkerPids;

// static void
// SendBgWorkerPids(void)
// {
// 	ListCell 		*iter;
// 	List 			*all_workers = NIL;
// 	BgWorkerPids 	*msg;
// 	int				 msg_len;
// 	int				 i;
// 	shm_mq_handle 	*mqh;

// 	mqh = shm_mq_attach(mq, NULL, NULL);

// 	foreach(iter, QueryDescStack)
// 	{
// 		QueryDesc	*curQueryDesc = (QueryDesc *) lfirst(iter);
// 		List 		*bgworker_pids = NIL;

// 		extract_running_bgworkers(curQueryDesc->planstate, &bgworker_pids);
// 		all_workers = list_concat(all_workers, bgworker_pids);
// 	}

// 	msg_len = offsetof(BgWorkerPids, pids)
// 			+ sizeof(pid_t) * list_length(all_workers);
// 	msg = palloc(msg_len);
// 	msg->number = list_length(all_workers);
// 	i = 0;
// 	foreach(iter, all_workers)
// 	{
// 		pid_t current_pid = lfirst_int(iter);

// 		AssertState(current_pid > 0);
// 		msg->pids[i++] = current_pid;
// 	}

// 	shm_mq_send(mqh, msg_len, msg, false);
// }

/*
 * Extracts all parallel worker `proc`s running by process `proc`
 */
// static List *
// GetRemoteBackendWorkers(PGPROC *proc)
// {
// 	int				 sig_result;
// 	shm_mq_handle	*mqh;
// 	shm_mq_result 	 mq_receive_result;
// 	BgWorkerPids	*msg;
// 	Size			 msg_len;
// 	int				 i;
// 	List			*result = NIL;

// 	Assert(proc && proc->backendId != InvalidBackendId);
// 	Assert(WorkerPollReason != INVALID_PROCSIGNAL);
// 	Assert(mq);

// 	mq = shm_mq_create(mq, QUEUE_SIZE);
// 	shm_mq_set_sender(mq, proc);
// 	shm_mq_set_receiver(mq, MyProc);

// 	sig_result = SendProcSignal(proc->pid, WorkerPollReason, proc->backendId);
// 	if (sig_result == -1)
// 		goto signal_error;

// 	mqh = shm_mq_attach(mq, NULL, NULL);
// 	mq_receive_result = shm_mq_receive(mqh, &msg_len, (void **) &msg, false);
// 	if (mq_receive_result != SHM_MQ_SUCCESS)
// 		goto mq_error;

// 	for (i = 0; i < msg->number; i++)
// 	{
// 		pid_t	pid = msg->pids[i];
// 		PGPROC *proc = BackendPidGetProc(pid);
// 		if (!proc || !proc->pid)
// 			continue;
// 		result = lcons(proc, result);
// 	}

// 	shm_mq_detach(mqh);

// 	return result;

// signal_error:
// 	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
// 				errmsg("invalid send signal")));
// mq_error:
// 	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
// 				errmsg("error in message queue data transmitting")));
// }

static shm_mq_msg *
copy_msg(shm_mq_msg *msg)
{
	shm_mq_msg *result = palloc(msg->length);

	memcpy(result, msg, msg->length);
	return result;
}

// static int 
// myNanoSleep(time_t sec, long nanosec)
// {
//    /* Setup timespec */
//    struct timespec req;
//    req.tv_sec = sec;
//    req.tv_nsec = nanosec;

//    /* Loop until we've slept long enough */
//    do 
//    {
//       /* Store remainder back on top of the original required time */
//       if( 0 != nanosleep( &req, &req ) )
//       {
//           /* If any error other than a signal interrupt occurs, return an error */
//           if(errno != EINTR)
//              return -1; 
//       }
//       else
//       {
//           /* nanosleep succeeded, so exit the loop */
//           break;
//       }
//    } while( req.tv_sec > 0 || req.tv_nsec > 0 );
//    return 0; /* Return success */
// }

/*		
  *	Get List of stack_frames as a stack of function calls starting from outermost call.		
  *		Each entry contains only query text.		
  *	Assume extension is enabled and QueryDescStack is not empty		
  */		
 static List *		
 runtime_explain()		
 {		
 	ListCell	    *i;		
 	List			*result = NIL;		

  	Assert(list_length(QueryDescStack) > 0);		

  	/* collect query state outputs of each plan entry of stack */		
 	foreach(i, QueryDescStack)		
 	{		
 		QueryDesc 	*currentQueryDesc = (QueryDesc *) lfirst(i);		
 		stack_frame_2	*qs_frame = palloc(sizeof(stack_frame_2));		

  		/* save query text */		
 		qs_frame->query = currentQueryDesc->sourceText;		

  		result = lcons(qs_frame, result);		
 	}		

  	return result;		
 }

 /*
 * Compute length of serialized stack frame
 */
static int
serialized_stack_frame_length(stack_frame_2 *qs_frame)
{
	return 	INTALIGN(strlen(qs_frame->query) + VARHDRSZ);
}

/*
 * Compute overall length of serialized stack of function calls
 */
static int
serialized_stack_length(List *qs_stack)
{
	ListCell 	*i;
	int			result = 0;

	foreach(i, qs_stack)
	{
		stack_frame_2 *qs_frame = (stack_frame_2 *) lfirst(i);
		result += serialized_stack_frame_length(qs_frame);
	}

	return result;
}

/*
 * Convert stack_frame_2 record into serialized text format version
 * 		Increment '*dest' pointer to the next serialized stack frame
 */
static void
serialize_stack_frame(char **dest, stack_frame_2 *qs_frame)
{
	SET_VARSIZE(*dest, strlen(qs_frame->query) + VARHDRSZ);
	memcpy(VARDATA(*dest), qs_frame->query, strlen(qs_frame->query));
	*dest += INTALIGN(VARSIZE(*dest));
}

/*
 * Convert List of stack_frame_2 records into serialized structures laid out sequentially
 */
static void
serialize_stack(char *dest, List *qs_stack)
{
	ListCell		*i;
	foreach(i, qs_stack)
	{
		stack_frame_2 *qs_frame = (stack_frame_2 *) lfirst(i);
		serialize_stack_frame(&dest, qs_frame);
	}
}

/*
 * Send state of current query to shared queue.
 * This function is called when fire custom signal QueryStatePollReason
 */
static shm_mq_msg *
GetQueryState()
{
	//shm_mq_handle 	*mqh;

	/* wait until caller sets this process as sender to message queue */
	//for (;;)
	//{
	//	if (shm_mq_get_sender(mq) == MyProc)
	//		break;
	//	WaitLatch(MyLatch, WL_LATCH_SET, 0, PG_WAIT_IPC);
	//	CHECK_FOR_INTERRUPTS();
	//	ResetLatch(MyLatch);
	//}

	//mqh = shm_mq_attach(mq, NULL, NULL);

	/* check if backend doesn't execute any query */
	//if (list_length(QueryDescStack) == 0)
	//{
	//	shm_mq_msg msg = { BASE_SIZEOF_SHM_MQ_MSG, MyProc, QUERY_NOT_RUNNING };
	//	shm_mq_send(mqh, msg.length, &msg, false);
	//}
	/* happy path */
	//else
	//{
		List			*qs_stack = runtime_explain();
		int				msglen = sizeof(shm_mq_msg) + serialized_stack_length(qs_stack);
		shm_mq_msg		*msg = palloc(msglen);

		msg->length = msglen;
		msg->proc = MyProc;
		msg->result_code = QS_RETURNED;
		msg->warnings = 0;	
		msg->stack_depth = list_length(qs_stack);
		serialize_stack(msg->stack, qs_stack);

		return msg;
		//shm_mq_send(mqh, msglen, msg, false);
	//}
}

static List *
GetRemoteBackendQueryStates(PGPROC *leader,
							List *pworkers)
{
	List			*result = NIL;
	//List			*alive_procs = NIL;
	//ListCell		*iter;
	//int		 		 sig_result;
	//shm_mq_handle  	*mqh;
	//shm_mq_result	 mq_receive_result;
	shm_mq_msg		*msg;
	//Size			 len;

	//Assert(QueryStatePollReason != INVALID_PROCSIGNAL);
	//Assert(mq);

	//pg_write_barrier();

	/* initialize message queue that will transfer query states */
	//mq = shm_mq_create(mq, QUEUE_SIZE);
	/*
	 * send signal `QueryStatePollReason` to all processes and define all alive
	 * 		ones
	 */
	msg = GetQueryState();
	//sig_result = SendProcSignal(leader->pid,
	//							QueryStatePollReason,
	//							leader->backendId);
	//elog(INFO, "GetRemoteBackendQueryStates 4");
	//if (sig_result == -1)
	//	goto signal_error;
	//foreach(iter, pworkers)
	//{
	//	PGPROC 	*proc = (PGPROC *) lfirst(iter);
	//	if (!proc || !proc->pid)
	//		continue;
	//	sig_result = SendProcSignal(proc->pid,
	//								QueryStatePollReason,
	//								proc->backendId);
	//	if (sig_result == -1)
	//	{
	//		if (errno != ESRCH)
	//			goto signal_error;
	//		continue;
	//	}
	//
	//		alive_procs = lappend(alive_procs, proc);
	//	}
	//elog(INFO, "GetRemoteBackendQueryStates 5");

	/* extract query state from leader process */
	//shm_mq_set_sender(mq, leader);
	//shm_mq_set_receiver(mq, MyProc);
	//mqh = shm_mq_attach(mq, NULL, NULL);
	//mq_receive_result = shm_mq_receive(mqh, &len, (void **) &msg, false);
	//if (mq_receive_result != SHM_MQ_SUCCESS)
	//	goto mq_error;
	//Assert(len == msg->length);
	result = lappend(result, copy_msg(msg));
	//shm_mq_detach(mqh);

	/*
	 * collect results from all alived parallel workers
	 */
	//foreach(iter, alive_procs)
	//{
	//	PGPROC 			*proc = (PGPROC *) lfirst(iter);
//
//		/* prepare message queue to transfer data */
//		mq = shm_mq_create(mq, QUEUE_SIZE);
//		shm_mq_set_sender(mq, proc);
//		shm_mq_set_receiver(mq, MyProc);	/* this function notifies the
//											   counterpart to come into data
//											   transfer */
//
//		/* retrieve result data from message queue */
//		mqh = shm_mq_attach(mq, NULL, NULL);
//		mq_receive_result = shm_mq_receive_with_timeout(mqh,
//														&len,
//														(void **) &msg,
//														MIN_TIMEOUT);
//		if (mq_receive_result != SHM_MQ_SUCCESS)
//			/* counterpart is died, not consider it */
//			continue;
//
//		Assert(len == msg->length);
//
//		/* aggregate result data */
//		result = lappend(result, copy_msg(msg));
//		shm_mq_detach(mqh);
//	}
	//elog(INFO, "GetRemoteBackendQueryStates 11");
	return result;

// signal_error:
// 	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
// 				errmsg("invalid send signal")));
// mq_error:
// 	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
// 				errmsg("error in message queue data transmitting")));
}