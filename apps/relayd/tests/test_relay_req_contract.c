/*
 * test_relay_req_contract — the NIP-01 REQ contract of libnostr-relay-server
 * (nostrc-prqu.14), with and without storage.
 *
 * Every REQ must reach a catch-up boundary promptly: EOSE once the stored
 * matches (possibly none) are sent, or CLOSED with a machine-readable
 * reason when the relay refuses it. Successful subscriptions remain live. The server runs in-process on a Unix
 * listener, first with no storage at all, then with a scripted fake
 * storage covering every query outcome the vtable allows, then real
 * nostrdb when available. Live delivery uses a second publishing connection.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <openssl/rand.h>

#include "ws_test_client.h"

#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json.h"
#include "nostr-keys.h"
#include "nostr-relay-server.h"
#include "nostr-storage.h"
#include "relayd_config.h"
#include "relay_policy.h"
#include "protocol_nip01.h"
#ifdef HAVE_TEST_NOSTRDB
#include "nostrdb_storage.h"
#endif

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
  char last_author[65]; /* first author of the first filter */
  int puts;
  void (*on_query)(void);
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
  if (g_fake.on_query) {
    void (*callback)(void) = g_fake.on_query;
    g_fake.on_query = NULL;
    callback();
  }
  g_fake.last_nfilters = nfilters;
  for (size_t i = 0; i < nfilters && i < 4; i++)
    g_fake.last_kinds[i] = nostr_filter_kinds_len(&filters[i])
                               ? nostr_filter_kinds_get(&filters[i], 0)
                               : -1;
  g_fake.last_author[0] = '\0';
  if (nfilters > 0 && nostr_filter_authors_len(&filters[0]) > 0)
    snprintf(g_fake.last_author, sizeof g_fake.last_author, "%s",
             nostr_filter_authors_get(&filters[0], 0));
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

static int fake_put_event(NostrStorage *st, const NostrEvent *ev) {
  (void)st; (void)ev;
  g_fake.puts++;
  return 0;
}

static NostrStorageVTable g_fake_vt = {
  .put_event = fake_put_event,
  .query = fake_query,
  .query_next = fake_query_next,
  .query_free = fake_query_free,
};

/* Commit-notification fixture. Its synchronous vtable put blocks until the
 * test releases it, making the old service-loop implementation fail client
 * B's latency bound. The asynchronous enqueue never waits. */
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned char ids[128][32];
  int queued;
  int committed;
  int release_sync;
  void (*notify)(void *);
  void *notify_ctx;
} AsyncFake;
static AsyncFake g_async = {
  .mutex = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER,
};

static int async_fake_put(NostrStorage *st, const NostrEvent *ev) {
  (void)st; (void)ev;
  pthread_mutex_lock(&g_async.mutex);
  g_async.queued++;
  pthread_cond_broadcast(&g_async.cond);
  while (!g_async.release_sync)
    pthread_cond_wait(&g_async.cond, &g_async.mutex);
  pthread_mutex_unlock(&g_async.mutex);
  return 0;
}

static int async_fake_start(NostrStorage *st, void (*notify)(void *), void *ctx) {
  (void)st;
  pthread_mutex_lock(&g_async.mutex);
  g_async.notify = notify;
  g_async.notify_ctx = ctx;
  pthread_mutex_unlock(&g_async.mutex);
  return 0;
}

static void async_fake_stop(NostrStorage *st) {
  (void)st;
  pthread_mutex_lock(&g_async.mutex);
  g_async.notify = NULL;
  g_async.notify_ctx = NULL;
  pthread_mutex_unlock(&g_async.mutex);
}

static int async_fake_enqueue(NostrStorage *st, const NostrEvent *ev) {
  (void)st;
  char *hex = nostr_event_get_id((NostrEvent*)ev);
  if (!hex) return -1;
  pthread_mutex_lock(&g_async.mutex);
  int ok = g_async.queued < 128 &&
           relay_policy_hex_to_id(hex, g_async.ids[g_async.queued]);
  if (ok) g_async.queued++;
  pthread_cond_broadcast(&g_async.cond);
  pthread_mutex_unlock(&g_async.mutex);
  free(hex);
  return ok ? 0 : -1;
}

static int async_fake_visible(NostrStorage *st, const unsigned char id[32]) {
  (void)st;
  pthread_mutex_lock(&g_async.mutex);
  int found = 0;
  for (int i = 0; i < g_async.committed; i++)
    if (memcmp(id, g_async.ids[i], 32) == 0) { found = 1; break; }
  pthread_mutex_unlock(&g_async.mutex);
  return found;
}

