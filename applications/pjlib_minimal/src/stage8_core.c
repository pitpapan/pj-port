#include <pjlib.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/printk.h>

#define STAGE8_ITERATIONS 3
#define WORKER_STACK_SIZE 8192

#if PJ_THREAD_ALLOCATE_STACK != 0
#error "Stage 8 expects POSIX-owned PJLIB thread stacks on Zephyr"
#endif

#if PJ_THREAD_SET_STACK_SIZE == 0
#error "Stage 8 expects requested stack sizes to reach pthread_attr_setstacksize"
#endif

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Stage 8] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Stage 8] FAIL %s:%d condition=%s\n",
	       test, line, condition);
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

struct worker_context {
	long tls_key;
	void *tls_value;
	pj_atomic_t *atomic;
	pj_mutex_t *mutex;
	pj_rwmutex_t *rwmutex;
	pj_sem_t *sem;
	pj_event_t *event;
	pj_barrier_t *barrier;
	volatile int reached;
	int result;
	int loops;
	int priority;
	int priority_min;
	int priority_max;
	int priority_policy;
	int priority_native_rc;
	pj_status_t priority_set_status;
};

static int join_worker(const char *test, pj_thread_t *thread,
		       struct worker_context *context)
{
	pj_status_t status;

	status = pj_thread_join(thread);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	status = pj_thread_destroy(thread);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	if (context && context->result != 0)
		return fail_value(test, __LINE__, "worker result == 0");
	return 0;
}

static int tls_worker(void *arg)
{
	struct worker_context *context = arg;

	if (!pj_thread_is_registered() || pj_thread_this() == NULL ||
	    pj_thread_local_get(context->tls_key) != NULL ||
	    pj_thread_local_set(context->tls_key, context->tls_value) != PJ_SUCCESS ||
	    pj_thread_local_get(context->tls_key) != context->tls_value)
		context->result = 1;
	return context->result;
}

struct external_thread_context {
	/* This descriptor is owned by the creator and remains live through join. */
	pj_thread_desc descriptor;
	pj_thread_t *thread;
	long tls_key;
	int tls_value;
	int result;
};

static void *external_thread(void *arg)
{
	struct external_thread_context *context = arg;

	if (pj_thread_is_registered()) {
		context->result = 1;
		return NULL;
	}
	if (pj_thread_register("stage8-ext", context->descriptor,
			       &context->thread) != PJ_SUCCESS ||
	    !pj_thread_is_registered() || pj_thread_this() != context->thread ||
	    pj_thread_local_get(context->tls_key) != NULL ||
	    pj_thread_local_set(context->tls_key, &context->tls_value) != PJ_SUCCESS ||
	    pj_thread_local_get(context->tls_key) != &context->tls_value) {
		context->result = 2;
	}
	if (pj_thread_is_registered() && pj_thread_unregister() != PJ_SUCCESS)
		context->result = 3;
	if (pj_thread_is_registered())
		context->result = 4;
	return NULL;
}

static int test_threads_and_tls(pj_pool_t *pool)
{
	const char *test = "threads/TLS";
	struct worker_context worker = {0};
	struct external_thread_context external = {0};
	pj_thread_t *thread;
	pthread_t native_thread;
	long key;
	long recycled_key;
	int main_value = 11;
	int worker_value = 22;
	int rc;

	CHECK_TRUE(test, pj_thread_is_registered());
	CHECK_STATUS(test, pj_thread_local_alloc(&key));
	CHECK_STATUS(test, pj_thread_local_set(key, &main_value));
	CHECK_TRUE(test, pj_thread_local_get(key) == &main_value);

	worker.tls_key = key;
	worker.tls_value = &worker_value;
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-tls", tls_worker,
					    &worker, WORKER_STACK_SIZE, 0, &thread));
	if (join_worker(test, thread, &worker) != 0)
		return -1;
	CHECK_TRUE(test, pj_thread_local_get(key) == &main_value);

	external.tls_key = key;
	external.tls_value = 33;
	rc = pthread_create(&native_thread, NULL, external_thread, &external);
	CHECK_TRUE(test, rc == 0);
	rc = pthread_join(native_thread, NULL);
	CHECK_TRUE(test, rc == 0);
	CHECK_TRUE(test, external.result == 0);
	CHECK_TRUE(test, pj_thread_local_get(key) == &main_value);

	pj_thread_local_free(key);
	CHECK_STATUS(test, pj_thread_local_alloc(&recycled_key));
	CHECK_TRUE(test, recycled_key == key);
	CHECK_TRUE(test, pj_thread_local_get(recycled_key) == NULL);
	pj_thread_local_free(recycled_key);
	return 0;
}

