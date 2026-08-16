#include <pjlib.h>

#include <string.h>

#include <zephyr/sys/printk.h>

#define LOOPBACK_ADDRESS "127.0.0.1"
#define POLL_THREAD_STACK 8192

struct concurrency_tracker {
	pj_mutex_t *mutex;
	volatile int active;
	volatile int maximum;
	volatile int total;
};

struct callback_state {
	volatile int reads;
	volatile int writes;
	volatile int accepts;
	volatile int connects;
	volatile pj_ssize_t last_read;
	volatile pj_ssize_t last_write;
	volatile pj_status_t last_status;
	pj_sock_t accepted;
	struct concurrency_tracker *tracker;
};

struct poll_worker_state {
	pj_ioqueue_t *ioqueue;
	volatile int started;
	volatile int stop;
	int result;
	int errors;
};

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Stage 10] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Stage 10] FAIL %s:%d condition=%s\n", test, line, condition);
	return -1;
}

#define CHECK_STATUS(test_name, expression)                                  \
	do {                                                                    \
		pj_status_t check_status_ = (expression);                          \
		if (check_status_ != PJ_SUCCESS)                                   \
			return fail_status((test_name), __LINE__, check_status_);     \
	} while (0)

#define CHECK_TRUE(test_name, condition)                                     \
	do {                                                                    \
		if (!(condition))                                                 \
			return fail_value((test_name), __LINE__, #condition);        \
	} while (0)

static int make_loopback_address(pj_sockaddr_in *address, pj_uint16_t port)
{
	pj_str_t host;

	pj_cstr(&host, LOOPBACK_ADDRESS);
	return pj_sockaddr_in_init(address, &host, port);
}

static pj_status_t create_bound_udp(pj_sock_t *sock, pj_sockaddr_in *address)
{
	int address_length;
	pj_status_t status;

	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, sock);
	if (status != PJ_SUCCESS)
		return status;
	status = make_loopback_address(address, 0);
	if (status != PJ_SUCCESS)
		return status;
	status = pj_sock_bind(*sock, (pj_sockaddr_t *)address, sizeof(*address));
	if (status != PJ_SUCCESS)
		return status;
	address_length = sizeof(*address);
	return pj_sock_getsockname(*sock, (pj_sockaddr_t *)address,
				   &address_length);
}

static void track_callback_begin(struct concurrency_tracker *tracker)
{
	if (tracker == NULL)
		return;
	pj_mutex_lock(tracker->mutex);
	++tracker->active;
	++tracker->total;
	if (tracker->active > tracker->maximum)
		tracker->maximum = tracker->active;
	pj_mutex_unlock(tracker->mutex);
	pj_thread_sleep(100);
}

static void track_callback_end(struct concurrency_tracker *tracker)
{
	if (tracker == NULL)
		return;
	pj_mutex_lock(tracker->mutex);
	--tracker->active;
	pj_mutex_unlock(tracker->mutex);
}

static void on_read_complete(pj_ioqueue_key_t *key,
			     pj_ioqueue_op_key_t *op_key,
			     pj_ssize_t bytes_read)
{
	struct callback_state *state = pj_ioqueue_get_user_data(key);

	PJ_UNUSED_ARG(op_key);
	track_callback_begin(state->tracker);
	state->last_read = bytes_read;
	++state->reads;
	track_callback_end(state->tracker);
}

static void on_write_complete(pj_ioqueue_key_t *key,
			      pj_ioqueue_op_key_t *op_key,
			      pj_ssize_t bytes_written)
{
	struct callback_state *state = pj_ioqueue_get_user_data(key);

	PJ_UNUSED_ARG(op_key);
	state->last_write = bytes_written;
	++state->writes;
}

static void on_accept_complete(pj_ioqueue_key_t *key,
			       pj_ioqueue_op_key_t *op_key,
			       pj_sock_t sock, pj_status_t status)
{
	struct callback_state *state = pj_ioqueue_get_user_data(key);

	PJ_UNUSED_ARG(op_key);
	state->accepted = sock;
	state->last_status = status;
	++state->accepts;
}

static void on_connect_complete(pj_ioqueue_key_t *key, pj_status_t status)
{
	struct callback_state *state = pj_ioqueue_get_user_data(key);

	state->last_status = status;
	++state->connects;
}

static const pj_ioqueue_callback callbacks = {
	&on_read_complete,
	&on_write_complete,
	&on_accept_complete,
	&on_connect_complete,
};