static const RelaydAsyncStorageOps g_async_ops = {
  .start = async_fake_start,
  .stop = async_fake_stop,
  .enqueue = async_fake_enqueue,
  .visible = async_fake_visible,
};
static NostrStorageVTable g_async_vt = {
  .put_event = async_fake_put,
  .query = fake_query,
  .query_next = fake_query_next,
  .query_free = fake_query_free,
};

static int async_wait_queued(int target) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 3;
  pthread_mutex_lock(&g_async.mutex);
  int rc = 0;
  while (g_async.queued < target && rc == 0)
    rc = pthread_cond_timedwait(&g_async.cond, &g_async.mutex, &deadline);
  int ok = g_async.queued >= target;
  pthread_mutex_unlock(&g_async.mutex);
  return ok;
}

static void async_commit_all(void) {
  pthread_mutex_lock(&g_async.mutex);
  g_async.committed = g_async.queued;
  void (*notify)(void *) = g_async.notify;
  void *ctx = g_async.notify_ctx;
  pthread_mutex_unlock(&g_async.mutex);
  if (notify) notify(ctx);
}

/* Invoked by query() on the service thread: queue the whole live batch
 * before this REQ installs its backfill, making scheduling fairness exact. */
static void commit_during_query(void) {
  async_commit_all();
  relayd_nip01_reconcile_all(lws_context_user(g_async.notify_ctx));
}

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

static int server_start_config(Server *s, NostrStorage *storage, const char *dir,
                        int max_subs, const RelaydAsyncStorageOps *async_ops,
                        unsigned int ack_timeout_ms, const RelaydConfig *cfg) {
  memset(s, 0, sizeof *s);
  if (relayd_config_load(NULL, &s->cfg) != 0) return -1;
  if (cfg) s->cfg = *cfg;
  s->cfg.max_subs = max_subs;
  s->cfg.rate_ops_per_sec = 10000;
  s->cfg.rate_burst = 10000;
  s->cfg.verification_conn_per_sec = 10000;
  s->cfg.verification_conn_burst = 10000;
  s->cfg.verification_ip_per_sec = 10000;
  s->cfg.verification_ip_burst = 10000;
  s->cfg.verification_global_per_sec = 10000;
  s->cfg.verification_global_burst = 10000;
  snprintf(s->sock_path, sizeof s->sock_path, "%s/relay.sock", dir);
  unlink(s->sock_path);
  s->listen_fd = unix_socket_cloexec();
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", s->sock_path);
  if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
      listen(s->listen_fd, 8) != 0)
    return -1;
  s->scfg.cfg = &s->cfg;
  s->scfg.storage = storage;
  s->scfg.async_storage = async_ops;
  s->scfg.ack_timeout_ms = ack_timeout_ms;
  s->scfg.stop_flag = &s->stop;
  s->scfg.listener.kind = NOSTR_RELAY_LISTENER_UNIX_FD;
  s->scfg.listener.u.unix_fd.fd = s->listen_fd;
  return pthread_create(&s->thread, NULL, server_main, s);
}

