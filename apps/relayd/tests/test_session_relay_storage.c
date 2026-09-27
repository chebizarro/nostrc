/*
 * test_session_relay_storage — the packaged session relay stores events
 * (nostrc-prqu.5).
 *
 * Built only when the relay links the nostrdb driver. Spawns the real
 * daemon with its default config (storage_driver = nostrdb) on private XDG
 * dirs, publishes a signed kind-1 event over relay.sock, reads it back with
 * a REQ by id, then restarts the daemon and reads it again from disk.
 *
 * Addressable events (nostrc-9tdc): every kind 30000-39999 EVENT runs the
 * relay core's "is a newer version stored?" query with a `#d` filter. A
 * kind-30023 article, a newer replacement, an older (stale) version and a
 * kind-31990 NIP-89 handler go through the same daemon, which must answer
 * each, serve the stored ones and still exit cleanly (the query once read
 * a pointer array as a NostrFilter and the daemon died with SIGSEGV).
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
#include "nostr-tag.h"

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

/* REQ @filter_json until an EVENT carrying `id` is served (nostrdb ingests
 * asynchronously). Returns 1 when it arrived, followed by EOSE. */
static int fetch_matching(int fd, const char *filter_json, const char *id) {
  char req[1024];
  for (int attempt = 0; attempt < 40; attempt++) {
    snprintf(req, sizeof req, "[\"REQ\",\"get%d\",%s]", attempt, filter_json);
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

static int fetch_by_id(int fd, const char *id) {
  char filter[128];
  snprintf(filter, sizeof filter, "{\"ids\":[\"%s\"]}", id);
  return fetch_matching(fd, filter, id);
}

/* Signed event; *id_out (free()) gets its id. Returns the EVENT frame. */
static char *signed_frame(const char *sk, const char *pk, int kind, int64_t created_at,
                          NostrTags *tags, const char *content, char **id_out) {
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_pubkey(ev, pk);
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, content);
  if (tags) nostr_event_set_tags(ev, tags);
  CHECK(nostr_event_sign(ev, sk) == 0, "sign kind %d", kind);
  *id_out = nostr_event_get_id(ev);
  char *json = nostr_event_serialize(ev);
  size_t n = strlen(json) + 16;
  char *frame = malloc(n);
  snprintf(frame, n, "[\"EVENT\",%s]", json);
  free(json);
  nostr_event_free(ev);
  return frame;
}

/* nostrc-9tdc: addressable events through the real daemon. */
static void check_addressable(int fd, const char *sk, const char *pk) {
  int64_t t = (int64_t)time(NULL) - 60;
  char *v1, *v2, *v0, *app;
  char *f1 = signed_frame(sk, pk, 30023, t, nostr_tags_new(1, nostr_tag_new("d", "slug", NULL)),
                          "article v1", &v1);
  char *f2 = signed_frame(sk, pk, 30023, t + 10,
                          nostr_tags_new(1, nostr_tag_new("d", "slug", NULL)), "article v2", &v2);
  char *f0 = signed_frame(sk, pk, 30023, t - 10,
                          nostr_tags_new(1, nostr_tag_new("d", "slug", NULL)), "article v0", &v0);
  char *fa = signed_frame(sk, pk, 31990, t,
                          nostr_tags_new(3, nostr_tag_new("d", "handler", NULL),
                                         nostr_tag_new("k", "30311", NULL),
                                         nostr_tag_new("web", "https://app.example/a/<bech32>",
                                                       "naddr", NULL)),
                          "{\"name\":\"app\"}", &app);
  char prefix[160], filter[512];

  snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", v1);
  expect_reply(fd, f1, prefix, "kind 30023 accepted");
  CHECK(fetch_by_id(fd, v1), "kind 30023 not served");

  snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", v2);
  expect_reply(fd, f2, prefix, "newer kind 30023 replaces it");
  snprintf(filter, sizeof filter, "{\"authors\":[\"%s\"],\"kinds\":[30023],\"#d\":[\"slug\"]}",
           pk);
  CHECK(fetch_matching(fd, filter, v2), "replacement not served for #d");

  /* The stale check now finds v2 stored: an older version is refused. */
  snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",false,\"invalid: newer", v0);
  expect_reply(fd, f0, prefix, "older kind 30023 refused as stale");

  snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", app);
  expect_reply(fd, fa, prefix, "kind 31990 accepted");
  CHECK(fetch_matching(fd, "{\"kinds\":[31990],\"#k\":[\"30311\"]}", app),
        "kind 31990 not served for #k");

  char *ids[] = {v1, v2, v0, app, f1, f2, f0, fa};
  for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) free(ids[i]);
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
    check_addressable(fd, sk, pk);
    /* Still serving on the same connection after the addressable writes. */
    CHECK(fetch_by_id(fd, id), "daemon stopped serving after addressable events");
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