static int poll_worker(void *arg)
{
	struct poll_worker_state *state = arg;
	pj_time_val timeout = {0, 100};

	state->started = 1;
	do {
		int result = pj_ioqueue_poll(state->ioqueue, &timeout);

		state->result = result;
		if (result < 0)
			++state->errors;
	} while (!state->stop);
	return 0;
}

static int join_worker(const char *test, pj_thread_t *thread)
{
	CHECK_STATUS(test, pj_thread_join(thread));
	CHECK_STATUS(test, pj_thread_destroy(thread));
	return 0;
}

static int test_backend_configuration(void)
{
	const char *test = "backend configuration";
	pj_ioqueue_cfg config;

	pj_ioqueue_cfg_default(&config);
	CHECK_TRUE(test, PJ_IOQUEUE_IMP == PJ_IOQUEUE_IMP_SELECT);
	CHECK_TRUE(test, PJ_IOQUEUE_HAS_SAFE_UNREG == 1);
	CHECK_TRUE(test, PJ_IOQUEUE_MAX_HANDLES == 32);
	CHECK_TRUE(test, config.default_concurrency ==
		   PJ_IOQUEUE_DEFAULT_ALLOW_CONCURRENCY);
	printk("[Stage 10] backend=%s max_handles=%d safe_unreg=%d default_concurrency=%d\n",
	       pj_ioqueue_name(), PJ_IOQUEUE_MAX_HANDLES,
	       PJ_IOQUEUE_HAS_SAFE_UNREG, config.default_concurrency);
	return 0;
}

static int test_udp_read_write_timeout(pj_pool_t *pool)
{
	const char *test = "UDP readiness/multiple sockets/timeout";
	pj_ioqueue_t *ioqueue;
	pj_ioqueue_key_t *receiver_key[2];
	pj_ioqueue_key_t *sender_key;
	pj_sock_t receiver[2];
	pj_sock_t sender;
	pj_sockaddr_in address[2];
	pj_sockaddr_in source[2];
	struct callback_state receiver_state[2] = {{0}, {0}};
	struct callback_state sender_state = {0};
	pj_ioqueue_op_key_t read_op[2];
	pj_ioqueue_op_key_t write_op[2];
	char receive_buffer[2][32] = {{0}, {0}};
	const char payload[2][16] = {"stage10-one", "stage10-two"};
	pj_ssize_t length;
	int address_length[2];
	pj_time_val timeout;
	pj_timestamp start;
	pj_timestamp end;
	unsigned elapsed;
	int immediate_writes = 0;
	int pending_writes = 0;
	int i;

	CHECK_STATUS(test, pj_ioqueue_create(pool, 3, &ioqueue));
	for (i = 0; i < 2; ++i) {
		CHECK_STATUS(test, create_bound_udp(&receiver[i], &address[i]));
		CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, receiver[i],
						       &receiver_state[i], &callbacks,
						       &receiver_key[i]));
		CHECK_TRUE(test, pj_ioqueue_get_user_data(receiver_key[i]) ==
			   &receiver_state[i]);
		pj_ioqueue_op_key_init(&read_op[i], sizeof(read_op[i]));
		address_length[i] = sizeof(source[i]);
		length = sizeof(receive_buffer[i]);
		CHECK_TRUE(test, pj_ioqueue_recvfrom(receiver_key[i], &read_op[i],
						 receive_buffer[i], &length,
						 PJ_IOQUEUE_ALWAYS_ASYNC,
						 (pj_sockaddr_t *)&source[i],
						 &address_length[i]) == PJ_EPENDING);
	}
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &sender));
	CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, sender,
					   &sender_state, &callbacks, &sender_key));
	CHECK_STATUS(test, pj_ioqueue_set_concurrency(sender_key, PJ_FALSE));
	CHECK_STATUS(test, pj_ioqueue_set_concurrency(sender_key, PJ_TRUE));
	for (i = 0; i < 2; ++i) {
		pj_status_t send_status;

		pj_ioqueue_op_key_init(&write_op[i], sizeof(write_op[i]));
		length = strlen(payload[i]);
		send_status = pj_ioqueue_sendto(sender_key, &write_op[i], payload[i],
						&length, PJ_IOQUEUE_ALWAYS_ASYNC,
						(pj_sockaddr_t *)&address[i],
						sizeof(address[i]));
		CHECK_TRUE(test, send_status == PJ_SUCCESS || send_status == PJ_EPENDING);
		if (send_status == PJ_SUCCESS)
			++immediate_writes;
		else
			++pending_writes;
	}
	for (i = 0; i < 20 && (receiver_state[0].reads != 1 ||
		 receiver_state[1].reads != 1 ||
		 sender_state.writes != pending_writes); ++i) {
		timeout.sec = 0;
		timeout.msec = 100;
		CHECK_TRUE(test, pj_ioqueue_poll(ioqueue, &timeout) >= 0);
	}
	CHECK_TRUE(test, receiver_state[0].reads == 1);
	CHECK_TRUE(test, receiver_state[1].reads == 1);
	CHECK_TRUE(test, sender_state.writes == pending_writes);
	CHECK_TRUE(test, immediate_writes + pending_writes == 2);
	for (i = 0; i < 2; ++i) {
		CHECK_TRUE(test, receiver_state[i].last_read ==
			   (pj_ssize_t)strlen(payload[i]));
		CHECK_TRUE(test, memcmp(receive_buffer[i], payload[i],
					 strlen(payload[i])) == 0);
	}
	pj_get_timestamp(&start);
	timeout.sec = 0;
	timeout.msec = 40;
	CHECK_TRUE(test, pj_ioqueue_poll(ioqueue, &timeout) == 0);
	pj_get_timestamp(&end);
	elapsed = pj_elapsed_msec(&start, &end);
	CHECK_TRUE(test, elapsed >= 25);
	CHECK_STATUS(test, pj_ioqueue_unregister(sender_key));
	CHECK_STATUS(test, pj_ioqueue_unregister(receiver_key[1]));
	CHECK_STATUS(test, pj_ioqueue_unregister(receiver_key[0]));
	CHECK_STATUS(test, pj_ioqueue_destroy(ioqueue));
	printk("[Stage 10] readable readiness, two sockets, write completion (fast=%d queued=%d), timeout (%u ms): PASSED\n",
	       immediate_writes, pending_writes, elapsed);
	return 0;
}