static int server_start(Server *s, NostrStorage *storage, const char *dir,
                        int max_subs, const RelaydAsyncStorageOps *async_ops,
                        unsigned int ack_timeout_ms) {
  return server_start_config(s, storage, dir, max_subs, async_ops,
                             ack_timeout_ms, NULL);
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

/* OpenSSL's test-only RAND method injects a partial output plus failure.
 * No production fault switch or weak RNG fallback is exposed. */
static const RAND_METHOD *saved_rand;
static int fail_challenge_rng;
static int test_rand_bytes(unsigned char *buf, int n) {
  if (fail_challenge_rng && n == 16) {
    memset(buf, 0x42, n / 2);
    return 0;
  }
  return saved_rand->bytes(buf, n);
}

static void run_rng_failure(const char *dir) {
  fprintf(stderr, "== challenge RNG failure\n");
  saved_rand = RAND_get_rand_method();
  RAND_METHOD method = *saved_rand;
  method.bytes = test_rand_bytes;
  CHECK(RAND_set_rand_method(&method) == 1, "install RNG fault");
  RelaydConfig cfg;
  CHECK(relayd_config_load(NULL, &cfg) == 0, "load config");
  snprintf(cfg.auth, sizeof cfg.auth, "%s", "required");
  fail_challenge_rng = 1;
  Server s;
  CHECK(server_start_config(&s, NULL, dir, 8, NULL, 0, &cfg) == 0,
        "server start");
  long long upgrade_ms = -1;
  int fd = ws_open(s.sock_path, &upgrade_ms);
  if (fd >= 0) {
    char *reply = ws_recv(fd);
    CHECK(reply == NULL, "RNG failure must close, never issue AUTH");
    free(reply);
    close(fd);
  }
  server_stop(&s);
  fail_challenge_rng = 0;
  CHECK(RAND_set_rand_method(saved_rand) == 1, "restore RNG");
  /* A healthy RNG must still issue a full challenge on the real path. */
  CHECK(server_start_config(&s, NULL, dir, 8, NULL, 0, &cfg) == 0,
        "server start healthy RNG");
  fd = connect_client(&s);
  if (fd >= 0) {
    char *reply = ws_recv(fd);
    CHECK(reply && strncmp(reply, "[\"AUTH\",\"", 9) == 0 && strlen(reply) == 43,
          "healthy RNG issues 128-bit challenge");
    free(reply);
    close(fd);
  }
  server_stop(&s);
}

static void run_storage_less(const char *dir) {
  fprintf(stderr, "== no storage\n");
  Server s;
  CHECK(server_start(&s, NULL, dir, 8, NULL, 0) == 0, "server start");
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
  CHECK(server_start(&s, &st, dir, 2, NULL, 0) == 0, "server start");
  int fd = connect_client(&s);
  if (fd >= 0) {
    g_fake.mode = FAKE_EMPTY_ITER;
    expect_reply(fd, "[\"REQ\",\"a\",{\"kinds\":[1]}]", "[\"EOSE\",\"a\"]",
                 "empty result -> EOSE");

    g_fake.mode = FAKE_NULL_OK;
    expect_reply(fd, "[\"REQ\",\"b\",{\"kinds\":[1]}]", "[\"EOSE\",\"b\"]",
                 "NULL iterator without error -> EOSE");

    expect_reply(fd, "[\"REQ\",\"full\",{\"kinds\":[1]}]",
                 "[\"CLOSED\",\"full\",\"rate-limited:", "EOSE subscriptions still count toward max_subs");
    CHECK(ws_send(fd, "[\"CLOSE\",\"a\"]") == 0, "close live a");
    CHECK(ws_send(fd, "[\"CLOSE\",\"b\"]") == 0, "close live b");
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
    CHECK(ws_send(fd, "[\"CLOSE\",\"d\"]") == 0, "close live d");

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

    CHECK(ws_send(fd, "[\"CLOSE\",\"m\"]") == 0, "close live m");
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

    CHECK(ws_send(fd, "[\"CLOSE\",\"p\"]") == 0, "close live p");
    CHECK(ws_send(fd, "[\"CLOSE\",\"q\"]") == 0, "close live q");
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
    CHECK(ws_send(fd, "[\"CLOSE\",\"r\"]") == 0, "close live r");
    /* Nothing else is pending: a fresh REQ's EOSE is the very next frame. */
    expect_reply(fd, "[\"REQ\",\"t\",{\"kinds\":[1]}]", "[\"EOSE\",\"t\"]",
                 "no duplicate EOSE after replacement");

    /* Replaceable EVENT: the stale-version check hands the store one real
     * filter (kind + author) -- it used to pass an array of pointers
     * reinterpreted as a filter array. An older stored version (the fake
     * yields created_at 1700000000) does not block the new one. */
    g_fake.mode = FAKE_ONE_EVENT;
    char *sk = nostr_key_generate_private();
    char *pk = sk ? nostr_key_get_public(sk) : NULL;
    NostrEvent *ev = nostr_event_new();
    CHECK(sk && pk && ev, "key/event setup");
    if (sk && pk && ev) {
      nostr_event_set_pubkey(ev, pk);
      nostr_event_set_kind(ev, 0);
      nostr_event_set_created_at(ev, (int64_t)time(NULL));
      nostr_event_set_content(ev, "{}");
      CHECK(nostr_event_sign(ev, sk) == 0, "sign");
      char *ej = nostr_event_serialize(ev);
      char *id = nostr_event_get_id(ev);
      char frame[2048], prefix[128];
      snprintf(frame, sizeof frame, "[\"EVENT\",%s]", ej);
      snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", id);
      int q0 = g_fake.queries;
      expect_reply(fd, frame, prefix, "replaceable EVENT -> OK true");
      CHECK(g_fake.queries == q0 + 2, "duplicate and stale checks query the store");
      CHECK(g_fake.last_nfilters == 1 && g_fake.last_kinds[0] == 0,
            "stale check filter: n=%zu kind=%d", g_fake.last_nfilters,
            g_fake.last_kinds[0]);
      CHECK(strcmp(g_fake.last_author, pk) == 0, "stale check author: %s",
            g_fake.last_author);
      CHECK(g_fake.puts == 1, "event not stored (%d puts)", g_fake.puts);
      free(ej);
      free(id);
    }
    nostr_event_free(ev);
    free(pk);
    free(sk);
    close(fd);
  }
  server_stop(&s);
  CHECK(g_fake.frees == 10, "iterators freed %d, want 10 (a d m p q r r t + duplicate/stale checks)",
        g_fake.frees);
}

static char *signed_event_frame(const char *sk, const char *pk,
                                int sequence, char **id_out) {
  NostrEvent *ev = nostr_event_new();
  if (!ev) return NULL;
  char content[64];
  snprintf(content, sizeof content, "async-ack-%d", sequence);
  nostr_event_set_pubkey(ev, pk);
  nostr_event_set_kind(ev, 1);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, content);
  if (nostr_event_sign(ev, sk) != 0) { nostr_event_free(ev); return NULL; }
  *id_out = nostr_event_get_id(ev);
  char *json = nostr_event_serialize(ev);
  nostr_event_free(ev);
  if (!json || !*id_out) { free(json); return NULL; }
  char *frame = malloc(strlen(json) + 16);
  if (frame) sprintf(frame, "[\"EVENT\",%s]", json);
  free(json);
  return frame;
}

