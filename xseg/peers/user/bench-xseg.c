/*
 * Copyright 2012 GRNET S.A. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 *   1. Redistributions of source code must retain the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer.
 *   2. Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY GRNET S.A. ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL GRNET S.A OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and
 * documentation are those of the authors and should not be
 * interpreted as representing official policies, either expressed
 * or implied, of GRNET S.A.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <pthread.h>
#include <xseg/xseg.h>
#include <peer.h>
#include <time.h>
#include <sys/util.h>
#include <signal.h>
#include <bench-xseg.h>
#include <limits.h>

char global_id[IDLEN];
uint64_t global_seed;

/*
 * This function checks two things:
 * a) If in-flight requests are less than given iodepth
 * b) If we have submitted al of the requests
 */
#define CAN_SEND_REQUEST(prefs)	\
	prefs->sub_tm->completed - prefs->rec_tm->completed < prefs->iodepth &&	\
	prefs->sub_tm->completed < prefs->max_requests	\

void custom_peer_usage()
{
	fprintf(stderr, "Custom peer options: \n"
			"  --------------------------------------------\n"
			"    -op       | None    | XSEG operation [read|write|info|delete]\n"
			"    --pattern | None    | I/O pattern [sync|rand]\n"
			"    -to       | None    | Total objects (not for read/write)\n"
			"    -ts       | None    | Total I/O size\n"
			"    -os       | 4M      | Object size\n"
			"    -bs       | 4k      | Block size\n"
			"    -dp       | None    | Destination port\n"
			"    --iodepth | 1       | Number of in-flight I/O requests\n"
			"    --verify  | no      | Verify written requests [no|meta|hash]\n"
			"    --seed    | None    | Inititalize LFSR and target names\n"
			"    --insanity| sane    | Adjust insanity level of benchmark:\n"
			"              |         |     [sane|eccentric|manic|paranoid]\n"
			"\n");
}