static int test_tcp_connect(pj_pool_t *pool)
{
	const char *test = "TCP connect completion";
	pj_ioqueue_t *ioqueue;
	pj_ioqueue_key_t *listener_key;
	pj_ioqueue_key_t *client_key;
	pj_sock_t listener;
	pj_sock_t client;
	pj_sock_t accepted = PJ_INVALID_SOCKET;
	pj_sockaddr_in address;
	pj_sockaddr_in local;
	pj_sockaddr_in remote;
	struct callback_state listener_state = {.accepted = PJ_INVALID_SOCKET};
	struct callback_state client_state = {0};
	pj_ioqueue_op_key_t accept_op;
	pj_status_t accept_status;
	pj_status_t connect_status;
	pj_time_val timeout;
	int address_length;
	int accept_address_length;
	int i;

	CHECK_STATUS(test, pj_ioqueue_create(pool, 2, &ioqueue));
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0,
					 &listener));
	CHECK_STATUS(test, make_loopback_address(&address, 0));
	CHECK_STATUS(test, pj_sock_bind(listener, (pj_sockaddr_t *)&address,
					 sizeof(address)));
	address_length = sizeof(address);
	CHECK_STATUS(test, pj_sock_getsockname(listener, (pj_sockaddr_t *)&address,
						&address_length));
	CHECK_STATUS(test, pj_sock_listen(listener, 1));
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0,
					 &client));
	CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, listener,
					   &listener_state, &callbacks,
					   &listener_key));
	CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, client,
					   &client_state, &callbacks, &client_key));
	pj_ioqueue_op_key_init(&accept_op, sizeof(accept_op));
	accept_address_length = sizeof(local);
	accept_status = pj_ioqueue_accept(listener_key, &accept_op, &accepted,
					  (pj_sockaddr_t *)&local,
					  (pj_sockaddr_t *)&remote,
					  &accept_address_length);
	CHECK_TRUE(test, accept_status == PJ_SUCCESS || accept_status == PJ_EPENDING);
	connect_status = pj_ioqueue_connect(client_key, (pj_sockaddr_t *)&address,
					    sizeof(address));
	CHECK_TRUE(test, connect_status == PJ_SUCCESS || connect_status == PJ_EPENDING);
	if (connect_status == PJ_SUCCESS) {
		client_state.last_status = PJ_SUCCESS;
		client_state.connects = 1;
	}
	for (i = 0; i < 20 &&
		 (client_state.connects != 1 ||
		  (accept_status == PJ_EPENDING && listener_state.accepts != 1)); ++i) {
		timeout.sec = 0;
		timeout.msec = 100;
		CHECK_TRUE(test, pj_ioqueue_poll(ioqueue, &timeout) >= 0);
	}
	if (accept_status == PJ_EPENDING)
		accepted = listener_state.accepted;
	CHECK_TRUE(test, client_state.connects == 1);
	CHECK_TRUE(test, client_state.last_status == PJ_SUCCESS);
	CHECK_TRUE(test, accepted != PJ_INVALID_SOCKET);
	if (accept_status == PJ_EPENDING)
		CHECK_TRUE(test, listener_state.last_status == PJ_SUCCESS);
	CHECK_STATUS(test, pj_sock_close(accepted));
	CHECK_STATUS(test, pj_ioqueue_unregister(client_key));
	CHECK_STATUS(test, pj_ioqueue_unregister(listener_key));
	CHECK_STATUS(test, pj_ioqueue_destroy(ioqueue));
	printk("[Stage 10] asynchronous connect and accept completion: PASSED\n");
	return 0;
}

