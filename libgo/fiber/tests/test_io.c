#include "test_net.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static int g_ok = 0;

static void reader(void *arg) {
  gof_socket_t fd = *(gof_socket_t*)arg;
  char buf[64];
  ssize_t r = gof_read(fd, buf, sizeof(buf) - 1);
  assert(r > 0);
  buf[r] = '\0';
  g_ok = (strcmp(buf, "hello, fiber") == 0);
}

int main(void) {
  gof_socket_t sp[2];
  int rc = test_socketpair(SOCK_STREAM, sp);
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
  test_close(sp[0]);
  test_close(sp[1]);
  printf("gof_test_io: OK\n");
  return 0;
}