static int stack_worker(void *arg)
{
	struct worker_context *context = arg;
	pj_thread_t *current = pj_thread_this();
	pthread_t *native = pj_thread_get_os_handle(current);
	struct sched_param param = {0};

	context->priority = pj_thread_get_prio(current);
	context->priority_min = pj_thread_get_prio_min(current);
	context->priority_max = pj_thread_get_prio_max(current);
	context->priority_set_status = context->priority < 0 ? PJ_EUNKNOWN :
		pj_thread_set_prio(current, context->priority);
	context->priority_native_rc = native == NULL ? EINVAL :
		pthread_getschedparam(*native, &context->priority_policy, &param);
	if (context->priority_native_rc == 0 &&
	    context->priority != param.sched_priority)
		context->result = 1;
	context->reached = 1;
	return context->result;
}

static int test_stack_and_priority(pj_pool_t *pool, int iteration)
{
	const char *test = "thread stack/priority";
	struct worker_context context = {0};
	pj_thread_t *thread;
	int registered_main_priority = pj_thread_get_prio(pj_thread_this());

	CHECK_STATUS(test, pj_thread_create(pool, "stage8-stack", stack_worker,
					    &context, WORKER_STACK_SIZE, 0, &thread));
	if (join_worker(test, thread, &context) != 0)
		return -1;
	CHECK_TRUE(test, context.reached == 1);
	CHECK_TRUE(test, context.priority_native_rc == 0);
	CHECK_TRUE(test, context.priority >= 0);
	CHECK_STATUS(test, context.priority_set_status);
	CHECK_TRUE(test, context.priority_min == 0 && context.priority_max == 0);

	printk("[Stage 8] iteration %d POSIX priority policy=%d worker=%d range-helpers=%d/%d registered-main=%d\n",
	       iteration, context.priority_policy, context.priority,
	       context.priority_min, context.priority_max, registered_main_priority);
	printk("[Stage 8] thread stacks: POSIX-owned, %d-byte request, OS-aligned; PJLIB joins without raw free\n",
	       WORKER_STACK_SIZE);
	return 0;
}

static int mutex_worker(void *arg)
{
	struct worker_context *context = arg;

	if (pj_mutex_lock(context->mutex) != PJ_SUCCESS) {
		context->result = 1;
		return 1;
	}
	context->reached = 1;
	if (pj_mutex_unlock(context->mutex) != PJ_SUCCESS)
		context->result = 2;
	return context->result;
}

static int rwmutex_worker(void *arg)
{
	struct worker_context *context = arg;

	if (pj_rwmutex_lock_write(context->rwmutex) != PJ_SUCCESS) {
		context->result = 1;
		return 1;
	}
	context->reached = 1;
	if (pj_rwmutex_unlock_write(context->rwmutex) != PJ_SUCCESS)
		context->result = 2;
	return context->result;
}