static void barrier(int fd);
static void expect_event(int fd, const char *sub, const char *id);

static void run_async_commit(const char *dir) {
  fprintf(stderr, "== async commit/timeout and independent client\n");
  pthread_mutex_lock(&g_async.mutex);
  g_async.queued = g_async.committed = g_async.release_sync = 0;
  pthread_mutex_unlock(&g_async.mutex);
  memset(&g_fake, 0, sizeof g_fake);
  g_fake.mode = FAKE_EMPTY_ITER;
  NostrStorage st = { .vt = &g_async_vt, .impl = NULL };
  Server s;
  CHECK(server_start(&s, &st, dir, 8, &g_async_ops, 2000) == 0,
        "async server start");
  int a = connect_client(&s);
  int b = connect_client(&s);
  char *sk = nostr_key_generate_private();
  char *pk = sk ? nostr_key_get_public(sk) : NULL;
  CHECK(a >= 0 && b >= 0 && sk && pk, "async fixture setup");
  if (a >= 0 && b >= 0 && sk && pk) {
    expect_reply(b, "[\"REQ\",\"live\",{\"kinds\":[1]}]",
                 "[\"EOSE\",\"live\"]", "async live catch up");
    char *id = NULL;
    char *frame = signed_event_frame(sk, pk, 0, &id);
    CHECK(frame && ws_send(a, frame) == 0, "send stalled EVENT A");
    CHECK(async_wait_queued(1), "EVENT A was not queued");
    /* A pending OK must not hold up REQ/EOSE even on the same socket. */
    CHECK(ws_send(a, "[\"REQ\",\"same\",{\"kinds\":[424242]}]") == 0,
          "send same-client REQ");
    char *same_reply = ws_recv(a);
    CHECK(same_reply && strcmp(same_reply, "[\"EOSE\",\"same\"]") == 0,
          "same-client REQ blocked behind pending OK: %s",
          same_reply ? same_reply : "(no reply)");
    free(same_reply);
    long long t0 = now_ms();
    CHECK(ws_send(b, "[\"REQ\",\"b\",{\"kinds\":[424242]}]") == 0,
          "send independent REQ B");
    char *reply = ws_recv(b);
    long long elapsed = now_ms() - t0;
    CHECK(reply && strcmp(reply, "[\"EOSE\",\"b\"]") == 0,
          "client B did not receive EOSE while A was stalled: %s",
          reply ? reply : "(no reply)");
    CHECK(elapsed < 500, "client B was delayed %lld ms by A's commit", elapsed);
    fprintf(stderr, "  client B EOSE while A's commit is stalled: %lld ms\n", elapsed);
    free(reply);
    /* Also releases the old blocking put_event path in the red check. */
    pthread_mutex_lock(&g_async.mutex);
    g_async.release_sync = 1;
    pthread_cond_broadcast(&g_async.cond);
    pthread_mutex_unlock(&g_async.mutex);
    async_commit_all();
    reply = ws_recv(a);
    char prefix[128];
    snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", id);
    CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0,
          "commit did not yield OK true: %s", reply ? reply : "(no reply)");
    free(reply);
    expect_event(b, "live", id);
    barrier(b);
    free(frame); free(id);

    frame = signed_event_frame(sk, pk, 1, &id);
    long long timeout_start = now_ms();
    CHECK(frame && ws_send(a, frame) == 0, "send timeout EVENT A");
    CHECK(async_wait_queued(2), "timeout EVENT was not queued");
    t0 = now_ms();
    CHECK(ws_send(b, "[\"REQ\",\"timeout-b\",{\"kinds\":[424242]}]") == 0,
          "send independent REQ during absent commit notification");
    reply = ws_recv(b);
    elapsed = now_ms() - t0;
    CHECK(reply && strcmp(reply, "[\"EOSE\",\"timeout-b\"]") == 0,
          "client B lost EOSE during commit timeout: %s",
          reply ? reply : "(no reply)");
    CHECK(elapsed < 500, "client B was delayed %lld ms by missing notification", elapsed);
    free(reply);
    reply = ws_recv(a);
    long long timeout_elapsed = now_ms() - timeout_start;
    snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",false,\"error:", id);
    CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0,
          "deadline did not yield OK false error: %s",
          reply ? reply : "(no reply)");
    CHECK(timeout_elapsed >= 1900 && timeout_elapsed < 3000,
          "2-second commit deadline took %lld ms", timeout_elapsed);
    fprintf(stderr, "  absent notification: client B EOSE %lld ms, OK false %lld ms\n",
            elapsed, timeout_elapsed);
    free(reply); free(frame); free(id);
    barrier(b); /* timed-out storage did not fan out */

    enum { BURST = 100 };
    char *frames[BURST], *ids[BURST];
    for (int i = 0; i < BURST; i++) {
      ids[i] = NULL;
      frames[i] = signed_event_frame(sk, pk, i + 2, &ids[i]);
      CHECK(frames[i] && ids[i], "sign burst EVENT %d", i);
    }
    long long burst_start = now_ms();
    for (int i = 0; i < BURST; i++)
      CHECK(frames[i] && ws_send(a, frames[i]) == 0, "send burst EVENT %d", i);
    CHECK(async_wait_queued(BURST + 2), "burst was not queued");
    long long queued_ms = now_ms() - burst_start;
    g_fake.on_query = commit_during_query;
    CHECK(ws_send(b, "[\"REQ\",\"fair\",{\"kinds\":[7]}]") == 0,
          "start backfill behind queued live traffic");
    for (int i = 0; i < BURST; i++) {
      reply = ws_recv(a);
      snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", ids[i]);
      CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0,
            "burst OK %d out of order: %s", i,
            reply ? reply : "(no reply)");
      free(reply);
      free(frames[i]);
    }
    int delivered = 0, got_eose = 0;
    while (delivered < BURST || !got_eose) {
      reply = ws_recv(b);
      CHECK(reply, "fair scheduler reply");
      if (!reply) break;
      if (strcmp(reply, "[\"EOSE\",\"fair\"]") == 0) {
        CHECK(!got_eose && delivered < BURST, "catch-up starved behind all live frames");
        got_eose = 1;
      } else {
        CHECK(delivered < BURST && strncmp(reply, "[\"EVENT\",\"live\",", 16) == 0 &&
              strstr(reply, ids[delivered]), "ordered live frame during fair catch-up: %s", reply);
        delivered++;
      }
      free(reply);
    }
    for (int i = 0; i < BURST; i++) free(ids[i]);
    fprintf(stderr, "  async burst %d: queued in %lld ms, all ordered OKs in %lld ms\n",
            BURST, queued_ms, now_ms() - burst_start);
    frame = signed_event_frame(sk, pk, 1000, &id);
    CHECK(frame && ws_send(a, frame) == 0, "send disconnecting publisher EVENT");
    CHECK(async_wait_queued(BURST + 3), "disconnect EVENT queued");
    shutdown(a, SHUT_WR);
    reply = ws_recv(a);
    CHECK(!reply, "publisher disconnected before commit");
    free(reply);
    close(a);
    async_commit_all();
    expect_event(b, "live", id);
    barrier(b);
    free(frame); free(id);
    close(b);
  }
  free(pk); free(sk);
  server_stop(&s);
}

