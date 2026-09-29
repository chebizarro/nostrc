/* Message Requests and New Message (privacy charter §7.9, PD-2, PD-8, PT-8,
 * §8.2 G18), end to end on real parts: a real account controller with the
 * mock signer on the private test bus, the real contact directory (G10) over
 * a recording scope transport (H1) and the fake GhClock (H6), real SQLCipher
 * stores with the G05 delegate, and a recording HTTP transport for NIP-05.
 *
 * Without arguments (no display needed):
 *  - Accept, Delete and Block persist across a store reopen; a deleted
 *    request's older backfill stays gone and a newer message starts it
 *    again; a blocked room's later messages are recorded as seen only (never
 *    listed or notified) until the account starts a conversation with them
 *    again (New Message), which no relay's self-copy does;
 *  - PT-8: the directory never asks for a request's sender (no REQ names
 *    it, no kind 0) through several scheduled runs, nor for a deleted or
 *    blocked one; after Accept, one REQ for the sender alone, with kind 0.
 * With --gui (needs a display; self-skips with 77 without one):
 *  - the Message Requests page: npub, count and first message as plain text,
 *    Delete/Block behind an AdwAlertDialog (cancel keeps it), unavailable
 *    without storage (and says why), PT-8 through its Accept button, which
 *    opens the conversation;
 *  - New Message: consent rows (no NIP-05 GET and no 10050 REQ before the
 *    user chooses them; Enter never looks anything up), the 10-recipient
 *    limit copy, secrets refused, note to self, Ctrl+N and opening the room;
 *    a blocked person's room is unblocked by starting it, said beforehand;
 *    a cancelled check round's late answers don't end the next round;
 *  - with GROUNDHOG_TEST_SCREENSHOTS=<dir>, PNGs of both (wide and 360x294).
 * Waits iterate the main context against a deadline; they never sleep. */
#include "gh-contact-directory.h"
#include "gh-conversation-list.h"
#include "gh-conversation-view.h"
#include "gh-new-message-dialog.h"
#include "gh-recipient.h"
#include "gh-requests-view.h"
#include "gh-store-conversations.h"
#include "gh-test-signer.h"

#include "nostr-tag.h"

#include <glib/gstdio.h>

void groundhog_register_resource(void);

#define DISC_A   "wss://discovery-a.test.invalid"
#define DISC_B   "wss://discovery-b.test.invalid"
#define R1       "wss://r1.test.invalid"
#define STORE_ID "0b8f3c2e-5d1a-4e6f-9a7b-2c3d4e5f6a7b"
#define T0       G_GINT64_CONSTANT(1900000000)
#define N_PEOPLE 14
/* This build creates no encrypted groups (GH_FEATURE_ENCRYPTED_GROUPS 0), so
 * the limit doesn't point to them; a build that does says the charter's copy. */
#define LIMIT_COPY "A private conversation can include up to 10 people. Larger groups " \
                   "aren't available in this version yet."
#define LIMIT_COPY_GROUPS "A private conversation can include up to 10 people. For larger " \
                          "groups, create an encrypted group."

typedef struct {
  gchar *sk;
  gchar *pk;
  gchar *npub;
} Person;

static Person people[N_PEOPLE];
static gchar *hex_alice, *npub_alice;
static GhTestBus shared_bus;
static gboolean gui;

static void
drain(void)
{
  for (int i = 0; i < 500 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

static void
rm_rf(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      rm_rf(child);
    }
    if (dir)
      g_dir_close(dir);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

/* ---- events and rumors ------------------------------------------------------------ */

static gchar *
signed_by(const gchar *sk, int kind, gint64 created_at, NostrTags *tags, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content ? content : "");
  nostr_event_set_tags(event, tags ? tags : nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

static gchar *
inbox_list(const Person *who, gint64 created_at, const gchar *relay)
{
  return signed_by(who->sk, 10050, created_at,
                   nostr_tags_new(1, nostr_tag_new("relay", relay, NULL)), "");
}

/* A NIP-17 rumor between the account and who: written by the account
 * (outgoing) or by who (incoming). */
static GhMessage *
rumor(const Person *who, gboolean from_account, gint64 created_at, const gchar *content,
      const gchar *subject)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, from_account ? hex_alice : who->pk);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", from_account ? who->pk : hex_alice, NULL));
  if (subject)
    nostr_tags_append(tags, nostr_tag_new("subject", subject, NULL));
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(hex_alice, raw, &error);
  free(raw);
  g_assert_no_error(error);
  return message;
}

static gchar *
random_id(void)
{
  GString *id = g_string_sized_new(64);
  for (guint i = 0; i < 8; i++)
    g_string_append_printf(id, "%08x", g_random_int());
  return g_string_free(id, FALSE);
}

static gchar *
room_id_with(const Person *who)
{
  return strcmp(hex_alice, who->pk) < 0 ? g_strjoin(",", hex_alice, who->pk, NULL)
                                        : g_strjoin(",", who->pk, hex_alice, NULL);
}

/* ---- recording scope transport (H1) ------------------------------------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  GPtrArray *authors;
  gboolean profiles; /* kind 0 asked for */
  gboolean answered;
} Req;

typedef struct {
  GPtrArray *reqs;
} Rec;

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_free(req->url);
  g_ptr_array_unref(req->authors);
  g_free(req);
}

static gpointer
rec_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
         GError **error)
{
  (void)error;
  Rec *rec = data;
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->authors = g_ptr_array_new_with_free_func(g_free);
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  for (size_t i = 0; i < nostr_filter_authors_len(filter); i++)
    g_ptr_array_add(req->authors, g_strdup(nostr_filter_authors_get(filter, i)));
  for (size_t i = 0; i < nostr_filter_kinds_len(filter); i++) {
    int kind = nostr_filter_kinds_get(filter, i);
    g_assert_true(kind == 0 || kind == 10050);
    req->profiles |= kind == 0;
  }
  /* Discovery relays only (PD-12). */
  g_assert_true(g_str_equal(url, DISC_A) || g_str_equal(url, DISC_B));
  g_ptr_array_add(rec->reqs, req);
  return req;
}

static void
rec_close(gpointer handle, gpointer data)
{
  (void)handle;
  (void)data;
}

static const GhRelayTransport rec_transport = { rec_open, rec_close };

static gboolean
req_asks(Req *req, const gchar *pubkey)
{
  return g_ptr_array_find_with_equal_func(req->authors, pubkey, g_str_equal, NULL);
}

/* How many REQs so far name pubkey; with profiles, only those asking kind 0. */
static guint
reqs_naming(Rec *rec, const gchar *pubkey, gboolean profiles_only)
{
  guint n = 0;
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    n += req_asks(req, pubkey) && (!profiles_only || req->profiles);
  }
  return n;
}

/* EOSE (after the given event, if any) on every REQ not answered yet. */
static void
answer_all(Rec *rec, const gchar *event_json)
{
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    if (req->answered)
      continue;
    req->answered = TRUE;
    if (event_json)
      gh_relay_scope_event(req->scope, req->url, event_json);
    gh_relay_scope_eose(req->scope, req->url);
  }
}

/* ---- recording HTTP transport (NIP-05) ----------------------------------------------- */