static int test_blocked_poll_unregister(pj_pool_t *pool)
{
	const char *test = "blocked poll safe unregister";
	pj_ioqueue_t *ioqueue;
	pj_ioqueue_key_t *target_key;
	pj_ioqueue_key_t *guard_key;
	pj_sock_t target;
	pj_sock_t guard;
	pj_sock_t sender;
	pj_sockaddr_in target_address;
	pj_sockaddr_in guard_address;
	struct callback_state target_state = {0};
	struct callback_state guard_state = {0};
	struct poll_worker_state worker = {0};
	pj_ioqueue_op_key_t target_op;
	pj_ioqueue_op_key_t guard_op;
	pj_thread_t *thread;
	pj_time_val timeout;
	pj_ssize_t length;
	char target_buffer[16];
	char guard_buffer[16];
	const char payload[] = "wake";
	int i;

	CHECK_STATUS(test, pj_ioqueue_create(pool, 2, &ioqueue));
	CHECK_STATUS(test, create_bound_udp(&target, &target_address));
	CHECK_STATUS(test, create_bound_udp(&guard, &guard_address));
	CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, target,
					   &target_state, &callbacks, &target_key));
	CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, guard,
					   &guard_state, &callbacks, &guard_key));
	pj_ioqueue_op_key_init(&target_op, sizeof(target_op));
	pj_ioqueue_op_key_init(&guard_op, sizeof(guard_op));
	length = sizeof(target_buffer);
	CHECK_TRUE(test, pj_ioqueue_recv(target_key, &target_op, target_buffer,
					&length, PJ_IOQUEUE_ALWAYS_ASYNC) == PJ_EPENDING);
	length = sizeof(guard_buffer);
	CHECK_TRUE(test, pj_ioqueue_recv(guard_key, &guard_op, guard_buffer,
					&length, PJ_IOQUEUE_ALWAYS_ASYNC) == PJ_EPENDING);
	worker.ioqueue = ioqueue;
	CHECK_STATUS(test, pj_thread_create(pool, "stage10-unreg", poll_worker,
					   &worker, POLL_THREAD_STACK, 0, &thread));
	for (i = 0; i < 100 && !worker.started; ++i)
		pj_thread_sleep(1);
	CHECK_TRUE(test, worker.started);
	pj_thread_sleep(30);
	CHECK_STATUS(test, pj_ioqueue_unregister(target_key));
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &sender));
	length = sizeof(payload) - 1;
	CHECK_STATUS(test, pj_sock_sendto(sender, payload, &length, 0,
					 (pj_sockaddr_t *)&guard_address,
					 sizeof(guard_address)));
	for (i = 0; i < 100 && guard_state.reads == 0; ++i)
		pj_thread_sleep(5);
	worker.stop = 1;
	if (join_worker(test, thread) != 0)
		return -1;
	if (guard_state.reads == 0) {
		timeout.sec = 0;
		timeout.msec = 100;
		CHECK_TRUE(test, pj_ioqueue_poll(ioqueue, &timeout) >= 0);
	}
	CHECK_TRUE(test, target_state.reads == 0);
	CHECK_TRUE(test, guard_state.reads == 1);
	CHECK_TRUE(test, guard_state.last_read == (pj_ssize_t)(sizeof(payload) - 1));
	CHECK_STATUS(test, pj_sock_close(sender));
	CHECK_STATUS(test, pj_ioqueue_unregister(guard_key));
	CHECK_STATUS(test, pj_ioqueue_destroy(ioqueue));
	printk("[Stage 10] unregister/close while select blocked, no stale callback (poll errors=%d): PASSED\n",
	       worker.errors);
	return 0;
}