static int test_mutexes(pj_pool_t *pool)
{
	const char *test = "mutexes/RW locks";
	struct worker_context context = {0};
	pj_mutex_t *simple;
	pj_mutex_t *recursive;
	pj_rwmutex_t *rwmutex;
	pj_thread_t *thread;

	CHECK_STATUS(test, pj_mutex_create_simple(pool, "stage8-simple", &simple));
	CHECK_STATUS(test, pj_mutex_lock(simple));
	CHECK_TRUE(test, pj_mutex_trylock(simple) != PJ_SUCCESS);
	context.mutex = simple;
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-mutex", mutex_worker,
					    &context, WORKER_STACK_SIZE, 0, &thread));
	CHECK_STATUS(test, pj_thread_sleep(10));
	CHECK_TRUE(test, context.reached == 0);
	CHECK_STATUS(test, pj_mutex_unlock(simple));
	if (join_worker(test, thread, &context) != 0)
		return -1;
	CHECK_STATUS(test, pj_mutex_destroy(simple));

	CHECK_STATUS(test, pj_mutex_create_recursive(pool, "stage8-rec", &recursive));
	CHECK_STATUS(test, pj_mutex_lock(recursive));
	CHECK_STATUS(test, pj_mutex_lock(recursive));
	CHECK_STATUS(test, pj_mutex_trylock(recursive));
	CHECK_STATUS(test, pj_mutex_unlock(recursive));
	CHECK_STATUS(test, pj_mutex_unlock(recursive));
	CHECK_STATUS(test, pj_mutex_unlock(recursive));
	CHECK_STATUS(test, pj_mutex_destroy(recursive));

	memset(&context, 0, sizeof(context));
	CHECK_STATUS(test, pj_rwmutex_create(pool, "stage8-rw", &rwmutex));
	CHECK_STATUS(test, pj_rwmutex_lock_read(rwmutex));
	context.rwmutex = rwmutex;
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-rw", rwmutex_worker,
					    &context, WORKER_STACK_SIZE, 0, &thread));
	CHECK_STATUS(test, pj_thread_sleep(10));
	CHECK_TRUE(test, context.reached == 0);
	CHECK_STATUS(test, pj_rwmutex_unlock_read(rwmutex));
	if (join_worker(test, thread, &context) != 0)
		return -1;
	CHECK_STATUS(test, pj_rwmutex_lock_write(rwmutex));
	CHECK_STATUS(test, pj_rwmutex_unlock_write(rwmutex));
	CHECK_STATUS(test, pj_rwmutex_destroy(rwmutex));
	return 0;
}

#if PJ_HAS_SEMAPHORE
static int semaphore_worker(void *arg)
{
	struct worker_context *context = arg;

	if (pj_sem_wait(context->sem) != PJ_SUCCESS)
		context->result = 1;
	else
		context->reached = 1;
	return context->result;
}
#endif

#if PJ_HAS_EVENT_OBJ
static int event_worker(void *arg)
{
	struct worker_context *context = arg;

	if (pj_event_wait(context->event) != PJ_SUCCESS)
		context->result = 1;
	else
		context->reached = 1;
	return context->result;
}
#endif

static int barrier_worker(void *arg)
{
	struct worker_context *context = arg;
	pj_int32_t result = pj_barrier_wait(context->barrier, 0);

	if (result != PJ_FALSE && result != PJ_TRUE)
		context->result = 1;
	else
		context->result = result;
	return 0;
}

static int test_signals(pj_pool_t *pool)
{
	const char *test = "semaphore/events/barrier";
	struct worker_context context = {0};
	pj_thread_t *thread;
	pj_barrier_t *barrier;
	pj_int32_t main_barrier_result;

#if PJ_HAS_SEMAPHORE
	pj_sem_t *sem;

	CHECK_STATUS(test, pj_sem_create(pool, "stage8-sem", 0, 1, &sem));
	CHECK_TRUE(test, pj_sem_trywait(sem) != PJ_SUCCESS);
	context.sem = sem;
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-sem", semaphore_worker,
					    &context, WORKER_STACK_SIZE, 0, &thread));
	CHECK_STATUS(test, pj_thread_sleep(10));
	CHECK_TRUE(test, context.reached == 0);
	CHECK_STATUS(test, pj_sem_post(sem));
	if (join_worker(test, thread, &context) != 0)
		return -1;
	CHECK_STATUS(test, pj_sem_destroy(sem));
#endif