typedef struct {
  GPtrArray *urls;
  gchar *answer;
} Http;

static void
http_get_async(gpointer data, const gchar *uri, gsize max_bytes, GCancellable *cancellable,
               GAsyncReadyCallback callback, gpointer user_data)
{
  Http *http = data;
  (void)max_bytes;
  g_ptr_array_add(http->urls, g_strdup(uri));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (http->answer)
    g_task_return_pointer(task, g_bytes_new(http->answer, strlen(http->answer)),
                          (GDestroyNotify)g_bytes_unref);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "The server answered 404");
  g_object_unref(task);
}

static GBytes *
http_get_finish(gpointer data, GAsyncResult *result, GError **error)
{
  (void)data;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static const GhHttpTransport http_transport = { http_get_async, http_get_finish };

/* ---- fixture ------------------------------------------------------------------------ */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GhClock *clock;
  gchar *data_dir;
  GhStore *store;
  GhStoreConversations *delegate;
  GhConversationStore *model;
  Rec rec;
  GhContactDirectory *dir;
  Http http;
  GhNip05 *nip05;
  GhWindow *window;
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub_alice);
  info->label = g_strdup("Alice");
  g_ptr_array_add(ids, info);
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static void
store_open(Fixture *f)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8)(0x5a ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { f->data_dir, hex_alice, NULL, NULL, f->clock };
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  f->delegate = gh_store_conversations_new(f->store);
  g_assert_true(gh_store_conversations_attach(f->delegate, f->model, 0, &error));
  g_assert_no_error(error);
  if (f->dir)
    g_assert_true(gh_contact_directory_set_store(f->dir, f->store, NULL));
}

static void
store_close(Fixture *f)
{
  if (f->dir)
    gh_contact_directory_set_store(f->dir, NULL, NULL);
  gh_store_conversations_close(f->delegate);
  g_clear_object(&f->delegate);
  gh_store_close(f->store);
  f->store = NULL;
}

/* A restart: the store closes, a new model is restored from it. */
static void
reopen(Fixture *f)
{
  store_close(f);
  if (f->dir)
    gh_contact_directory_set_conversations(f->dir, NULL);
  g_clear_object(&f->model);
  f->model = gh_conversation_store_new();
  if (f->dir)
    gh_contact_directory_set_conversations(f->dir, f->model);
  store_open(f);
}

static void
fixture_up(Fixture *f, gboolean with_directory)
{
  memset(f, 0, sizeof *f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC_A, DISC_B, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  g_settings_set_string(f->settings, "network-mode", "system");
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  f->data_dir = g_dir_make_tmp("groundhog-requests-XXXXXX", NULL);
  f->model = gh_conversation_store_new();
  f->rec.reqs = g_ptr_array_new_with_free_func(req_free);
  if (with_directory) {
    GhContactDirectoryConfig config = {
      .accounts = f->accounts,
      .settings = f->settings,
      .clock = f->clock,
      .transport = &rec_transport,
      .transport_data = &f->rec,
    };
    f->dir = gh_contact_directory_new(&config);
    gh_contact_directory_set_conversations(f->dir, f->model);
  }
  f->http.urls = g_ptr_array_new_with_free_func(g_free);
  f->nip05 = gh_nip05_new(f->settings, &http_transport, &f->http);
  store_open(f);
}

static void
fixture_down(Fixture *f)
{
  if (f->window)
    gtk_window_destroy(GTK_WINDOW(f->window));
  drain();
  store_close(f);
  if (f->dir)
    gh_test_release(f->dir);
  g_clear_object(&f->nip05);
  g_clear_object(&f->model);
  drain();
  g_ptr_array_unref(f->rec.reqs);
  g_ptr_array_unref(f->http.urls);
  g_free(f->http.answer);
  gh_test_release(f->accounts);
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  g_settings_reset(f->settings, "discovery-relays");
  g_settings_reset(f->settings, "network-mode");
  g_object_unref(f->settings);
  gh_test_signer_down(&shared_bus, &f->mock);
  gh_clock_unref(f->clock);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
}

static GhConversationAddResult
deliver(Fixture *f, const Person *who, gboolean from_account, gint64 created_at,
        const gchar *content, const gchar *subject)
{
  g_autoptr(GhMessage) message = rumor(who, from_account, created_at, content, subject);
  g_autofree gchar *wrap = random_id();
  g_autoptr(GError) error = NULL;
  GhConversationAddResult result = gh_conversation_store_admit(f->model, message,
                                                               from_account ? NULL : wrap,
                                                               &error);
  g_assert_no_error(error);
  return result;
}

static GhConversation *
room_of(Fixture *f, const Person *who)
{
  g_autofree gchar *id = room_id_with(who);
  return gh_conversation_store_lookup(f->model, id);
}

static gboolean
blocked(Fixture *f, const Person *who)
{
  g_autofree gchar *id = room_id_with(who);
  gboolean is_blocked = FALSE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_is_blocked(f->delegate, id, &is_blocked, &error));
  g_assert_no_error(error);
  return is_blocked;
}

/* Runs every directory timer due within the next seconds, answering each
 * REQ with EOSE (nothing found). */
static void
run_directory_for(Fixture *f, gint64 seconds)
{
  gint64 end = gh_clock_get_monotonic_time(f->clock) + seconds * G_USEC_PER_SEC;
  while (TRUE) {
    gint64 next = gh_clock_fake_get_next_deadline(f->clock);
    if (next < 0 || next > end)
      break;
    gh_clock_fake_advance(f->clock, MAX(next - gh_clock_get_monotonic_time(f->clock), 0));
    drain();
    answer_all(&f->rec, NULL);
    drain();
  }
  gh_clock_fake_advance(f->clock, MAX(end - gh_clock_get_monotonic_time(f->clock), 0));
  drain();
}

/* ---- store: Accept, Delete, Block persist ------------------------------------------ */