static int test_callback_concurrency(pj_pool_t *pool)
{
	const char *test = "callback concurrency";
	pj_ioqueue_t *ioqueue;
	pj_ioqueue_key_t *keys[2];
	pj_sock_t receivers[2];
	pj_sock_t sender;
	pj_sockaddr_in addresses[2];
	struct concurrency_tracker tracker = {0};
	struct callback_state states[2] = {{.tracker = &tracker},
					   {.tracker = &tracker}};
	struct poll_worker_state workers[2] = {{0}, {0}};
	pj_ioqueue_op_key_t operations[2];
	pj_thread_t *threads[2];
	char buffers[2][16];
	const char payload[] = "parallel";
	pj_ssize_t length;
	pj_status_t send_status = PJ_SUCCESS;
	int send_failure_index = -1;
	int i;

	CHECK_STATUS(test, pj_mutex_create_simple(pool, "stage10-track",
						 &tracker.mutex));
	CHECK_STATUS(test, pj_ioqueue_create(pool, 2, &ioqueue));
	CHECK_STATUS(test, pj_ioqueue_set_default_concurrency(ioqueue, PJ_FALSE));
	CHECK_STATUS(test, pj_ioqueue_set_default_concurrency(ioqueue, PJ_TRUE));
	for (i = 0; i < 2; ++i) {
		CHECK_STATUS(test, create_bound_udp(&receivers[i], &addresses[i]));
		CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, receivers[i],
						       &states[i], &callbacks, &keys[i]));
		pj_ioqueue_op_key_init(&operations[i], sizeof(operations[i]));
		length = sizeof(buffers[i]);
		CHECK_TRUE(test, pj_ioqueue_recv(keys[i], &operations[i], buffers[i],
						&length, PJ_IOQUEUE_ALWAYS_ASYNC) ==
			   PJ_EPENDING);
	}
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &sender));
	for (i = 0; i < 2; ++i) {
		length = sizeof(payload) - 1;
		send_status = pj_sock_sendto(sender, payload, &length, 0,
					    (pj_sockaddr_t *)&addresses[i],
					    sizeof(addresses[i]));
		if (send_status != PJ_SUCCESS) {
			send_failure_index = i;
			break;
		}
	}
	if (send_status == PJ_SUCCESS) {
		for (i = 0; i < 2; ++i) {
			workers[i].ioqueue = ioqueue;
			CHECK_STATUS(test, pj_thread_create(pool, "stage10-poll", poll_worker,
							   &workers[i], POLL_THREAD_STACK, 0,
							   &threads[i]));
		}
		for (i = 0; i < 200 &&
			 (!workers[0].started || !workers[1].started); ++i)
			pj_thread_sleep(1);
		CHECK_TRUE(test, workers[0].started && workers[1].started);
	}
	for (i = 0; i < 400 && tracker.total < 2; ++i)
		pj_thread_sleep(5);
	if (send_status == PJ_SUCCESS) {
		for (i = 0; i < 2; ++i)
			workers[i].stop = 1;
		for (i = 0; i < 2; ++i) {
			if (join_worker(test, threads[i]) != 0)
				return -1;
			CHECK_TRUE(test, workers[i].errors == 0);
		}
	}
	if (send_status != PJ_SUCCESS) {
		printk("[Stage 10] concurrency send failed at index=%d\n",
		       send_failure_index);
		pj_sock_close(sender);
		for (i = 0; i < 2; ++i)
			pj_ioqueue_unregister(keys[i]);
		pj_ioqueue_destroy(ioqueue);
		pj_mutex_destroy(tracker.mutex);
		return fail_status(test, __LINE__, send_status);
	}
	CHECK_TRUE(test, tracker.total == 2);
	CHECK_TRUE(test, tracker.maximum >= 2);
	CHECK_STATUS(test, pj_sock_close(sender));
	for (i = 0; i < 2; ++i)
		CHECK_STATUS(test, pj_ioqueue_unregister(keys[i]));
	CHECK_STATUS(test, pj_ioqueue_destroy(ioqueue));
	CHECK_STATUS(test, pj_mutex_destroy(tracker.mutex));
	printk("[Stage 10] two polling threads, concurrent callbacks (max=%d): PASSED\n",
	       tracker.maximum);
	return 0;
}

