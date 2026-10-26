/* Included by test_relay_req_contract.c: real WebSocket entrypoints with
 * controlled admission/visibility and commit notifications, never sleeps. */
static void async_reset(void) {
  pthread_mutex_lock(&g_async.mutex);
  g_async.queued = g_async.committed = g_async.release_sync = 0;
  g_async.enqueue_error = g_async.visible_error = g_async.visible_error_reads = 0;
  memset(g_async.committed_out_of_order, 0, sizeof g_async.committed_out_of_order);
  pthread_mutex_unlock(&g_async.mutex);
  memset(&g_fake, 0, sizeof g_fake);
  g_fake.mode = FAKE_EMPTY_ITER;
}

static int async_queued_count(void) {
  pthread_mutex_lock(&g_async.mutex);
  int n = g_async.queued;
  pthread_mutex_unlock(&g_async.mutex);
  return n;
}

static void async_commit_index(int index) {
  pthread_mutex_lock(&g_async.mutex);
  g_async.committed_out_of_order[index] = 1;
  void (*notify)(void *) = g_async.notify;
  void *ctx = g_async.notify_ctx;
  pthread_mutex_unlock(&g_async.mutex);
  if (notify) notify(ctx);
}

static void expect_closed_without_text(int fd) {
  unsigned char h[2], payload[125];
  errno = 0;
  if (read_full(fd, h, sizeof h) != 0) {
    CHECK(errno != EAGAIN && errno != EWOULDBLOCK, "transport close timed out");
    return;
  }
  size_t len = h[1] & 0x7f;
  CHECK((h[0] & 0x0f) == 8 && !(h[1] & 0x80) && len <= 125,
        "expected close frame, not an OK or other text");
  if ((h[0] & 0x0f) != 8 || len > 125) return;
  CHECK(read_full(fd, payload, len) == 0, "read close payload");
  const unsigned char close_reply[] = {0x88, 0x80, 0, 0, 0, 0};
  ssize_t sent;
  do { sent = send(fd, close_reply, sizeof close_reply, 0); } while (sent < 0 && errno == EINTR);
  /* The server may already have completed its close before the echo. EOF
   * below still proves closure and rejects any text frame after NOTICE. */
  CHECK(sent == sizeof close_reply || (sent < 0 && (errno == EPIPE || errno == ECONNRESET)),
        "reply to close: sent=%zd errno=%d", sent, errno);
  unsigned char byte;
  ssize_t n = recv(fd, &byte, 1, 0);
  CHECK(n == 0 || (n < 0 && errno == ECONNRESET),
        "transport did not close after close handshake: n=%zd errno=%d byte=%02x", n, errno, n > 0 ? byte : 0);
}

static void expect_unknown(int fd, const char *id) {
  char *reply = ws_recv(fd);
  CHECK(reply && strncmp(reply, "[\"NOTICE\",", 10) == 0 && strstr(reply, id) &&
        strstr(reply, "error: storage outcome unknown"),
        "unknown must be event-bearing NOTICE, not OK: %s", reply ? reply : "(none)");
  free(reply);
  expect_closed_without_text(fd);
}

static void expect_stored(int fd, const char *id, int duplicate) {
  char prefix[160];
  snprintf(prefix, sizeof prefix, "[\"OK\",\"%s\",true,\"%s", id,
           duplicate ? "duplicate:" : "");
  char *reply = ws_recv(fd);
  CHECK(reply && strncmp(reply, prefix, strlen(prefix)) == 0,
        "expected confirmed OK: %s", reply ? reply : "(none)");
  free(reply);
}