static void
test_accept_delete_block_persist(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  const Person *x = &people[0], *y = &people[1], *z = &people[2];
  g_assert_cmpint(deliver(&f, x, FALSE, T0 - 300, "hi from X", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, x, FALSE, T0 - 200, "again", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, y, FALSE, T0 - 300, "hi from Y", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, z, FALSE, T0 - 300, "hi from Z", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_true(gh_conversation_get_is_request(room_of(&f, x)));

  /* Accept (what the view's Accept does) survives a restart. */
  gh_conversation_accept(room_of(&f, x));
  reopen(&f);
  g_assert_false(gh_conversation_get_is_request(room_of(&f, x)));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room_of(&f, x))), ==, 2);
  g_assert_true(gh_conversation_get_is_request(room_of(&f, y)));

  /* Delete: gone, older backfill stays gone, a newer message starts over. */
  g_autofree gchar *y_room = room_id_with(y);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_forget(f.delegate, y_room, &error));
  g_assert_no_error(error);
  g_assert_null(room_of(&f, y));
  reopen(&f);
  g_assert_null(room_of(&f, y));
  g_assert_cmpint(deliver(&f, y, FALSE, T0 - 100, "old backfill", NULL), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_null(room_of(&f, y));
  g_assert_cmpint(deliver(&f, y, FALSE, T0 + 10, "new", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_true(gh_conversation_get_is_request(room_of(&f, y)));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room_of(&f, y))), ==, 1);

  /* Block: gone and seen-only from now on, across restarts. */
  g_autofree gchar *z_room = room_id_with(z);
  g_assert_false(blocked(&f, z));
  g_assert_true(gh_store_conversations_block_and_forget(f.delegate, z_room, &error));
  g_assert_no_error(error);
  g_assert_null(room_of(&f, z));
  g_assert_true(blocked(&f, z));
  g_autoptr(GhMessage) later = rumor(z, FALSE, T0 + 20, "let me in", NULL);
  g_autofree gchar *wrap = random_id();
  g_assert_cmpint(gh_conversation_store_admit(f.model, later, wrap, &error), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_no_error(error);
  g_assert_null(room_of(&f, z));
  /* Recorded as seen: the inbox skips the wrap before any signer call. */
  g_assert_true(gh_conversation_store_has_wrap(f.model, wrap));
  reopen(&f);
  g_assert_null(room_of(&f, z));
  g_assert_true(blocked(&f, z));
  g_assert_cmpint(deliver(&f, z, FALSE, T0 + 30, "still here", NULL), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_null(room_of(&f, z));
  /* Nothing of the blocked sender's text reached the store's rows. */
  g_assert_false(gh_store_conversations_block_and_forget(f.delegate, "not a room", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);

  /* New Message to them again (G18) unblocks the room: said first
   * (gh_conversation_store_is_blocked), then opened, accepted and empty
   * (Block forgot it), never hidden. Sending in it is test_outbox.c's
   * send-lifts-block (T-enqueue). */
  const gchar *z_peers[] = { z->pk, NULL };
  g_assert_true(gh_conversation_store_is_blocked(f.model, z_peers));
  GhConversation *opened = gh_conversation_store_open_room(f.model, z_peers, &error);
  g_assert_no_error(error);
  g_assert_true(opened == room_of(&f, z));
  g_assert_false(blocked(&f, z));
  g_assert_false(gh_conversation_store_is_blocked(f.model, z_peers));
  g_assert_false(gh_conversation_get_is_request(opened));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(opened)), ==, 0);
  g_assert_cmpint(deliver(&f, z, FALSE, T0 + 50, "thanks", NULL), ==, GH_CONVERSATION_ADD_NEW);
  reopen(&f);
  g_assert_false(gh_conversation_get_is_request(room_of(&f, z)));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room_of(&f, z))), ==, 1);
  fixture_down(&f);
}

/* A block kept for Undo (gh_store_conversations_set_blocked) survives every
 * self-copy a relay delivers: a replayed duplicate, one older than the read
 * marker, and a new one written on another device (the block is this
 * device's, P8). Writing from this device lifts it: New Message to them here,
 * or sending (T-enqueue, test_outbox.c send-lifts-block). */