#if PJ_HAS_EVENT_OBJ
	{
		pj_event_t *event;

		memset(&context, 0, sizeof(context));
		CHECK_STATUS(test, pj_event_create(pool, "stage8-auto", PJ_FALSE,
						   PJ_FALSE, &event));
		CHECK_TRUE(test, pj_event_trywait(event) != PJ_SUCCESS);
		context.event = event;
		CHECK_STATUS(test, pj_thread_create(pool, "stage8-event", event_worker,
						    &context, WORKER_STACK_SIZE, 0, &thread));
		CHECK_STATUS(test, pj_thread_sleep(10));
		CHECK_TRUE(test, context.reached == 0);
		CHECK_STATUS(test, pj_event_set(event));
		if (join_worker(test, thread, &context) != 0)
			return -1;
		CHECK_TRUE(test, pj_event_trywait(event) != PJ_SUCCESS);
		CHECK_STATUS(test, pj_event_destroy(event));

		CHECK_STATUS(test, pj_event_create(pool, "stage8-manual", PJ_TRUE,
						   PJ_TRUE, &event));
		CHECK_STATUS(test, pj_event_trywait(event));
		CHECK_STATUS(test, pj_event_trywait(event));
		CHECK_STATUS(test, pj_event_reset(event));
		CHECK_TRUE(test, pj_event_trywait(event) != PJ_SUCCESS);
		CHECK_STATUS(test, pj_event_destroy(event));

		memset(&context, 0, sizeof(context));
		CHECK_STATUS(test, pj_event_create(pool, "stage8-pulse", PJ_FALSE,
						   PJ_FALSE, &event));
		context.event = event;
		CHECK_STATUS(test, pj_thread_create(pool, "stage8-pulse", event_worker,
						    &context, WORKER_STACK_SIZE, 0, &thread));
		CHECK_STATUS(test, pj_thread_sleep(10));
		CHECK_STATUS(test, pj_event_pulse(event));
		if (join_worker(test, thread, &context) != 0)
			return -1;
		CHECK_STATUS(test, pj_event_destroy(event));
	}
#endif

	memset(&context, 0, sizeof(context));
	CHECK_STATUS(test, pj_barrier_create(pool, 2, &barrier));
	context.barrier = barrier;
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-barrier", barrier_worker,
					    &context, WORKER_STACK_SIZE, 0, &thread));
	main_barrier_result = pj_barrier_wait(barrier, 0);
	CHECK_TRUE(test, main_barrier_result == PJ_FALSE ||
			 main_barrier_result == PJ_TRUE);
	CHECK_STATUS(test, pj_thread_join(thread));
	CHECK_STATUS(test, pj_thread_destroy(thread));
	CHECK_TRUE(test, context.result == PJ_FALSE || context.result == PJ_TRUE);
	CHECK_TRUE(test, context.result + main_barrier_result == 1);
	CHECK_STATUS(test, pj_barrier_destroy(barrier));
	return 0;
}

static int atomic_worker(void *arg)
{
	struct worker_context *context = arg;
	int i;

	for (i = 0; i < context->loops; ++i)
		pj_atomic_inc(context->atomic);
	return 0;
}

static int test_atomics(pj_pool_t *pool)
{
	const char *test = "atomics";
	struct worker_context first = {.loops = 1000};
	struct worker_context second = {.loops = 1000};
	pj_atomic_t *atomic;
	pj_thread_t *first_thread;
	pj_thread_t *second_thread;

	CHECK_STATUS(test, pj_atomic_create(pool, 10, &atomic));
	CHECK_TRUE(test, pj_atomic_get(atomic) == 10);
	pj_atomic_inc(atomic);
	pj_atomic_dec(atomic);
	CHECK_TRUE(test, pj_atomic_add_and_get(atomic, 5) == 15);
	pj_atomic_set(atomic, 0);
	first.atomic = atomic;
	second.atomic = atomic;
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-atomic1", atomic_worker,
					    &first, WORKER_STACK_SIZE, 0, &first_thread));
	CHECK_STATUS(test, pj_thread_create(pool, "stage8-atomic2", atomic_worker,
					    &second, WORKER_STACK_SIZE, 0, &second_thread));
	if (join_worker(test, first_thread, &first) != 0 ||
	    join_worker(test, second_thread, &second) != 0)
		return -1;
	CHECK_TRUE(test, pj_atomic_get(atomic) == 2000);
	CHECK_STATUS(test, pj_atomic_destroy(atomic));
	return 0;
}

static int test_exceptions(void)
{
	const char *test = "exceptions";
	pj_exception_id_t id;
	int caught = 0;
	PJ_USE_EXCEPTION;

	CHECK_STATUS(test, pj_exception_id_alloc("stage8", &id));
	PJ_TRY {
		PJ_THROW(id);
	}
	PJ_CATCH(id) {
		caught = (PJ_GET_EXCEPTION() == id);
	}
	PJ_CATCH_ANY {
		caught = -1;
	}
	PJ_END;
	CHECK_TRUE(test, caught == 1);
	CHECK_TRUE(test, strcmp(pj_exception_id_name(id), "stage8") == 0);
	CHECK_STATUS(test, pj_exception_id_free(id));
	return 0;
}