int custom_peer_init(struct peerd *peer, int argc, char *argv[])
{
	struct bench *prefs;
	char total_objects[MAX_ARG_LEN + 1];
	char total_size[MAX_ARG_LEN + 1];
	char object_size[MAX_ARG_LEN + 1];
	char block_size[MAX_ARG_LEN + 1];
	char op[MAX_ARG_LEN + 1];
	char pattern[MAX_ARG_LEN + 1];
	char insanity[MAX_ARG_LEN + 1];
	struct xseg *xseg = peer->xseg;
	unsigned int xseg_page_size = 1 << xseg->config.page_shift;
	long dst_port = -1;
	int r;

	op[0] = 0;
	pattern[0] = 0;
	total_objects[0] = 0;
	total_size[0] = 0;
	block_size[0] = 0;
	object_size[0] = 0;
	insanity[0] = 0;

#ifdef MT
	for (i = 0; i < nr_threads; i++) {
		prefs = peer->thread[i]->priv;
		prefs = malloc(sizeof(struct bench));
		if (!prefs) {
			perror("malloc");
			return -1;
		}
	}
#endif
	prefs = malloc(sizeof(struct bench));
	if (!prefs) {
		perror("malloc");
		return -1;
	}
	prefs->flags = 0;

	//Begin reading the benchmark-specific arguments
	BEGIN_READ_ARGS(argc, argv);
	READ_ARG_STRING("-op", op, MAX_ARG_LEN);
	READ_ARG_STRING("--pattern", pattern, MAX_ARG_LEN);
	READ_ARG_STRING("-to", total_objects, MAX_ARG_LEN);
	READ_ARG_STRING("-ts", total_size, MAX_ARG_LEN);
	READ_ARG_STRING("-os", object_size, MAX_ARG_LEN);
	READ_ARG_STRING("-bs", block_size, MAX_ARG_LEN);
	READ_ARG_ULONG("--iodepth", prefs->iodepth);
	READ_ARG_ULONG("-dp", dst_port);
	READ_ARG_STRING("--insanity", block_size, MAX_ARG_LEN);
	END_READ_ARGS();

	/*****************************\
	 * Check I/O type parameters *
	\*****************************/

	//We support 4 xseg operations: X_READ, X_WRITE, X_DELETE, X_INFO
	//The I/O pattern of thesee operations can be either synchronous (sync) or
	//random (rand)
	if (!op[0]) {
		XSEGLOG2(&lc, E, "xseg operation needs to be supplied\n");
		goto arg_fail;
	}
	r = read_op(op);
	if (r < 0) {
		XSEGLOG2(&lc, E, "Invalid syntax: -op %s\n", op);
		goto arg_fail;
	}
	prefs->op = r;

	if (!pattern[0]) {
		XSEGLOG2(&lc, E, "I/O pattern needs to be supplied\n");
		goto arg_fail;
	}
	r = read_pattern(pattern);
	if (r < 0) {
		XSEGLOG2(&lc, E, "Invalid syntax: --pattern %s\n", pattern);
		goto arg_fail;
	}
	prefs->flags |= (uint8_t)r << PATTERN_FLAG;

	/**************************\
	 * Check timer parameters *
	\**************************/

	//Most of the times, not all timers need to be used.
	//We can choose which timers will be used by adjusting the "insanity"
	//level of the benchmark i.e. the obscurity of code paths (get request,
	//submit request) that will be timed.
	if (!insanity[0])
		strcpy(insanity, "sane");

	prefs->insanity = read_insanity(insanity);
	if (prefs->insanity < 0) {
		XSEGLOG2(&lc, E, "Invalid syntax: --insanity %s\n", insanity);
		goto arg_fail;
	}

	/*
	 * If we have a request other than read/write, we don't need to check
	 * about size parameters, but only how many objects we want to affect
	 */
	if (prefs->op != X_READ && prefs->op != X_WRITE) {

		/***************************\
		 * Check object parameters *
		\***************************/

		if (!total_objects[0]) {
			XSEGLOG2(&lc, E,
					"Total number of objects needs to be supplied\n");
			goto arg_fail;
		}
		prefs->to = str2num(total_objects);
		if (!prefs->to) {
			XSEGLOG2(&lc, E, "Invalid syntax: -to %s\n", total_objects);
			goto arg_fail;
		}

		//In this case, the maximum number of requests is the total number of
		//objects we will handle
		prefs->max_requests = prefs->to;
	} else {

		/*************************\
		 * Check size parameters *
		\*************************/

		//Block size (bs): Defaults to 4K.
		//It must be a number followed by one of these characters:
		//						[k|K|m|M|g|G]
		//If not, it will be considered as size in bytes.
		//Must be integer multiple of segment's page size (typically 4k).
		if (!block_size[0])
			strcpy(block_size,"4k");

		if (!prefs->iodepth)
			prefs->iodepth = 1;

		prefs->bs = str2num(block_size);
		if (!prefs->bs) {
			XSEGLOG2(&lc, E, "Invalid syntax: -bs %s\n", block_size);
			goto arg_fail;
		} else if (prefs->bs % xseg_page_size) {
			XSEGLOG2(&lc, E, "Misaligned block size: %s\n", block_size);
			goto arg_fail;
		}

		//Total I/O size (ts): Must be supplied by user.
		//Must have the same format as "total size"
		//Must be integer multiple of "block size"
		if (!total_size[0]) {
			XSEGLOG2(&lc, E, "Total I/O size needs to be supplied\n");
			goto arg_fail;
		}

		prefs->ts = str2num(total_size);
		if (!prefs->ts) {
			XSEGLOG2(&lc, E, "Invalid syntax: -ts %s\n", total_size);
			goto arg_fail;
		} else if (prefs->ts % prefs->bs) {
			XSEGLOG2(&lc, E, "Misaligned total I/O size: %s\n", total_size);
			goto arg_fail;
		} else if (prefs->ts > xseg->segment_size) {
			XSEGLOG2(&lc, E,
					"Total I/O size exceeds segment size\n", total_size);
			goto arg_fail;
		}

		//Object size (os): Defaults to 4M.
		//Must have the same format as "total size"
		//Must be integer multiple of "block size"
		if (!object_size[0])
			strcpy(object_size,"4M");

		prefs->os = str2num(object_size);
		if (!prefs->os) {
			XSEGLOG2(&lc, E, "Invalid syntax: -os %s\n", object_size);
			goto arg_fail;
		} else if (prefs->os % prefs->bs) {
			XSEGLOG2(&lc, E, "Misaligned object size: %s\n", object_size);
			goto arg_fail;
		}

		//In this case, the maximum number of requests is the number of blocks
		//we need to cover the total I/O size
		prefs->max_requests = prefs->ts / prefs->bs;
	}

	/*************************
	 * Check port parameters *
	 *************************/

	if (dst_port < 0){
		XSEGLOG2(&lc, E, "Destination port needs to be supplied\n");
		goto arg_fail;
	}

	prefs->src_port = peer->portno_start; //TODO: allow user to change this
	prefs->dst_port = (xport) dst_port;

	/*********************************
	 * Create timers for all metrics *
	 *********************************/

	if (init_timer(&prefs->total_tm, TM_SANE))
		goto tm_fail;
	if (init_timer(&prefs->sub_tm, TM_MANIC))
		goto tm_fail;
	if (init_timer(&prefs->get_tm, TM_PARANOID))
		goto tm_fail;
	if (init_timer(&prefs->rec_tm, TM_ECCENTRIC))
		goto tm_fail;

	/********************************\
	 * Customize struct peerd/prefs *
	 \********************************/

	prefs->peer = peer;

	//The following function initializes the global_id, global_seed extern
	//variables.
	create_id();

	if ((prefs->flags & PATTERN_FLAG) == IO_RAND) {
		prefs->lfsr = malloc(sizeof(struct lfsr));
		if (!prefs->lfsr) {
			perror("malloc");
			goto lfsr_fail;
		}
		//FIXME: Give a name to max requests, not just prefs->ts / prefs->bs
		//FIXME: handle better the seed passing than just giving UINT64_MAX
		if (lfsr_init(prefs->lfsr, prefs->max_requests, UINT64_MAX)) {
			XSEGLOG2(&lc, E, "LFSR could not be initialized\n");
			goto lfsr_fail;
		}
	}

	peer->peerd_loop = custom_peerd_loop;
	peer->priv = (void *) prefs;
	return 0;

arg_fail:
	custom_peer_usage();
lfsr_fail:
	free(prefs->lfsr);
tm_fail:
	free(prefs->total_tm);
	free(prefs->sub_tm);
	free(prefs->get_tm);
	free(prefs->rec_tm);
	free(prefs);
	return -1;
}


