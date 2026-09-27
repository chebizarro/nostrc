/*
 * test_session_relay_storage — the packaged session relay stores events
 * (nostrc-prqu.5).
 *
 * Built only when the relay links the nostrdb driver. Spawns the real
 * daemon with its default config (storage_driver = nostrdb) on private XDG
 * dirs, publishes a signed kind-1 event over relay.sock, reads it back with
 * a REQ by id, then restarts the daemon and reads it again from disk.
 *
 * Needs $NOSTR_SESSION_RELAYD; exits 77 (SKIP) when it is unset.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ws_test_client.h"

#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-keys.h"

static pid_t spawn_daemon(const char *bin, const char *xrd, const char *state) {
  pid_t pid = fork();
  if (pid != 0) return pid;
  setenv("XDG_RUNTIME_DIR", xrd, 1);
  setenv("XDG_DATA_HOME", state, 1);
  setenv("XDG_CONFIG_HOME", state, 1);
  setenv("HOME", state, 1);
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent", 1);
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
  unsetenv("LISTEN_PID");
  execl(bin, bin, (char *)NULL);
  _exit(127);
}

static int wait_socket(const char *path) {
  struct stat st;
  for (int i = 0; i < 1000; i++) {
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) return 0;
    sleep_ms(10);
  }
  return -1;
}

static int stop_daemon(pid_t pid) {
  kill(pid, SIGTERM);
  int status = 0;
  for (int i = 0; i < 100; i++) {
    if (waitpid(pid, &status, WNOHANG) == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    sleep_ms(100);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return -1;
}

/* REQ the event by id until it is served (nostrdb ingests asynchronously).
 * Returns 1 when an EVENT carrying `id` arrived, followed by EOSE. */
static int fetch_by_id(int fd, const char *id) {
  char req[256];
  for (int attempt = 0; attempt < 40; attempt++) {
    snprintf(req, sizeof req, "[\"REQ\",\"get%d\",{\"ids\":[\"%s\"]}]",
             attempt, id);
    if (ws_send(fd, req) != 0) return 0;
    int found = 0;
    for (;;) {
      char *m = ws_recv(fd);
      if (!m) return 0;
      int is_eose = strncmp(m, "[\"EOSE\"", 7) == 0;
      if (strncmp(m, "[\"EVENT\"", 8) == 0 && strstr(m, id)) found = 1;
      if (strncmp(m, "[\"CLOSED\"", 9) == 0) {
        fprintf(stderr, "  REQ refused: %s\n", m);
        free(m);
        return 0;
      }
      free(m);
      if (is_eose) break;
    }
    if (found) {
      fprintf(stderr, "  served after %d REQ(s)\n", attempt + 1);
      return 1;
    }
    sleep_ms(50);
  }
  return 0;
}

int main(void) {
  const char *bin = getenv("NOSTR_SESSION_RELAYD");
  if (!bin || !*bin) {
    fprintf(stderr, "NOSTR_SESSION_RELAYD unset; SKIP\n");
    return 77;
  }
  signal(SIGPIPE, SIG_IGN);
  nostr_json_init();

  char xrd[] = "/tmp/nsr-st-rt-XXXXXX";
  char state[] = "/tmp/nsr-st-state-XXXXXX";
  if (!mkdtemp(xrd) || !mkdtemp(state)) {
    perror("mkdtemp");
    return 1;
  }
  chmod(xrd, 0700);
  char sock_path[600];
  snprintf(sock_path, sizeof sock_path, "%s/nostr/relay.sock", xrd);

  /* A real signed event. */
  char *sk = nostr_key_generate_private();
  char *pk = sk ? nostr_key_get_public(sk) : NULL;
  NostrEvent *ev = nostr_event_new();
  CHECK(sk && pk && ev, "key/event setup");
  if (!sk || !pk || !ev) return 1;
  nostr_event_set_pubkey(ev, pk);
  nostr_event_set_kind(ev, 1);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, "session relay storage round-trip");
  CHECK(nostr_event_sign(ev, sk) == 0, "sign");
  char *id = nostr_event_get_id(ev);
  char *ejson = nostr_event_serialize(ev);
  CHECK(id && ejson, "serialize");
  if (!id || !ejson) return 1;
  size_t flen = strlen(ejson) + 16;
  char *frame = malloc(flen);
  snprintf(frame, flen, "[\"EVENT\",%s]", ejson);

  pid_t pid = spawn_daemon(bin, xrd, state);
  CHECK(wait_socket(sock_path) == 0, "socket never appeared");
  long long up = -1;
  int fd = ws_open(sock_path, &up);
  CHECK(fd >= 0, "upgrade failed");
  if (fd >= 0) {
    char ok_prefix[128];
    snprintf(ok_prefix, sizeof ok_prefix, "[\"OK\",\"%s\",true", id);
    expect_reply(fd, frame, ok_prefix, "signed EVENT accepted");
    CHECK(fetch_by_id(fd, id), "stored event not served by REQ");
    /* An empty result still ends in EOSE with storage on. */
    expect_reply(fd, "[\"REQ\",\"none\",{\"kinds\":[31999],\"limit\":1}]",
                 "[\"EOSE\",\"none\"]", "no match -> EOSE");
    close(fd);
  }
  CHECK(stop_daemon(pid) == 0, "daemon did not exit cleanly");

  /* Same store after a restart: the event is on disk. */
  pid = spawn_daemon(bin, xrd, state);
  CHECK(wait_socket(sock_path) == 0, "socket never appeared (restart)");
  fd = ws_open(sock_path, &up);
  CHECK(fd >= 0, "upgrade failed (restart)");
  if (fd >= 0) {
    CHECK(fetch_by_id(fd, id), "event lost across restart");
    close(fd);
  }
  CHECK(stop_daemon(pid) == 0, "daemon did not exit cleanly (restart)");

  free(frame);
  free(ejson);
  free(id);
  nostr_event_free(ev);
  free(pk);
  free(sk);
  if (g_failures) {
    fprintf(stderr, "test_session_relay_storage: %d failure(s) (state kept in %s)\n",
            g_failures, state);
    return 1;
  }
  fprintf(stderr, "test_session_relay_storage: ok\n");
  return 0;
}
