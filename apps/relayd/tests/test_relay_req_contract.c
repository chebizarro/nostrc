/*
 * test_relay_req_contract — the NIP-01 REQ contract of libnostr-relay-server
 * (nostrc-prqu.14), with and without storage.
 *
 * Every REQ must get a terminal reply promptly: EOSE once the stored
 * matches (possibly none) are sent, or CLOSED with a machine-readable
 * reason when the relay refuses it. The server runs in-process on a Unix
 * listener, first with no storage at all, then with a scripted fake
 * storage covering every query outcome the vtable allows.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#include "ws_test_client.h"

#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json.h"
#include "nostr-relay-server.h"
#include "nostr-storage.h"
#include "relayd_config.h"

/* ---- scripted fake storage ------------------------------------------- */

typedef enum {
  FAKE_EMPTY_ITER, /* iterator that yields nothing */
  FAKE_NULL_OK,    /* NULL iterator, err == 0: "no results" */
  FAKE_NULL_ERR,   /* NULL iterator, err != 0: the store failed */
  FAKE_ONE_EVENT,  /* iterator that yields one event */
} FakeMode;

static struct {
  FakeMode mode;
  int queries;
  int frees;
  size_t last_nfilters;
  int last_kinds[4]; /* first kind of each filter the store was handed */
} g_fake;

typedef struct { int remaining; } FakeIter;

static const char *k_event_json =
    "{\"id\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"pubkey\":\"2222222222222222222222222222222222222222222222222222222222222222\","
    "\"created_at\":1700000000,\"kind\":1,\"tags\":[],\"content\":\"hi\","
    "\"sig\":\"33333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333\"}";

static void *fake_query(NostrStorage *st, const NostrFilter *filters,
                        size_t nfilters, size_t limit, uint64_t since,
                        uint64_t until, int *err) {
  (void)st; (void)limit; (void)since; (void)until;
  g_fake.queries++;
  g_fake.last_nfilters = nfilters;
  for (size_t i = 0; i < nfilters && i < 4; i++)
    g_fake.last_kinds[i] = nostr_filter_kinds_len(&filters[i])
                               ? nostr_filter_kinds_get(&filters[i], 0)
                               : -1;
  *err = 0;
  switch (g_fake.mode) {
    case FAKE_NULL_OK: return NULL;
    case FAKE_NULL_ERR: *err = -5; return NULL;
    case FAKE_EMPTY_ITER:
    case FAKE_ONE_EVENT: {
      FakeIter *it = calloc(1, sizeof *it);
      if (it) it->remaining = g_fake.mode == FAKE_ONE_EVENT ? 1 : 0;
      return it;
    }
  }
  return NULL;
}

static int fake_query_next(NostrStorage *st, void *itp, NostrEvent *out,
                           size_t *n) {
  (void)st;
  FakeIter *it = itp;
  if (it->remaining <= 0) { *n = 0; return 0; }
  it->remaining--;
  if (nostr_event_deserialize(out, k_event_json) != 0) { *n = 0; return -5; }
  *n = 1;
  return 0;
}

static void fake_query_free(NostrStorage *st, void *it) {
  (void)st;
  g_fake.frees++;
  free(it);
}

static NostrStorageVTable g_fake_vt = {
  .query = fake_query,
  .query_next = fake_query_next,
  .query_free = fake_query_free,
};

/* ---- in-process server ------------------------------------------------ */

typedef struct {
  NostrRelayServerConfig scfg;
  RelaydConfig cfg;
  volatile int stop;
  pthread_t thread;
  char sock_path[108];
  int listen_fd;
} Server;

static void *server_main(void *arg) {
  Server *s = arg;
  nostr_relay_server_run(&s->scfg);
  return NULL;
}

static int server_start(Server *s, NostrStorage *storage, const char *dir,
                        int max_subs) {
  memset(s, 0, sizeof *s);
  if (relayd_config_load(NULL, &s->cfg) != 0) return -1;
  s->cfg.max_subs = max_subs;
  snprintf(s->sock_path, sizeof s->sock_path, "%s/relay.sock", dir);
  unlink(s->sock_path);
  s->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", s->sock_path);
  if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
      listen(s->listen_fd, 8) != 0)
    return -1;
  s->scfg.cfg = &s->cfg;
  s->scfg.storage = storage;
  s->scfg.stop_flag = &s->stop;
  s->scfg.listener.kind = NOSTR_RELAY_LISTENER_UNIX_FD;
  s->scfg.listener.u.unix_fd.fd = s->listen_fd;
  return pthread_create(&s->thread, NULL, server_main, s);
}

