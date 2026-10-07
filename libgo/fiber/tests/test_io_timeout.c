#include "../include/libgo/fiber.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_result = -1;
static int g_saved_errno = 0;

static void run_fiber(void *arg) {
  (void)arg;
  int lsock = socket(AF_INET, SOCK_STREAM, 0);
  assert(lsock >= 0);

  int opt = 1;
  setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0); /* let OS pick port */
  int rc = bind(lsock, (struct sockaddr*)&addr, sizeof(addr));
  assert(rc == 0);

  rc = listen(lsock, 1);
  assert(rc == 0);

  socklen_t alen = sizeof(addr);
  rc = getsockname(lsock, (struct sockaddr*)&addr, &alen);
  assert(rc == 0);

  /* No client connects. Accept with a short timeout and expect ETIMEDOUT.
   * Capture the fiber's errno for diagnostics (main's is unrelated). */
  int c = gof_accept(lsock, NULL, NULL, 50);
  g_saved_errno = errno;
  close(lsock);
  if (c != -1) {
    close(c);
    g_result = 1;
    return;
  }
  g_result = (g_saved_errno == ETIMEDOUT) ? 0 : 2;
}

int main(void) {
  gof_init(0);
  gof_spawn(run_fiber, NULL, 0);
  gof_run();
  if (g_result == 0) {
    printf("gof_test_io_timeout: OK\n");
    return 0;
  }
  printf("gof_test_io_timeout: FAIL rc=%d fiber_errno=%d\n", g_result, g_saved_errno);
  return 1;
}