static void run_storage_waiters(const char *dir, int replay_ttl) {
  fprintf(stderr, "== storage waiters, independent deadlines, replay TTL=%d\n", replay_ttl);
  async_reset();
  NostrStorage st = { .vt = &g_async_vt };
  Server s;
  RelaydConfig cfg;
  CHECK(relayd_config_load(NULL, &cfg) == 0, "waiter config");
  cfg.replay_ttl_seconds = replay_ttl;
  CHECK(server_start_config(&s, &st, dir, 8, &g_async_ops, 600, &cfg) == 0, "waiter server");
  int sub = connect_client(&s), a = connect_client(&s), b = connect_client(&s);
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1]}]", "[\"EOSE\",\"live\"]", "waiter subscriber");
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *id = NULL, *frame = signed_event_frame(sk, pk, 2000, &id);
  CHECK(ws_send(a, frame) == 0 && async_wait_queued(1), "first waiter enqueue");
  CHECK(ws_send(b, frame) == 0, "second waiter submit");
  barrier(b); /* proves attachment before releasing commit */
  CHECK(async_queued_count() == 1, "waiter resubmission re-enqueued");
  async_commit_index(0);
  expect_stored(a, id, 0);
  expect_stored(b, id, 1);
  expect_event(sub, "live", id);
  barrier(sub);
  free(frame); free(id);

  frame = signed_event_frame(sk, pk, 2001, &id);
  CHECK(ws_send(a, frame) == 0 && async_wait_queued(2), "deadline enqueue");
  expect_unknown(a, id);
  close(a);
  /* This waiter gets a fresh deadline, not the original expired deadline. */
  CHECK(ws_send(b, frame) == 0, "attach after original deadline");
  barrier(b);
  CHECK(async_queued_count() == 2, "late waiter re-enqueued");
  async_commit_index(1);
  expect_stored(b, id, 1);
  expect_event(sub, "live", id);
  barrier(sub);
  free(frame); free(id);

  int c = connect_client(&s);
  frame = signed_event_frame(sk, pk, 2002, &id);
  CHECK(ws_send(b, frame) == 0 && async_wait_queued(3), "two deadline enqueue");
  CHECK(ws_send(c, frame) == 0, "attach expiring waiter");
  barrier(c);
  CHECK(async_queued_count() == 3, "expiring waiter re-enqueued");
  expect_unknown(b, id);
  expect_unknown(c, id);
  barrier(sub);
  async_commit_index(2);
  expect_event(sub, "live", id);
  barrier(sub);
  close(b); close(c); close(sub);
  free(frame); free(id); free(pk); free(sk);
  server_stop(&s);
}

static void run_out_of_order_acks(const char *dir) {
  fprintf(stderr, "== later commit ACK passes earlier unresolved id on same connection\n");
  async_reset();
  NostrStorage st = { .vt = &g_async_vt };
  Server s;
  CHECK(server_start(&s, &st, dir, 8, &g_async_ops, 2000) == 0, "out-of-order server");
  int sub = connect_client(&s), pub = connect_client(&s);
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1]}]", "[\"EOSE\",\"live\"]", "unordered subscriber");
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *ids[2] = {0}, *frames[2];
  for (int i = 0; i < 2; i++) {
    frames[i] = signed_event_frame(sk, pk, 2400 + i, &ids[i]);
    CHECK(ws_send(pub, frames[i]) == 0 && async_wait_queued(i + 1), "unordered enqueue");
  }
  async_commit_index(1);
  expect_stored(pub, ids[1], 0);
  expect_event(sub, "live", ids[1]);
  async_commit_index(0);
  expect_stored(pub, ids[0], 0);
  expect_event(sub, "live", ids[0]);
  barrier(sub);
  for (int i = 0; i < 2; i++) { free(frames[i]); free(ids[i]); }
  close(pub); close(sub); free(pk); free(sk);
  server_stop(&s);
}

