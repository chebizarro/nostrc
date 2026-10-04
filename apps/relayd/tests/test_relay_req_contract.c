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
#include "nostr-keys.h"
#include "nostr-relay-server.h"
#include "nostr-storage.h"
#include "relayd_config.h"
#include "relay_policy.h"

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
                        int max_subs, const RelaydAsyncStorageOps *async_ops,
                        unsigned int ack_timeout_ms) {
  memset(s, 0, sizeof *s);
  if (relayd_config_load(NULL, &s->cfg) != 0) return -1;
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
      CHECK(g_fake.queries == q0 + 1, "stale check did not query the store");
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
  CHECK(g_fake.frees == 9, "iterators freed %d, want 9 (a d m p q r r t + stale check)",
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
    async_commit_all();
    for (int i = 0; i < BURST; i++) {
      reply = ws_recv(a);
      snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true", ids[i]);
      CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0,
            "burst OK %d out of order: %s", i,
            reply ? reply : "(no reply)");
      free(reply); free(frames[i]); free(ids[i]);
    }
    fprintf(stderr, "  async burst %d: queued in %lld ms, all ordered OKs in %lld ms\n",
            BURST, queued_ms, now_ms() - burst_start);
    close(a); close(b);
  }
  free(pk); free(sk);
  server_stop(&s);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  nostr_json_init();
  char dir[] = "/tmp/relay-req-contract-XXXXXX";
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  run_storage_less(dir);
  run_with_storage(dir);
  run_async_commit(dir);
  rmdir(dir);
  if (g_failures) {
    fprintf(stderr, "test_relay_req_contract: %d failure(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "test_relay_req_contract: ok\n");
  return 0;
}