static int send_request(struct peerd *peer, struct bench *prefs)
{
	struct xseg_request *req;
	struct xseg *xseg = peer->xseg;
	struct peer_req *pr;
	xport srcport = prefs->src_port;
	xport dstport = prefs->dst_port;
	xport p;

	int r;
	uint64_t new;
	uint64_t size = prefs->bs;

	//srcport and dstport must already be provided by the user.
	//returns struct xseg_request with basic initializations
	XSEGLOG2(&lc, D, "Get new request\n");
	timer_start(prefs, prefs->get_tm);
	req = xseg_get_request(xseg, srcport, dstport, X_ALLOC);
	if (!req) {
		XSEGLOG2(&lc, W, "Cannot get request\n");
		return -1;
	}
	timer_stop(prefs, prefs->get_tm, NULL);

	//Allocate enough space for the data and the target's name
	XSEGLOG2(&lc, D, "Prepare new request\n");
	r = xseg_prep_request(xseg, req, TARGETLEN, size);
	if (r < 0) {
		XSEGLOG2(&lc, W, "Cannot prepare request! (%lu, %llu)\n",
				TARGETLEN, (unsigned long long)size);
		goto put_xseg_request;
	}

	//Determine what the next target/chunk will be, based on I/O pattern
	new = determine_next(prefs);
	//Create a target of this format: "bench-<obj_no>"
	create_target(prefs, req, new);

	if(prefs->op == X_WRITE || prefs->op == X_READ) {
		req->size = size;
		//Calculate the chunk offset inside the object
		req->offset = (new * prefs->bs) % prefs->os;
		XSEGLOG2(&lc, D, "Offset of request %lu is %lu\n", new, req->offset);

		if(prefs->op == X_WRITE)
			create_chunk(prefs, req, new);
	}

	req->op = prefs->op;

	//Measure this?
	XSEGLOG2(&lc, D, "Allocate peer request\n");
	pr = alloc_peer_req(peer);
	if (!pr) {
		XSEGLOG2(&lc, W, "Cannot allocate peer request (%ld remaining)\n",
				peer->nr_ops - xq_count(&peer->free_reqs));
		goto put_xseg_request;
	}
	pr->peer = peer;
	pr->portno = srcport;
	pr->req = req;
	pr->priv = malloc(sizeof(struct timespec));
	if(!pr->priv) {
		perror("malloc");
		goto put_peer_request;
	}

	XSEGLOG2(&lc, D, "Set request data\n");
	r = xseg_set_req_data(xseg, req, pr);
	if (r < 0) {
		XSEGLOG2(&lc, W, "Cannot set request data\n");
		goto put_peer_request;
	}

	/*
	 * Start measuring receive time.
	 * When we receive a request, we need to have its submission time to
	 * measure elapsed time. Thus, we memcpy its submission time to pr->priv.
	 * QUESTION: Is this the fastest way?
	 */
	timer_start(prefs, prefs->rec_tm);
	memcpy(pr->priv, &prefs->rec_tm->start_time, sizeof(struct timespec));

	//Submit the request from the source port to the target port
	XSEGLOG2(&lc, D, "Submit request %lu\n", new);
	//QUESTION: Can't we just use the submision time calculated previously?
	timer_start(prefs, prefs->sub_tm);
	p = xseg_submit(xseg, req, srcport, X_ALLOC);
	if (p == NoPort) {
		XSEGLOG2(&lc, W, "Cannot submit request\n");
		goto put_peer_request;
	}
	timer_stop(prefs, prefs->sub_tm, NULL);

	//Send SIGIO to the process that has binded this port to inform that
	//IO is possible
	xseg_signal(xseg, p);

	return 0;

put_peer_request:
	free(pr->priv);
	free_peer_req(peer, pr);
put_xseg_request:
	if (xseg_put_request(xseg, req, srcport))
		XSEGLOG2(&lc, W, "Cannot put request\n");
	return -1;
}