static void run_waiter_saturation(const char *dir) {
  fprintf(stderr, "== waiter capacity: unknown close, never rejection or re-enqueue\n");
  async_reset();
  RelaydConfig cfg;
  CHECK(relayd_config_load(NULL, &cfg) == 0, "waiter cap config");
  cfg.storage_max_pending_events = 1;
  NostrStorage st = { .vt = &g_async_vt };
  Server s;
  CHECK(server_start_config(&s, &st, dir, 8, &g_async_ops, 2000, &cfg) == 0, "waiter cap server");
  int sub = connect_client(&s), a = connect_client(&s);
  int b = connect_client(&s), c = connect_client(&s);
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1]}]", "[\"EOSE\",\"live\"]", "waiter cap subscriber");
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *id = NULL, *frame = signed_event_frame(sk, pk, 2500, &id);
  CHECK(ws_send(a, frame) == 0 && async_wait_queued(1), "waiter cap enqueue");
  CHECK(ws_send(b, frame) == 0, "waiter cap attach");
  barrier(b);
  CHECK(ws_send(c, frame) == 0, "excess waiter submit");
  expect_unknown(c, id);
  CHECK(async_queued_count() == 1, "excess waiter re-enqueued");
  async_commit_index(0);
  expect_stored(a, id, 0); expect_stored(b, id, 1);
  expect_event(sub, "live", id); barrier(sub);
  close(a); close(b); close(c); close(sub);
  free(frame); free(id); free(pk); free(sk);
  server_stop(&s);
}

static void run_storage_saturation(const char *dir) {
  fprintf(stderr, "== saturation and NO enqueue-order watermark\n");
  async_reset();
  RelaydConfig cfg;
  CHECK(relayd_config_load(NULL, &cfg) == 0, "saturation config");
  cfg.storage_max_pending_events = 2;
  NostrStorage st = { .vt = &g_async_vt };
  Server s;
  CHECK(server_start_config(&s, &st, dir, 8, &g_async_ops, 600, &cfg) == 0, "saturation server");
  int sub = connect_client(&s), a = connect_client(&s), b = connect_client(&s);
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1]}]", "[\"EOSE\",\"live\"]", "saturation subscriber");
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *ids[3] = {0}, *frames[3];
  for (int i = 0; i < 3; i++) frames[i] = signed_event_frame(sk, pk, 3000 + i, &ids[i]);
  CHECK(ws_send(a, frames[0]) == 0 && async_wait_queued(1), "reserve earlier");
  CHECK(ws_send(b, frames[1]) == 0 && async_wait_queued(2), "reserve later");
  expect_unknown(a, ids[0]); expect_unknown(b, ids[1]);
  close(a); close(b);
  int retry = connect_client(&s);
  char refusal[192];
  snprintf(refusal, sizeof refusal, "[\"OK\",\"%s\",false,\"rate-limited: storage reconciliation capacity exhausted\"]", ids[2]);
  expect_reply(retry, frames[2], refusal, "full table refuses before enqueue");
  CHECK(async_queued_count() == 2, "saturated write reached backend");
  barrier(sub);
  /* Later enqueue commits first: legal with parallel nostrdb ingesters. */
  async_commit_index(1);
  expect_event(sub, "live", ids[1]);
  CHECK(ws_send(retry, frames[2]) == 0 && async_wait_queued(3), "commit releases one slot");
  async_commit_index(2);
  expect_stored(retry, ids[2], 0);
  expect_event(sub, "live", ids[2]);
  CHECK(ws_send(retry, frames[0]) == 0, "join earlier still-unknown operation");
  barrier(retry);
  CHECK(async_queued_count() == 3, "later commit incorrectly retired earlier reservation");
  async_commit_index(0);
  expect_stored(retry, ids[0], 1);
  expect_event(sub, "live", ids[0]);
  barrier(sub);
  close(retry); close(sub);
  for (int i = 0; i < 3; i++) { free(frames[i]); free(ids[i]); }
  free(pk); free(sk);
  server_stop(&s);
}

#ifdef HAVE_TEST_NOSTRDB
/* Hold one admitted write ahead of the real nostrdb adapter. Releasing the
 * fault uses real ingest, writer commit notification and committed queries. */
static struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  char *json;
  int enqueues;
} delayed = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0 };

