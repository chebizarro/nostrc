/* Exercise the private idle notification protocol in isolation, with the
 * production scheduler compiled into this test instead of its archive object.
 * Hold S.mu across notifications so a signaled worker cannot consume the first
 * wake before the next one chooses its destination. */
#include "../sched/sched.c"
#include <assert.h>

static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int waiting;
static int work[3];

static void *idle_worker(void *arg) {
  gof_worker *w = arg;
  pthread_mutex_lock(&S.mu);
  ++waiting;
  pthread_cond_signal(&ready);
  w->idle_until = UINT64_MAX;
  while (!work[w->index]) pthread_cond_wait(&w->cv, &S.mu);
  w->idle_until = 0;
  w->wake_pending = 0;
  pthread_mutex_unlock(&S.mu);
  return NULL;
}

int main(void) {
  gof_worker workers[3] = {0};
  pthread_t threads[3];
  S.workers = workers;
  S.nworkers = 3;
  S.bootstrapped = 1;
  pthread_mutex_init(&S.mu, NULL);
  for (int round = 0; round < 3; ++round) {
    waiting = 0;
    memset(work, 0, sizeof(work));
    for (int i = 0; i < 3; ++i) {
      workers[i].index = i;
      pthread_cond_init(&workers[i].cv, NULL);
      assert(pthread_create(&threads[i], NULL, idle_worker, &workers[i]) == 0);
    }
    pthread_mutex_lock(&S.mu);
    while (waiting != 3) pthread_cond_wait(&ready, &S.mu);
    work[0] = work[1] = work[2] = 1;
    wake_one_locked();
    wake_one_locked(); /* must not collapse onto the first pending worker */
    int target = (5 - round) % 3; /* the worker not selected by the rotating pair */
    wake_worker_locked(&workers[target]);
    wake_worker_locked(&workers[target]); /* targeted duplicate is harmless */
    pthread_mutex_unlock(&S.mu);
    for (int i = 0; i < 3; ++i) {
      assert(pthread_join(threads[i], NULL) == 0);
      pthread_cond_destroy(&workers[i].cv);
    }
  }
  pthread_mutex_destroy(&S.mu);
  puts("idle wakes: distinct untargeted workers, exact targets, rotating selection");
  return 0;
}
