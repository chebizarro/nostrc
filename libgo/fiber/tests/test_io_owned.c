#define _POSIX_C_SOURCE 200809L
/* Assertions are the workload in the optional syscall-count mode too. */
#undef NDEBUG
#include "libgo/fiber.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

__attribute__((noinline, no_sanitize("thread"))) static int test_error(void) { return errno; }
static int fd[2];
static _Atomic int blocked;
void gof_trace_on_block(gof_socket_t f, int events) {
  (void)events;
  if (f == fd[0]) atomic_store(&blocked, 1);
}
static void make_blocking(int f) {
  int flags = fcntl(f, F_GETFL);
  assert(flags >= 0 && fcntl(f, F_SETFL, flags & ~O_NONBLOCK) == 0);
}
static void retire(int f) {
  assert(gof_io_reset_fd(f) == 0);
  assert(close(f) == 0);
}
static void reader(void *unused) {
  (void)unused;
  char byte;
  assert(gof_read(fd[0], &byte, 1) == 1 && byte == 'x');
}
static void writer(void *unused) {
  (void)unused;
  assert(gof_write(fd[1], "x", 1) == 1);
}
static void exchange(void) {
  assert(gof_spawn(reader, NULL, 0));
  assert(gof_spawn(writer, NULL, 0));
  gof_run(); /* a stale blocking verdict would prevent writer from running */
}
static void busy_owner(void *unused) {
  (void)unused;
  while (!atomic_load(&blocked)) gof_yield();
  assert(gof_io_register_owned_fd(fd[0]) == -1 && test_error() == EBUSY);
  writer(NULL);
}
static void full_writer(void *unused) {
  (void)unused;
  assert(gof_write(fd[1], "x", 1) == 1);
}
static void drainer(void *unused) {
  (void)unused;
  char bytes[8192];
  assert(read(fd[0], bytes, sizeof bytes) > 0);
}
static void count_calls(int owned) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  if (owned) {
    assert(gof_io_register_owned_fd(fd[0]) == 0);
    assert(gof_io_register_owned_fd(fd[1]) == 0);
  }
  for (int i = 0; i < 10000; ++i) {
    char byte;
    assert(gof_write(fd[1], "x", 1) == 1);
    assert(gof_read(fd[0], &byte, 1) == 1 && byte == 'x');
  }
  retire(fd[0]); retire(fd[1]);
  printf("owned=%d: 10000 stream write/read pairs\n", owned);
}
int main(int argc, char **argv) {
  if (argc == 2) {
    assert(!strcmp(argv[1], "--count-owned") || !strcmp(argv[1], "--count-unowned"));
    count_calls(!strcmp(argv[1], "--count-owned"));
    return 0;
  }
  gof_init(0);
  assert(gof_io_register_owned_fd(-1) == -1 && errno == EINVAL);
  assert(pipe(fd) == 0);
  assert(gof_io_register_owned_fd(fd[0]) == 0);
  assert(gof_io_register_owned_fd(fd[1]) == 0);
  exchange();

  /* An owner must revoke before giving an alias permission to mutate flags. */
  int alias = dup(fd[0]); assert(alias >= 0);
  assert(gof_io_reset_fd(fd[0]) == 0);
  make_blocking(alias);
  assert(close(alias) == 0);
  exchange();

  /* Registration cannot clear or adopt a live readiness wait. */
  atomic_store(&blocked, 0);
  assert(gof_spawn(reader, NULL, 0));
  assert(gof_spawn(busy_owner, NULL, 0));
  gof_run();
  int reused = fd[0];
  retire(fd[0]); retire(fd[1]);
  assert(gof_io_register_owned_fd(reused) == -1 && errno == EBADF);
  assert(pipe(fd) == 0);
  if (fd[0] != reused) {
    assert(dup2(fd[0], reused) == reused);
    assert(close(fd[0]) == 0);
    fd[0] = reused;
  }
  assert(!(fcntl(fd[0], F_GETFL) & O_NONBLOCK));
  exchange(); /* same fd number, different generation, no owned registration */
  retire(fd[0]); retire(fd[1]);

  assert(pipe(fd) == 0);
  assert(gof_io_register_owned_fd(fd[1]) == 0);
  char fill[4096] = {0};
  while (write(fd[1], fill, sizeof fill) > 0) {}
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  alias = dup(fd[1]); assert(alias >= 0);
  assert(gof_io_reset_fd(fd[1]) == 0);
  make_blocking(alias);
  assert(close(alias) == 0);
  assert(gof_spawn(full_writer, NULL, 0));
  assert(gof_spawn(drainer, NULL, 0));
  gof_run(); /* stale write ownership must not block this worker either */
  retire(fd[0]); retire(fd[1]);

  /* Even an illicit alias change cannot turn cached socket messages blocking.
   * read/write require revocation first; MSG_DONTWAIT is extra socket safety. */
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  assert(gof_io_register_owned_fd(fd[0]) == 0);
  alias = dup(fd[0]); assert(alias >= 0);
  make_blocking(alias);
  char byte;
  assert(gof_recvfrom(fd[0], &byte, 1, 0, NULL, NULL, 0) == -1 && errno == ETIMEDOUT);
  assert(gof_io_reset_fd(fd[0]) == 0);
  assert(close(alias) == 0);
  retire(fd[0]); retire(fd[1]);
  puts("owned fd: read/write, alias revocation, busy rejection, reuse, socket safety OK");
  return 0;
}
