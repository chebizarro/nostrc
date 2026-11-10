#include <nostr-gobject-1.0/gnostr-list-service.h>
#include "nostr-keys.h"
#include "nostr-kinds.h"
#include "json.h"
#include <jansson.h>
#include <string.h>

static char *sk, *pk;
static const char *urls[] = {"wss://one.example", "wss://two.example", "wss://three.example", NULL};
typedef struct {
  GHashTable *storage;
  GQueue signs, publications, crypto;
  gboolean valid, fail_commit, fail_decrypt, fail_open, hold_sign, hold_publish, hold_crypto;
  guint sign_count, publish_count, opens, closes, crypto_running, crypto_peak, derive_count;
  char *last_signed;
  gboolean derive_once;
} Backend;
static void pump(void) { guint n = 0; while (g_main_context_pending(NULL) && n++ < 10000) g_main_context_iteration(NULL, FALSE); g_assert_cmpuint(n, <, 10000); }
static gboolean generation(gpointer data, const char *account, guint64 gen) { (void)account; return ((Backend *)data)->valid && gen == 7; }
static char *key(int kind, const char *id) { return g_strdup_printf("%d:%s", kind, id); }
static char *load(gpointer data, int kind, const char *id, GError **error) { (void)error; char *k = key(kind, id); char *v = g_strdup(g_hash_table_lookup(((Backend *)data)->storage, k)); g_free(k); return v; }
static gboolean commit(gpointer data, int kind, const char *id, const char *json, GError **error) {
  Backend *b = data;
  if (b->fail_commit) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Injected transaction failure"); return FALSE; }
  g_hash_table_replace(b->storage, key(kind, id), g_strdup(json)); return TRUE;
}
static gpointer open_sub(gpointer data, GNostrListService *s, const char *account, guint64 gen, guint64 serial, const GNostrListReadPlanEntry *entry, GError **error) {
  (void)s; (void)account; (void)gen; (void)serial; (void)entry; (void)error; Backend *b = data; b->opens++; if (b->fail_open) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected open failure"); return NULL; } return b;
}
static void close_sub(gpointer data, gpointer sub) { (void)sub; ((Backend *)data)->closes++; }
static gint64 clock_now(gpointer data) { (void)data; return 1000; }
static void sign_complete(GTask *task) {
  const char *json = g_task_get_task_data(task); NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  g_task_return_pointer(task, nostr_event_serialize(ev), g_free); nostr_event_free(ev); g_object_unref(task);
}
static void sign_start(gpointer data, const char *json, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) {
  Backend *b = data; b->sign_count++; GTask *task = g_task_new(NULL, cancel, cb, u);
  g_task_set_task_data(task, g_strdup(json), g_free);
  if (b->hold_sign) g_queue_push_tail(&b->signs, task); else sign_complete(task);
}
static char *text_finish(gpointer data, GAsyncResult *res, GError **error) { (void)data; return g_task_propagate_pointer(G_TASK(res), error); }
static void crypto_complete(Backend *b, GTask *task) {
  b->crypto_running--; g_task_return_pointer(task, g_strdup(g_task_get_task_data(task)), g_free); g_object_unref(task);
}
static void encrypt_start(gpointer data, const char *account, const char *plain, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) {
  (void)account; Backend *b = data;
  GTask *task = g_task_new(NULL, cancel, cb, u); g_task_set_task_data(task, g_strdup(plain), g_free);
  b->crypto_running++; b->crypto_peak = MAX(b->crypto_peak, b->crypto_running);
  if (b->hold_crypto) g_queue_push_tail(&b->crypto, task); else crypto_complete(b, task);
}
static void decrypt_start(gpointer data, const char *account, const char *cipher, gboolean legacy, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) {
  (void)legacy; Backend *b = data;
  if (b->fail_decrypt) { GTask *task = g_task_new(NULL, cancel, cb, u); g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected unreadable ciphertext"); g_object_unref(task); }
  else encrypt_start(data, account, cipher, cancel, cb, u);
}
static void publication_complete(GTask *task, GNostrListPublishOutcome first) {
  char **targets = g_task_get_task_data(task); GVariantBuilder builder; g_variant_builder_init(&builder, G_VARIANT_TYPE("a{si}"));
  for (guint i = 0; targets[i]; i++) g_variant_builder_add(&builder, "{si}", targets[i], i ? GNOSTR_LIST_REJECTED : first);
  g_task_return_pointer(task, g_variant_ref_sink(g_variant_builder_end(&builder)), (GDestroyNotify)g_variant_unref); g_object_unref(task);
}
static void publish_start(gpointer data, const char *json, const char *const *targets, const char *purpose, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) {
  (void)purpose; Backend *b = data; b->publish_count++; g_free(b->last_signed); b->last_signed = g_strdup(json);
  NostrEvent *ev = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_signed(ev, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK); g_assert_true(nostr_event_check_signature(ev)); nostr_event_free(ev);
  GTask *task = g_task_new(NULL, cancel, cb, u); g_task_set_task_data(task, g_strdupv((char **)targets), (GDestroyNotify)g_strfreev);
  if (b->hold_publish) g_queue_push_tail(&b->publications, task); else publication_complete(task, GNOSTR_LIST_ACCEPTED);
}
static GVariant *publish_finish(gpointer data, GAsyncResult *res, GError **error) { (void)data; return g_task_propagate_pointer(G_TASK(res), error); }
static GNostrListOperation *word_op(const char *word, gboolean private_tag) {
  const char *fields[] = {"word", word};
  return gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, private_tag, fields, 2, 0, 0);
}
static GPtrArray *derive(gpointer data, GNostrListDocument *doc, GError **error) {
  (void)doc; (void)error; Backend *b = data; b->derive_count++;
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  if (b->derive_once) { b->derive_once = FALSE; g_ptr_array_add(ops, word_op("derived", TRUE)); }
  return ops;
}
static GNostrListAdapter adapter = {.generation_valid = generation, .load = load, .commit = commit,
  .open_subscription = open_sub, .close_subscription = close_sub, .now = clock_now,
  .encrypt_async = encrypt_start, .encrypt_finish = text_finish, .decrypt_async = decrypt_start, .decrypt_finish = text_finish,
  .sign_async = sign_start, .sign_finish = text_finish, .publish_async = publish_start, .publish_finish = publish_finish};