static int delayed_enqueue(NostrStorage *st, const NostrEvent *ev) {
  (void)st;
  char *json = nostr_event_serialize(ev);
  if (!json) return -1;
  pthread_mutex_lock(&delayed.mutex);
  if (delayed.json) {
    pthread_mutex_unlock(&delayed.mutex);
    free(json);
    return -1;
  }
  delayed.json = json;
  delayed.enqueues++;
  pthread_cond_broadcast(&delayed.cond);
  pthread_mutex_unlock(&delayed.mutex);
  return 0;
}

static void run_real_late_commit(const char *dir, NostrStorage *st) {
  fprintf(stderr, "== fault-held real nostrdb commit past publisher deadline\n");
  RelaydAsyncStorageOps ops = *nostrdb_storage_async_ops();
  ops.enqueue = delayed_enqueue;
  Server s;
  CHECK(server_start(&s, st, dir, 8, &ops, 600) == 0, "real late server");
  int sub = connect_client(&s), pub = connect_client(&s);
  expect_reply(sub, "[\"REQ\",\"late\",{\"kinds\":[1],\"limit\":0}]", "[\"EOSE\",\"late\"]", "real late subscriber");
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *id = NULL, *frame = signed_event_frame(sk, pk, 5000, &id);
  CHECK(ws_send(pub, frame) == 0, "real delayed submit");
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 3;
  pthread_mutex_lock(&delayed.mutex);
  int rc = 0;
  while (!delayed.json && rc == 0)
    rc = pthread_cond_timedwait(&delayed.cond, &delayed.mutex, &deadline);
  CHECK(delayed.json && delayed.enqueues == 1, "real delayed admission");
  pthread_mutex_unlock(&delayed.mutex);
  expect_unknown(pub, id);
  close(pub);
  barrier(sub);
  pub = connect_client(&s);
  CHECK(ws_send(pub, frame) == 0, "real unresolved resubmit");
  barrier(pub);
  pthread_mutex_lock(&delayed.mutex);
  CHECK(delayed.enqueues == 1, "real unresolved event re-enqueued");
  char *json = delayed.json;
  delayed.json = NULL;
  pthread_mutex_unlock(&delayed.mutex);
  NostrEvent *ev = nostr_event_new();
  CHECK(json && ev && nostr_event_deserialize(ev, json) == 0, "release delayed event");
  CHECK(ev && nostrdb_storage_async_ops()->enqueue(st, ev) == 0, "real ingest release");
  nostr_event_free(ev); free(json);
  expect_stored(pub, id, 1);
  expect_event(sub, "late", id);
  barrier(sub);
  char req[160];
  snprintf(req, sizeof req, "[\"REQ\",\"saved\",{\"ids\":[\"%s\"]}]", id);
  CHECK(ws_send(pub, req) == 0, "query actual committed id");
  expect_event(pub, "saved", id);
  char *reply = ws_recv(pub);
  CHECK(reply && strcmp(reply, "[\"EOSE\",\"saved\"]") == 0, "real committed catch-up EOSE");
  free(reply);
  CHECK(ws_send(pub, frame) == 0, "real committed resubmit");
  expect_stored(pub, id, 1);
  barrier(sub);
  close(pub); close(sub);
  free(frame); free(id); free(pk); free(sk);
  server_stop(&s);
}
#endif