static int test_repeated_safe_registration(pj_pool_t *pool)
{
	const char *test = "repeated safe registration";
	pj_ioqueue_t *ioqueue;
	pj_ioqueue_key_t *key;
	struct callback_state state = {0};
	pj_sock_t sock;
	int i;

	CHECK_STATUS(test, pj_ioqueue_create(pool, 1, &ioqueue));
	for (i = 0; i < 3; ++i) {
		CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
						 &sock));
		CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, sock, &state,
						       &callbacks, &key));
		CHECK_STATUS(test, pj_ioqueue_unregister(key));
		/* A stale/double unregister must be harmless during quarantine. */
		CHECK_STATUS(test, pj_ioqueue_unregister(key));
		pj_thread_sleep(PJ_IOQUEUE_KEY_FREE_DELAY + 100);
	}
	CHECK_STATUS(test, pj_ioqueue_destroy(ioqueue));
	printk("[Stage 10] repeated register/unregister and %d ms safe-key delay: PASSED\n",
	       PJ_IOQUEUE_KEY_FREE_DELAY);
	return 0;
}

static int test_maximum_handles(pj_pool_t *pool)
{
	const char *test = "configured maximum handles";
	pj_ioqueue_t *ioqueue;
	pj_ioqueue_key_t *keys[PJ_IOQUEUE_MAX_HANDLES];
	pj_sock_t sockets[PJ_IOQUEUE_MAX_HANDLES];
	struct callback_state states[PJ_IOQUEUE_MAX_HANDLES];
	pj_sock_t overflow;
	pj_ioqueue_key_t *overflow_key;
	pj_status_t status;
	int i;

	memset(states, 0, sizeof(states));
	CHECK_STATUS(test, pj_ioqueue_create(pool, PJ_IOQUEUE_MAX_HANDLES, &ioqueue));
	for (i = 0; i < PJ_IOQUEUE_MAX_HANDLES; ++i) {
		CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
						 &sockets[i]));
		CHECK_STATUS(test, pj_ioqueue_register_sock(pool, ioqueue, sockets[i],
						       &states[i], &callbacks, &keys[i]));
	}
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
					 &overflow));
	status = pj_ioqueue_register_sock(pool, ioqueue, overflow, NULL, &callbacks,
					  &overflow_key);
	CHECK_TRUE(test, status == PJ_ETOOMANY);
	CHECK_STATUS(test, pj_sock_close(overflow));
	for (i = PJ_IOQUEUE_MAX_HANDLES - 1; i >= 0; --i)
		CHECK_STATUS(test, pj_ioqueue_unregister(keys[i]));
	CHECK_STATUS(test, pj_ioqueue_destroy(ioqueue));
	printk("[Stage 10] configured maximum of %d handles and overflow rejection: PASSED\n",
	       PJ_IOQUEUE_MAX_HANDLES);
	return 0;
}

int stage10_ioqueue_run(void)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool;
	pj_status_t status;

	printk("[Stage 10] PJLIB ioqueue semantic validation\n");
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	pj_caching_pool_init(&caching_pool, NULL, 0);
	pool = pj_pool_create(&caching_pool.factory, "stage10", 65536, 32768, NULL);
	if (pool == NULL) {
		fail_value("pool setup", __LINE__, "pool != NULL");
		pj_caching_pool_destroy(&caching_pool);
		pj_shutdown();
		return 1;
	}
	if (test_backend_configuration() != 0 ||
	    test_udp_read_write_timeout(pool) != 0 ||
	    test_tcp_connect(pool) != 0 ||
	    test_blocked_poll_unregister(pool) != 0 ||
	    test_callback_concurrency(pool) != 0 ||
	    test_repeated_safe_registration(pool) != 0 ||
	    test_maximum_handles(pool) != 0) {
		pj_pool_release(pool);
		pj_caching_pool_destroy(&caching_pool);
		pj_shutdown();
		return 1;
	}
	pj_pool_release(pool);
	pj_caching_pool_destroy(&caching_pool);
	pj_shutdown();
	printk("STAGE 10 RESULT: PASSED\n");
	return 0;
}
