#include "debug.h"
#include "../sched/sched.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

/* Fiber registry for introspection (nostrc-l1no)
 * Maintains a linked list of all live fibers for gof_list(). */

typedef struct fiber_node {
  struct fiber_node *next;
  gof_fiber *f;
} fiber_node;

static fiber_node *fiber_registry = NULL;
static pthread_mutex_t registry_mu = PTHREAD_MUTEX_INITIALIZER;

/* Called by scheduler when a fiber is created */
void gof_introspect_register(gof_fiber *f) {
  if (!f) return;
  fiber_node *node = (fiber_node*)malloc(sizeof(fiber_node));
  if (!node) return;
  node->f = f;
  pthread_mutex_lock(&registry_mu);
  node->next = fiber_registry;
  fiber_registry = node;
  pthread_mutex_unlock(&registry_mu);
}

/* Called by scheduler when a fiber is destroyed */
void gof_introspect_unregister(gof_fiber *f) {
  if (!f) return;
  pthread_mutex_lock(&registry_mu);
  fiber_node **pp = &fiber_registry;
  while (*pp) {
    if ((*pp)->f == f) {
      fiber_node *to_free = *pp;
      *pp = to_free->next;
      free(to_free);
      break;
    }
    pp = &(*pp)->next;
  }
  pthread_mutex_unlock(&registry_mu);
}

void gof_set_name(const char *name) {
  gof_fiber *f = gof_sched_current();
  if (f) {
    f->name = name;  /* Note: caller must ensure string lifetime */
  }
}

size_t gof_list(gof_info *out, size_t max) {
  pthread_mutex_lock(&registry_mu);

  /* Count total fibers */
  size_t count = 0;
  for (fiber_node *node = fiber_registry; node; node = node->next) {
    count++;
  }

  /* If out is NULL, just return count */
  if (!out) {
    pthread_mutex_unlock(&registry_mu);
    return count;
  }

  /* Fill output array */
  size_t written = 0;
  for (fiber_node *node = fiber_registry; node && written < max; node = node->next) {
    gof_fiber *f = node->f;
    if (!f) continue;

    out[written].id = f->id;
    out[written].name = f->name;
    out[written].stack_size = f->stack.size;
    out[written].stack_used = 0;  /* Stack usage tracking not yet implemented */
    out[written].state = f->state;
    out[written].last_run_ns = 0;  /* Per-fiber timing not yet tracked */
    written++;
  }

  pthread_mutex_unlock(&registry_mu);
  return written;
}

void gof_dump_stacks(int fd) {
  char buf[512];
  int len;
  ssize_t rc;

  pthread_mutex_lock(&registry_mu);

  len = snprintf(buf, sizeof(buf), "=== Fiber Stack Dump ===\n");
  if (len > 0) { rc = write(fd, buf, (size_t)len); (void)rc; }

  size_t count = 0;
  for (fiber_node *node = fiber_registry; node; node = node->next) {
    gof_fiber *f = node->f;
    if (!f) continue;

    const char *state_str = "unknown";
    switch (f->state) {
      case GOF_RUNNABLE: state_str = "runnable"; break;
      case GOF_BLOCKED:  state_str = "blocked";  break;
      case GOF_FINISHED: state_str = "finished"; break;
    }

    len = snprintf(buf, sizeof(buf),
                   "fiber %llu [%s]: name=%s stack=%zu bytes\n",
                   (unsigned long long)f->id,
                   state_str,
                   f->name ? f->name : "(unnamed)",
                   f->stack.size);
    if (len > 0) { rc = write(fd, buf, (size_t)len); (void)rc; }
    count++;
  }

  len = snprintf(buf, sizeof(buf), "=== Total: %zu fibers ===\n", count);
  if (len > 0) { rc = write(fd, buf, (size_t)len); (void)rc; }

  pthread_mutex_unlock(&registry_mu);
}