static void
test_replayed_self_copy_keeps_block(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  const Person *w = &people[3];
  g_autofree gchar *room = room_id_with(w);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(deliver(&f, w, TRUE, T0 + 100, "hi", NULL), ==, GH_CONVERSATION_ADD_NEW);
  deliver(&f, w, FALSE, T0 + 110, "hey", NULL);
  g_assert_true(gh_store_conversations_set_blocked(f.delegate, room, TRUE, &error));
  g_assert_no_error(error);
  g_assert_true(blocked(&f, w));

  /* The same self-copy again (a duplicate), and an older one never seen. */
  deliver(&f, w, TRUE, T0 + 100, "hi", NULL);
  g_assert_true(blocked(&f, w));
  deliver(&f, w, TRUE, T0 + 50, "an older self-copy", NULL);
  g_assert_true(blocked(&f, w));
  g_assert_null(room_of(&f, w));
  reopen(&f);
  g_assert_true(blocked(&f, w));

  /* A new self-copy from another device, after the block: kept unlisted. */
  g_autoptr(GhMessage) elsewhere = rumor(w, TRUE, T0 + 200, "from my phone", NULL);
  g_autofree gchar *wrap = random_id();
  g_assert_cmpint(gh_conversation_store_admit(f.model, elsewhere, wrap, &error), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_no_error(error);
  g_assert_true(blocked(&f, w));
  g_assert_null(room_of(&f, w));

  /* New Message to them lifts it, with the history kept for Undo. */
  const gchar *peers[] = { w->pk, NULL };
  GhConversation *opened = gh_conversation_store_open_room(f.model, peers, &error);
  g_assert_no_error(error);
  g_assert_false(blocked(&f, w));
  g_assert_true(opened == room_of(&f, w));
  g_assert_false(gh_conversation_get_is_request(opened));
  /* hi, hey, the older self-copy and the one from the phone. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(opened)), ==, 4);
  fixture_down(&f);
}

/* ---- PT-8 without a display ------------------------------------------------------- */

/* The directory never asks for a request's sender, nor for a deleted or
 * blocked one, however many scheduled runs pass; accepting asks for that
 * sender alone, kind 0 included, within U(5, 60) s. */
static void
test_pt8_until_accept(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *x = &people[0], *y = &people[1], *z = &people[2], *w = &people[3];
  deliver(&f, x, FALSE, T0 - 300, "request", NULL);
  deliver(&f, y, FALSE, T0 - 300, "to delete", NULL);
  deliver(&f, z, FALSE, T0 - 300, "to block", NULL);
  deliver(&f, w, TRUE, T0 - 300, "a contact", NULL);
  g_autofree gchar *y_room = room_id_with(y);
  g_autofree gchar *z_room = room_id_with(z);
  g_assert_true(gh_store_conversations_forget(f.delegate, y_room, NULL));
  g_assert_true(gh_store_conversations_block_and_forget(f.delegate, z_room, NULL));
  /* Two days of scheduled refreshes. */
  run_directory_for(&f, 2 * 86400);
  g_assert_cmpuint(reqs_naming(&f.rec, w->pk, FALSE), >, 0);
  for (const Person *p = people; p < people + 3; p++)
    g_assert_cmpuint(reqs_naming(&f.rec, p->pk, FALSE), ==, 0);

  guint before = f.rec.reqs->len;
  gh_conversation_accept(room_of(&f, x));
  run_directory_for(&f, 60);
  g_assert_cmpuint(f.rec.reqs->len, >, before);
  Req *named = g_ptr_array_index(f.rec.reqs, before);
  g_assert_cmpuint(named->authors->len, ==, 1);
  g_assert_true(req_asks(named, x->pk));
  g_assert_true(named->profiles);
  for (const Person *p = people + 1; p < people + 3; p++)
    g_assert_cmpuint(reqs_naming(&f.rec, p->pk, FALSE), ==, 0);
  fixture_down(&f);
}

/* ---- GUI helpers ---------------------------------------------------------------------- */

static gpointer
template_child(gpointer widget, GType type, const char *name)
{
  GObject *child = gtk_widget_get_template_child(GTK_WIDGET(widget), type, name);
  g_assert_nonnull(child);
  return child;
}

static void
attach_window(Fixture *f, gboolean with_backend);

static gboolean
store_forget(gpointer data, GhConversation *request, GError **error)
{
  Fixture *f = data;
  return gh_store_conversations_forget(f->delegate, gh_conversation_get_room_id(request), error);
}

static gboolean
store_block(gpointer data, GhConversation *request, GError **error)
{
  Fixture *f = data;
  return gh_store_conversations_block_and_forget(f->delegate, gh_conversation_get_room_id(request), error);
}

static const gchar *
dir_name(gpointer data, const gchar *pubkey)
{
  return gh_contact_directory_get_display_name(data, pubkey);
}

static const gchar *
dir_nip05(gpointer data, const gchar *pubkey)
{
  return gh_contact_directory_get_nip05(data, pubkey);
}

static GhNewMessageConfig
new_message_config(Fixture *f)
{
  GhNewMessageConfig config = {
    .conversations = f->model,
    .inboxes = f->dir ? GH_INBOX_RESOLVER(f->dir) : NULL,
    .nip05 = f->nip05,
    .settings = f->settings,
    .display_name = f->dir ? dir_name : NULL,
    .claimed_nip05 = f->dir ? dir_nip05 : NULL,
    .names_data = f->dir,
  };
  return config;
}

/* The window the application builds: list, requests page (with the store's
 * Delete/Block as gh-app-services.c wires them) and New Message. */
static void
attach_window(Fixture *f, gboolean with_backend)
{
  f->window = gh_window_new(NULL);
  gh_conversation_list_attach(f->window, f->model, f->settings);
  if (with_backend) {
    static const GhRequestsBackend backend = { store_forget, store_block };
    gh_requests_view_set_backend(gh_conversation_list_get_requests_view(f->window), &backend, f,
                                 NULL);
  }
  GhNewMessageConfig config = new_message_config(f);
  gh_new_message_attach(f->window, &config);
  GhStatus *status = gh_window_get_status(f->window);
  gh_status_set_account_active(status, TRUE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  gh_status_set_inbox(status, GH_STATUS_INBOX_LIVE, NULL);
  gtk_window_set_default_size(GTK_WINDOW(f->window), 900, 600);
  gtk_window_present(GTK_WINDOW(f->window));
  drain();
}

static GhRequestsView *
requests_view(Fixture *f)
{
  return gh_conversation_list_get_requests_view(f->window);
}

static const gchar *
label_text(gpointer widget, GType type, const gchar *name)
{
  return gtk_label_get_text(template_child(widget, type, name));
}

static void
respond(AdwAlertDialog *dialog, const gchar *response)
{
  g_assert_nonnull(dialog);
  /* As a button does: the dialog answers, then closes. */
  g_autoptr(AdwAlertDialog) keep = g_object_ref(dialog);
  g_signal_emit_by_name(dialog, "response", response);
  if (gtk_widget_get_root(GTK_WIDGET(dialog)))
    adw_dialog_force_close(ADW_DIALOG(dialog));
  drain();
}

/* ---- GUI: the Message Requests page -------------------------------------------------- */

static void
test_gui_requests_view(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *x = &people[0], *y = &people[1], *z = &people[2];
  deliver(&f, x, FALSE, T0 - 300, "<b>hi</b> https://example.com/prize &amp; more", "Prize");
  deliver(&f, x, FALSE, T0 - 200, "second", NULL);
  deliver(&f, y, FALSE, T0 - 250, "from y", NULL);
  deliver(&f, z, FALSE, T0 - 240, "from z", NULL);
  attach_window(&f, TRUE);
  GhRequestsView *view = requests_view(&f);
  GhContentPage *content = gh_window_get_content(f.window);
  GtkStack *stack = gh_content_page_get_stack(content);

  /* Choosing a request shows it in the Message Requests page. */
  g_assert_true(gh_window_open_item(f.window, room_of(&f, x)));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "requests");
  g_assert_true(gh_requests_view_get_request(view) == room_of(&f, x));
  g_assert_null(gh_conversation_view_get_conversation(
    GH_CONVERSATION_VIEW(gh_content_page_get_view(content))));
  g_autofree gchar *short_npub = gh_recipient_npub_short(x->pk);
  g_autofree gchar *grouped = gh_recipient_npub_grouped(x->pk);
  g_assert_cmpstr(label_text(view, GH_TYPE_REQUESTS_VIEW, "title_label"), ==, short_npub);
  g_assert_cmpstr(label_text(view, GH_TYPE_REQUESTS_VIEW, "npub_label"), ==, grouped);
  g_assert_cmpstr(label_text(view, GH_TYPE_REQUESTS_VIEW, "count_label"), ==, "2 messages");
  g_assert_cmpstr(label_text(view, GH_TYPE_REQUESTS_VIEW, "subject_label"), ==, "Prize");
  /* Plain text: no markup, nothing linkified. */
  GtkLabel *message = template_child(view, GH_TYPE_REQUESTS_VIEW, "message_label");
  g_assert_cmpstr(gtk_label_get_text(message), ==,
                  "<b>hi</b> https://example.com/prize &amp; more");
  g_assert_false(gtk_label_get_use_markup(message));
  g_assert_cmpstr(gtk_label_get_label(message), ==, gtk_label_get_text(message));

  /* Delete asks first; Cancel keeps it. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(view), "requests.delete", NULL));
  AdwAlertDialog *confirm = gh_requests_view_get_confirmation(view);
  g_assert_cmpstr(adw_alert_dialog_get_heading(confirm), ==, "Delete Request?");
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(confirm, "delete-confirm"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  g_assert_cmpstr(adw_alert_dialog_get_close_response(confirm), ==, "delete-cancel");
  respond(confirm, "delete-cancel");
  g_assert_null(gh_requests_view_get_confirmation(view));
  g_assert_nonnull(room_of(&f, x));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(view), "requests.delete", NULL));
  respond(gh_requests_view_get_confirmation(view), "delete-confirm");
  g_assert_null(room_of(&f, x));
  g_assert_null(gh_requests_view_get_request(view));

  /* Block asks first too, naming the npub. */
  g_assert_true(gh_window_open_item(f.window, room_of(&f, y)));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(view), "requests.block", NULL));
  confirm = gh_requests_view_get_confirmation(view);
  g_autofree gchar *y_short = gh_recipient_npub_short(y->pk);
  g_autofree gchar *heading = g_strdup_printf("Block %s?", y_short);
  g_assert_cmpstr(adw_alert_dialog_get_heading(confirm), ==, heading);
  /* Honest about what Block does: this conversation, undone by writing. */
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(confirm), "in this conversation"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(confirm),
                          "start a new message to them: that unblocks the conversation"));
  respond(confirm, "block-confirm");
  g_assert_null(room_of(&f, y));
  g_assert_true(blocked(&f, y));
  g_assert_cmpint(deliver(&f, y, FALSE, T0 + 5, "hello?", NULL), ==, GH_CONVERSATION_ADD_HIDDEN);

  /* Both persisted. */
  reopen(&f);
  g_assert_null(room_of(&f, x));
  g_assert_null(room_of(&f, y));
  g_assert_true(blocked(&f, y));
  fixture_down(&f);
}

