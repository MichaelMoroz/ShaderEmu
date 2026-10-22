/* pthreads: create and join, a mutex under contention, a condition variable, thread-local storage, detached threads,
   pthread_once, and thread-specific data. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "check.h"

static __thread int tls_counter = 5;
static __thread char tls_buf[40];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long shared_total;
static int ready, turn;

static void *adder(void *arg)
{
	intptr_t id = (intptr_t)arg;
	tls_counter += (int)id;            /* this thread's own copy */
	tls_buf[0] = (char)('A' + id);
	for (int i = 0; i < 2000; i++) {
		pthread_mutex_lock(&mu);
		long v = shared_total;
		if (i % 97 == 0) sched_yield();   /* hold the lock across a switch now and then */
		shared_total = v + 1;
		pthread_mutex_unlock(&mu);
	}
	return (void *)(intptr_t)(tls_counter * 100 + tls_buf[0]);
}

static void *waiter(void *arg)
{
	pthread_mutex_lock(&mu);
	while (!ready) pthread_cond_wait(&cv, &mu);
	turn++;
	pthread_mutex_unlock(&mu);
	return 0;
}

static int once_runs;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void once_fn(void) { once_runs++; }
static void *call_once(void *arg) { pthread_once(&once, once_fn); return 0; }

static pthread_key_t key;
static int destructed;
static void destructor(void *p) { destructed += (int)(intptr_t)p; }
static void *with_key(void *arg) { pthread_setspecific(key, arg); return pthread_getspecific(key); }

static volatile int detached_done;
static void *detached(void *arg) { detached_done = 1; return 0; }

int main(void)
{
	enum { N = 4 };
	pthread_t t[N];
	tls_buf[0] = 'M';
	for (intptr_t i = 0; i < N; i++) CHECK_INT(pthread_create(&t[i], 0, adder, (void *)i), 0);
	int good = 1;
	for (intptr_t i = 0; i < N; i++) {
		void *r;
		CHECK_INT(pthread_join(t[i], &r), 0);
		if ((intptr_t)r != (5 + i) * 100 + 'A' + i) good = 0;
	}
	CHECK(good);
	CHECK_INT(shared_total, N * 2000);
	CHECK_INT(tls_counter, 5);        /* the main thread's copy is untouched */
	CHECK(tls_buf[0] == 'M');

	/* a condition variable */
	for (int i = 0; i < N; i++) pthread_create(&t[i], 0, waiter, 0);
	usleep(20000);
	pthread_mutex_lock(&mu);
	ready = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	for (int i = 0; i < N; i++) pthread_join(t[i], 0);
	CHECK_INT(turn, N);

	/* pthread_once */
	for (int i = 0; i < N; i++) pthread_create(&t[i], 0, call_once, 0);
	for (int i = 0; i < N; i++) pthread_join(t[i], 0);
	CHECK_INT(once_runs, 1);

	/* keys */
	CHECK_INT(pthread_key_create(&key, destructor), 0);
	for (intptr_t i = 0; i < N; i++) pthread_create(&t[i], 0, with_key, (void *)(i + 1));
	good = 1;
	for (intptr_t i = 0; i < N; i++) {
		void *r;
		pthread_join(t[i], &r);
		if ((intptr_t)r != i + 1) good = 0;
	}
	CHECK(good);
	CHECK_INT(destructed, 1 + 2 + 3 + 4);

	/* detached */
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 32768);
	pthread_t d;
	CHECK_INT(pthread_create(&d, &at, detached, 0), 0);
	for (int i = 0; i < 100 && !detached_done; i++) usleep(1000);
	CHECK(detached_done);

	/* a timed wait that times out (futex with a timeout) */
	static pthread_cond_t cv2 = PTHREAD_COND_INITIALIZER;
	struct timespec ts, now;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 50000000;
	if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
	pthread_mutex_lock(&mu);
	CHECK_INT(pthread_cond_timedwait(&cv2, &mu, &ts), ETIMEDOUT);
	pthread_mutex_unlock(&mu);
	clock_gettime(CLOCK_REALTIME, &now);
	CHECK(now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec));

	CHECK(pthread_equal(pthread_self(), pthread_self()));
	return DONE("threads");
}