static void run_live_overflow(const char *dir) {
  fprintf(stderr, "== bounded live queue overflow disconnect\n");
  pthread_mutex_lock(&g_async.mutex);
  g_async.queued = g_async.committed = g_async.release_sync = 0;
  pthread_mutex_unlock(&g_async.mutex);
  memset(&g_fake, 0, sizeof g_fake);
  g_fake.mode = FAKE_EMPTY_ITER;
  NostrStorage st = { .vt = &g_async_vt };
  Server s;
  CHECK(server_start(&s, &st, dir, 16, &g_async_ops, 5000) == 0, "overflow server start");
  int sub = connect_client(&s), pub = connect_client(&s);
  for (int i = 0; i < 16; i++) {
    char req[128], eose[64];
    snprintf(req, sizeof req, "[\"REQ\",\"s%d\",{\"kinds\":[1]}]", i);
    snprintf(eose, sizeof eose, "[\"EOSE\",\"s%d\"]", i);
    expect_reply(sub, req, eose, "overflow subscription EOSE");
  }
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *ids[20];
  for (int i = 0; i < 20; i++) {
    char *frame = signed_event_frame(sk, pk, i, &ids[i]);
    CHECK(frame && ws_send(pub, frame) == 0, "queue overflow event");
    free(frame);
  }
  CHECK(async_wait_queued(20), "overflow batch queued");
  /* One service-thread reconciliation creates 320 deliveries before any
   * writable callback: deterministically exceeds the 256-frame bound. */
  async_commit_all();
  char *reply = ws_recv(sub);
  CHECK(!reply, "overflow must disconnect instead of silently dropping events: %s",
        reply ? reply : "(closed)");
  free(reply);
  for (int i = 0; i < 20; i++) {
    char prefix[160];
    snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", ids[i]);
    reply = ws_recv(pub);
    CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0, "publisher OK during subscriber overflow");
    free(reply); free(ids[i]);
  }
  free(pk); free(sk);
  close(sub); close(pub);
  server_stop(&s);
}