static int test_pools(pj_pool_factory *factory)
{
	const char *test = "pools";
	pj_pool_t *pool;
	pj_size_t used_before;
	pj_size_t used_after;
	unsigned char *zeroed;
	void *first;
	void *aligned;
	unsigned i;

	pool = pj_pool_create(factory, "stage8-pool", 512, 512, NULL);
	CHECK_TRUE(test, pool != NULL);
	used_before = pj_pool_get_used_size(pool);
	first = pj_pool_alloc(pool, 37);
	aligned = pj_pool_aligned_alloc(pool, 64, 16);
	zeroed = pj_pool_calloc(pool, 32, 1);
	CHECK_TRUE(test, first != NULL && aligned != NULL && zeroed != NULL);
	CHECK_TRUE(test, ((uintptr_t)first % PJ_POOL_ALIGNMENT) == 0);
	CHECK_TRUE(test, ((uintptr_t)aligned % 16) == 0);
	for (i = 0; i < 32; ++i)
		CHECK_TRUE(test, zeroed[i] == 0);
	used_after = pj_pool_get_used_size(pool);
	CHECK_TRUE(test, used_after > used_before);
	CHECK_TRUE(test, pj_pool_get_capacity(pool) >= used_after);
	pj_pool_reset(pool);
	CHECK_TRUE(test, pj_pool_get_used_size(pool) <= used_after);
	pj_pool_release(pool);
	return 0;
}

static int test_time(void)
{
	const char *test = "sleep/time/timestamp";
	pj_time_val wall_start;
	pj_time_val wall_stop;
	pj_timestamp start;
	pj_timestamp stop;
	pj_timestamp previous;
	pj_timestamp current;
	pj_timestamp frequency;
	pj_timestamp synthetic_start;
	pj_timestamp synthetic_stop;
	pj_uint32_t elapsed_usec;
	pj_uint64_t elapsed_msec;
	long wall_elapsed;
	unsigned i;

	CHECK_TRUE(test, sizeof(pj_timestamp) == 8);
	CHECK_STATUS(test, pj_get_timestamp_freq(&frequency));
	CHECK_TRUE(test, frequency.u64 > 0);
	CHECK_STATUS(test, pj_gettimeofday(&wall_start));
	CHECK_STATUS(test, pj_get_timestamp(&start));
	CHECK_STATUS(test, pj_thread_sleep(30));
	CHECK_STATUS(test, pj_get_timestamp(&stop));
	CHECK_STATUS(test, pj_gettimeofday(&wall_stop));
	CHECK_TRUE(test, pj_cmp_timestamp(&start, &stop) < 0);
	elapsed_usec = pj_elapsed_usec(&start, &stop);
	CHECK_TRUE(test, elapsed_usec >= 20000 && elapsed_usec <= 250000);
	wall_elapsed = (wall_stop.sec - wall_start.sec) * 1000L +
		       wall_stop.msec - wall_start.msec;
	CHECK_TRUE(test, wall_elapsed >= 20 && wall_elapsed <= 250);

	previous = stop;
	for (i = 0; i < 64; ++i) {
		CHECK_STATUS(test, pj_get_timestamp(&current));
		CHECK_TRUE(test, pj_cmp_timestamp(&previous, &current) <= 0);
		previous = current;
	}

	/* Cross the 32-bit boundary and verify frequency-based 64-bit conversion. */
	synthetic_start.u64 = ((pj_uint64_t)1 << 32) - frequency.u64;
	synthetic_stop.u64 = synthetic_start.u64 + frequency.u64 * 2;
	elapsed_msec = pj_elapsed_msec64(&synthetic_start, &synthetic_stop);
	CHECK_TRUE(test, elapsed_msec == 2000);
	printk("[Stage 8] clock frequency=%u:%u sleep=%u us wall=%ld ms epoch=%ld\n",
	       frequency.u32.hi, frequency.u32.lo, elapsed_usec, wall_elapsed,
	       wall_start.sec);
	printk("[Stage 8] wall clock policy: POSIX gettimeofday; absolute epoch requires target RTC setup\n");
	return 0;
}

struct timer_context {
	int count;
	int order[2];
};