static void
test_gui_requests_without_storage(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  deliver(&f, &people[0], FALSE, T0 - 300, "hi", NULL);
  attach_window(&f, FALSE);
  GhRequestsView *view = requests_view(&f);
  g_assert_true(gh_window_open_item(f.window, room_of(&f, &people[0])));
  GtkWidget *block = template_child(view, GH_TYPE_REQUESTS_VIEW, "block_button");
  GtkWidget *remove = template_child(view, GH_TYPE_REQUESTS_VIEW, "delete_button");
  g_assert_false(gtk_widget_is_sensitive(block));
  g_assert_false(gtk_widget_is_sensitive(remove));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(block), ==,
                  "Needs private message storage on this device");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(remove), ==,
                  "Needs private message storage on this device");
  g_assert_null(gh_requests_view_get_confirmation(view));
  GtkWidget *accept = template_child(view, GH_TYPE_REQUESTS_VIEW, "accept_button");
  g_assert_true(gtk_widget_is_sensitive(accept));
  fixture_down(&f);
}

/* PT-8 through the page: nothing is asked about the sender while the
 * request is shown, however long; Accept opens the conversation and lets
 * the directory ask for the sender's name. */
static void
test_gui_pt8_accept(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *x = &people[4], *w = &people[5];
  deliver(&f, x, FALSE, T0 - 300, "hello", NULL);
  deliver(&f, w, TRUE, T0 - 300, "a contact", NULL);
  attach_window(&f, TRUE);
  GhRequestsView *view = requests_view(&f);
  g_assert_true(gh_window_open_item(f.window, room_of(&f, x)));
  run_directory_for(&f, 86400);
  g_assert_cmpuint(reqs_naming(&f.rec, w->pk, FALSE), >, 0);
  g_assert_cmpuint(reqs_naming(&f.rec, x->pk, FALSE), ==, 0);
  g_autofree gchar *short_npub = gh_recipient_npub_short(x->pk);
  g_assert_cmpstr(label_text(view, GH_TYPE_REQUESTS_VIEW, "title_label"), ==, short_npub);

  GtkWidget *accept = template_child(view, GH_TYPE_REQUESTS_VIEW, "accept_button");
  g_signal_emit_by_name(accept, "clicked");
  drain();
  GhConversation *room = room_of(&f, x);
  g_assert_false(gh_conversation_get_is_request(room));
  GhContentPage *content = gh_window_get_content(f.window);
  g_assert_true(gh_content_page_get_conversation_shown(content));
  g_assert_true(gh_conversation_view_get_conversation(
    GH_CONVERSATION_VIEW(gh_content_page_get_view(content))) == room);
  g_assert_false(gh_sidebar_page_get_show_requests(gh_window_get_sidebar(f.window)));
  guint before = f.rec.reqs->len;
  run_directory_for(&f, 60);
  g_assert_cmpuint(f.rec.reqs->len, >, before);
  Req *named = g_ptr_array_index(f.rec.reqs, before);
  g_assert_cmpuint(named->authors->len, ==, 1);
  g_assert_true(req_asks(named, x->pk));
  g_assert_true(named->profiles);
  /* Persisted by the delegate. */
  reopen(&f);
  g_assert_false(gh_conversation_get_is_request(room_of(&f, x)));
  fixture_down(&f);
}

/* ---- GUI: New Message ------------------------------------------------------------- */

static guint search_changes;

static void
on_search_changed(void)
{
  search_changes++;
}

static GhNewMessageDialog *
open_dialog(Fixture *f)
{
  g_assert_true(g_action_group_get_action_enabled(G_ACTION_GROUP(f->window), "new-message"));
  g_action_group_activate_action(G_ACTION_GROUP(f->window), "new-message", NULL);
  drain();
  AdwDialog *dialog = adw_application_window_get_visible_dialog(
    ADW_APPLICATION_WINDOW(f->window));
  g_assert_true(GH_IS_NEW_MESSAGE_DIALOG(dialog));
  g_signal_connect(template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "entry"), "search-changed",
                   G_CALLBACK(on_search_changed), NULL);
  return GH_NEW_MESSAGE_DIALOG(dialog);
}

static gboolean
searched(gpointer data)
{
  return search_changes > GPOINTER_TO_UINT(data);
}

static void
type_into(GhNewMessageDialog *dialog, const gchar *text)
{
  GtkEditable *entry = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "entry");
  guint before = search_changes;
  gtk_editable_set_text(entry, text);
  gh_test_spin_until(searched, GUINT_TO_POINTER(before));
  drain();
}

static GhNewMessageItem *
suggestion(GhNewMessageDialog *dialog, guint i)
{
  GListModel *model = gh_new_message_dialog_get_suggestions(dialog);
  g_assert_cmpuint(i, <, g_list_model_get_n_items(model));
  GhNewMessageItem *item = g_list_model_get_item(model, i);
  g_object_unref(item); /* the model keeps it */
  return item;
}

static void
choose(GhNewMessageDialog *dialog, guint i)
{
  GtkListBox *results = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "results");
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(results, (int)i);
  g_assert_nonnull(row);
  g_signal_emit_by_name(results, "row-activated", row);
  drain();
}

static guint
n_recipients(GhNewMessageDialog *dialog)
{
  return g_list_model_get_n_items(gh_new_message_dialog_get_recipients(dialog));
}

static void
press_enter(GhNewMessageDialog *dialog)
{
  g_signal_emit_by_name(template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "entry"), "activate");
  drain();
}

static GhNewMessageItem *
person(GhNewMessageDialog *dialog, const gchar *pubkey)
{
  GListModel *model = gh_new_message_dialog_get_people(dialog);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(model, i);
    if (g_strcmp0(gh_new_message_item_get_pubkey(item), pubkey) == 0)
      return item; /* the model keeps it */
  }
  g_error("no confirm row for %s", pubkey);
  return NULL;
}

static gboolean
people_checked(gpointer data)
{
  GListModel *model = gh_new_message_dialog_get_people(data);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(model, i);
    if (gh_new_message_item_get_busy(item))
      return FALSE;
  }
  return TRUE;
}

/* Consent: typing contacts nothing; only the rows that say what they
 * contact do, and only when chosen. */
