#include "../include/libgo/fiber.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

static volatile int g_stop = 0;
static size_t g_total_msgs = 0;
static size_t g_msg_size = 64;

static void reader(void *arg) {
  int fd = *(int*)arg;
  char *buf = (char*)malloc(g_msg_size);
  if (!buf) return;
  while (!g_stop) {
    ssize_t r = gof_read(fd, buf, g_msg_size);
    if (r <= 0) {
      if (errno == EINTR) continue;
      break;
    }
    // Count whole messages received (may come fragmented; keep it simple)
    if ((size_t)r == g_msg_size) {
      __sync_fetch_and_add(&g_total_msgs, 1);
    }
  }
  free(buf);
}

static void writer(void *arg) {
  int fd = *(int*)arg;
  char *buf = (char*)malloc(g_msg_size);
  if (!buf) return;
  memset(buf, 'x', g_msg_size);
  while (!g_stop) {
    ssize_t w = gof_write(fd, buf, g_msg_size);
    if (w <= 0) {
      if (errno == EINTR) continue;
      break;
    }
    // Yield to allow other fibers to run
    gof_yield();
  }
  free(buf);
}

static long now_ms(void) {
  struct timeval tv; gettimeofday(&tv, NULL);
  return (long)tv.tv_sec * 1000L + (tv.tv_usec / 1000);
}

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s [-conns N] [-duration_ms D] [-msg_size S]\n", prog);
  fprintf(stderr, "  Env knobs (read by library): GOF_NWORKERS, GOF_NPOLLERS, GOF_POLL_PARTITION, GOF_AFFINITY, GOF_REBALANCE, ...\n");
}

typedef struct {
  int duration_ms;
  int *sp;      // pointer to socketpair array of size 2*conns
  int conns;    // number of socket pairs
} stop_params;

static void stopper(void *arg) {
  stop_params *p = (stop_params*)arg;
  // Sleep for requested duration inside a fiber so the scheduler runs
  gof_sleep_ms((uint64_t)p->duration_ms);
  g_stop = 1;
  // Proactively wake any blocked readers/writers by shutting down sockets
  if (p->sp && p->conns > 0) {
    for (int i = 0; i < p->conns; ++i) {
      int a = p->sp[i * 2 + 0];
      int b = p->sp[i * 2 + 1];
      if (a >= 0) shutdown(a, SHUT_RDWR);
      if (b >= 0) shutdown(b, SHUT_RDWR);
    }
  }
}

int main(int argc, char **argv) {
  int conns = 64;
  int duration_ms = 1000;
  g_msg_size = 64;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-conns") == 0 && i + 1 < argc) {
      conns = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-duration_ms") == 0 && i + 1 < argc) {
      duration_ms = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-msg_size") == 0 && i + 1 < argc) {
      g_msg_size = (size_t)atoi(argv[++i]);
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (conns < 1) conns = 1;

  int *sp = (int*)malloc(sizeof(int) * 2 * conns);
  if (!sp) { perror("malloc"); return 1; }

  for (int i = 0; i < conns; ++i) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &sp[i * 2]) != 0) {
      perror("socketpair");
      return 1;
    }
  }

  gof_init(0);

  // Spawn one reader and one writer per connection, crossing the pair.
  // Pass stable addresses from the socket array.
  for (int i = 0; i < conns; ++i) {
    int *pa = &sp[i * 2 + 0];
    int *pb = &sp[i * 2 + 1];
    gof_spawn(reader, pb, 0); // reader on b
    gof_spawn(writer, pa, 0); // writer on a
  }

  // Start a stopper fiber that will set g_stop after duration and shutdown sockets
  stop_params spm = { .duration_ms = duration_ms, .sp = sp, .conns = conns };
  gof_spawn(stopper, &spm, 0);

  long start = now_ms();

  // Run the scheduler; it will return when all fibers exit.
  gof_run();

  // Close sockets after fibers have stopped
  for (int i = 0; i < conns; ++i) {
    close(sp[i * 2 + 0]);
    close(sp[i * 2 + 1]);
  }

  long elapsed = now_ms() - start;
  double secs = elapsed / 1000.0;
  double mps = g_total_msgs / (secs > 0 ? secs : 1.0);

  // Report, leaving env knobs to the user
  printf("bench_poll_partition: conns=%d msg_size=%zu duration_ms=%d\n", conns, g_msg_size, duration_ms);
  printf("  total_msgs=%zu elapsed_ms=%ld msgs_per_sec=%.2f\n", g_total_msgs, elapsed, mps);

  free(sp);
  return 0;
}
