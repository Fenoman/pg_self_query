/*
 * signal_handler.c
 *		Collect current query state and send it to requestor in custom signal handler
 *
 * Copyright (c) 2016-2016, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/pg_self_query/signal_handler.c
 */

#include "pg_self_query.h"

#include "commands/explain.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/*
 * Structure of stack frame of fucntion call which resulted from analyze of query state
 */
typedef struct
{
	const char	*query;
} stack_frame;

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
 		stack_frame	*qs_frame = palloc(sizeof(stack_frame));		

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
serialized_stack_frame_length(stack_frame *qs_frame)
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
		stack_frame *qs_frame = (stack_frame *) lfirst(i);
		result += serialized_stack_frame_length(qs_frame);
	}

	return result;
}

/*
 * Convert stack_frame record into serialized text format version
 * 		Increment '*dest' pointer to the next serialized stack frame
 */
static void
serialize_stack_frame(char **dest, stack_frame *qs_frame)
{
	SET_VARSIZE(*dest, strlen(qs_frame->query) + VARHDRSZ);
	memcpy(VARDATA(*dest), qs_frame->query, strlen(qs_frame->query));
	*dest += INTALIGN(VARSIZE(*dest));
}

/*
 * Convert List of stack_frame records into serialized structures laid out sequentially
 */
static void
serialize_stack(char *dest, List *qs_stack)
{
	ListCell		*i;
	foreach(i, qs_stack)
	{
		stack_frame *qs_frame = (stack_frame *) lfirst(i);
		serialize_stack_frame(&dest, qs_frame);
	}
}

/*
 * Send state of current query to shared queue.
 * This function is called when fire custom signal QueryStatePollReason
 */
static shm_mq_msg *
GetQueryState(void)
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
