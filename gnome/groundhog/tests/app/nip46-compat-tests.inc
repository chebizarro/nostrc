/* nostrc-p15n5.3 */
typedef struct {
  TestBunker bunker;
  GhNip46Session *session;
  gchar *uri;
  NostrNip46ConnectURI parsed;
  Await wait;
} QrFixture;

static NostrTimestamp observed_since;

static gpointer
since_scope_open(GhRelayScope *scope, const gchar *url,
                 const NostrFilters *filters, gpointer data, GError **error)
{
  observed_since = nostr_filter_get_since(&filters->filters[0]);
  return bunker_scope_transport.open(scope, url, filters, data, error);
}
static const GhRelayTransport since_scope_transport = {
  since_scope_open, bunker_close
};

static void
qr_teardown(QrFixture *f)
{
  /* Cancelling may complete the pairing task from an idle; drain it while
   * this fixture (on the caller's stack) is still alive. */
  gh_nip46_session_cancel(f->session);
  until(await_done, &f->wait);
  g_clear_error(&f->wait.error);
  g_free(f->wait.result);
  nostr_nip46_uri_connect_free(&f->parsed);
  g_free(f->uri);
  g_object_unref(f->session);
  bunker_clear(&f->bunker);
}

static void
qr_reply_ok(QrFixture *f, const gchar *result_json)
{
  char *reply = nostr_nip46_response_build_ok("connect", result_json);
  bunker_reply(&f->bunker, reply);
  free(reply);
}

static void
qr_open(QrFixture *f, gboolean short_deadlines)
{
  memset(f, 0, sizeof *f);
  bunker_init(&f->bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  f->session = gh_nip46_session_new_qr(relays, &since_scope_transport, NULL,
    &bunker_publish_transport, NULL, &f->bunker, &f->uri, &error);
  g_assert_no_error(error);
  g_free(f->bunker.client_pubkey);
  f->bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(f->session));
  g_assert_cmpint(nostr_nip46_uri_parse_connect(f->uri, &f->parsed), ==, 0);
  if (short_deadlines) gh_nip46_session_set_test_deadlines(f->session, 1, 1, 2, 1);
  gh_nip46_session_set_test_windows(f->session, 50, 100, 10);
  gh_nip46_session_pair_async(f->session, NULL, pair_done, &f->wait);
  bunker_eose(&f->bunker);
}

static gboolean
qr_bound(gpointer data)
{
  QrFixture *f = data;
  return gh_nip46_session_get_remote_pubkey(f->session) != NULL;
}

/* The signer answers get_public_key; pairing completes with the user key. */
static void
qr_complete(QrFixture *f)
{
  g_assert_cmpstr(gh_nip46_session_get_remote_pubkey(f->session), ==,
                  f->bunker.signer_pubkey);
  until(has_publish, &f->bunker);
  bunker_accept(&f->bunker);
  reply_result(&f->bunker, f->bunker.user_pubkey);
  until(await_done, &f->wait);
  g_assert_no_error(f->wait.error);
  g_assert_cmpstr(f->wait.result, ==, f->bunker.user_pubkey);
}

static void
qr_reply_secret(QrFixture *f)
{
  g_autofree gchar *secret_json = g_strdup_printf("\"%s\"", f->parsed.secret);
  qr_reply_ok(f, secret_json);
}

typedef struct { guint count; gchar *last; } Hints;

static void
on_pair_progress(GhNip46Session *session, gint stage, const gchar *detail, gpointer data)
{
  (void)session;
  Hints *hints = data;
  if (stage != GH_NIP46_PAIR_STAGE_IGNORED_REPLY) return;
  hints->count++;
  g_free(hints->last);
  hints->last = g_strdup(detail);
}

/* nostrc-8xfib.1: older Amber builds reply a bare "ack" to nostrconnect.
 * It is accepted after the short window, and the npub the signer then
 * reports is what the user confirms. */