/* All absence assertions use a same-socket EOSE barrier, not a quiet-period
 * timeout. The EVENT's OK first establishes storage/fanout completion. */
static void barrier(int fd) {
  expect_reply(fd, "[\"REQ\",\"barrier\",{\"kinds\":[65535],\"limit\":0}]",
               "[\"EOSE\",\"barrier\"]", "no stray EVENT before barrier");
  CHECK(ws_send(fd, "[\"CLOSE\",\"barrier\"]") == 0, "close barrier");
}

static char *make_frame(const char *verb, const char *sk, const char *pk,
                        int kind, int seq, const char *tags, char **id) {
  char json[2048];
  snprintf(json, sizeof json,
           "{\"pubkey\":\"%s\",\"created_at\":%lld,\"kind\":%d,\"tags\":%s,\"content\":\"live-%d\"}",
           pk, (long long)time(NULL) - 100 + seq, kind, tags ? tags : "[]", seq);
  NostrEvent *ev = nostr_event_new();
  CHECK(ev && nostr_event_deserialize(ev, json) == 0, "build live event");
  CHECK(ev && nostr_event_sign(ev, sk) == 0, "sign live event");
  *id = ev ? nostr_event_get_id(ev) : NULL;
  char *serialized = ev ? nostr_event_serialize(ev) : NULL;
  nostr_event_free(ev);
  CHECK(serialized && *id, "serialize live event");
  if (!serialized) return NULL;
  size_t need = strlen(serialized) + 32;
  char *frame = malloc(need);
  if (frame) snprintf(frame, need, "[\"%s\",%s]", verb, serialized);
  free(serialized);
  return frame;
}

static void expect_event(int fd, const char *sub, const char *id) {
  char prefix[128];
  snprintf(prefix, sizeof prefix, "[\"EVENT\",\"%s\",", sub);
  char *reply = ws_recv(fd);
  CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0 && strstr(reply, id),
        "live EVENT %s/%s: %s", sub, id, reply ? reply : "(none)");
  free(reply);
}

static void publish(int fd, const char *frame, const char *id, int accepted) {
  char prefix[160];
  snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",%s", id, accepted ? "true" : "false");
  expect_reply(fd, frame, prefix, "publish storage result");
}