static void server_stop(Server *s) {
  s->stop = 1; /* observed within LOOP_WAKE_US (1 s) */
  pthread_join(s->thread, NULL);
  close(s->listen_fd);
  unlink(s->sock_path);
}

static int connect_client(Server *s) {
  long long upgrade_ms = -1;
  int fd = ws_open(s->sock_path, &upgrade_ms);
  CHECK(fd >= 0, "WebSocket upgrade failed");
  return fd;
}

/* ---- scenarios ---------------------------------------------------------- */

static void run_storage_less(const char *dir) {
  fprintf(stderr, "== no storage\n");
  Server s;
  CHECK(server_start(&s, NULL, dir, 8) == 0, "server start");
  int fd = connect_client(&s);
  if (fd >= 0) {
    expect_reply(fd, "[\"REQ\",\"a\",{\"kinds\":[1],\"limit\":1}]",
                 "[\"EOSE\",\"a\"]", "plain REQ -> EOSE");
    expect_reply(fd, "[\"REQ\",\"b\",{\"kinds\":[1]},{\"authors\":[\"ab\"]}]",
                 "[\"EOSE\",\"b\"]", "multi-filter REQ -> EOSE");
    expect_reply(fd, "[\"REQ\",\"c\",{\"search\":\"hello\"}]",
                 "[\"CLOSED\",\"c\",\"unsupported:", "search -> CLOSED unsupported");
    expect_reply(fd, "[\"REQ\",\"d\"]", "[\"CLOSED\",\"d\",\"invalid:",
                 "REQ without filter -> CLOSED invalid");
    expect_reply(fd, "[\"REQ\",\"e\",{\"kinds\":\"nope\"}]",
                 "[\"CLOSED\",\"e\",\"invalid:", "malformed filter -> CLOSED invalid");
    expect_reply(fd, "[\"REQ\",\"f\",42]", "[\"CLOSED\",\"f\",\"invalid:",
                 "non-object filter -> CLOSED invalid");
    expect_reply(fd, "[\"REQ\",\"w\",{\"kinds\":[1]} ]", "[\"EOSE\",\"w\"]",
                 "space before closing bracket -> EOSE");
    expect_reply(fd, "[\"REQ\",{\"kinds\":[1]}]", "[\"NOTICE\",\"invalid:",
                 "REQ without subscription id -> NOTICE");
    /* 65 chars: one over NIP-01's limit; the CLOSED echoes the full id. */
    expect_reply(fd,
                 "[\"REQ\",\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",{}]",
                 "[\"CLOSED\",\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"invalid:",
                 "65-char subscription id -> CLOSED invalid");
    expect_reply(fd, "[\"COUNT\",\"g\",{\"kinds\":[1]}]",
                 "[\"CLOSED\",\"g\",\"unsupported:", "COUNT -> CLOSED unsupported");
    close(fd);
  }
  server_stop(&s);
}