static void
test_gui_new_message_consent(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *bob = &people[6], *carol = &people[7], *dave = &people[8];
  deliver(&f, dave, TRUE, T0 - 300, "hi Dave", NULL); /* a contact */
  f.http.answer = g_strdup_printf("{\"names\":{\"bob\":\"%s\"}}", bob->pk);
  attach_window(&f, TRUE);
  GhNewMessageDialog *dialog = open_dialog(&f);
  guint reqs = f.rec.reqs->len;

  /* Empty: Note to Self and the people you talk to (local). */
  g_assert_cmpint(gh_new_message_item_get_kind(suggestion(dialog, 0)), ==,
                  GH_NEW_MESSAGE_ITEM_NOTE_TO_SELF);
  g_assert_cmpint(gh_new_message_item_get_kind(suggestion(dialog, 1)), ==,
                  GH_NEW_MESSAGE_ITEM_CONTACT);
  g_assert_cmpstr(gh_new_message_item_get_pubkey(suggestion(dialog, 1)), ==, dave->pk);

  /* An address gives a consent row naming its host; nothing is fetched. */
  type_into(dialog, "Bob@Example.com");
  GListModel *suggestions = gh_new_message_dialog_get_suggestions(dialog);
  g_assert_cmpuint(g_list_model_get_n_items(suggestions), ==, 1);
  GhNewMessageItem *lookup = suggestion(dialog, 0);
  g_assert_cmpint(gh_new_message_item_get_kind(lookup), ==, GH_NEW_MESSAGE_ITEM_LOOKUP);
  g_assert_cmpstr(gh_new_message_item_get_title(lookup), ==, "Look up bob@example.com");
  g_assert_true(g_str_has_prefix(gh_new_message_item_get_subtitle(lookup),
                                 "Connects to example.com"));
  press_enter(dialog); /* Enter never looks anything up */
  g_assert_cmpuint(f.http.urls->len, ==, 0);
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs);
  g_assert_cmpuint(n_recipients(dialog), ==, 0);

  /* Chosen: one GET of that address, and the person is added. */
  choose(dialog, 0);
  g_assert_cmpuint(f.http.urls->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.http.urls, 0), ==,
                  "https://example.com/.well-known/nostr.json?name=bob");
  g_assert_cmpuint(n_recipients(dialog), ==, 1);
  g_autoptr(GhNewMessageItem) chip = g_list_model_get_item(
    gh_new_message_dialog_get_recipients(dialog), 0);
  g_assert_cmpstr(gh_new_message_item_get_pubkey(chip), ==, bob->pk);
  g_assert_cmpstr(gh_new_message_item_get_title(chip), ==, "bob@example.com");

  /* A pasted npub is checked offline and added with Enter. */
  g_autofree gchar *uri = g_strconcat("nostr:", carol->npub, NULL);
  type_into(dialog, uri);
  g_assert_cmpint(gh_new_message_item_get_kind(suggestion(dialog, 0)), ==,
                  GH_NEW_MESSAGE_ITEM_PUBKEY);
  press_enter(dialog);
  g_assert_cmpuint(n_recipients(dialog), ==, 2);
  /* A name that isn't known here finds nothing and asks nobody. */
  type_into(dialog, "someone new");
  g_assert_cmpuint(g_list_model_get_n_items(suggestions), ==, 0);
  g_assert_cmpuint(f.http.urls->len, ==, 1);
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs);

  /* The confirm page: npubs in groups of four, not checked yet. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.next", NULL));
  drain();
  g_autofree gchar *grouped = gh_recipient_npub_grouped(carol->pk);
  g_assert_cmpstr(gh_new_message_item_get_subtitle(person(dialog, carol->pk)), ==, grouped);
  g_assert_cmpstr(gh_new_message_item_get_status(person(dialog, bob->pk)), ==, "Not checked");
  AdwActionRow *check = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "check_row");
  g_assert_cmpstr(adw_action_row_get_subtitle(check), ==,
                  "Asks your discovery relays (discovery-a.test.invalid, "
                  "discovery-b.test.invalid) for their message relays. Those relays learn "
                  "whom you asked about.");
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs);
  /* Honest about what a room means (P4, W17): it sends, one wrap per
   * person, and everyone sees who else is in it. */
  GtkLabel *note = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "start_note");
  g_assert_nonnull(strstr(gtk_label_get_text(note),
                          "Each message is encrypted separately for each person"));
  g_assert_nonnull(strstr(gtk_label_get_text(note), "can see who else is in it"));
  g_assert_null(strstr(gtk_label_get_text(note), "isn't possible"));

  /* Chosen: each person's 10050 only (no kind 0), from discovery relays. */
  adw_action_row_activate(check);
  drain();
  g_assert_cmpuint(f.rec.reqs->len, >, reqs);
  for (guint i = reqs; i < f.rec.reqs->len; i++) {
    Req *req = g_ptr_array_index(f.rec.reqs, i);
    g_assert_false(req->profiles);
    g_assert_cmpuint(req->authors->len, ==, 1);
    g_assert_true(req_asks(req, bob->pk) || req_asks(req, carol->pk));
  }
  g_autofree gchar *bob_inbox = inbox_list(bob, T0 - 10, R1);
  for (guint i = reqs; i < f.rec.reqs->len; i++) {
    Req *req = g_ptr_array_index(f.rec.reqs, i);
    req->answered = TRUE;
    if (req_asks(req, bob->pk))
      gh_relay_scope_event(req->scope, req->url, bob_inbox);
    gh_relay_scope_eose(req->scope, req->url);
  }
  gh_test_spin_until(people_checked, dialog);
  g_assert_cmpstr(gh_new_message_item_get_status(person(dialog, bob->pk)), ==,
                  "Can receive private messages");
  g_assert_cmpstr(gh_new_message_item_get_status(person(dialog, carol->pk)), ==,
                  "Hasn't set up private messaging yet");

  /* Start: the room opens (empty, accepted) and nothing was sent. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.start", NULL));
  drain();
  const gchar *peers[] = { bob->pk, carol->pk, NULL };
  g_autoptr(GhConversation) opened = NULL;
  GListModel *rooms = G_LIST_MODEL(f.model);
  for (guint i = 0; i < g_list_model_get_n_items(rooms) && !opened; i++) {
    g_autoptr(GhConversation) room = g_list_model_get_item(rooms, i);
    const gchar *const *have = gh_conversation_get_peers(room);
    if (g_strv_length((gchar **)have) == 2 && g_strv_contains(have, peers[0]) &&
        g_strv_contains(have, peers[1]))
      opened = g_object_ref(room);
  }
  g_assert_nonnull(opened);
  g_assert_false(gh_conversation_get_is_request(opened));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(opened)), ==, 0);
  g_assert_true(gh_sidebar_page_get_selected(gh_window_get_sidebar(f.window)) == opened);
  g_assert_true(gh_content_page_get_conversation_shown(gh_window_get_content(f.window)));
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(f.window)));
  g_assert_cmpuint(f.http.urls->len, ==, 1);
  fixture_down(&f);
}

static void
test_gui_new_message_limit(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  attach_window(&f, TRUE);
  GhNewMessageDialog *dialog = open_dialog(&f);
  GtkWidget *limit = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "limit_label");
  g_assert_false(gtk_widget_get_visible(limit));
  for (guint i = 0; i < GH_CONVERSATION_MAX_PEERS; i++) {
    type_into(dialog, people[i].npub);
    press_enter(dialog);
    g_assert_cmpuint(n_recipients(dialog), ==, i + 1);
  }
  /* At 10: the limit, said in place, pointing to no missing feature. */
  g_assert_true(gtk_widget_get_visible(limit));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(limit)), ==, LIMIT_COPY);
  {
    /* A build with encrypted groups points to them instead (charter §7.9). */
    GhNewMessageConfig config = new_message_config(&f);
    config.encrypted_groups = TRUE;
    GhNewMessageDialog *groups = g_object_ref_sink(gh_new_message_dialog_new(&config));
    g_assert_cmpstr(label_text(groups, GH_TYPE_NEW_MESSAGE_DIALOG, "limit_label"), ==,
                    LIMIT_COPY_GROUPS);
    g_object_unref(groups);
  }
  type_into(dialog, people[GH_CONVERSATION_MAX_PEERS].npub);
  GhNewMessageItem *eleventh = suggestion(dialog, 0);
  g_assert_false(gh_new_message_item_get_enabled(eleventh));
  press_enter(dialog);
  choose(dialog, 0);
  g_assert_cmpuint(n_recipients(dialog), ==, GH_CONVERSATION_MAX_PEERS);
  /* The model refuses an eleventh as well. */
  const gchar *eleven[GH_CONVERSATION_MAX_PEERS + 2] = { NULL };
  for (guint i = 0; i <= GH_CONVERSATION_MAX_PEERS; i++)
    eleven[i] = people[i].pk;
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_conversation_store_open_room(f.model, eleven, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  /* Removing one (the chip's button) makes room again. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.remove", "s",
                                           people[0].pk));
  drain();
  g_assert_cmpuint(n_recipients(dialog), ==, GH_CONVERSATION_MAX_PEERS - 1);
  g_assert_false(gtk_widget_get_visible(limit));
  adw_dialog_force_close(ADW_DIALOG(dialog));
  fixture_down(&f);
}

static void
test_gui_new_message_refusals_and_note_to_self(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  attach_window(&f, TRUE);
  GhNewMessageDialog *dialog = open_dialog(&f);
  GtkLabel *error_label = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "input_error");
  GtkEditable *entry = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "entry");

  /* A secret key is never kept: the entry is emptied and says why. */
  type_into(dialog, "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5");
  g_assert_cmpstr(gtk_editable_get_text(entry), ==, "");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(error_label)));
  g_assert_true(g_str_has_prefix(gtk_label_get_text(error_label), "That's a secret key."));
  g_assert_cmpuint(n_recipients(dialog), ==, 0);
  type_into(dialog, "note1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(error_label)));
  g_assert_cmpuint(g_list_model_get_n_items(gh_new_message_dialog_get_suggestions(dialog)), ==,
                   0);
  type_into(dialog, "");

  /* Tor mode (G09): the lookup would go through Tor, and the row says who
   * learns what; nothing is fetched before the click. */
  guint fetched = f.http.urls->len;
  g_settings_set_string(f.settings, "network-mode", "tor");
  type_into(dialog, "bob@example.com");
  GhNewMessageItem *lookup = suggestion(dialog, 0);
  g_assert_true(gh_new_message_item_get_enabled(lookup));
  g_assert_cmpstr(gh_new_message_item_get_subtitle(lookup), ==,
                  "Connects to example.com through Tor. example.com learns whom you looked "
                  "up, not your IP address");
  g_assert_cmpuint(f.http.urls->len, ==, fetched);
  /* Outside Tor a .onion address is never looked up. */
  type_into(dialog, "");
  g_settings_set_string(f.settings, "network-mode", "system");
  type_into(dialog, "bob@groundhogexample.onion");
  lookup = suggestion(dialog, 0);
  g_assert_false(gh_new_message_item_get_enabled(lookup));
  g_assert_cmpstr(gh_new_message_item_get_subtitle(lookup), ==,
                  ".onion addresses can only be reached through Tor");
  choose(dialog, 0);
  g_assert_cmpuint(f.http.urls->len, ==, fetched);

  /* Note to Self opens the account's own room. */
  type_into(dialog, "");
  g_assert_cmpint(gh_new_message_item_get_kind(suggestion(dialog, 0)), ==,
                  GH_NEW_MESSAGE_ITEM_NOTE_TO_SELF);
  choose(dialog, 0);
  GhConversation *self_room = gh_conversation_store_lookup(f.model, hex_alice);
  g_assert_nonnull(self_room);
  g_assert_cmpuint(g_strv_length((gchar **)gh_conversation_get_peers(self_room)), ==, 0);
  g_assert_true(gh_sidebar_page_get_selected(gh_window_get_sidebar(f.window)) == self_room);
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(f.window)));
  /* Opening it again reuses the room. */
  GError *error = NULL;
  g_assert_true(gh_conversation_store_open_room(f.model, NULL, &error) == self_room);

  /* Without an account there is nothing to start: the action is off. */
  gh_conversation_store_set_account(f.model, NULL, NULL, NULL, NULL);
  g_assert_false(g_action_group_get_action_enabled(G_ACTION_GROUP(f.window), "new-message"));
  fixture_down(&f);
}