static void run_live(const char *dir, NostrStorage *st,
                     const RelaydAsyncStorageOps *ops) {
  Server s;
  CHECK(server_start(&s, st, dir, 8, ops, 0) == 0, "live server start");
  int sub = connect_client(&s), pub = connect_client(&s), other = connect_client(&s);
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1],\"limit\":0},{\"kinds\":[1],\"limit\":0}]",
               "[\"EOSE\",\"live\"]", "overlapping filters catch up");
  expect_reply(other, "[\"REQ\",\"other\",{\"kinds\":[2],\"limit\":0}]",
               "[\"EOSE\",\"other\"]", "nonmatching catch up");
  if (ops) {
    expect_reply(sub, "[\"REQ\",\"search\",{\"kinds\":[1],\"search\":\"live\",\"limit\":0}]",
                 "[\"EOSE\",\"search\"]", "live search catch up");
    expect_reply(sub, "[\"REQ\",\"nosearch\",{\"kinds\":[1],\"search\":\"zebra\",\"limit\":0}]",
                 "[\"EOSE\",\"nosearch\"]", "nonmatching search catch up");
  }
  char *id = NULL, *frame = make_frame("EVENT", sk, pk, 1, 0, NULL, &id);
  publish(pub, frame, id, 1);
  expect_event(sub, "live", id);
  if (ops) expect_event(sub, "search", id);
  barrier(sub); /* overlapping filters yield only one EVENT */
  barrier(other);
  if (ops) {
    CHECK(ws_send(sub, "[\"CLOSE\",\"search\"]") == 0, "close search");
    CHECK(ws_send(sub, "[\"CLOSE\",\"nosearch\"]") == 0, "close nonmatching search");
    barrier(sub);
  }
  publish(pub, frame, id, 1); /* replay-cache duplicate */
  barrier(sub);
  free(frame); free(id);

  CHECK(ws_send(sub, "[\"CLOSE\",\"live\"]") == 0, "CLOSE live");
  barrier(sub); /* CLOSE processed before publishing from the other socket */
  frame = make_frame("EVENT", sk, pk, 1, 1, NULL, &id);
  publish(pub, frame, id, 1);
  barrier(sub);
  free(frame); free(id);

  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1],\"limit\":0}]",
               "[\"EOSE\",\"live\"]", "resubscribe");
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[2],\"limit\":0}]",
               "[\"EOSE\",\"live\"]", "same-id replacement after EOSE");
  frame = make_frame("EVENT", sk, pk, 1, 2, NULL, &id);
  publish(pub, frame, id, 1);
  barrier(sub);
  free(frame); free(id);
  frame = make_frame("EVENT", sk, pk, 2, 3, NULL, &id);
  publish(pub, frame, id, 1);
  expect_event(sub, "live", id);
  expect_event(other, "other", id);
  barrier(sub);
  free(frame); free(id);

  /* A valid signed but forbidden AUTH-kind EVENT must never fan out. */
  expect_reply(sub, "[\"REQ\",\"reject\",{\"kinds\":[22242],\"limit\":0}]",
               "[\"EOSE\",\"reject\"]", "rejection subscription");
  frame = make_frame("EVENT", sk, pk, 22242, 4, NULL, &id);
  publish(pub, frame, id, 0);
  barrier(sub);
  free(frame); free(id);
  CHECK(ws_send(sub, "[\"CLOSE\",\"reject\"]") == 0, "close reject");

  const int kinds[] = {0, 10001, 30023};
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    char req[160];
    snprintf(req, sizeof req, "[\"REQ\",\"replace\",{\"kinds\":[%d],\"limit\":0}]", kinds[i]);
    expect_reply(sub, req, "[\"EOSE\",\"replace\"]", "replaceable catch up");
    frame = make_frame("EVENT", sk, pk, kinds[i], 10, "[[\"d\",\"article\"]]", &id);
    publish(pub, frame, id, 1);
    expect_event(sub, "replace", id);
    char *newid = NULL, *newframe = make_frame("EVENT", sk, pk, kinds[i], 11,
                                              "[[\"d\",\"article\"]]", &newid);
    publish(pub, newframe, newid, 1);
    expect_event(sub, "replace", newid);
    if (ops) { /* real store rejects a superseded version */
      publish(pub, frame, id, 1); /* ingress duplicate is acknowledged, not redelivered */
      barrier(sub);
      char *staleid = NULL, *stale = make_frame("EVENT", sk, pk, kinds[i], 9,
                                                "[[\"d\",\"article\"]]", &staleid);
      publish(pub, stale, staleid, 0);
      barrier(sub);
      free(stale); free(staleid);
    }
    free(frame); free(id); free(newframe); free(newid);
  }
  close(sub); /* live subscriptions and retained filters freed under ASan */
  frame = make_frame("EVENT", sk, pk, 2, 30, NULL, &id);
  publish(pub, frame, id, 1);
  expect_event(other, "other", id);
  close(pub); close(other);
  server_stop(&s);
  if (ops) {
    /* A fresh relay policy/cache must still suppress an existing storage id. */
    CHECK(server_start(&s, st, dir, 8, ops, 0) == 0, "restart live server");
    sub = connect_client(&s); pub = connect_client(&s);
    expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[2],\"limit\":0}]",
                 "[\"EOSE\",\"live\"]", "restart subscription");
    publish(pub, frame, id, 1);
    barrier(sub);
    close(sub); close(pub);
    server_stop(&s);
  }
  free(frame); free(id);
  free(pk); free(sk);
}

static void authenticate(int fd, const char *sk, const char *pk) {
  char *challenge = ws_recv(fd);
  CHECK(challenge && strlen(challenge) == 43, "receive AUTH challenge");
  char tags[256];
  snprintf(tags, sizeof tags, "[[\"challenge\",\"%.32s\"],[\"relay\",\"wss://vault.example.com/\"]]",
           challenge ? challenge + 9 : "");
  free(challenge);
  char *id = NULL, *frame = make_frame("AUTH", sk, pk, 22242, 1, tags, &id);
  publish(fd, frame, id, 1);
  free(frame); free(id);
}