static void run_with_storage(const char *dir) {
  fprintf(stderr, "== fake storage\n");
  NostrStorage st = { .vt = &g_fake_vt, .impl = NULL };
  Server s;
  memset(&g_fake, 0, sizeof g_fake);
  CHECK(server_start(&s, &st, dir, 2) == 0, "server start");
  int fd = connect_client(&s);
  if (fd >= 0) {
    g_fake.mode = FAKE_EMPTY_ITER;
    expect_reply(fd, "[\"REQ\",\"a\",{\"kinds\":[1]}]", "[\"EOSE\",\"a\"]",
                 "empty result -> EOSE");

    g_fake.mode = FAKE_NULL_OK;
    expect_reply(fd, "[\"REQ\",\"b\",{\"kinds\":[1]}]", "[\"EOSE\",\"b\"]",
                 "NULL iterator without error -> EOSE");

    g_fake.mode = FAKE_NULL_ERR;
    expect_reply(fd, "[\"REQ\",\"c\",{\"kinds\":[1]}]",
                 "[\"CLOSED\",\"c\",\"error:", "query error -> CLOSED error");

    g_fake.mode = FAKE_ONE_EVENT;
    expect_reply(fd, "[\"REQ\",\"d\",{\"kinds\":[1]}]", "[\"EVENT\",\"d\",",
                 "stored match -> EVENT");
    char *eose = ws_recv(fd);
    CHECK(eose && strcmp(eose, "[\"EOSE\",\"d\"]") == 0,
          "EVENT then EOSE, got %s", eose ? eose : "(nothing)");
    free(eose);

    /* NIP-01 multi-filter REQ: every filter reaches the store as one
     * contiguous array (was: arr[0] reinterpreted as an array). */
    g_fake.mode = FAKE_EMPTY_ITER;
    expect_reply(fd,
                 "[\"REQ\",\"m\",{\"kinds\":[7],\"#t\":[\"}{\"]},{\"kinds\":[30023]}]",
                 "[\"EOSE\",\"m\"]", "two-filter REQ -> EOSE");
    CHECK(g_fake.last_nfilters == 2, "store saw %zu filters, want 2",
          g_fake.last_nfilters);
    CHECK(g_fake.last_kinds[0] == 7 && g_fake.last_kinds[1] == 30023,
          "store saw kinds %d,%d, want 7,30023", g_fake.last_kinds[0],
          g_fake.last_kinds[1]);

    /* Pipelined REQs are queued and served in order (max_subs = 2 here);
     * one past the limit is refused with CLOSED "rate-limited:". */
    unsigned char *f1 = NULL, *f2 = NULL, *f3 = NULL;
    size_t n1 = ws_encode("[\"REQ\",\"p\",{\"kinds\":[1]}]", &f1);
    size_t n2 = ws_encode("[\"REQ\",\"q\",{\"kinds\":[1]}]", &f2);
    size_t n3 = ws_encode("[\"REQ\",\"o\",{\"kinds\":[1]}]", &f3);
    unsigned char both3[512];
    CHECK(n1 && n2 && n3 && n1 + n2 + n3 <= sizeof both3, "encode p/q/o");
    memcpy(both3, f1, n1);
    memcpy(both3 + n1, f2, n2);
    memcpy(both3 + n1 + n2, f3, n3);
    CHECK(write_all(fd, both3, n1 + n2 + n3) == 0, "send p/q/o");
    free(f1); free(f2); free(f3);
    char *r[3];
    for (int i = 0; i < 3; i++) r[i] = ws_recv(fd);
    fprintf(stderr, "  pipelined REQs: %s | %s | %s\n", r[0] ? r[0] : "-",
            r[1] ? r[1] : "-", r[2] ? r[2] : "-");
    CHECK(r[0] && strncmp(r[0], "[\"CLOSED\",\"o\",\"rate-limited:", 28) == 0,
          "third pipelined REQ not refused: %s", r[0] ? r[0] : "(nothing)");
    CHECK(r[1] && strcmp(r[1], "[\"EOSE\",\"p\"]") == 0, "p: %s",
          r[1] ? r[1] : "(nothing)");
    CHECK(r[2] && strcmp(r[2], "[\"EOSE\",\"q\"]") == 0, "q: %s",
          r[2] ? r[2] : "(nothing)");
    for (int i = 0; i < 3; i++) free(r[i]);

    /* Reusing the in-flight id replaces the subscription (NIP-01): the
     * first iterator is freed, and exactly one EOSE follows. */
    CHECK(ws_send_pair(fd, "[\"REQ\",\"r\",{\"kinds\":[1]}]",
                       "[\"REQ\",\"r\",{\"kinds\":[2]}]") == 0,
          "send pair r/r");
    char *r3 = ws_recv(fd);
    CHECK(r3 && strcmp(r3, "[\"EOSE\",\"r\"]") == 0,
          "replaced REQ: %s", r3 ? r3 : "(nothing)");
    free(r3);
    CHECK(g_fake.last_kinds[0] == 2, "replacement filter not used");
    /* Nothing else is pending: a fresh REQ's EOSE is the very next frame. */
    expect_reply(fd, "[\"REQ\",\"t\",{\"kinds\":[1]}]", "[\"EOSE\",\"t\"]",
                 "no duplicate EOSE after replacement");
    close(fd);
  }
  server_stop(&s);
  CHECK(g_fake.frees == 8, "iterators freed %d, want 8 (a d m p q r r t)",
        g_fake.frees);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  nostr_json_init();
  char dir[] = "/tmp/relay-req-contract-XXXXXX";
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  run_storage_less(dir);
  run_with_storage(dir);
  rmdir(dir);
  if (g_failures) {
    fprintf(stderr, "test_relay_req_contract: %d failure(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "test_relay_req_contract: ok\n");
  return 0;
}
