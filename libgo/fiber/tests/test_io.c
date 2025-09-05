#include "../include/libgo/fiber.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

static int g_ok = 0;

static void reader(void *arg) {
  int fd = *(int*)arg;
  char buf[64];
  ssize_t r = gof_read(fd, buf, sizeof(buf));
  assert(r > 0);
  buf[r] = '\0';
  g_ok = (strcmp(buf, "hello, fiber") == 0);
}

int main(void) {
  int sp[2];
  int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
  assert(rc == 0);

  gof_init(0);
  gof_spawn(reader, &sp[1], 0);

  const char *msg = "hello, fiber";
  // Give reader a chance to block in read
  gof_sleep_ms(10);
  ssize_t w = gof_write(sp[0], msg, strlen(msg));
  assert(w == (ssize_t)strlen(msg));

  gof_run();

  assert(g_ok == 1);
  close(sp[0]);
  close(sp[1]);
  printf("gof_test_io: OK\n");
  return 0;
}