/* New Message to someone blocked (W15 review B1): the confirm page says that
 * starting unblocks the conversation, and Start lists it with the history
 * it kept, accepted and no longer blocked; never an empty room hidden in
 * front of a blocked one. */
static void
test_gui_new_message_unblocks(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  const Person *v = &people[9];
  g_autofree gchar *room = room_id_with(v);
  g_assert_cmpint(deliver(&f, v, TRUE, T0 - 300, "hi V", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, v, FALSE, T0 - 200, "hi back", NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_set_blocked(f.delegate, room, TRUE, &error));
  g_assert_no_error(error);
  attach_window(&f, TRUE);
  GhNewMessageDialog *dialog = open_dialog(&f);
  type_into(dialog, v->npub);
  press_enter(dialog);
  g_assert_cmpuint(n_recipients(dialog), ==, 1);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.next", NULL));
  drain();
  GtkLabel *note = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "start_note");
  g_assert_true(g_str_has_prefix(gtk_label_get_text(note),
                                 "You blocked this conversation. Starting it unblocks it"));
  g_assert_true(blocked(&f, v)); /* nothing changes before Start */

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.start", NULL));
  drain();
  GhConversation *opened = room_of(&f, v);
  g_assert_nonnull(opened);
  g_assert_false(blocked(&f, v));
  g_assert_false(gh_conversation_get_is_request(opened));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(opened)), ==, 2);
  g_assert_true(gh_sidebar_page_get_selected(gh_window_get_sidebar(f.window)) == opened);
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(f.window)));
  /* Their next message is listed again. */
  g_assert_cmpint(deliver(&f, v, FALSE, T0 + 10, "welcome back", NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  fixture_down(&f);
}

static GtkWidget *
check_spinner(GhNewMessageDialog *dialog)
{
  return template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "check_spinner");
}

/* The inbox check across rounds (W15 review non-blocking #8): going back and
 * checking again cancels the first round, whose late (cancelled) answers
 * must not count against the new one. Before, they did: the spinner stopped
 * and "Check finished" was announced while a check was still running. */