/*
 * This function substitutes the default generic_peerd_loop of peer.c.
 * It's plugged to struct peerd at custom peer's initialisation
 */
int custom_peerd_loop(void *arg)
{
#ifdef MT
	struct thread *t = (struct thread *) arg;
	struct peerd *peer = t->peer;
	char *id = t->arg;
#else
	struct peerd *peer = (struct peerd *) arg;
	char id[4] = {'P','e','e','r'};
#endif
	struct xseg *xseg = peer->xseg;
	struct bench *prefs = peer->priv;
	xport portno_start = peer->portno_start;
	xport portno_end = peer->portno_end;
	uint64_t threshold=1000/(1 + portno_end - portno_start);
	pid_t pid =syscall(SYS_gettid);
	int r;


	XSEGLOG2(&lc, I, "%s has tid %u.\n",id, pid);
	xseg_init_local_signal(xseg, peer->portno_start);
	uint64_t loops;

	timer_start(prefs, prefs->total_tm);
	while (!isTerminate()) {
#ifdef MT
		if (t->func) {
			XSEGLOG2(&lc, D, "%s executes function\n", id);
			xseg_cancel_wait(xseg, peer->portno_start);
			t->func(t->arg);
			t->func = NULL;
			t->arg = NULL;
			continue;
		}
#endif
send_request:
		while (CAN_SEND_REQUEST(prefs)) {
			XSEGLOG2(&lc, D, "Because %lu < %lu && %lu < %lu\n",
					prefs->sub_tm->completed - prefs->rec_tm->completed,
					prefs->iodepth, prefs->sub_tm->completed,
					prefs->max_requests);
			xseg_cancel_wait(xseg, peer->portno_start);
			XSEGLOG2(&lc, D, "Start sending new request\n");
			r = send_request(peer, prefs);
			if (r < 0)
				break;
		}
		//Heart of peerd_loop. This loop is common for everyone.
		for (loops = threshold; loops > 0; loops--) {
			if (check_ports(peer)) {
				if (prefs->max_requests == prefs->rec_tm->completed)
					return 0;
				else
					//If an old request has just been acked, the most sensible
					//thing to do is to immediately send a new one
					goto send_request;
			}
		}
		XSEGLOG2(&lc, I, "%s goes to sleep\n",id);
		xseg_prepare_wait(xseg, peer->portno_start);
#ifdef ST_THREADS
		if (ta){
			st_sleep(0);
			continue;
		}
#endif
		xseg_wait_signal(xseg, 10000000UL);
		xseg_cancel_wait(xseg, peer->portno_start);
		XSEGLOG2(&lc, I, "%s woke up\n", id);
	}

	XSEGLOG2(&lc, I, "peer->free_reqs = %d, peer->nr_ops = %d\n",
			xq_count(&peer->free_reqs), peer->nr_ops);
	return 0;
}

void custom_peer_finalize(struct peerd *peer)
{
	struct bench *prefs = peer->priv;
	//TODO: Measure mean time, standard variation
	struct tm_result total; //mean, std;

	if (!prefs->total_tm->completed)
		timer_stop(prefs, prefs->total_tm, NULL);

	separate_by_order(prefs->total_tm->sum, &total);
	print_res(total, "Total Time");
	return;
}


static void handle_received(struct peerd *peer, struct peer_req *pr)
{
	//FIXME: handle null pointer
	struct bench *prefs = peer->priv;
	struct timer *rec = prefs->rec_tm;

	if (!pr->req) {
		//This is a serious error, so we must stop
		XSEGLOG2(&lc, E, "Received peer request with no xseg request");
		terminated++;
		return;
	}

	if (!pr->priv) {
		XSEGLOG2(&lc, W, "Cannot find submission time of request");
		return;
	}

	timer_stop(prefs, rec, pr->priv);

	if (xseg_put_request(peer->xseg, pr->req, pr->portno))
		XSEGLOG2(&lc, W, "Cannot put xseg request\n");

	//QUESTION, can't we just keep the malloced memory for future use?
	free(pr->priv);
	free_peer_req(peer, pr);
}

int dispatch(struct peerd *peer, struct peer_req *pr, struct xseg_request *req,
		enum dispatch_reason reason)
{
	switch (reason) {
		case dispatch_accept:
			//This is wrong, benchmarking peer should not accept requests,
			//only receive them.
			XSEGLOG2(&lc, W, "Bench peer should not accept requests\n");
			complete(peer, pr);
			break;
		case dispatch_receive:
			handle_received(peer, pr);
			break;
		default:
			fail(peer, pr);
	}
	return 0;
}