static void run_vault_live(const char *dir, NostrStorage *st,
                           const RelaydAsyncStorageOps *ops) {
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *outsider_sk = nostr_key_generate_private(), *outsider_pk = nostr_key_get_public(outsider_sk);
  RelaydConfig cfg;
  CHECK(relayd_config_load(NULL, &cfg) == 0, "vault config");
  snprintf(cfg.auth, sizeof cfg.auth, "%s", "optional");
  snprintf(cfg.relay_url, sizeof cfg.relay_url, "%s", "wss://vault.example.com/");
  cfg.vault_enabled = 1;
  cfg.vault_pubkeys_count = 1;
  snprintf(cfg.vault_pubkeys[0], sizeof cfg.vault_pubkeys[0], "%s", pk);
  cfg.max_filters = 2;
  Server s;
  CHECK(server_start_config(&s, st, dir, 8, ops, 0, &cfg) == 0, "vault server start");
  int allowed = connect_client(&s), pub = connect_client(&s);
  int unauth = connect_client(&s), outsider = connect_client(&s);
  authenticate(allowed, sk, pk);
  authenticate(pub, sk, pk);
  authenticate(outsider, outsider_sk, outsider_pk);
  char *challenge = ws_recv(unauth);
  CHECK(challenge && strstr(challenge, "AUTH"), "unauth challenge");
  free(challenge);
  expect_reply(allowed, "[\"REQ\",\"vault\",{\"limit\":0}]",
               "[\"EOSE\",\"vault\"]", "allowlisted kindless live REQ");
  expect_reply(unauth, "[\"REQ\",\"vault\",{\"limit\":0}]",
               "[\"CLOSED\",\"vault\",\"auth-required:", "unauth kindless denied");
  expect_reply(outsider, "[\"REQ\",\"vault\",{\"limit\":0}]",
               "[\"CLOSED\",\"vault\",\"restricted:", "outsider kindless denied");
  expect_reply(allowed, "[\"REQ\",\"excess\",{\"kinds\":[1]},{\"kinds\":[2]},{\"kinds\":[3]}]",
               "[\"CLOSED\",\"excess\",\"invalid:", "max_filters enforced");
  const int protected[] = {30078, 445, 1059};
  for (int i = 0; i < 3; i++) {
    char req[128];
    snprintf(req, sizeof req, "[\"REQ\",\"vault\",{\"kinds\":[%d]}]", protected[i]);
    expect_reply(unauth, req, "[\"CLOSED\",\"vault\",\"auth-required:", "unauth protected REQ denied");
    expect_reply(outsider, req, "[\"CLOSED\",\"vault\",\"restricted:", "outsider protected REQ denied");
    char *id = NULL, *frame = make_frame("EVENT", sk, pk, protected[i], 40 + i,
                                        "[[\"d\",\"vault\"]]", &id);
    publish(pub, frame, id, 1);
    expect_event(allowed, "vault", id);
    barrier(unauth);
    barrier(outsider);
    free(frame); free(id);
  }
  close(allowed); close(pub); close(unauth); close(outsider);
  free(pk); free(sk); free(outsider_pk); free(outsider_sk);
  server_stop(&s);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  nostr_json_init();
  char dir[] = "/tmp/relay-req-contract-XXXXXX";
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  run_rng_failure(dir);
  run_storage_less(dir);
  run_with_storage(dir);
  run_async_commit(dir);
  run_live_overflow(dir);
  memset(&g_fake, 0, sizeof g_fake);
  g_fake.mode = FAKE_EMPTY_ITER;
  NostrStorage fake = { .vt = &g_fake_vt };
  run_live(dir, &fake, NULL);
  run_vault_live(dir, &fake, NULL);
#ifdef HAVE_TEST_NOSTRDB
  char db[256];
  snprintf(db, sizeof db, "%s/db", dir);
  NostrStorage *real = nostrdb_storage_new();
  CHECK(real && real->vt->open(real, db, NULL) == 0, "open real nostrdb");
  if (real) {
    run_live(dir, real, nostrdb_storage_async_ops());
    run_vault_live(dir, real, nostrdb_storage_async_ops());
    real->vt->close(real);
    free(real);
  }
  char path[300];
  snprintf(path, sizeof path, "%s/data.mdb", db); unlink(path);
  snprintf(path, sizeof path, "%s/lock.mdb", db); unlink(path);
  CHECK(rmdir(db) == 0, "remove test database");
#endif
  CHECK(rmdir(dir) == 0, "remove test directory");
  if (g_failures) {
    fprintf(stderr, "test_relay_req_contract: %d failure(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "test_relay_req_contract: ok\n");
  return 0;
}
