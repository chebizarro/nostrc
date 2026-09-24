/* porthome_sandbox_stub — hostile-helper stand-in.
 *
 * Bead nostrc-ww50 (Phase 2.5B).
 *
 * The sandbox integration test forks this binary under
 * nh_porthome_spawn_sandboxed and passes a mode argument. Each mode
 * probes ONE invariant the sandbox is supposed to enforce; the mode
 * translates its outcome to a distinct exit code that the parent
 * asserts on.
 *
 * Modes:
 *   write-etc-passwd  open /etc/passwd O_WRONLY -> we expect EACCES.
 *                     Exit 0 if the open was refused; 1 if it
 *                     succeeded (a serious failure of the sandbox).
 *   bind-lowport      socket(AF_INET)+bind() to port 80. We expect
 *                     EACCES/EPERM (or EADDRINUSE only if something
 *                     else is on the port — treat that as "close
 *                     enough" but log). Exit 0 on refusal.
 *   spin-cpu          Burn CPU. The parent set RLIMIT_CPU to 1 s;
 *                     the kernel sends SIGKILL when soft==hard limit
 *                     is exceeded, so this binary NEVER exits under
 *                     the sandbox — the parent sees WIFSIGNALED with
 *                     SIGKILL/SIGXCPU. If we ever DO get to return,
 *                     it means the rlimit was skipped: exit 2.
 *   report-uid        Print `uid=<n>` to stdout and exit 0.
 *   noop              Exit 0. Used as a smoke probe.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) { fputs("usage: stub <mode>\n", stderr); return 64; }
  const char *mode = argv[1];

  if (!strcmp(mode, "noop")) {
    return 0;
  }
  if (!strcmp(mode, "report-uid")) {
    printf("uid=%u\n", (unsigned)getuid());
    return 0;
  }
  if (!strcmp(mode, "write-etc-passwd")) {
    int fd = open("/etc/passwd", O_WRONLY | O_APPEND);
    if (fd >= 0) {
      /* Actively write a byte to be sure — some filesystems reject
       * later. */
      const char *b = "#";
      ssize_t w = write(fd, b, 1);
      close(fd);
      if (w > 0) return 1;
      /* Open worked but write failed — still a partial failure. */
      return 1;
    }
    /* EACCES / EROFS / EPERM are all acceptable "refusal" outcomes. */
    return 0;
  }
  if (!strcmp(mode, "bind-lowport")) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0; /* socket() itself refused — refusal counts */
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(80);
    int b = bind(s, (struct sockaddr *)&sa, sizeof sa);
    int e = errno;
    close(s);
    if (b == 0) return 1; /* somehow succeeded — sandbox failed */
    /* EACCES / EPERM = privileged port refused. EADDRINUSE means
     * another service is on :80 and we couldn't test cleanly — count
     * that as SKIP-equivalent so the test doesn't false-fail on hosts
     * with an active httpd. */
    if (e == EACCES || e == EPERM) return 0;
    if (e == EADDRINUSE) return 3;
    /* Any other errno means we made progress into the kernel and it
     * refused for another reason — surface it distinctly. */
    return 4;
  }
  if (!strcmp(mode, "spin-cpu")) {
    /* Pin a hot loop; RLIMIT_CPU will trigger SIGXCPU / SIGKILL. */
    volatile unsigned long long x = 0;
    for (;;) { x += 1; if ((x & 0xffffffull) == 0) (void)x; }
    return 2;
  }

  fputs("stub: unknown mode\n", stderr);
  return 64;
}