static void backend_init(Backend *b) { memset(b, 0, sizeof(*b)); b->valid = TRUE; b->storage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); }
static void backend_clear(Backend *b) { g_assert_true(g_queue_is_empty(&b->signs)); g_assert_true(g_queue_is_empty(&b->publications)); g_assert_true(g_queue_is_empty(&b->crypto)); g_hash_table_unref(b->storage); g_free(b->last_signed); }
static GNostrListReadPlanEntry plan(int kind, GNostrListPublishPolicy policy) {
  GNostrListReadPlanEntry p = {.kind = kind, .identifier = "", .sources = urls, .targets = urls, .publish_policy = policy, .required_sources = GNOSTR_LIST_SOURCES_MAJORITY}; return p;
}
static GNostrListService *service_new(Backend *b, GNostrListReadPlanEntry *p) {
  GNostrListService *s = gnostr_list_service_new(pk, 7, &adapter, b, NULL); GError *error = NULL;
  g_assert_true(gnostr_list_service_start(s, p, 1, &error)); g_assert_no_error(error); pump(); return s;
}
static void end(GNostrListService *s, Backend *b) { gnostr_list_service_close(s); pump(); g_object_unref(s); backend_clear(b); }
static void notify(GNostrListService *s, int kind, const char *id, guint source, GNostrListReadNotification type, const char *json) {
  gnostr_list_service_notify(s, gnostr_list_service_get_read_serial(s), kind, id, urls[source], type, json); pump();
}
static char *event(int kind, const char *identifier, gint64 time, const char *tags, const char *content) {
  char *json = g_strdup_printf("{\"pubkey\":\"%s\",\"created_at\":%lld,\"kind\":%d,\"tags\":%s,\"content\":\"\"}", pk, (long long)time, kind, tags);
  NostrEvent *ev = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_compact(ev, json, NULL), ==, 1); g_free(json);
  (void)identifier; if (content) nostr_event_set_content(ev, content);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0); json = nostr_event_serialize(ev); nostr_event_free(ev); return json;
}
static GNostrListDocument *apply_word(GNostrListService *s, int kind, const char *id, const char *word, gboolean p) {
  GNostrListDocument *doc = gnostr_list_service_lookup(s, kind, id); GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref); g_ptr_array_add(ops, word_op(word, p));
  GError *error = NULL; GNostrListDocument *saved = gnostr_list_service_apply(s, kind, id, gnostr_list_document_get_local_revision(doc), ops, &error);
  g_assert_no_error(error); g_assert_nonnull(saved); g_object_unref(doc); g_ptr_array_unref(ops); return saved;
}
static guint count(GNostrListService *s, int kind, const char *id, gboolean p) {
  GNostrListDocument *doc = gnostr_list_service_lookup(s, kind, id); guint n = nostr_nip51_document_tag_count(gnostr_list_document_get_codec(doc), p); g_object_unref(doc); return n;
}
static void eose(GNostrListService *s, int kind) { notify(s, kind, "", 0, GNOSTR_LIST_EOSE, NULL); notify(s, kind, "", 1, GNOSTR_LIST_EOSE, NULL); }
static void test_ordering(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p);
  char *a = event(p.kind, "", 10, "[[\"word\",\"old\"],[\"x-extension\",\"\",\"tail\"]]", NULL);
  char *newest = event(p.kind, "", 20, "[[\"word\",\"new\"]]", NULL);
  notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, newest); notify(s, p.kind, "", 1, GNOSTR_LIST_EVENT, a); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, newest);
  g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 1);
  char *tie = event(p.kind, "", 20, "[]", NULL); notify(s, p.kind, "", 2, GNOSTR_LIST_EVENT, tie);
  json_t *j1 = json_loads(newest, 0, NULL), *j2 = json_loads(tie, 0, NULL);
  const char *winner = strcmp(json_string_value(json_object_get(j1, "id")), json_string_value(json_object_get(j2, "id"))) < 0 ? newest : tie;
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, ""); g_assert_cmpstr(gnostr_list_document_get_source_json(doc), ==, winner); g_object_unref(doc);
  json_decref(j1); json_decref(j2); g_free(a); g_free(newest); g_free(tie); end(s, &b);
}
static void test_local_transaction_rebase(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p);
  char *base = event(p.kind, "", 10, "[[\"word\",\"removed\"],[\"x\",\"\",\"tail\"]]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, base);
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, ""); const NostrListTag *tag = nostr_nip51_document_tag(gnostr_list_document_get_codec(doc), FALSE, 0);
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref); g_ptr_array_add(ops, gnostr_list_operation_remove(FALSE, tag, G_MAXUINT));
  GNostrListDocument *saved = gnostr_list_service_apply(s, p.kind, "", 0, ops, NULL); g_assert_nonnull(saved); g_object_unref(saved); g_object_unref(doc); g_ptr_array_unref(ops);
  char *next = event(p.kind, "", 20, "[[\"word\",\"removed\"],[\"word\",\"remote\"],[\"x\",\"\",\"tail\"]]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, next);
  g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 2);
  b.fail_commit = TRUE; doc = gnostr_list_service_lookup(s, p.kind, ""); ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref); g_ptr_array_add(ops, word_op("rollback", FALSE)); GError *error = NULL;
  saved = gnostr_list_service_apply(s, p.kind, "", 1, ops, &error); g_assert_null(saved); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE); g_clear_error(&error);
  g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 2); g_assert_cmpuint(nostr_nip51_tag_size(nostr_nip51_document_tag(gnostr_list_document_get_codec(doc), FALSE, 1)), ==, 3);
  g_object_unref(doc); g_ptr_array_unref(ops); g_free(base); g_free(next); end(s, &b);
}
static void test_policy_majority(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED); GNostrListService *s = service_new(&b, &p);
  g_object_unref(apply_word(s, p.kind, "", "private", TRUE)); eose(s, p.kind); g_assert_cmpuint(b.sign_count, ==, 0);
  gnostr_list_service_set_private_sync_enabled(s, TRUE); pump(); g_assert_cmpuint(b.publish_count, ==, 1); GNostrListDocument *item = g_list_model_get_item(G_LIST_MODEL(s), 0); g_assert_false(gnostr_list_document_get_dirty(item)); g_object_unref(item);
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, ""); g_assert_nonnull(strstr(gnostr_list_document_get_target_outcomes(doc), "wss://two.example")); g_object_unref(doc); end(s, &b);
}
static void test_partial_frames(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED); GNostrListService *s = service_new(&b, &p); gnostr_list_service_set_private_sync_enabled(s, TRUE);
  g_object_unref(apply_word(s, p.kind, "", "private", TRUE)); notify(s, p.kind, "", 0, GNOSTR_LIST_EOSE, NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EOSE, NULL);
  notify(s, p.kind, "", 1, GNOSTR_LIST_CLOSED, NULL); notify(s, p.kind, "", 2, GNOSTR_LIST_AUTH_REQUIRED, NULL);
  g_assert_cmpuint(b.sign_count, ==, 0); g_assert_cmpint(gnostr_list_service_get_state(s, p.kind, ""), ==, GNOSTR_LIST_PARTIAL);
  notify(s, p.kind, "", 1, GNOSTR_LIST_EOSE, NULL); g_assert_cmpuint(b.publish_count, ==, 1); end(s, &b);
}
static void test_terminal_subscription_retry(void) {
  const GNostrListReadNotification failures[] = {GNOSTR_LIST_CLOSED, GNOSTR_LIST_ERROR};
  for (guint i = 0; i < G_N_ELEMENTS(failures); i++) {
    Backend b; backend_init(&b);
    GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED);
    GNostrListService *s = service_new(&b, &p);
    gnostr_list_service_set_private_sync_enabled(s, TRUE);
    g_object_unref(apply_word(s, p.kind, "", "private", TRUE));
    guint64 old = gnostr_list_service_get_read_serial(s);
    notify(s, p.kind, "", 0, failures[i], "restricted: test refusal");
    g_autoptr(GNostrListDocument) failed = gnostr_list_service_lookup(s, p.kind, "");
    g_assert_cmpstr(gnostr_list_document_get_error(failed), ==, "restricted: test refusal");
    gnostr_list_service_retry(s, p.kind, ""); pump();
    g_assert_cmpuint(b.opens, ==, 2);
    g_assert_cmpuint(b.closes, ==, 1);
    gnostr_list_service_notify(s, old, p.kind, "", urls[0], GNOSTR_LIST_EOSE, NULL);
    gnostr_list_service_notify(s, old, p.kind, "", urls[1], GNOSTR_LIST_EOSE, NULL); pump();
    g_assert_cmpuint(b.sign_count, ==, 0); /* a retired subscription cannot complete catch-up */
    eose(s, p.kind);
    g_assert_cmpuint(b.publish_count, ==, 1);
    end(s, &b);
  }
}
static void test_read_only_and_dependency(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_FOLLOW_LIST, GNOSTR_LIST_READ_ONLY); p.waiting_for_dependency = TRUE; GNostrListService *s = service_new(&b, &p);
  g_assert_cmpuint(b.opens, ==, 0); guint64 old = gnostr_list_service_get_read_serial(s); p.waiting_for_dependency = FALSE;
  g_assert_true(gnostr_list_service_update_read_plan(s, &p, 1, NULL)); g_assert_cmpuint(b.opens, ==, 1);
  gnostr_list_service_notify(s, old, p.kind, "", urls[0], GNOSTR_LIST_EOSE, NULL); pump(); g_assert_cmpint(gnostr_list_service_get_state(s, p.kind, ""), ==, GNOSTR_LIST_READING);
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref); g_ptr_array_add(ops, word_op("forbidden", FALSE)); GError *error = NULL;
  g_assert_null(gnostr_list_service_apply(s, p.kind, "", 0, ops, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&error); g_ptr_array_unref(ops);
  p.publish_policy = GNOSTR_LIST_AUTO_ALWAYS; g_assert_false(gnostr_list_service_update_read_plan(s, &p, 1, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error); end(s, &b);
}
static void test_unreadable(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED); GNostrListService *s = service_new(&b, &p);
  char *old = event(p.kind, "", 10, "[[\"word\",\"usable\"]]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, old);
  b.fail_decrypt = TRUE; char *cipher = event(p.kind, "", 20, "[]", "[[\"word\",\"private\"]]"); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, cipher);
  g_assert_cmpint(gnostr_list_service_get_state(s, p.kind, ""), ==, GNOSTR_LIST_UNREADABLE); g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 1);
  gnostr_list_service_set_private_sync_enabled(s, TRUE); eose(s, p.kind); g_assert_cmpuint(b.publish_count, ==, 0);
  b.fail_decrypt = FALSE; gnostr_list_service_retry(s, p.kind, ""); pump(); g_assert_cmpuint(count(s, p.kind, "", TRUE), ==, 1); g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 0);
  g_free(old); g_free(cipher); end(s, &b);
}
typedef struct { gboolean done; GVariant *outcomes; GError *error; } Result;
static void published(GObject *source, GAsyncResult *res, gpointer data) { Result *r = data; r->outcomes = gnostr_list_service_publish_finish(GNOSTR_LIST_SERVICE(source), res, &r->error); r->done = TRUE; }
static void explicit_publish(GNostrListService *s, Result *r, GCancellable *cancel) { gnostr_list_service_publish_async(s, NOSTR_KIND_MUTE_LIST, "", GNOSTR_LIST_APPROVE_PUBLIC | GNOSTR_LIST_APPROVE_PRIVATE, cancel, published, r); pump(); }
static void result_clear(Result *r) { g_clear_pointer(&r->outcomes, g_variant_unref); g_clear_error(&r->error); memset(r, 0, sizeof(*r)); }
static void test_sign_race(void) {
  Backend b; backend_init(&b); b.hold_sign = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p); eose(s, p.kind);
  g_object_unref(apply_word(s, p.kind, "", "one", TRUE)); Result r = {0}; explicit_publish(s, &r, NULL); g_assert_cmpuint(b.signs.length, ==, 1);
  g_object_unref(apply_word(s, p.kind, "", "two", TRUE)); sign_complete(g_queue_pop_head(&b.signs)); pump(); g_assert_true(r.done); g_assert_cmpuint(b.publish_count, ==, 0); result_clear(&r);
  explicit_publish(s, &r, NULL); sign_complete(g_queue_pop_head(&b.signs)); pump(); g_assert_true(r.done); g_assert_no_error(r.error); g_assert_cmpuint(b.publish_count, ==, 1); result_clear(&r); end(s, &b);
}
static void test_publish_race_and_cancel(void) {
  Backend b; backend_init(&b); b.hold_publish = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p); eose(s, p.kind);
  g_object_unref(apply_word(s, p.kind, "", "one", TRUE)); Result r = {0}; explicit_publish(s, &r, NULL); g_assert_cmpuint(b.publications.length, ==, 1);
  g_object_unref(apply_word(s, p.kind, "", "two", TRUE)); publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_ACCEPTED); pump();
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, ""); g_assert_true(gnostr_list_document_get_dirty(doc)); g_object_unref(doc); result_clear(&r);
  b.hold_crypto = TRUE; GCancellable *cancel = g_cancellable_new(); explicit_publish(s, &r, cancel); g_assert_cmpuint(b.crypto.length, ==, 1); g_cancellable_cancel(cancel); crypto_complete(&b, g_queue_pop_head(&b.crypto)); pump(); g_assert_true(r.done); g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_assert_cmpuint(b.publish_count, ==, 1);
  result_clear(&r); g_object_unref(cancel); end(s, &b);
}
static void test_resume(void) {
  Backend b; backend_init(&b); b.hold_publish = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED); GNostrListService *s = service_new(&b, &p);
  g_object_unref(apply_word(s, p.kind, "", "saved", TRUE)); gnostr_list_service_set_private_sync_enabled(s, TRUE); eose(s, p.kind);
  g_assert_cmpuint(b.sign_count, ==, 1); publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_TRANSPORT_FAILURE); pump();
  char *signed_json = g_strdup(b.last_signed); gnostr_list_service_close(s); pump(); g_object_unref(s);
  s = service_new(&b, &p); eose(s, p.kind); g_assert_cmpuint(b.publish_count, ==, 1); gnostr_list_service_set_private_sync_enabled(s, TRUE); pump();
  g_assert_cmpuint(b.publish_count, ==, 2); g_assert_cmpuint(b.sign_count, ==, 1); g_assert_cmpstr(b.last_signed, ==, signed_json);
  publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_ACCEPTED); pump(); g_free(signed_json); end(s, &b);
}
static void test_concurrency_shutdown(void) {
  Backend b; backend_init(&b); b.hold_crypto = TRUE;
  GNostrListService *s = gnostr_list_service_new(pk, 7, &adapter, &b, NULL); GNostrListReadPlanEntry p[17]; char *ids[17];
  for (guint i = 0; i < 17; i++) { ids[i] = g_strdup_printf("list-%u", i); p[i] = plan(NOSTR_KIND_BOOKMARK_SETS, GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED); p[i].identifier = ids[i]; }
  g_assert_true(gnostr_list_service_start(s, p, 17, NULL)); gnostr_list_service_set_private_sync_enabled(s, TRUE);
  for (guint i = 0; i < 17; i++) { g_object_unref(apply_word(s, p[i].kind, ids[i], "queued", TRUE)); notify(s, p[i].kind, ids[i], 0, GNOSTR_LIST_EOSE, NULL); notify(s, p[i].kind, ids[i], 1, GNOSTR_LIST_EOSE, NULL); }
  g_assert_cmpuint(b.crypto_running, ==, 16); g_assert_cmpuint(b.crypto_peak, ==, 16); crypto_complete(&b, g_queue_pop_head(&b.crypto)); pump(); g_assert_cmpuint(b.crypto_running, ==, 16); g_assert_cmpuint(b.crypto_peak, ==, 16);
  gnostr_list_service_close(s); while (!g_queue_is_empty(&b.crypto)) crypto_complete(&b, g_queue_pop_head(&b.crypto)); pump(); g_assert_cmpuint(b.publish_count, ==, 1);
  for (guint i = 0; i < 17; i++) g_free(ids[i]); g_object_unref(s); backend_clear(&b);
}
static void test_derive_generation(void) {
  Backend b; backend_init(&b); GNostrListAdapter a = adapter; a.derive_operations = derive; b.derive_once = TRUE;
  GNostrListService *s = gnostr_list_service_new(pk, 7, &a, &b, NULL); GNostrListReadPlanEntry p = plan(NOSTR_KIND_SIMPLE_GROUP_LIST, GNOSTR_LIST_AUTO_ALWAYS); p.required_sources = GNOSTR_LIST_SOURCES_ALL;
  g_assert_true(gnostr_list_service_start(s, &p, 1, NULL)); eose(s, p.kind); g_assert_cmpuint(b.sign_count, ==, 0);
  notify(s, p.kind, "", 2, GNOSTR_LIST_EOSE, NULL); g_assert_cmpuint(b.publish_count, ==, 1); g_assert_cmpuint(b.derive_count, >=, 1);
  b.valid = FALSE; GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref); g_ptr_array_add(ops, word_op("revoked", TRUE)); g_assert_null(gnostr_list_service_apply(s, p.kind, "", 1, ops, NULL)); g_ptr_array_unref(ops);
  end(s, &b);
}
static void test_occurrence_move(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p);
  char *base = event(p.kind, "", 10, "[[\"word\",\"same\"],[\"word\",\"middle\"],[\"word\",\"same\"],[\"x\",\"\",\"tail\"]]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, base);
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, "");
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_ptr_array_add(ops, gnostr_list_operation_move(FALSE, nostr_nip51_document_tag(gnostr_list_document_get_codec(doc), FALSE, 0), 1, 0));
  GNostrListDocument *saved = gnostr_list_service_apply(s, p.kind, "", 0, ops, NULL); g_assert_nonnull(saved);
  const NostrListDocument *codec = gnostr_list_document_get_codec(saved);
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, FALSE, 1), 1), ==, "same");
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, FALSE, 2), 1), ==, "middle");
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, FALSE, 3), 2), ==, "tail");
  g_object_unref(doc); g_object_unref(saved); g_ptr_array_unref(ops); g_free(base); end(s, &b);
}
static void test_oversized_remote(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p);
  char *base = event(p.kind, "", 10, "[[\"word\",\"usable\"]]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, base);
  char *content = g_strnfill(NOSTR_LIST_MAX_EVENT_BYTES + 1, 'a'); char *large = event(p.kind, "", 20, "[]", content); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, large);
  g_assert_cmpint(gnostr_list_service_get_state(s, p.kind, ""), ==, GNOSTR_LIST_UNREADABLE); g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 1);
  char *older = event(p.kind, "", 15, "[]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, older); g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 1);
  char *newest = event(p.kind, "", 30, "[]", NULL); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, newest); g_assert_cmpuint(count(s, p.kind, "", FALSE), ==, 0);
  g_free(base); g_free(content); g_free(large); g_free(older); g_free(newest); end(s, &b);
}
static void test_explicit_visibility_and_resume_targets(void) {
  Backend b; backend_init(&b); b.hold_publish = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p); eose(s, p.kind);
  g_object_unref(apply_word(s, p.kind, "", "public", FALSE)); gnostr_list_service_set_private_sync_enabled(s, TRUE); pump(); g_assert_cmpuint(b.sign_count, ==, 0);
  Result r = {0}; gnostr_list_service_publish_async(s, p.kind, "", GNOSTR_LIST_APPROVE_PRIVATE, NULL, published, &r); pump(); g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); result_clear(&r);
  explicit_publish(s, &r, NULL); publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_TRANSPORT_FAILURE); pump(); result_clear(&r);
  gnostr_list_service_close(s); pump(); g_object_unref(s);
  const char *changed[] = {urls[1], NULL}; p.targets = changed;
  s = service_new(&b, &p); eose(s, p.kind); gnostr_list_service_set_private_sync_enabled(s, TRUE); pump();
  g_assert_cmpuint(b.sign_count, ==, 2); g_assert_cmpuint(b.publish_count, ==, 2);
  publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_ACCEPTED); pump(); end(s, &b);
}
static void test_partial_retry_preserves_accepted(void) {
  Backend b; backend_init(&b); GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p); eose(s, p.kind);
  g_object_unref(apply_word(s, p.kind, "", "private", TRUE)); Result r = {0}; explicit_publish(s, &r, NULL); g_assert_no_error(r.error); result_clear(&r);
  gnostr_list_service_retry(s, p.kind, ""); pump(); g_assert_cmpuint(b.sign_count, ==, 1); g_assert_cmpuint(b.publish_count, ==, 2);
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, ""); json_t *outcomes = json_loads(gnostr_list_document_get_target_outcomes(doc), 0, NULL);
  g_assert_cmpint(json_integer_value(json_object_get(outcomes, urls[0])), ==, GNOSTR_LIST_ACCEPTED); g_assert_false(gnostr_list_document_get_dirty(doc)); json_decref(outcomes); g_object_unref(doc); end(s, &b);
}
static void test_own_event_and_accepted_prefix(void) {
  Backend b; backend_init(&b); b.hold_publish = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p); eose(s, p.kind);
  g_object_unref(apply_word(s, p.kind, "", "one", TRUE)); Result r = {0}; explicit_publish(s, &r, NULL);
  char *wire = g_strdup(b.last_signed); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, wire);
  g_assert_cmpuint(count(s, p.kind, "", TRUE), ==, 1);
  GNostrListDocument *doc = gnostr_list_service_lookup(s, p.kind, ""); g_assert_true(gnostr_list_document_get_dirty(doc)); g_object_unref(doc);
  g_object_unref(apply_word(s, p.kind, "", "tail", TRUE)); publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_ACCEPTED); pump();
  g_assert_no_error(r.error); g_assert_true(r.done); result_clear(&r); g_assert_cmpuint(count(s, p.kind, "", TRUE), ==, 2);
  doc = gnostr_list_service_lookup(s, p.kind, ""); g_assert_true(gnostr_list_document_get_dirty(doc)); g_assert_cmpint(gnostr_list_document_get_created_at(doc), ==, 1000); g_object_unref(doc);
  explicit_publish(s, &r, NULL); json_t *j = json_loads(b.last_signed, 0, NULL); g_assert_cmpint(json_integer_value(json_object_get(j, "created_at")), ==, 1001); json_decref(j);
  publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_ACCEPTED); pump(); result_clear(&r); g_free(wire); end(s, &b);
}
static void test_unreadable_invalidates_sign(void) {
  Backend b; backend_init(&b); b.hold_sign = TRUE; b.fail_decrypt = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p); eose(s, p.kind);
  g_object_unref(apply_word(s, p.kind, "", "local", TRUE)); Result r = {0}; explicit_publish(s, &r, NULL);
  char *newer = event(p.kind, "", 20, "[]", "unreadable"); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, newer);
  sign_complete(g_queue_pop_head(&b.signs)); pump(); g_assert_cmpuint(b.publish_count, ==, 0); g_assert_true(r.done); result_clear(&r); g_free(newer); end(s, &b);
}
static void test_private_resume_rebase(void) {
  Backend b; backend_init(&b); b.hold_publish = TRUE; GNostrListReadPlanEntry p = plan(NOSTR_KIND_MUTE_LIST, GNOSTR_LIST_EXPLICIT_ONLY); GNostrListService *s = service_new(&b, &p);
  char *a = event(p.kind, "", 10, "[]", "[[\"word\",\"remote-A\"]]"); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, a);
  g_object_unref(apply_word(s, p.kind, "", "local", TRUE)); eose(s, p.kind); Result r = {0}; explicit_publish(s, &r, NULL);
  publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_TRANSPORT_FAILURE); pump();
  char *newer = event(p.kind, "", 20, "[]", "[[\"word\",\"remote-B\"]]"); notify(s, p.kind, "", 0, GNOSTR_LIST_EVENT, newer);
  /* Private-only rebase leaves the local revision unchanged, but invalidates
   * the old stored ciphertext even though its public tags still match. */
  result_clear(&r); g_assert_cmpuint(b.sign_count, ==, 1);
  gnostr_list_service_close(s); pump(); g_object_unref(s); s = service_new(&b, &p); eose(s, p.kind); gnostr_list_service_set_private_sync_enabled(s, TRUE); pump();
  g_assert_cmpuint(b.sign_count, ==, 2);
  json_t *j = json_loads(b.last_signed, 0, NULL); const char *content = json_string_value(json_object_get(j, "content")); g_assert_nonnull(strstr(content, "remote-B")); g_assert_null(strstr(content, "remote-A")); json_decref(j);
  publication_complete(g_queue_pop_head(&b.publications), GNOSTR_LIST_ACCEPTED); pump(); g_free(a); g_free(newer); end(s, &b);
}
static void test_open_retry_auto_always_resume(void) {
  Backend b; backend_init(&b); b.fail_open = TRUE; GNostrListService *s = gnostr_list_service_new(pk, 7, &adapter, &b, NULL);
  GNostrListReadPlanEntry p = plan(NOSTR_KIND_SIMPLE_GROUP_LIST, GNOSTR_LIST_AUTO_ALWAYS); p.required_sources = GNOSTR_LIST_SOURCES_ALL; GError *error = NULL;
  g_assert_false(gnostr_list_service_start(s, &p, 1, &error)); g_clear_error(&error); b.fail_open = FALSE; gnostr_list_service_retry(s, p.kind, ""); pump(); g_assert_cmpuint(b.opens, ==, 2);
  g_object_unref(apply_word(s, p.kind, "", "group", TRUE)); eose(s, p.kind); notify(s, p.kind, "", 2, GNOSTR_LIST_EOSE, NULL); g_assert_cmpuint(b.sign_count, ==, 1);
  gnostr_list_service_close(s); pump(); g_object_unref(s); s = service_new(&b, &p); eose(s, p.kind); notify(s, p.kind, "", 2, GNOSTR_LIST_EOSE, NULL);
  g_assert_cmpuint(b.publish_count, ==, 2); g_assert_cmpuint(b.sign_count, ==, 1); end(s, &b);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL); sk = nostr_key_generate_private(); pk = nostr_key_get_public(sk);
  g_test_add_func("/list-service/ordering", test_ordering);
  g_test_add_func("/list-service/own-event-accepted-prefix", test_own_event_and_accepted_prefix);
  g_test_add_func("/list-service/unreadable-invalidates-sign", test_unreadable_invalidates_sign);
  g_test_add_func("/list-service/private-resume-rebase", test_private_resume_rebase);
  g_test_add_func("/list-service/open-retry-auto-always-resume", test_open_retry_auto_always_resume);
  g_test_add_func("/list-service/terminal-subscription-retry", test_terminal_subscription_retry);
  g_test_add_func("/list-service/occurrence-move", test_occurrence_move);
  g_test_add_func("/list-service/oversized-remote", test_oversized_remote);
  g_test_add_func("/list-service/explicit-visibility-resume-targets", test_explicit_visibility_and_resume_targets);
  g_test_add_func("/list-service/partial-retry-preserves-accepted", test_partial_retry_preserves_accepted);
  g_test_add_func("/list-service/local-transaction-rebase", test_local_transaction_rebase);
  g_test_add_func("/list-service/policy-majority", test_policy_majority);
  g_test_add_func("/list-service/partial-frames", test_partial_frames);
  g_test_add_func("/list-service/read-only-dependency", test_read_only_and_dependency);
  g_test_add_func("/list-service/unreadable", test_unreadable);
  g_test_add_func("/list-service/sign-race", test_sign_race);
  g_test_add_func("/list-service/publish-race-cancel", test_publish_race_and_cancel);
  g_test_add_func("/list-service/resume", test_resume);
  g_test_add_func("/list-service/concurrency-shutdown", test_concurrency_shutdown);
  g_test_add_func("/list-service/derive-generation", test_derive_generation);
  int result = g_test_run(); g_free(sk); g_free(pk); return result;
}
