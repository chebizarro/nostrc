/*
 * test_session_relay_ws — a real NIP-01 client against nostr-session-relayd
 * over its Unix socket.
 *
 * Spawns the built daemon (fallback-bind under a private XDG_RUNTIME_DIR,
 * storage forced off with `storage_driver = "none"`) and speaks RFC 6455 to
 * it the way NIP-01 clients do: no Sec-WebSocket-Protocol header.
 *
 *   - The upgrade must complete promptly even when the client connects
 *     after the daemon has gone idle (nostrc-q9ba part 2: the listen fd was
 *     not in lws's poll set, so late clients were never accepted).
 *   - Frames must reach the NIP-01 handler (nostrc-q9ba part 2: a
 *     subprotocol-less upgrade was bound to the "http" protocol, so REQ and
 *     EVENT were silently dropped).
 *
 * Needs $NOSTR_SESSION_RELAYD; exits 77 (SKIP) when it is unset.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ws_test_client.h"

static pid_t spawn_daemon(const char *bin, const char *xrd, const char *state) {
  pid_t pid = fork();
  if (pid != 0) return pid;
  setenv("XDG_RUNTIME_DIR", xrd, 1);
  setenv("XDG_DATA_HOME", state, 1);
  setenv("XDG_CONFIG_HOME", state, 1);
  setenv("HOME", state, 1);
  /* No session bus: SessionRelay1 is optional and must not be required. */
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent", 1);
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
  unsetenv("LISTEN_PID");
  execl(bin, bin, (char *)NULL);
  _exit(127);
}

int main(void) {
  const char *bin = getenv("NOSTR_SESSION_RELAYD");
  if (!bin || !*bin) {
    fprintf(stderr, "NOSTR_SESSION_RELAYD unset; SKIP\n");
    return 77;
  }
  signal(SIGPIPE, SIG_IGN);

  char xrd[] = "/tmp/nsr-ws-rt-XXXXXX";
  char state[] = "/tmp/nsr-ws-state-XXXXXX";
  if (!mkdtemp(xrd) || !mkdtemp(state)) {
    perror("mkdtemp");
    return 1;
  }
  chmod(xrd, 0700);
  char path[600];
  snprintf(path, sizeof path, "%s/nostr", state);
  mkdir(path, 0700);
  snprintf(path, sizeof path, "%s/nostr/session-relay.conf", state);
  if (write_file(path, "storage_driver = \"none\"\n") != 0) {
    perror("write session-relay.conf");
    return 1;
  }

  pid_t pid = spawn_daemon(bin, xrd, state);
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  char sock_path[600];
  snprintf(sock_path, sizeof sock_path, "%s/nostr/relay.sock", xrd);
  struct stat st;
  int appeared = 0;
  for (int i = 0; i < 800 && !appeared; i++) {
    appeared = stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode);
    if (!appeared) sleep_ms(10);
  }
  CHECK(appeared, "socket never appeared at %s", sock_path);

  if (appeared) {
    /* Let the daemon go idle: the old loop was parked inside lws by now. */
    sleep_ms(300);
    for (int round = 0; round < 2; round++) {
      long long upgrade_ms = -1;
      int fd = ws_open(sock_path, &upgrade_ms);
      CHECK(fd >= 0, "round %d: WebSocket upgrade over relay.sock failed",
            round);
      if (fd < 0) break;
      fprintf(stderr, "round %d: upgrade in %lld ms\n", round, upgrade_ms);
      CHECK(upgrade_ms < 1000, "round %d: upgrade took %lld ms", round,
            upgrade_ms);
      /* nostrc-prqu.14: a storage-less relay answers a plain REQ with EOSE
       * in one round trip instead of leaving the client to time out. */
      expect_reply(fd, "[\"REQ\",\"x\",{\"kinds\":[1],\"limit\":1}]",
                   "[\"EOSE\",\"x\"]", "plain REQ without storage");
      expect_reply(fd, "[\"REQ\",\"s\",{\"search\":\"x\"}]",
                   "[\"CLOSED\",\"s\",\"unsupported: search\"",
                   "search REQ without storage");
      expect_reply(fd, "[\"EVENT\",{\"id\":\"00\"}]", "[\"OK\",",
                   "EVENT is answered");
      close(fd);
      sleep_ms(200);
    }
  }

  kill(pid, SIGTERM);
  int status = 0;
  int reaped = 0;
  for (int i = 0; i < 50 && !reaped; i++) {
    reaped = waitpid(pid, &status, WNOHANG) == pid;
    if (!reaped) sleep_ms(100);
  }
  CHECK(reaped, "daemon did not exit within 5 s of SIGTERM");
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  } else {
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "daemon exit status 0x%x", status);
  }
  CHECK(access(sock_path, F_OK) != 0,
        "fallback socket %s not unlinked at shutdown", sock_path);

  if (g_failures) {
    fprintf(stderr, "test_session_relay_ws: %d failure(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "test_session_relay_ws: ok\n");
  return 0;
}