static void timer_callback(pj_timer_heap_t *heap, pj_timer_entry *entry)
{
	struct timer_context *context = entry->user_data;

	PJ_UNUSED_ARG(heap);
	if (context->count < 2)
		context->order[context->count++] = entry->id;
}

static int test_timer_heap(pj_pool_t *pool)
{
	const char *test = "timer heap";
	struct timer_context context = {0};
	pj_timer_heap_t *heap;
	pj_timer_entry early;
	pj_timer_entry late;
	pj_time_val early_delay = {0, 10};
	pj_time_val late_delay = {0, 30};
	pj_timestamp start;
	pj_timestamp now;

	CHECK_STATUS(test, pj_timer_heap_create(pool, 4, &heap));
	pj_timer_entry_init(&early, 1, &context, timer_callback);
	pj_timer_entry_init(&late, 2, &context, timer_callback);
	CHECK_STATUS(test, pj_timer_heap_schedule(heap, &late, &late_delay));
	CHECK_STATUS(test, pj_timer_heap_schedule(heap, &early, &early_delay));
	CHECK_TRUE(test, pj_timer_heap_count(heap) == 2);
	CHECK_STATUS(test, pj_get_timestamp(&start));
	do {
		(void)pj_timer_heap_poll(heap, NULL);
		if (context.count == 2)
			break;
		CHECK_STATUS(test, pj_thread_sleep(2));
		CHECK_STATUS(test, pj_get_timestamp(&now));
	} while (pj_elapsed_msec(&start, &now) < 250);
	CHECK_TRUE(test, context.count == 2);
	CHECK_TRUE(test, context.order[0] == 1 && context.order[1] == 2);
	CHECK_TRUE(test, pj_timer_heap_count(heap) == 0);
	pj_timer_heap_destroy(heap);
	return 0;
}

static int test_native_errors(void)
{
	const char *test = "native errors";
	pj_status_t status = PJ_STATUS_FROM_OS(EINVAL);
	char text[PJ_ERR_MSG_SIZE];

	CHECK_TRUE(test, status != PJ_SUCCESS);
	CHECK_TRUE(test, PJ_STATUS_TO_OS(status) == EINVAL);
	pj_set_os_error(status);
	CHECK_TRUE(test, errno == EINVAL);
	errno = EAGAIN;
	CHECK_TRUE(test, pj_get_os_error() == PJ_STATUS_FROM_OS(EAGAIN));
	pj_strerror(status, text, sizeof(text));
	CHECK_TRUE(test, text[0] != '\0');
	return 0;
}

static int run_iteration(int iteration)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool;
	pj_status_t status;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);

	pj_caching_pool_init(&caching_pool, NULL, 0);
	pool = pj_pool_create(&caching_pool.factory, "stage8", 8192, 8192, NULL);
	if (pool == NULL) {
		fail_value("pool setup", __LINE__, "pool != NULL");
		goto done;
	}

	if (test_threads_and_tls(pool) != 0 ||
	    test_stack_and_priority(pool, iteration) != 0 ||
	    test_mutexes(pool) != 0 ||
	    test_signals(pool) != 0 ||
	    test_atomics(pool) != 0 ||
	    test_exceptions() != 0 ||
	    test_pools(&caching_pool.factory) != 0 ||
	    test_time() != 0 ||
	    test_timer_heap(pool) != 0 ||
	    test_native_errors() != 0)
		goto release_pool;

	result = 0;
	printk("[Stage 8] iteration %d core runtime: PASSED\n", iteration);

release_pool:
	pj_pool_release(pool);
done:
	pj_caching_pool_destroy(&caching_pool);
	pj_shutdown();
	printk("[Stage 8] iteration %d shutdown complete\n", iteration);
	return result;
}

int stage8_core_run(void)
{
	int iteration;

	printk("[Stage 8] core runtime validation (%d repeated lifecycles)\n",
	       STAGE8_ITERATIONS);
	for (iteration = 1; iteration <= STAGE8_ITERATIONS; ++iteration) {
		if (run_iteration(iteration) != 0) {
			printk("STAGE 8 RESULT: FAILED at iteration %d\n", iteration);
			return 1;
		}
	}
	printk("STAGE 8 RESULT: PASSED (%d/%d iterations)\n",
	       STAGE8_ITERATIONS, STAGE8_ITERATIONS);
	return 0;
}