static void
test_gui_new_message_check_rounds(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *bob = &people[10], *carol = &people[11];
  attach_window(&f, TRUE);
  GhNewMessageDialog *dialog = open_dialog(&f);
  type_into(dialog, bob->npub);
  press_enter(dialog);
  type_into(dialog, carol->npub);
  press_enter(dialog);
  AdwNavigationView *navigation = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG,
                                                 "navigation");
  AdwActionRow *check = template_child(dialog, GH_TYPE_NEW_MESSAGE_DIALOG, "check_row");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.next", NULL));
  drain();
  adw_action_row_activate(check); /* round 1: two checks */
  drain();
  g_assert_true(gtk_widget_get_visible(check_spinner(dialog)));

  /* Back, Next, Check again at once: round 1 is cancelled, round 2 runs. */
  adw_navigation_view_pop(navigation);
  drain();
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.next", NULL));
  adw_action_row_activate(check);
  drain(); /* round 1's cancelled answers arrive */

  /* Only Bob's lookup answers: Carol's check is still running. */
  g_autofree gchar *bob_inbox = inbox_list(bob, T0 - 10, R1);
  for (guint i = 0; i < f.rec.reqs->len; i++) {
    Req *req = g_ptr_array_index(f.rec.reqs, i);
    if (req->answered || !req_asks(req, bob->pk))
      continue;
    req->answered = TRUE;
    gh_relay_scope_event(req->scope, req->url, bob_inbox);
    gh_relay_scope_eose(req->scope, req->url);
  }
  drain();
  g_assert_false(gh_new_message_item_get_busy(person(dialog, bob->pk)));
  g_assert_true(gh_new_message_item_get_busy(person(dialog, carol->pk)));
  g_assert_true(gtk_widget_get_visible(check_spinner(dialog)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(check)));

  answer_all(&f.rec, NULL);
  gh_test_spin_until(people_checked, dialog);
  g_assert_false(gtk_widget_get_visible(check_spinner(dialog)));
  g_assert_cmpstr(gh_new_message_item_get_status(person(dialog, carol->pk)), ==,
                  "Hasn't set up private messaging yet");
  adw_dialog_force_close(ADW_DIALOG(dialog));
  fixture_down(&f);
}

/* ---- screenshots (opt-in evidence) ------------------------------------------------ */

static void
save_png(GtkWidget *window, const char *dir, const char *name)
{
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree char *path = g_strdup_printf("%s/groundhog-g18-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

static void
shoot(Fixture *f, const char *dir, const char *name)
{
  drain();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(f->window));
  save_png(GTK_WIDGET(f->window), dir, name);
}

static void
resize(Fixture *f, int width, int height)
{
  gtk_window_set_default_size(GTK_WINDOW(f->window), width, height);
  drain();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(f->window));
}

static void
test_gui_screenshots(void)
{
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  static const int sizes[][2] = { { 900, 600 }, { 360, 294 } };
  static const char *const size_names[] = { "wide", "narrow" };
  for (guint s = 0; s < G_N_ELEMENTS(sizes); s++) {
    Fixture f;
    fixture_up(&f, TRUE);
    deliver(&f, &people[0], FALSE, T0 - 3600, "Hi! I found your npub on a forum. <b>Can we "
            "talk?</b> https://example.com/hello", "Hello from a stranger");
    deliver(&f, &people[0], FALSE, T0 - 3500, "Second message", NULL);
    deliver(&f, &people[1], TRUE, T0 - 7200, "See you tomorrow", NULL);
    attach_window(&f, TRUE);
    resize(&f, sizes[s][0], sizes[s][1]);
    g_assert_true(gh_window_open_item(f.window, room_of(&f, &people[0])));
    g_autofree gchar *requests = g_strdup_printf("requests-%s", size_names[s]);
    shoot(&f, dir, requests);

    GhNewMessageDialog *dialog = open_dialog(&f);
    g_autofree gchar *empty = g_strdup_printf("new-message-%s", size_names[s]);
    shoot(&f, dir, empty);
    type_into(dialog, people[2].npub);
    press_enter(dialog);
    type_into(dialog, "bob@example.com");
    g_autofree gchar *consent = g_strdup_printf("new-message-consent-%s", size_names[s]);
    shoot(&f, dir, consent);
    type_into(dialog, "");
    for (guint i = 3; i < 3 + GH_CONVERSATION_MAX_PEERS - 1; i++) {
      type_into(dialog, people[i].npub);
      press_enter(dialog);
    }
    g_autofree gchar *limit = g_strdup_printf("new-message-limit-%s", size_names[s]);
    shoot(&f, dir, limit);
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-message.next", NULL));
    g_autofree gchar *confirm = g_strdup_printf("new-message-confirm-%s", size_names[s]);
    shoot(&f, dir, confirm);
    adw_dialog_force_close(ADW_DIALOG(dialog));
    fixture_down(&f);
  }
  g_log_set_always_fatal(fatal);
}

int
main(int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
    if (g_str_equal(argv[i], "--gui")) {
      gui = TRUE;
      for (int j = i; j < argc - 1; j++)
        argv[j] = argv[j + 1];
      argc--;
      break;
    }
  if (gui) {
    if (!nostrc_test_bus_available()) {
      g_printerr("groundhog-requests-gui skipped: dbus-daemon is not installed\n");
      return 77;
    }
#ifdef __APPLE__
    /* As in the composer tests: GTK 4.22's macOS accessibility backend has
     * no announce. Linux (AT-SPI) is unaffected. */
    g_setenv("GTK_A11Y", "none", FALSE);
#endif
    /* GTK first, on the session bus it was given; the private bus for the
     * mock signer comes up after it, beside it (gh_test_bus_up_beside_gtk()). */
    if (!gtk_init_check()) {
      g_printerr("groundhog-requests-gui skipped: no graphical display\n");
      return 77;
    }
    adw_init();
    groundhog_register_resource();
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
                 "gtk-enable-animations", FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
  }
  g_test_init(&argc, &argv, NULL);
  hex_alice = gh_test_pub(1);
  npub_alice = gh_test_npub(1);
  for (guint i = 0; i < N_PEOPLE; i++) {
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);
    people[i].sk = g_strdup(sk);
    people[i].pk = g_strdup(pk);
    people[i].npub = gh_recipient_npub(pk);
    free(sk);
    free(pk);
  }
  if (gui)
    gh_test_bus_up_beside_gtk(&shared_bus);
  else
    gh_test_bus_up(&shared_bus);
  if (gui) {
    g_test_add_func("/groundhog/requests/gui/requests-view", test_gui_requests_view);
    g_test_add_func("/groundhog/requests/gui/without-storage", test_gui_requests_without_storage);
    g_test_add_func("/groundhog/requests/gui/pt8-accept", test_gui_pt8_accept);
    g_test_add_func("/groundhog/requests/gui/new-message-consent", test_gui_new_message_consent);
    g_test_add_func("/groundhog/requests/gui/new-message-limit", test_gui_new_message_limit);
    g_test_add_func("/groundhog/requests/gui/new-message-refusals-and-note-to-self",
                    test_gui_new_message_refusals_and_note_to_self);
    g_test_add_func("/groundhog/requests/gui/new-message-unblocks",
                    test_gui_new_message_unblocks);
    g_test_add_func("/groundhog/requests/gui/new-message-check-rounds",
                    test_gui_new_message_check_rounds);
    g_test_add_func("/groundhog/requests/gui/screenshots", test_gui_screenshots);
  } else {
    g_test_add_func("/groundhog/requests/accept-delete-block-persist",
                    test_accept_delete_block_persist);
    g_test_add_func("/groundhog/requests/pt8-until-accept", test_pt8_until_accept);
    g_test_add_func("/groundhog/requests/replayed-self-copy-keeps-block",
                    test_replayed_self_copy_keeps_block);
  }
  int result = g_test_run();
  gh_test_bus_down(&shared_bus);
  for (guint i = 0; i < N_PEOPLE; i++) {
    g_free(people[i].sk);
    g_free(people[i].pk);
    g_free(people[i].npub);
  }
  g_free(hex_alice);
  g_free(npub_alice);
  return result;
}
