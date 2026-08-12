/*
 * Stage 5 compile probe for the POSIX surface used by PJLIB's existing
 * Unix/BSD implementations. This is intentionally not a runtime test:
 * semantic validation belongs to the later subsystem-specific stages.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

#include <zephyr/sys/printk.h>

static void *probe_thread(void *arg)
{
    return arg;
}

__attribute__((unused)) static void probe_posix_surface(void)
{
    pthread_t thread;
    pthread_key_t key;
    pthread_mutex_t mutex;
    pthread_mutexattr_t mutex_attr;
    pthread_attr_t thread_attr;
    pthread_cond_t condition;
    pthread_rwlock_t rwlock;
    pthread_barrier_t barrier;
    sem_t semaphore;
    struct timespec timespec_value;
    struct timeval timeval_value;
    struct sockaddr address;
    struct addrinfo *addresses;
    struct pollfd poll_fd;
    fd_set read_fds;
    socklen_t option_length = sizeof(int);
    int option_value;
    struct sched_param sched_param;
    int fd;

    (void)pthread_attr_init(&thread_attr);
    (void)pthread_attr_setstacksize(&thread_attr, 4096);
    (void)pthread_create(&thread, NULL, probe_thread, NULL);
    (void)pthread_join(thread, NULL);
    thread = pthread_self();
    (void)pthread_getschedparam(thread, &option_value, &sched_param);
    (void)pthread_setschedparam(thread, option_value, &sched_param);
    (void)sched_get_priority_min(SCHED_RR);
    (void)sched_get_priority_max(SCHED_RR);
    (void)pthread_attr_destroy(&thread_attr);

    (void)pthread_key_create(&key, NULL);
    (void)pthread_setspecific(key, &thread);
    (void)pthread_getspecific(key);
    (void)pthread_key_delete(key);

    (void)pthread_mutexattr_init(&mutex_attr);
    (void)pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&mutex, &mutex_attr);
    (void)pthread_mutex_lock(&mutex);
    (void)pthread_mutex_trylock(&mutex);
    (void)pthread_mutex_unlock(&mutex);
    (void)pthread_mutex_destroy(&mutex);
    (void)pthread_mutexattr_destroy(&mutex_attr);

    (void)pthread_cond_init(&condition, NULL);
    (void)pthread_cond_wait(&condition, &mutex);
    (void)pthread_cond_timedwait(&condition, &mutex, &timespec_value);
    (void)pthread_cond_signal(&condition);
    (void)pthread_cond_broadcast(&condition);
    (void)pthread_cond_destroy(&condition);

    (void)pthread_rwlock_init(&rwlock, NULL);
    (void)pthread_rwlock_rdlock(&rwlock);
    (void)pthread_rwlock_wrlock(&rwlock);
    (void)pthread_rwlock_unlock(&rwlock);
    (void)pthread_rwlock_destroy(&rwlock);

    (void)pthread_barrier_init(&barrier, NULL, 1);
    (void)pthread_barrier_wait(&barrier);
    (void)pthread_barrier_destroy(&barrier);

    (void)sem_init(&semaphore, 0, 1);
    (void)sem_wait(&semaphore);
    (void)sem_trywait(&semaphore);
    (void)sem_post(&semaphore);
    (void)sem_destroy(&semaphore);

    (void)clock_gettime(CLOCK_MONOTONIC, &timespec_value);
    (void)clock_getres(CLOCK_MONOTONIC, &timespec_value);
    (void)gettimeofday(&timeval_value, NULL);
    (void)nanosleep(&timespec_value, NULL);
    (void)localtime_r(&timeval_value.tv_sec, &(struct tm){0});
    (void)stat("/", &(struct stat){0});
    (void)strcasecmp("pj", "PJ");
    (void)strncasecmp("pjlib", "PJ", 2);

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    (void)bind(fd, &address, sizeof(address));
    (void)connect(fd, &address, sizeof(address));
    (void)listen(fd, 1);
    (void)accept(fd, &address, &option_length);
    (void)send(fd, &option_value, sizeof(option_value), 0);
    (void)recv(fd, &option_value, sizeof(option_value), 0);
    (void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &option_value, &option_length);
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option_value,
                     sizeof(option_value));

    (void)getaddrinfo("localhost", NULL, NULL, &addresses);
    freeaddrinfo(addresses);

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    (void)select(fd + 1, &read_fds, NULL, NULL, &timeval_value);

    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    (void)poll(&poll_fd, 1, 0);
    (void)close(fd);
}

struct runtime_probe_context {
    pthread_key_t first_key;
    pthread_key_t second_key;
    sem_t *semaphore;
    int first_value;
    int second_value;
    int worker_status;
};

static void *runtime_probe_thread(void *arg)
{
    struct runtime_probe_context *context = arg;

    context->worker_status =
        pthread_setspecific(context->first_key, &context->first_value) != 0 ||
        pthread_setspecific(context->second_key, &context->second_value) != 0 ||
        pthread_getspecific(context->first_key) != &context->first_value ||
        pthread_getspecific(context->second_key) != &context->second_value;

    if (sem_post(context->semaphore) != 0) {
        context->worker_status = 1;
        return NULL;
    }

    return context->worker_status == 0 ? context : NULL;
}

static int timespec_is_later(const struct timespec *before,
                             const struct timespec *after)
{
    return after->tv_sec > before->tv_sec ||
           (after->tv_sec == before->tv_sec &&
            after->tv_nsec >= before->tv_nsec);
}

int posix_probe_run(void)
{
    struct runtime_probe_context context = {0};
    const struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 1000000};
    struct timespec before;
    struct timespec after;
    pthread_mutexattr_t mutex_attr;
    pthread_mutex_t mutex;
    pthread_attr_t thread_attr;
    pthread_t thread;
    sem_t semaphore;
    void *thread_result = NULL;
    int status;
    int rc = -1;

    if (pthread_mutexattr_init(&mutex_attr) != 0 ||
        pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&mutex, &mutex_attr) != 0) {
        printk("POSIX probe failed: recursive mutex setup\n");
        return -1;
    }

    if (pthread_mutex_lock(&mutex) != 0 ||
        pthread_mutex_lock(&mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != 0) {
        printk("POSIX probe failed: recursive mutex behavior\n");
        goto destroy_mutex;
    }

    if (pthread_key_create(&context.first_key, NULL) != 0 ||
        pthread_key_create(&context.second_key, NULL) != 0) {
        printk("POSIX probe failed: TLS key allocation\n");
        goto destroy_mutex;
    }

    if (sem_init(&semaphore, 0, 0) != 0) {
        printk("POSIX probe failed: semaphore setup\n");
        goto delete_keys;
    }

    context.semaphore = &semaphore;
    context.first_value = 1;
    context.second_value = 2;

    status = pthread_attr_init(&thread_attr);
    if (status != 0) {
        printk("POSIX probe failed: pthread_attr_init=%d\n", status);
        goto destroy_semaphore;
    }

    status = pthread_attr_setstacksize(&thread_attr, 8192);
    if (status != 0) {
        printk("POSIX probe failed: pthread_attr_setstacksize=%d\n", status);
        (void)pthread_attr_destroy(&thread_attr);
        goto destroy_semaphore;
    }

    status = pthread_create(&thread, &thread_attr, runtime_probe_thread,
                            &context);
    (void)pthread_attr_destroy(&thread_attr);
    if (status != 0) {
        printk("POSIX probe failed: pthread_create=%d\n", status);
        goto destroy_semaphore;
    }

    status = sem_wait(&semaphore);
    if (status != 0) {
        printk("POSIX probe failed: sem_wait=%d\n", status);
        goto destroy_semaphore;
    }

    status = pthread_join(thread, &thread_result);
    if (status != 0) {
        printk("POSIX probe failed: pthread_join=%d\n", status);
        goto destroy_semaphore;
    }

    if (context.worker_status != 0 || thread_result != &context) {
        printk("POSIX probe failed: TLS result=%d return=%p\n",
               context.worker_status, thread_result);
        goto destroy_semaphore;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &before) != 0 ||
        nanosleep(&sleep_time, NULL) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &after) != 0 ||
        !timespec_is_later(&before, &after)) {
        printk("POSIX probe failed: monotonic clock behavior\n");
        goto destroy_semaphore;
    }

    rc = 0;

destroy_semaphore:
    (void)sem_destroy(&semaphore);
delete_keys:
    (void)pthread_key_delete(context.second_key);
    (void)pthread_key_delete(context.first_key);
destroy_mutex:
    (void)pthread_mutex_destroy(&mutex);
    (void)pthread_mutexattr_destroy(&mutex_attr);
    return rc;
}