static void run_storage_failures_and_bytes(const char *dir) {
  fprintf(stderr, "== non-admission, read error unknown, serialized byte budget\n");
  async_reset();
  NostrStorage st = { .vt = &g_async_vt };
  Server s;
  CHECK(server_start(&s, &st, dir, 8, &g_async_ops, 600) == 0, "failure server");
  int sub = connect_client(&s), pub = connect_client(&s);
  expect_reply(sub, "[\"REQ\",\"live\",{\"kinds\":[1]}]", "[\"EOSE\",\"live\"]", "failure subscriber");
  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *id = NULL, *frame = signed_event_frame(sk, pk, 4000, &id);
  pthread_mutex_lock(&g_async.mutex);
  g_async.enqueue_error = 1;
  pthread_mutex_unlock(&g_async.mutex);
  char refusal[192];
  snprintf(refusal, sizeof refusal, "[\"OK\",\"%s\",false,\"error: store enqueue failed\"]", id);
  expect_reply(pub, frame, refusal, "definitive enqueue failure");
  CHECK(async_queued_count() == 0, "failed enqueue was admitted");
  barrier(sub);
  pthread_mutex_lock(&g_async.mutex);
  g_async.enqueue_error = 0;
  pthread_mutex_unlock(&g_async.mutex);
  CHECK(ws_send(pub, frame) == 0 && async_wait_queued(1), "retry after definitive failure");
  pthread_mutex_lock(&g_async.mutex);
  g_async.visible_error = 1;
  pthread_mutex_unlock(&g_async.mutex);
  expect_unknown(pub, id); /* a lookup error is not write failure */
  close(pub);
  barrier(sub);
  pthread_mutex_lock(&g_async.mutex);
  int errors_before_commit = g_async.visible_error_reads;
  pthread_mutex_unlock(&g_async.mutex);
  async_commit_index(0);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 3;
  pthread_mutex_lock(&g_async.mutex);
  int rc = 0;
  while (g_async.visible_error_reads <= errors_before_commit && rc == 0)
    rc = pthread_cond_timedwait(&g_async.cond, &g_async.mutex, &deadline);
  CHECK(g_async.visible_error_reads > errors_before_commit, "commit visibility read fault observed");
  g_async.visible_error = 0;
  pthread_mutex_unlock(&g_async.mutex);
  /* No new notification or request: the failed READ's backoff recovers. */
  expect_event(sub, "live", id);
  barrier(sub);
  close(sub);
  free(frame); free(id);
  server_stop(&s);

  async_reset();
  RelaydConfig cfg;
  CHECK(relayd_config_load(NULL, &cfg) == 0, "byte config");
  cfg.storage_max_pending_events = 8;
  cfg.storage_max_pending_bytes = 1024;
  CHECK(server_start_config(&s, &st, dir, 8, &g_async_ops, 2000, &cfg) == 0, "byte server");
  pub = connect_client(&s);
  char *frames[4], *ids[4] = {0};
  int admitted[4], n = 0, refused = -1;
  size_t bytes = 0;
  for (int i = 0; i < 4; i++) {
    frames[i] = signed_event_frame(sk, pk, 4100 + i, &ids[i]);
    size_t payload_bytes = strlen(frames[i]) - strlen("[\"EVENT\","); /* JSON plus NUL */
    admitted[i] = bytes + payload_bytes <= 1024;
    if (admitted[i]) {
      bytes += payload_bytes;
      CHECK(ws_send(pub, frames[i]) == 0 && async_wait_queued(++n), "byte-admitted event");
    } else {
      refused = i;
      snprintf(refusal, sizeof refusal, "[\"OK\",\"%s\",false,\"rate-limited: storage reconciliation capacity exhausted\"]", ids[i]);
      expect_reply(pub, frames[i], refusal, "byte budget refuses before enqueue");
    }
  }
  CHECK(n > 0 && refused >= 0 && async_queued_count() == n, "byte-bound admission count");
  async_commit_all();
  for (int i = 0; i < 4; i++) if (admitted[i]) expect_stored(pub, ids[i], 0);
  if (refused >= 0) {
    CHECK(ws_send(pub, frames[refused]) == 0 && async_wait_queued(n + 1), "commit releases bytes");
    async_commit_all();
    expect_stored(pub, ids[refused], 0);
  }
  for (int i = 0; i < 4; i++) { free(frames[i]); free(ids[i]); }
  close(pub);
  free(pk); free(sk);
  server_stop(&s);
  async_reset();
}