static void
test_qr_ack_accepted(void)
{
  QrFixture f;
  qr_open(&f, FALSE);
  qr_reply_ok(&f, "\"ack\"");
  g_assert_null(gh_nip46_session_get_remote_pubkey(f.session));
  until(qr_bound, &f);
  qr_complete(&f);
  qr_teardown(&f);
}

/* A secret-carrying reply inside the ack window beats an earlier bare ack,
 * even from another author. */
static void
test_qr_secret_beats_ack(void)
{
  QrFixture f;
  qr_open(&f, FALSE);
  char *reply = nostr_nip46_response_build_ok("connect", "\"ack\"");
  char *other = nostr_key_generate_private();
  bunker_reply_from(&f.bunker, other, f.bunker.client_pubkey, reply);
  free(reply);
  free(other);
  qr_reply_secret(&f);
  qr_complete(&f);
  qr_teardown(&f);
}

/* Stray or wrong replies are ignored (with a hint), never fatal: the real
 * reply that follows still pairs. */
static void
test_qr_wrong_secret_ignored(void)
{
  QrFixture f;
  qr_open(&f, FALSE);
  Hints hints = { 0 };
  g_signal_connect(f.session, "pair-progress", G_CALLBACK(on_pair_progress), &hints);
  qr_reply_ok(&f, "\"wrong\"");
  const gchar *params[] = { "wrong" };
  char *req = nostr_nip46_request_build("c", "connect", params, 1);
  bunker_reply(&f.bunker, req);
  free(req);
  NostrEvent *event = NULL;
  g_assert_cmpint(nostr_nip46_build_response_event(f.bunker.signer_pubkey,
    f.bunker.client_pubkey, "x", &event), ==, 0);
  nostr_event_set_content(event, "AAAA?iv=AAAAAAAAAAAAAAAAAAAAAA==");
  g_assert_cmpint(nostr_event_sign(event, f.bunker.signer_secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  BunkerHandle *scope = g_ptr_array_index(f.bunker.scopes, 0);
  gh_relay_scope_event(scope->scope, scope->url, json);
  free(json);
  nostr_event_free(event);
  g_assert_cmpuint(hints.count, ==, 3);
  g_assert_nonnull(strstr(hints.last, "NIP-04"));
  g_assert_false(f.wait.done);
  g_assert_null(gh_nip46_session_get_remote_pubkey(f.session));
  qr_reply_secret(&f);
  qr_complete(&f);
  g_free(hints.last);
  qr_teardown(&f);
}

/* A signer error keeps the attempt alive; its sanitised text is shown and
 * is named again if the attempt times out. */
static void
test_qr_signer_error_hint(void)
{
  QrFixture f;
  qr_open(&f, TRUE);
  Hints hints = { 0 };
  g_signal_connect(f.session, "pair-progress", G_CALLBACK(on_pair_progress), &hints);
  char *reply = nostr_nip46_response_build_err("connect", "user\nrejected\x01 it");
  bunker_reply(&f.bunker, reply);
  free(reply);
  g_assert_cmpuint(hints.count, ==, 1);
  g_assert_nonnull(strstr(hints.last, "signer said: user rejected it"));
  g_assert_false(f.wait.done);
  until(await_done, &f.wait);
  g_assert_error(f.wait.error, GH_NIP46_SESSION_ERROR, GH_NIP46_SESSION_ERROR_TIMED_OUT);
  g_assert_nonnull(strstr(f.wait.error->message, "user rejected it"));
  g_assert_null(gh_nip46_session_get_remote_pubkey(f.session));
  g_free(hints.last);
  qr_teardown(&f);
}

/* Amber and others add a relay hint to the p tag. */
static void
test_qr_p_tag_relay_hint(void)
{
  QrFixture f;
  qr_open(&f, FALSE);
  g_autofree gchar *secret_json = g_strdup_printf("\"%s\"", f.parsed.secret);
  char *reply = nostr_nip46_response_build_ok("connect", secret_json);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, f.bunker.signer_secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, f.bunker.client_pubkey, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)reply,
                  strlen(reply), &ciphertext), ==, 0);
  NostrEvent *event = NULL;
  g_assert_cmpint(nostr_nip46_build_response_event(f.bunker.signer_pubkey,
    f.bunker.client_pubkey, ciphertext, &event), ==, 0);
  NostrTags *tags = nostr_event_get_tags(event);
  gboolean hinted = FALSE;
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get(tag, 0), "p") == 0) {
      nostr_tag_append(tag, RELAY);
      g_assert_cmpuint(nostr_tag_size(tag), ==, 3);
      hinted = TRUE;
    }
  }
  g_assert_true(hinted);
  nostr_event_set_content(event, ciphertext);
  g_assert_cmpint(nostr_event_sign(event, f.bunker.signer_secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  BunkerHandle *scope = g_ptr_array_index(f.bunker.scopes, 0);
  gh_relay_scope_event(scope->scope, scope->url, json);
  free(json); free(ciphertext); free(reply);
  nostr_event_free(event);
  g_assert_cmpstr(gh_nip46_session_get_remote_pubkey(f.session), ==,
                  f.bunker.signer_pubkey);
  qr_teardown(&f);
}

static void
test_reply_window_and_unknown_ids(void)
{
  QrFixture f;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  qr_open(&f, FALSE);
  g_assert_cmpint((gint64)observed_since, <=, now - 600 + 5);
  g_assert_cmpint((gint64)observed_since, >=, now - 600 - 5);
  g_autofree gchar *secret_json = g_strdup_printf("\"%s\"", f.parsed.secret);
  qr_reply_ok(&f, secret_json);
  until(has_publish, &f.bunker);
  bunker_accept(&f.bunker);
  /* Unknown id: ignored. The real reply completes; replaying it after
   * completion is ignored (dedup by request id). */
  char *reply = nostr_nip46_response_build_ok("not-a-request", "\"x\"");
  bunker_reply(&f.bunker, reply);
  free(reply);
  g_assert_false(f.wait.done);
  g_autofree gchar *request_json = bunker_request_json(&f.bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_autofree gchar *quoted = g_strdup_printf("\"%s\"", f.bunker.user_pubkey);
  reply = nostr_nip46_response_build_ok(request.id, quoted);
  bunker_reply(&f.bunker, reply);
  until(await_done, &f.wait);
  bunker_reply(&f.bunker, reply);
  free(reply);
  nostr_nip46_request_free(&request);
  g_assert_no_error(f.wait.error);
  g_assert_cmpuint(f.wait.calls, ==, 1);
  g_assert_cmpstr(f.wait.result, ==, f.bunker.user_pubkey);
  qr_teardown(&f);
}

static void
test_pair_timeout_names_relays(void)
{
  QrFixture f;
  qr_open(&f, TRUE);
  until(await_done, &f.wait);
  g_assert_error(f.wait.error, GH_NIP46_SESSION_ERROR, GH_NIP46_SESSION_ERROR_TIMED_OUT);
  g_assert_nonnull(strstr(f.wait.error->message, "No reply from signer on " RELAY));
  qr_teardown(&f);
}

/* Some bunkers echo the connect secret instead of "ack". */
static void
test_bunker_secret_echo(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=wss%%3A%%2F%%2Fbunker.test.invalid&secret=once",
                                          bunker.signer_pubkey);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_bunker(uri,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &error);
  g_assert_no_error(error);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  Await wait = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &wait);
  bunker_eose(&bunker);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  reply_result(&bunker, "once");
  PublishCount next = { &bunker, bunker.publishes->len + 1 };
  until(publish_count, &next);
  bunker_accept(&bunker);
  reply_result(&bunker, bunker.user_pubkey);
  until(await_done, &wait);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.result, ==, bunker.user_pubkey);
  g_free(wait.result);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}
