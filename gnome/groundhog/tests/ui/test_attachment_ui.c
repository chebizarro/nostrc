/* G22 attachment UI in the window (privacy charter §6, D6, §7.7, §7.14,
 * §8.2 G22: AT-7 in the UI, a cancel test and Save through the Save dialog's
 * path). The account stack of the composer tests (tests/ui/send-stack.h:
 * the mock signer on a private bus, FakeSecret, SQLCipher stores in private
 * directories, recording relay transports and a scripted recipient inbox
 * directory) with GhAttachments and the attachment UI attached as
 * gh_app_services_attach_window() does, and the local Blossom server
 * (tests/media/blossom-fixture.c) on loopback:
 *  - attach -> sheet (metadata removed, the photo, what the server learns)
 *    -> Send -> upload -> outbox -> the wrap published to the recipient's
 *    inbox relay, the sender's card showing the file with no download;
 *  - the first use without a server asks for one and never picks one;
 *  - a server that wants a known account: consent, then the account signs,
 *    kept per account (nostrc-dnsc), revoked from Preferences;
 *  - a dropped file and a pasted image take the same path, text still
 *    pastes as text;
 *  - a received file's card fetches nothing until Download (AT-7), shows the
 *    photo after it, Cancel stops it (AT-8), Save As writes only the chosen
 *    file, failures are said in words;
 *  - Preferences › Attachments: the servers' order, the limit, the cache and
 *    Clear.
 * Needs a display: it self-skips (77) without one. Waits iterate the main
 * context against a deadline; they never sleep. */
#include "gh-test-signer.h"
#include "send-stack.h"

#include "blossom-fixture.h"
#include "socks5-fixture.h"
#include "gh-attachment-card.h"
#include "gh-attachment-ui.h"
#if GROUNDHOG_TEST_MLS_FILES
#include "gh-mls-attachment-ui.h"
#endif
#include "gh-preferences-dialog.h"
#include "gh-store-blossom.h"
#include "gh-store-media.h"
#include "../gh-test-port.h"

#include <string.h>

#include "nostrc-test-gdk-frame.h"

#define DISCOVERY "wss://discovery.test.invalid"
#define INBOX_A   "wss://inbox-a.test.invalid"   /* the account's own (key 1) */
#define INBOX_B   "wss://inbox-b.test.invalid"   /* key 2's */
#define FIRST_USE_ONION "firstuseblossomfixtureb3srh6u2f7xk7wudhvla6.onion"

static GhTestBus bus;
static GhTestSigner signer;
static gchar *xdg_root;

/* ---- recording relay transports (H1) ---------------------------------------------- */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gboolean discovery;
  gboolean closed;
} Req;

static GPtrArray *reqs;

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_free(req->url);
  g_free(req);
}

static gpointer
rec_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
         GError **error)
{
  (void)data;
  (void)error;
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->discovery = nostr_filter_kinds_len(&filters->filters[0]) == 2;
  g_ptr_array_add(reqs, req);
  return req;
}

static void
rec_close(gpointer handle, gpointer data)
{
  (void)data;
  ((Req *)handle)->closed = TRUE;
}

static const GhRelayTransport rec_transport = { rec_open, rec_close };

static gboolean
rec_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)handle; (void)json; (void)data; (void)error;
  return TRUE;
}

static void
rec_resubscribe(gpointer handle, gpointer data)
{
  (void)handle; (void)data;
}

static const GhRelayAuthTransport rec_auth = { rec_send_auth, rec_resubscribe };

static Req *
open_req(const gchar *url, gboolean discovery)
{
  for (guint i = reqs->len; i > 0; i--) {
    Req *req = g_ptr_array_index(reqs, i - 1);
    if (!req->closed && req->discovery == discovery && g_str_equal(req->url, url))
      return req;
  }
  return NULL;
}

static gboolean
inbox_req_open(gpointer data)
{
  (void)data;
  return open_req(INBOX_A, FALSE) != NULL;
}

/* Publishing: every wrap handed to a relay, accepted. */
typedef struct {
  GhRelayPublish *publish;
  gchar *url;
  gchar *event_id;
  gchar *p;
  gboolean answered;
  gboolean closed;
} Pub;

static GPtrArray *pubs;

static void
pub_free(gpointer data)
{
  Pub *pub = data;
  gh_relay_publish_unref(pub->publish);
  g_free(pub->url);
  g_free(pub->event_id);
  g_free(pub->p);
  g_free(pub);
}

static gboolean
pub_accept(gpointer data)
{
  Pub *pub = data;
  if (!pub->closed && !pub->answered) {
    pub->answered = TRUE;
    gh_relay_publish_ok(pub->publish, pub->url, pub->event_id, TRUE, "");
  }
  return G_SOURCE_REMOVE;
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  (void)data;
  (void)error;
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(wrap, event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(wrap), ==, 1059);
  Pub *pub = g_new0(Pub, 1);
  char *p = nostr_nip59_get_recipient(wrap);
  pub->p = g_strdup(p);
  free(p);
  nostr_event_free(wrap);
  pub->publish = gh_relay_publish_ref(publish);
  pub->url = g_strdup(url);
  pub->event_id = g_strdup(gh_relay_publish_get_event_id(publish));
  g_ptr_array_add(pubs, pub);
  g_idle_add(pub_accept, pub);
  return pub;
}

static void
pub_close(gpointer handle, gpointer data)
{
  (void)data;
  ((Pub *)handle)->closed = TRUE;
}

static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };

static guint
pubs_to(const gchar *url, const gchar *p)
{
  guint n = 0;
  for (guint i = 0; i < pubs->len; i++) {
    Pub *pub = g_ptr_array_index(pubs, i);
    n += g_str_equal(pub->url, url) && g_str_equal(pub->p, p);
  }
  return n;
}

/* ---- a scripted recipient inbox directory (GhInboxResolver) ------------------------ */

#define DIR_TYPE_RESOLVER (dir_resolver_get_type())
G_DECLARE_FINAL_TYPE(DirResolver, dir_resolver, DIR, RESOLVER, GObject)

struct _DirResolver {
  GObject parent_instance;
  GHashTable *inboxes; /* pubkey hex -> relay URL */
};

static void
dir_resolve_async(GhInboxResolver *resolver, const gchar *pubkey, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer data)
{
  DirResolver *self = DIR_RESOLVER(resolver);
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  const gchar *url = g_hash_table_lookup(self->inboxes, pubkey);
  result->recipient = g_strdup(pubkey);
  result->sources = result->answered = 1;
  result->status = url ? GH_INBOX_FOUND : GH_INBOX_NOT_FOUND;
  if (url) {
    result->relays = g_new0(gchar *, 2);
    result->relays[0] = g_strdup(url);
  }
  g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
dir_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
dir_resolver_iface_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = dir_resolve_async;
  iface->resolve_finish = dir_resolve_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(DirResolver, dir_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER, dir_resolver_iface_init))

static void
dir_resolver_finalize(GObject *object)
{
  g_hash_table_unref(DIR_RESOLVER(object)->inboxes);
  G_OBJECT_CLASS(dir_resolver_parent_class)->finalize(object);
}

static void
dir_resolver_class_init(DirResolverClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = dir_resolver_finalize;
}

static void
dir_resolver_init(DirResolver *self)
{
  self->inboxes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/* ---- a JPEG with EXIF, and a PNG -------------------------------------------------------- */

#define GPS_TEXT "GPSInfo 47.3769N 8.5417E"

/* A 4x3 JPEG of about size bytes with an APP1 EXIF naming a place. */
static GBytes *
make_jpeg(gsize size, guint32 seed)
{
  GByteArray *out = g_byte_array_new();
  static const guint8 head[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x07, 'J', 'F', 'I', 'F', 0x00 };
  g_byte_array_append(out, head, sizeof head);
  static const gchar exif[] = "Exif\0\0MM " GPS_TEXT;
  guint8 app1[4] = { 0xFF, 0xE1, 0x00, (guint8)(sizeof exif - 1 + 2) };
  g_byte_array_append(out, app1, 4);
  g_byte_array_append(out, (const guint8 *)exif, sizeof exif - 1);
  static const guint8 sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x03, 0x00, 0x04, 1, 1, 0x11,
                                0 };
  g_byte_array_append(out, sof, sizeof sof);
  static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0 };
  g_byte_array_append(out, sos, sizeof sos);
  while (out->len + 2 < size) {
    seed = seed * 1103515245u + 12345u;
    guint8 byte = (guint8)(seed >> 16);
    if (byte == 0xFF)
      byte = 0xFE;
    g_byte_array_append(out, &byte, 1);
  }
  static const guint8 eoi[] = { 0xFF, 0xD9 };
  g_byte_array_append(out, eoi, 2);
  return g_byte_array_free_to_bytes(out);
}

/* A real 8x6 PNG (GTK decodes it, so the card can show it). */
static GBytes *
make_png(guint8 shade)
{
  guint8 pixels[8 * 6 * 4];
  for (guint i = 0; i < sizeof pixels; i += 4) {
    pixels[i] = shade;
    pixels[i + 1] = (guint8)(255 - shade);
    pixels[i + 2] = 64;
    pixels[i + 3] = 255;
  }
  g_autoptr(GBytes) raw = g_bytes_new(pixels, sizeof pixels);
  g_autoptr(GdkTexture) texture =
    gdk_memory_texture_new(8, 6, GDK_MEMORY_R8G8B8A8, raw, 8 * 4);
  return gdk_texture_save_to_png_bytes(texture);
}

static gboolean
bytes_contain(GBytes *bytes, const gchar *needle)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  gsize n = strlen(needle);
  for (gsize i = 0; n <= size && i <= size - n; i++)
    if (memcmp(data + i, needle, n) == 0)
      return TRUE;
  return FALSE;
}

/* ---- fixture ------------------------------------------------------------------------ */

typedef struct {
  SendStack s;
  DirResolver *directory;
  gint64 list_time;
  BlossomFixture *blossom;
  GhNetHttp *http;
  GhAttachments *attachments;
  gchar *server;        /* the fixture, normalized */
  gchar *saved;         /* Save As writes here (the Save dialog's choice) */
  guint save_asked;
  gchar *save_name;     /* the name Save As suggested last */
} Fixture;

static void
fixture_init(Fixture *f)
{
  memset(f, 0, sizeof *f);
  reqs = g_ptr_array_new_with_free_func(req_free);
  pubs = g_ptr_array_new_with_free_func(pub_free);
  signer.hold = FALSE;
  signer.deny = FALSE;
  f->directory = g_object_new(DIR_TYPE_RESOLVER, NULL);
  g_hash_table_insert(f->directory->inboxes, g_strdup(stack_hex[2]), g_strdup(INBOX_B));
  g_hash_table_insert(f->directory->inboxes, g_strdup(stack_hex[1]), g_strdup(INBOX_A));
  const gchar *discovery[] = { DISCOVERY, NULL };
  send_stack_init(&f->s, 1, bus.client, discovery);
  f->s.scope_transport = &rec_transport;
  f->s.auth_transport = &rec_auth;
  f->s.publish_transport = &pub_transport;
  f->s.resolver = GH_INBOX_RESOLVER(f->directory);
  f->list_time = 1700000000;
  f->blossom = blossom_fixture_new();
  f->server = gh_blossom_client_normalize_server(blossom_fixture_url(f->blossom), NULL);
  f->saved = g_build_filename(f->s.root, "saved.jpg", NULL);
}

static void
own_inbox_list(Fixture *f)
{
  Req *discovery = open_req(DISCOVERY, TRUE);
  g_assert_nonnull(discovery);
  const gchar *urls[] = { INBOX_A, NULL };
  g_autofree gchar *list = stack_inbox_list(f->s.key, ++f->list_time, urls);
  gh_relay_scope_event(discovery->scope, DISCOVERY, list);
  gh_relay_scope_eose(discovery->scope, DISCOVERY);
  gh_test_spin_until(inbox_req_open, NULL);
}

static void
sign_async(gpointer data, const gchar *unsigned_event_json, GCancellable *cancellable,
           GAsyncReadyCallback callback, gpointer callback_data)
{
  gh_account_controller_sign_with_cancellable_async(GH_ACCOUNT_CONTROLLER(data),
                                                    unsigned_event_json, cancellable, callback,
                                                    callback_data);
}

static GFile *
save_target(const gchar *suggested_name, gpointer data)
{
  Fixture *f = data;
  g_free(f->save_name);
  f->save_name = g_strdup(suggested_name);
  f->save_asked++;
  return g_file_new_for_path(f->saved);
}

/* The stack, its GhAttachments (as gh-app-services.c makes it) and the
 * window with the attachment UI. servers: the fixture (TRUE) or none. */
static void
fixture_up(Fixture *f, gboolean with_server)
{
  send_stack_up(&f->s);
  g_assert_cmpint(gh_account_store_get_state(f->s.store), ==, GH_ACCOUNT_STORE_OPEN);
  own_inbox_list(f);
  if (with_server) {
    const gchar *servers[] = { blossom_fixture_url(f->blossom), NULL };
    g_settings_set_strv(f->s.settings, "blossom-servers", servers);
  }
  f->http = gh_net_http_new(f->s.settings);
  GhAttachmentsConfig config = {
    .settings = f->s.settings,
    .http = f->http,
    .sign_async = sign_async,
    .sign_finish = gh_account_controller_sign_finish,
    .sign_data = f->s.accounts,
  };
  f->attachments = gh_attachments_new(&config);
  gh_attachments_set_allow_private_hosts(f->attachments, TRUE); /* the loopback fixture */
  gh_attachments_set_store(f->attachments, gh_account_store_get_store(f->s.store));
  send_stack_window(&f->s, 900, 700);
  GhAttachmentUiConfig ui = {
    .account_store = f->s.store,
    .conversations = f->s.model,
    .attachments = f->attachments,
    .settings = f->s.settings,
    .allow_onion = TRUE,
  };
  gh_attachment_ui_attach(f->s.window, &ui);
  gh_attachment_ui_set_save_target(f->s.window, save_target, f);
  gh_test_run_until_idle();
}

static void
fixture_clear(Fixture *f)
{
  if (f->s.window) {
    gpointer weak = f->s.window;
    g_object_add_weak_pointer(G_OBJECT(f->s.window), &weak);
    gtk_window_destroy(GTK_WINDOW(g_steal_pointer(&f->s.window)));
    gh_test_spin_until(stack_is_null, &weak);
  }
  if (f->attachments) {
    gh_attachments_set_store(f->attachments, NULL);
    g_object_run_dispose(G_OBJECT(f->attachments));
  }
  g_clear_object(&f->attachments);
  g_clear_object(&f->http);
  if (f->s.accounts) {
    send_stack_down(&f->s);
    signer.deny = TRUE;
    gh_test_signer_release_all(&signer);
    signer.deny = FALSE;
    GhTestSenders check = { &bus, &signer };
    gh_test_spin_until(gh_test_signer_senders_closed, &check);
  }
  gh_test_run_until_idle();
  blossom_fixture_free(f->blossom);
  send_stack_clear(&f->s);
  g_clear_object(&f->directory);
  g_clear_pointer(&reqs, g_ptr_array_unref);
  g_clear_pointer(&pubs, g_ptr_array_unref);
  g_free(f->server);
  g_free(f->saved);
  g_free(f->save_name);
}

typedef struct {
  GhConversationStore *model;
  gchar *room;
} RoomWait;

static gboolean
room_listed(gpointer data)
{
  RoomWait *wait = data;
  return gh_conversation_store_lookup(wait->model, wait->room) != NULL;
}

/* A gift wrap (NIP-59) for key wrap_to of rumor_json sealed by key from. */
static gchar *
craft_wrap(guint from, guint wrap_to, const gchar *rumor_json, gint64 created_at)
{
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[from], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, stack_hex[wrap_to], sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, stack_hex[from]);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, gh_test_secret[from]), ==, 0);
  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, gh_test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, stack_hex[wrap_to], ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static gint64 message_offset;

/* Key 2 sends the account a text on its inbox relay: their conversation,
 * accepted. */
static GhConversation *
receive_text(Fixture *f, const gchar *content)
{
  const guint to[] = { f->s.key, 0 };
  g_autofree gchar *wrap = stack_craft_wrap(2, f->s.key, to,
                                            g_get_real_time() / G_USEC_PER_SEC - 600 +
                                              message_offset++,
                                            content);
  gh_relay_scope_event(open_req(INBOX_A, FALSE)->scope, INBOX_A, wrap);
  const guint members[] = { 2, f->s.key, 0 };
  RoomWait wait = { f->s.model, stack_room(members) };
  gh_test_spin_until(room_listed, &wait);
  GhConversation *conversation = gh_conversation_store_lookup(f->s.model, wait.room);
  g_free(wait.room);
  gh_conversation_accept(conversation);
  return conversation;
}

typedef struct {
  GhConversation *conversation;
  const gchar *rumor_id;
} MessageWait;

static gboolean
message_listed(gpointer data)
{
  MessageWait *wait = data;
  return gh_conversation_lookup_message(wait->conversation, wait->rumor_id) != NULL;
}

/* Key 2 sends the account the file on its inbox relay. */
static GhMessage *
receive_file(Fixture *f, GhConversation *conversation, const GhNip17File *file)
{
  gint64 created_at = g_get_real_time() / G_USEC_PER_SEC - 600 + message_offset++;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *id = NULL;
  g_autofree gchar *rumor = gh_nip17_file_rumor_new(stack_hex[2], stack_hex[f->s.key], file,
                                                    created_at, 0, &id, &error);
  g_assert_no_error(error);
  g_autofree gchar *wrap = craft_wrap(2, f->s.key, rumor, created_at);
  gh_relay_scope_event(open_req(INBOX_A, FALSE)->scope, INBOX_A, wrap);
  MessageWait wait = { conversation, id };
  gh_test_spin_until(message_listed, &wait);
  return gh_conversation_lookup_message(conversation, id);
}

/* A file on the fixture, encrypted and uploaded as a sender would. */
typedef struct {
  gboolean done;
  GhNip17File *file;
  GError *error;
} Uploaded;

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  Uploaded *u = data;
  u->file = gh_attachments_upload_finish(GH_ATTACHMENTS(source), result, NULL, &u->error);
  u->done = TRUE;
}

static gboolean
uploaded(gpointer data)
{
  return ((Uploaded *)data)->done;
}

static GhNip17File *
upload_file(Fixture *f, GBytes *bytes)
{
  Uploaded u = { 0 };
  gh_attachments_upload_async(f->attachments, bytes, NULL, NULL, on_uploaded, &u);
  gh_test_spin_until(uploaded, &u);
  g_assert_no_error(u.error);
  return u.file;
}

/* ---- widgets ------------------------------------------------------------------------ */

static GhAttachmentCard *
find_card(GtkWidget *widget, GhMessage *message)
{
  if (GH_IS_ATTACHMENT_CARD(widget) &&
      gh_attachment_card_get_message(GH_ATTACHMENT_CARD(widget)) == message)
    return GH_ATTACHMENT_CARD(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GhAttachmentCard *found = find_card(c, message);
    if (found)
      return found;
  }
  return NULL;
}

typedef struct {
  Fixture *f;
  GhMessage *message;
  GhAttachmentCard *card;
} CardWait;

static gboolean
card_shown(gpointer data)
{
  CardWait *wait = data;
  wait->card = find_card(GTK_WIDGET(wait->f->s.window), wait->message);
  return wait->card && gtk_widget_get_mapped(GTK_WIDGET(wait->card));
}

static GhAttachmentCard *
card_of(Fixture *f, GhMessage *message)
{
  CardWait wait = { f, message, NULL };
  gh_test_spin_until(card_shown, &wait);
  return wait.card;
}

/* The visible button of the card with this label (mnemonic included). */
static GtkWidget *
card_button(GtkWidget *widget, const gchar *label)
{
  if (GTK_IS_BUTTON(widget) && gtk_widget_get_visible(widget) &&
      g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = card_button(c, label);
    if (found)
      return found;
  }
  return NULL;
}

static GtkPicture *
shown_picture(GtkWidget *widget)
{
  if (GTK_IS_PICTURE(widget) && gtk_widget_get_visible(widget) &&
      gtk_picture_get_paintable(GTK_PICTURE(widget)))
    return GTK_PICTURE(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkPicture *found = shown_picture(c);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean
transfer_not_downloading(gpointer data)
{
  return gh_attachment_transfer_get_state(data) != GH_ATTACHMENT_STATE_DOWNLOADING;
}

static gboolean
fixture_held(gpointer data)
{
  return blossom_fixture_held(data) >= 1;
}

/* Accessible properties exist only with an accessibility backend: send-stack
 * turns it off on macOS (GTK 4.22 has no announce there); Linux checks. */
static void
assert_accessible_label(GtkWidget *widget, const gchar *label)
{
  if (g_strcmp0(g_getenv("GTK_A11Y"), "none") == 0)
    return;
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label);
}

/* Stands in for GtkTextView's own paste (its class handler, which would read
 * the shared system clipboard): a handler connected after the composer's,
 * so it is reached only when the composer let the paste through as text,
 * as it does for users. It pastes from the test's private clipboard. */
typedef struct {
  GdkClipboard *clipboard;
  guint reached;
} TextPaste;

static void
on_text_paste(GtkTextView *text_view, TextPaste *paste)
{
  paste->reached++;
  g_signal_stop_emission_by_name(text_view, "paste-clipboard");
  gtk_text_buffer_paste_clipboard(gtk_text_view_get_buffer(text_view), paste->clipboard, NULL,
                                  gtk_text_view_get_editable(text_view));
}

static gboolean
caption_pasted(gpointer data)
{
  g_autofree gchar *text = gh_composer_dup_text(GH_COMPOSER(data));
  return g_strcmp0(text, "a caption") == 0;
}

static gboolean
toast_shown(gpointer data)
{
  return gh_attachment_ui_get_last_toast(GH_WINDOW(data)) != NULL;
}

/* The user closes the sheet (Escape, the close button). libadwaita 1.5
 * lets a close asked for during the opening animation go, so it is asked
 * until the sheet is gone (as the other GUI tests close their dialogs). */
static gboolean
sheet_closing(gpointer data)
{
  Fixture *f = data;
  GhAttachmentSheet *sheet = gh_attachment_ui_get_sheet(f->s.window);
  if (!sheet)
    return TRUE;
  adw_dialog_close(ADW_DIALOG(sheet));
  return FALSE;
}

static void
close_sheet(Fixture *f)
{
  gh_test_spin_until(sheet_closing, f);
}

static gboolean
sheet_open(gpointer data)
{
  Fixture *f = data;
  return gh_attachment_ui_get_sheet(f->s.window) != NULL;
}

static gboolean
sheet_closed(gpointer data)
{
  Fixture *f = data;
  return gh_attachment_ui_get_sheet(f->s.window) == NULL;
}

typedef struct {
  Fixture *f;
  const gchar *page;
} PageWait;

static gboolean
sheet_page(gpointer data)
{
  PageWait *wait = data;
  GhAttachmentSheet *sheet = gh_attachment_ui_get_sheet(wait->f->s.window);
  return sheet && g_strcmp0(gh_attachment_sheet_get_page(sheet), wait->page) == 0;
}

static GhAttachmentSheet *
wait_page(Fixture *f, const gchar *page)
{
  PageWait wait = { f, page };
  gh_test_spin_until(sheet_page, &wait);
  return gh_attachment_ui_get_sheet(f->s.window);
}

/* The account's own kind-15 message in conversation (the newest). */
static GhMessage *
own_file_message(GhConversation *conversation)
{
  GListModel *model = G_LIST_MODEL(conversation);
  for (guint i = g_list_model_get_n_items(model); i > 0; i--) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i - 1);
    if (gh_message_is_self(message) && gh_message_get_kind(message) == GH_NIP17_FILE_KIND)
      return message; /* the conversation holds it */
  }
  return NULL;
}

static gboolean
has_own_file(gpointer data)
{
  return own_file_message(data) != NULL;
}

typedef struct {
  GhMessage *message;
  GhMessageStatus status;
} StatusWait;

static gboolean
status_is(gpointer data)
{
  StatusWait *wait = data;
  return gh_message_get_status(wait->message) == wait->status;
}

/* ---- tests ---------------------------------------------------------------------------- */

/* Attach -> the sheet says what will be sent and what the server learns ->
 * Send -> the upload (ciphertext only, a throwaway key) -> the outbox -> the
 * wrap on the recipient's inbox relay; the sender's card shows the photo
 * with no download. Relay groups get no attach button. */
static void
test_attach_send(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  GhComposer *composer = send_stack_composer(&f.s);
  /* No conversation shown: nowhere to send a file. */
  g_assert_false(gh_composer_get_can_attach(composer));
  GhConversation *bob = receive_text(&f, "Send me the photo?");
  send_stack_select(&f.s, bob);
  g_assert_true(gh_composer_get_can_attach(composer));
  GtkWidget *attach = GTK_WIDGET(gh_composer_get_attach_button(composer));
  g_assert_true(gtk_widget_get_visible(attach));
  assert_accessible_label(attach, "Attach File");

  g_autoptr(GBytes) jpeg = make_jpeg(48 * 1024, 1);
  g_assert_true(bytes_contain(jpeg, GPS_TEXT));
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "IMG_2041.jpg", "image/jpeg");
  GhAttachmentSheet *sheet = wait_page(&f, "preview");
  g_assert_cmpstr(gh_attachment_sheet_get_metadata_text(sheet), ==,
                  "Location and camera data removed");
  g_assert_nonnull(strstr(gh_attachment_sheet_get_server_note(sheet), "127.0.0.1"));
  g_assert_nonnull(strstr(gh_attachment_sheet_get_server_note(sheet), "your IP address"));
  /* Nothing left before Send. */
  gh_test_run_until_idle();
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);

  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  BlossomRequest *put = g_ptr_array_index(blossom_fixture_requests(f.blossom), 0);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_pubkey, !=, stack_hex[1]); /* a throwaway key (AT-6) */
  gh_test_spin_until(has_own_file, bob);
  GhMessage *mine = own_file_message(bob);
  g_autoptr(GhNip17File) sent = gh_message_dup_file(mine);
  GBytes *blob = blossom_fixture_get_blob(f.blossom, sent->x);
  g_assert_nonnull(blob);
  g_assert_false(bytes_contain(blob, GPS_TEXT));
  g_assert_false(bytes_contain(blob, "JFIF"));
  StatusWait sent_wait = { mine, GH_MESSAGE_STATUS_SENT };
  gh_test_spin_until(status_is, &sent_wait);
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, 1);
  /* The sender's card: the photo, from the encrypted cache, no GET. */
  GhAttachmentCard *card = card_of(&f, mine);
  GhAttachmentTransfer *transfer = gh_attachment_card_get_transfer(card);
  g_assert_nonnull(transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_false(bytes_contain(gh_attachment_transfer_get_plaintext(transfer), GPS_TEXT));
  g_assert_nonnull(card_button(GTK_WIDGET(card), "_Save As…"));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 0);
  fixture_clear(&f);
}

static gboolean label_with(GtkWidget *widget, const gchar *prefix);

static GtkButton *
button_labeled(GtkWidget *widget, const gchar *label)
{
  if (GTK_IS_BUTTON(widget) && gtk_widget_get_visible(widget) &&
      g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return GTK_BUTTON(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkButton *found = button_labeled(child, label);
    if (found)
      return found;
  }
  return NULL;
}

/* D6: with no server the first use asks for one, refuses what isn't an
 * https server, never picks one, and contacts nothing. */
static void
test_first_use_server(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024, 2);
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "photo.jpg", "image/jpeg");
  GhAttachmentSheet *sheet = wait_page(&f, "servers");
  g_assert_true(label_with(GTK_WIDGET(sheet),
                           "Files are encrypted on this device before upload. The server receives "
                           "unreadable bytes, not the original file's type."));
  AdwEntryRow *entry = gh_attachment_sheet_get_server_entry(sheet);
  gtk_editable_set_text(GTK_EDITABLE(entry), "http://files.example.com");
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.use-server", NULL);
  g_assert_cmpstr(gh_attachment_sheet_get_page(sheet), ==, "servers");
  g_assert_nonnull(gh_attachment_sheet_get_error(sheet));
  g_auto(GStrv) none = g_settings_get_strv(f.s.settings, "blossom-servers");
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  /* Editing clears the error in place. */
  gtk_editable_set_text(GTK_EDITABLE(entry), "HTTPS://Files.Example.com/");
  g_assert_null(gh_attachment_sheet_get_error(sheet));
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.use-server", NULL);
  wait_page(&f, "preview");
  g_auto(GStrv) chosen = g_settings_get_strv(f.s.settings, "blossom-servers");
  g_assert_cmpstr(chosen[0], ==, "https://files.example.com");
  g_assert_null(chosen[1]);
  g_assert_nonnull(strstr(gh_attachment_sheet_get_server_note(sheet), "files.example.com"));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  /* Closing sends nothing. */
  close_sheet(&f);
  g_assert_null(own_file_message(bob));
  fixture_clear(&f);
}

/* First-use suggestions are visible, but neither saved nor contacted until
 * the user explicitly chooses one. Each button persists only its own URL. */
static void
test_suggested_encrypted_media_servers(void)
{
  static const gchar *const hosts[] = {
    "blossom.divine.video", "blossom.ditto.pub", "cdn.hzrd149.com"
  };
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024, 3);
  for (guint selected = 0; selected < G_N_ELEMENTS(hosts); selected++) {
    g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", NULL));
    gh_attachment_ui_offer_bytes(f.s.window, jpeg, "photo.jpg", "image/jpeg");
    GhAttachmentSheet *sheet = wait_page(&f, "servers");
    g_assert_true(label_with(GTK_WIDGET(sheet), "White Noise uses these servers for encrypted media."));
    g_assert_true(label_with(GTK_WIDGET(sheet), "The chosen server sees your IP address"));
    for (guint i = 0; i < G_N_ELEMENTS(hosts); i++)
      g_assert_nonnull(button_labeled(GTK_WIDGET(sheet), hosts[i]));
    g_auto(GStrv) before = g_settings_get_strv(f.s.settings, "blossom-servers");
    g_assert_cmpuint(g_strv_length(before), ==, 0);
    g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
    g_signal_emit_by_name(button_labeled(GTK_WIDGET(sheet), hosts[selected]), "clicked");
    wait_page(&f, "preview");
    g_auto(GStrv) after = g_settings_get_strv(f.s.settings, "blossom-servers");
    g_autofree gchar *expected = g_strdup_printf("https://%s", hosts[selected]);
    g_assert_cmpstr(after[0], ==, expected);
    g_assert_null(after[1]);
    g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
    close_sheet(&f);
  }
  fixture_clear(&f);
}

static Socks5Fixture *
first_use_proxy(Fixture *f)
{
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, blossom_fixture_port(f->blossom));
  g_assert_true(g_settings_set_string(f->s.settings, "network-mode", "tor"));
  g_assert_true(g_settings_set_string(f->s.settings, "tor-socks-address",
                                      socks5_fixture_address(socks)));
  return socks;
}

/* A first-use suggestion belongs to this offer, not to a later settings or
 * client snapshot. A concurrent stale settings write must not turn Send back
 * into Choose an Attachment Server before the first PUT. */
static void
test_first_use_suggested_send(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  Socks5Fixture *socks = first_use_proxy(&f);
  g_autofree gchar *server =
    g_strdup_printf("http://" FIRST_USE_ONION ":%u", blossom_fixture_port(f.blossom));
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  g_auto(GStrv) empty = g_settings_get_strv(f.s.settings, "blossom-servers");
  g_assert_null(empty[0]);
  g_autoptr(GBytes) png = make_png(200);
  gh_attachment_ui_offer_bytes(f.s.window, png, "photo.png", "image/png");
  GhAttachmentSheet *sheet = wait_page(&f, "servers");
  gh_attachment_sheet_set_suggestion_for_test(sheet, 0, server);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  g_signal_emit_by_name(button_labeled(GTK_WIDGET(sheet), "Fixture server"), "clicked");
  wait_page(&f, "preview");
  g_auto(GStrv) chosen = g_settings_get_strv(f.s.settings, "blossom-servers");
  g_assert_cmpstr(chosen[0], ==, server);
  g_assert_null(chosen[1]);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  /* Model a lagging settings writer after the sheet accepted the choice. */
  g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", NULL));
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  gh_test_spin_until(has_own_file, bob);
  fixture_clear(&f);
  socks5_fixture_free(socks);
}

/* The server shown by a live sheet wins over an old client server snapshot. */
static void
test_live_server_choice(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024, 5);
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "photo.jpg", "image/jpeg");
  GhAttachmentSheet *sheet = wait_page(&f, "servers");
  /* A setting delivered to the UI later than an already-created client's
   * snapshot must still be the one used for this send. */
  const gchar *chosen[] = { blossom_fixture_url(f.blossom), NULL };
  g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", chosen));
  const gchar *stale[] = { NULL };
  gh_blossom_client_set_servers(gh_attachments_get_client(f.attachments), stale);
  gh_attachment_sheet_show_preview(sheet, NULL);
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  gh_test_spin_until(has_own_file, bob);
  GhBlossomClient *client = gh_attachments_get_client(f.attachments);
  g_auto(GStrv) still_stale = gh_blossom_client_dup_servers(client);
  g_assert_null(still_stale[0]); /* this upload never touched the shared override */
  gh_blossom_client_set_servers(client, NULL);
  g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", chosen));
  g_auto(GStrv) after = gh_blossom_client_dup_servers(client);
  g_assert_cmpstr(after[0], ==, blossom_fixture_url(f.blossom));
  fixture_clear(&f);
}

/* An image-only server's rejection stays visible in the Send File sheet. */
static void
test_opaque_server_refusal(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  blossom_fixture_set_strict_upload(f.blossom, TRUE);
  blossom_fixture_reject_opaque(f.blossom, TRUE);
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024, 6);
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "photo.jpg", "image/jpeg");
  GhAttachmentSheet *sheet = wait_page(&f, "servers");
  g_assert_true(label_with(GTK_WIDGET(sheet),
                           "Files are encrypted on this device before upload. The server receives "
                           "unreadable bytes, not the original file's type."));
  const gchar *chosen[] = { blossom_fixture_url(f.blossom), NULL };
  g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", chosen));
  gh_attachment_sheet_show_preview(sheet, NULL);
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  wait_page(&f, "sending");
  wait_page(&f, "preview");
  g_assert_nonnull(strstr(gh_attachment_sheet_get_error(sheet), "HTTP 415"));
  g_assert_nonnull(strstr(gh_attachment_sheet_get_error(sheet), "File type not allowed"));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  g_assert_null(own_file_message(bob));
  fixture_clear(&f);
}

/* Charter §6 step 4, AT-6, nostrc-dnsc: a server that wants a known account
 * gets the account's signature only after consent, which is kept for this
 * account and revoked from Preferences. */
static void
test_consent(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  blossom_fixture_require_pubkey(f.blossom, stack_hex[1]);
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024, 3);
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "photo.jpg", "image/jpeg");
  GhAttachmentSheet *sheet = wait_page(&f, "preview");
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  wait_page(&f, "consent");
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1); /* refused, throwaway key */
  g_assert_false(gh_attachments_get_consent(f.attachments, f.server));

  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.consent", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 2);
  GPtrArray *requests = blossom_fixture_requests(f.blossom);
  BlossomRequest *put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(put->auth_pubkey, ==, stack_hex[1]);
  gh_test_spin_until(has_own_file, bob);
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_account_store_get_store(f.s.store);
  g_auto(GStrv) kept = gh_store_blossom_dup_consents(store, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(kept[0], ==, f.server);
  g_assert_null(kept[1]);

  /* Another file to the same server: no second question. */
  guint puts = blossom_fixture_count(f.blossom, "PUT");
  g_autoptr(GBytes) second = make_jpeg(8 * 1024, 30);
  gh_attachment_ui_offer_bytes(f.s.window, second, "photo2.jpg", "image/jpeg");
  sheet = wait_page(&f, "preview");
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, puts + 1);

  /* "Don't Upload" sends nothing: after a revocation the server asks again. */
  g_assert_true(gh_attachments_set_consent(f.attachments, f.server, FALSE, &error));
  g_autoptr(GBytes) third = make_jpeg(8 * 1024, 31);
  gh_attachment_ui_offer_bytes(f.s.window, third, "photo3.jpg", "image/jpeg");
  sheet = wait_page(&f, "preview");
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  wait_page(&f, "consent");
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.cancel", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_false(gh_attachments_get_consent(f.attachments, f.server));
  g_auto(GStrv) none = gh_store_blossom_dup_consents(store, &error);
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  fixture_clear(&f);
}

/* ---- Preferences › Attachments --------------------------------------------------------- */

static gboolean
prefs_cache_size(gpointer data, gint64 *out_bytes, GError **error)
{
  return gh_attachments_get_cache_size(GH_ATTACHMENTS(data), out_bytes, error);
}

static gboolean
prefs_clear_cache(gpointer data, GError **error)
{
  return gh_attachments_clear_cache(GH_ATTACHMENTS(data), error);
}

static gboolean
prefs_get_consent(gpointer data, const gchar *server)
{
  return gh_attachments_get_consent(GH_ATTACHMENTS(data), server);
}

static gboolean
prefs_revoke_consent(gpointer data, const gchar *server, GError **error)
{
  return gh_attachments_set_consent(GH_ATTACHMENTS(data), server, FALSE, error);
}

static GtkWidget *
tooltip_button(GtkWidget *widget, const gchar *tooltip)
{
  if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_widget_get_tooltip_text(widget), tooltip) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = tooltip_button(c, tooltip);
    if (found)
      return found;
  }
  return NULL;
}

static gpointer
prefs_child(GhPreferencesDialog *dialog, const gchar *name)
{
  return gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG, name);
}

static GtkWidget *
server_row(GhPreferencesDialog *dialog, gint index)
{
  return GTK_WIDGET(gtk_list_box_get_row_at_index(prefs_child(dialog, "blossom_list"), index));
}

static gboolean
rooted(gpointer data)
{
  return gtk_widget_get_root(GTK_WIDGET(data)) != NULL;
}

/* The servers in the order they're tried (Move Up, Move Down, Remove), the
 * largest file, the decrypted copies with Clear…, and Revoke for a server
 * the account uploads to as itself. */
static void
test_preferences(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  const gchar *servers[] = { blossom_fixture_url(f.blossom), "https://second.example.com", NULL };
  g_settings_set_strv(f.s.settings, "blossom-servers", servers);
  GhConversation *bob = receive_text(&f, "A file for the cache");
  g_autoptr(GBytes) png = make_png(20);
  g_autoptr(GhNip17File) file = upload_file(&f, png);
  GhMessage *message = receive_file(&f, bob, file);
  g_autoptr(GhAttachmentTransfer) transfer =
    g_object_ref(gh_attachments_lookup(f.attachments, message));
  gh_attachments_download(f.attachments, transfer);
  gh_test_spin_until(transfer_not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_attachments_set_consent(f.attachments, "https://second.example.com", TRUE,
                                           &error));

  GhPreferencesDialog *dialog = gh_preferences_dialog_new(f.s.settings,
                                                          GH_PREFERENCES_FEATURES_ALL);
  static const GhPreferencesAttachments funcs = {
    .max_file_size = GH_BLOSSOM_MAX_FILE_SIZE,
    .get_cache_size = prefs_cache_size,
    .clear_cache = prefs_clear_cache,
    .get_consent = prefs_get_consent,
    .revoke_consent = prefs_revoke_consent,
  };
  gh_preferences_dialog_set_attachments(dialog, &funcs, g_object_ref(f.attachments),
                                        g_object_unref);
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog), "messages");
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(f.s.window));
  gh_test_spin_until(rooted, dialog);

  g_assert_true(gtk_widget_get_visible(prefs_child(dialog, "attachment_info_list")));
  g_autofree gchar *limit = g_format_size(GH_BLOSSOM_MAX_FILE_SIZE);
  g_assert_cmpstr(adw_action_row_get_subtitle(prefs_child(dialog, "attachment_limit_row")), ==,
                  limit);
  g_autofree gchar *size = g_format_size(g_bytes_get_size(png));
  const gchar *cache = adw_action_row_get_subtitle(prefs_child(dialog, "attachment_cache_row"));
  g_assert_nonnull(strstr(cache, size));
  g_assert_nonnull(strstr(cache, "encrypted on this device"));

  /* Order: the first can't move up; Move Down swaps it with the second. */
  GtkWidget *first = server_row(dialog, 0);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, servers[0]);
  g_assert_false(gtk_widget_get_sensitive(tooltip_button(first, "Move Up")));
  g_autofree gchar *down_label = g_strdup_printf("Move %s down", servers[0]);
  GtkWidget *down = tooltip_button(first, "Move Down");
  assert_accessible_label(down, down_label);
  g_signal_emit_by_name(down, "clicked");
  gh_test_run_until_idle();
  g_auto(GStrv) swapped = g_settings_get_strv(f.s.settings, "blossom-servers");
  g_assert_cmpstr(swapped[0], ==, servers[1]);
  g_assert_cmpstr(swapped[1], ==, servers[0]);

  /* The server the account uploads to as itself says so, and Revoke takes
   * it back (in the store, for this account). */
  GtkWidget *consented = server_row(dialog, 0);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(consented)), ==,
                  "Uploads here are signed by your account");
  GtkWidget *revoke = tooltip_button(consented, "Stop uploading here as your account");
  g_assert_nonnull(revoke);
  g_signal_emit_by_name(revoke, "clicked");
  gh_test_run_until_idle();
  g_assert_false(gh_attachments_get_consent(f.attachments, "https://second.example.com"));
  g_auto(GStrv) left = gh_store_blossom_dup_consents(gh_account_store_get_store(f.s.store),
                                                     &error);
  g_assert_cmpuint(g_strv_length(left), ==, 0);
  g_assert_null(tooltip_button(server_row(dialog, 0), "Stop uploading here as your account"));

  /* Clear…: confirmed, the decrypted copies go and the card is IDLE. */
  AdwAlertDialog *alert = prefs_child(dialog, "clear_attachments_dialog");
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(alert, "clear"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  g_assert_cmpstr(adw_alert_dialog_get_default_response(alert), ==, "keep");
  g_assert_cmpstr(adw_alert_dialog_get_close_response(alert), ==, "keep");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "prefs.clear-attachments", NULL));
  gh_test_spin_until(rooted, alert);
  g_signal_emit_by_name(alert, "response", "clear");
  adw_dialog_force_close(ADW_DIALOG(alert));
  gh_test_run_until_idle();
  gint64 bytes = -1;
  g_assert_true(gh_attachments_get_cache_size(f.attachments, &bytes, &error));
  g_assert_cmpint(bytes, ==, 0);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_cmpstr(gh_preferences_dialog_get_last_toast(dialog), ==,
                  "Downloaded files were deleted from this device");
  g_assert_cmpstr(adw_action_row_get_subtitle(prefs_child(dialog, "attachment_cache_row")), ==,
                  "None on this device");
  /* Nothing left to clear: the button follows its disabled action. */
  g_assert_false(gtk_widget_get_sensitive(prefs_child(dialog, "attachment_clear_button")));
  adw_dialog_force_close(ADW_DIALOG(dialog));
  gh_test_run_until_idle();
  fixture_clear(&f);
}

/* A dropped file and a pasted image go the same way as a chosen one; a
 * text paste stays text. */
static void
test_drop_and_paste(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  GhConversation *bob = receive_text(&f, "Hi");
  send_stack_select(&f.s, bob);
  GhComposer *composer = send_stack_composer(&f.s);

  /* A file dropped on the composer. */
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024, 4);
  g_autofree gchar *path = g_build_filename(f.s.root, "IMG_0001.jpg", NULL);
  g_assert_true(g_file_set_contents(path, g_bytes_get_data(jpeg, NULL),
                                    (gssize)g_bytes_get_size(jpeg), NULL));
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(GTK_WIDGET(composer));
  GtkDropTarget *drop = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !drop; i++) {
    g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(controller), "groundhog-composer-drop") == 0)
      drop = GTK_DROP_TARGET(controller);
  }
  g_assert_nonnull(drop);
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_TYPE_FILE);
  g_value_set_object(&value, file);
  gboolean dropped = FALSE;
  g_signal_emit_by_name(drop, "drop", &value, 10.0, 10.0, &dropped);
  g_value_unset(&value);
  g_assert_true(dropped);
  GhAttachmentSheet *sheet = wait_page(&f, "preview");
  g_assert_cmpstr(gh_attachment_sheet_get_metadata_text(sheet), ==,
                  "Location and camera data removed");
  close_sheet(&f);

  /* A dropped web address (W18 review B1): refused before any I/O, since
   * GVfs would fetch it directly, outside GhNetHttp and Tor. No sheet. */
  g_autoptr(GFile) web = g_file_new_for_uri("https://127.0.0.1:9/photo.jpg");
  g_assert_false(g_file_is_native(web));
  gh_attachment_ui_offer_file(GH_WINDOW(f.s.window), web);
  g_assert_cmpstr(gh_attachment_ui_get_last_toast(GH_WINDOW(f.s.window)), ==,
                  "Only files on this device can be sent");
  g_assert_false(sheet_open(&f));

  /* A pasted image: a fresh PNG of its pixels, the photo shown; nothing
   * pasted as text. */
  GtkTextView *text_view = gh_composer_get_text_view(composer);
  /* A private clipboard, never the system one (nostrc-rjz2): that one is
   * shared with every other process (a parallel test, the user), and on
   * macOS what this process puts there re-enters its main loop when another
   * process reads it, which GLib reports as a failed poll(2) (fatal here). */
  g_autoptr(GdkClipboard) clipboard =
    g_object_new(GDK_TYPE_CLIPBOARD, "display", gtk_widget_get_display(GTK_WIDGET(text_view)),
                 NULL);
  gh_composer_set_clipboard(composer, clipboard);
  TextPaste text_paste = { clipboard, 0 };
  gulong text_handler = g_signal_connect(text_view, "paste-clipboard",
                                         G_CALLBACK(on_text_paste), &text_paste);
  g_autoptr(GBytes) png = make_png(200);
  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes(png, NULL);
  g_assert_nonnull(texture);
  gdk_clipboard_set_texture(clipboard, texture);
  g_signal_emit_by_name(text_view, "paste-clipboard");
  gh_test_spin_until(sheet_open, &f);
  sheet = wait_page(&f, "preview");
  g_assert_true(gh_attachment_sheet_get_has_thumbnail(sheet));
  g_assert_cmpstr(gh_attachment_sheet_get_metadata_text(sheet), ==,
                  "Location and camera data removed");
  g_autofree gchar *typed = gh_composer_dup_text(composer);
  g_assert_cmpstr(typed, ==, "");
  g_assert_cmpuint(text_paste.reached, ==, 0); /* the composer took the image */
  close_sheet(&f);

  /* Text pastes as text: the composer lets it through to the text view's
   * own paste. */
  gdk_clipboard_set_text(clipboard, "a caption");
  gtk_widget_grab_focus(GTK_WIDGET(text_view));
  g_signal_emit_by_name(text_view, "paste-clipboard");
  g_assert_cmpuint(text_paste.reached, ==, 1);
  gh_test_spin_until(caption_pasted, composer);
  g_signal_handler_disconnect(text_view, text_handler);
  g_assert_null(gh_attachment_ui_get_sheet(f.s.window));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  fixture_clear(&f);
}

/* AT-7 in the UI: a received file's card fetches nothing until Download;
 * then the photo (after the decode guard) and Save As; Save As writes only
 * where the Save dialog chose. */
static void
test_card_download_and_save(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  GhConversation *bob = receive_text(&f, "Here it comes");
  g_autoptr(GBytes) png = make_png(90);
  g_autoptr(GhNip17File) file = upload_file(&f, png);
  GhMessage *message = receive_file(&f, bob, file);
  send_stack_select(&f.s, bob);
  GhAttachmentCard *card = card_of(&f, message);
  gh_test_run_until_idle();
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 0);
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "Photo"));
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "Not downloaded"));
  GtkWidget *download = card_button(GTK_WIDGET(card), "_Download");
  g_assert_nonnull(download);
  g_assert_nonnull(strstr(gtk_widget_get_tooltip_text(download), "127.0.0.1"));
  g_assert_null(shown_picture(GTK_WIDGET(card)));
  g_assert_false(g_file_test(f.saved, G_FILE_TEST_EXISTS));

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.download", NULL));
  GhAttachmentTransfer *transfer = gh_attachment_card_get_transfer(card);
  gh_test_spin_until(transfer_not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  gh_test_run_until_idle();
  GtkPicture *picture = shown_picture(GTK_WIDGET(card));
  g_assert_nonnull(picture);
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(gtk_picture_get_paintable(picture)), ==, 8);
  g_assert_nonnull(card_button(GTK_WIDGET(card), "_Save As…"));
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "Downloaded"));
  /* Nothing written anywhere before the user saves. */
  g_assert_false(g_file_test(f.saved, G_FILE_TEST_EXISTS));
  g_assert_cmpuint(f.save_asked, ==, 0);

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.save", NULL));
  gh_test_spin_until(toast_shown, f.s.window);
  g_assert_cmpuint(f.save_asked, ==, 1);
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(f.saved, &contents, &length, NULL));
  g_assert_cmpmem(contents, length, g_bytes_get_data(png, NULL), g_bytes_get_size(png));
  g_assert_nonnull(strstr(gh_attachment_ui_get_last_toast(f.s.window),
                          "aren't protected by Groundhog"));
  fixture_clear(&f);
}

/* AT-8 in the UI: Cancel stops a download at once, Download is offered
 * again, nothing is kept; failures are said in words. */
static void
test_card_cancel_and_errors(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  GhConversation *bob = receive_text(&f, "Two files");
  g_autoptr(GBytes) jpeg = make_jpeg(256 * 1024, 5);
  g_autoptr(GhNip17File) file = upload_file(&f, jpeg);
  GhMessage *message = receive_file(&f, bob, file);
  send_stack_select(&f.s, bob);
  GhAttachmentCard *card = card_of(&f, message);

  blossom_fixture_set_stall(f.blossom, TRUE);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.download", NULL));
  gh_test_spin_until(fixture_held, f.blossom);
  gh_test_run_until_idle();
  GhAttachmentTransfer *transfer = gh_attachment_card_get_transfer(card);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==,
                  GH_ATTACHMENT_STATE_DOWNLOADING);
  g_assert_nonnull(card_button(GTK_WIDGET(card), "_Cancel"));
  g_assert_null(card_button(GTK_WIDGET(card), "_Download"));
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "Downloading"));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.cancel", NULL));
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_nonnull(card_button(GTK_WIDGET(card), "_Download"));
  blossom_fixture_release_held(f.blossom);
  blossom_fixture_set_stall(f.blossom, FALSE);
  gh_test_run_until_idle();
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_store_media_get(gh_account_store_get_store(f.s.store), file, NULL, &error));
  g_assert_no_error(error);

  /* Damaged on the server: said, no Try Again. */
  GBytes *stored = blossom_fixture_get_blob(f.blossom, file->x);
  gsize size = g_bytes_get_size(stored);
  guint8 *tampered = g_memdup2(g_bytes_get_data(stored, NULL), size);
  tampered[size - 20] ^= 0x10;
  g_autoptr(GBytes) bad = g_bytes_new_take(tampered, size);
  blossom_fixture_put_blob(f.blossom, file->x, bad);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.download", NULL));
  gh_test_spin_until(transfer_not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_FAILED);
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "changed or damaged"));
  g_assert_null(card_button(GTK_WIDGET(card), "_Try Again"));
  g_assert_null(shown_picture(GTK_WIDGET(card)));

  /* The server gone: said, with Try Again. */
  g_autoptr(GhNip17File) gone = gh_nip17_file_copy(file);
  g_free(gone->url);
  gone->url = g_strdup_printf("http://127.0.0.1:%u/%s", gh_test_refused_port(), gone->x);
  gone->nonce[0] ^= 1;
  GhMessage *unreachable = receive_file(&f, bob, gone);
  GhAttachmentCard *second = card_of(&f, unreachable);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(second), "attachment.download", NULL));
  GhAttachmentTransfer *failing = gh_attachment_card_get_transfer(second);
  gh_test_spin_until(transfer_not_downloading, failing);
  g_assert_cmpint(gh_attachment_transfer_get_state(failing), ==, GH_ATTACHMENT_STATE_FAILED);
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(second), "Can't reach the server"));
  g_assert_nonnull(card_button(GTK_WIDGET(second), "_Try Again"));
  fixture_clear(&f);
}

/* ---- main ------------------------------------------------------------------------------ */

/* ---- an encrypted group's files through the group delegate (W25) ------------------ */

#define STUB_GROUP "c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3"

/* What gh-mls-attachment-ui.c would do, recorded: the window's attach
 * flow, sheet and cards hand an MLS conversation's files to it. */
typedef struct {
  guint sends;
  GBytes *file;
  gchar *name;
  gchar *mime;
  GStrv servers;
  GStrv policy_servers; /* NULL: this group has no 0x800b policy */
  GhAttachments *upload_attachments; /* only the first-use group fixture */
  GError *fail_with;     /* the next send's error, or NULL */
  gchar *fail_server;
  GhAttachmentTransfer *transfer;
  guint lookups, downloads;
  GSimpleAction *group;  /* the group as watched: "enabled" is "can send now" */
  gboolean ended;        /* a change the group doesn't notify */
  guint can_sends;
} StubGroups;

static gboolean
stub_can_send(GhConversation *conversation, gpointer data)
{
  StubGroups *stub = data;
  stub->can_sends++;
  return g_str_has_suffix(gh_conversation_get_room_id(conversation), STUB_GROUP) &&
         (!stub->group || g_action_get_enabled(G_ACTION(stub->group))) && !stub->ended;
}

static GObject *
stub_watch(GhConversation *conversation, gpointer data)
{
  StubGroups *stub = data;
  return g_str_has_suffix(gh_conversation_get_room_id(conversation), STUB_GROUP)
           ? G_OBJECT(stub->group) : NULL;
}

static GStrv
stub_dup_policy_servers(GhConversation *conversation, gpointer data)
{
  (void)conversation;
  StubGroups *stub = data;
  return stub->policy_servers ? g_strdupv(stub->policy_servers) : NULL;
}

static void
stub_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  g_autoptr(GhNip17File) uploaded =
    gh_attachments_upload_finish(GH_ATTACHMENTS(source), result, NULL, &error);
  if (uploaded)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, error);
  g_object_unref(task);
}

static void
stub_send_async(GhConversation *conversation, GBytes *file, const gchar *name, const gchar *mime,
                const gchar *const *servers, GCancellable *cancellable,
                GAsyncReadyCallback callback, gpointer user_data, gpointer data)
{
  (void)conversation;
  StubGroups *stub = data;
  stub->sends++;
  g_clear_pointer(&stub->file, g_bytes_unref);
  stub->file = g_bytes_ref(file);
  g_free(stub->name);
  stub->name = g_strdup(name);
  g_free(stub->mime);
  stub->mime = g_strdup(mime);
  g_strfreev(stub->servers);
  stub->servers = g_strdupv((gchar **)servers);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (stub->upload_attachments) {
    gh_attachments_upload_on_servers_async(stub->upload_attachments, servers, file, mime,
                                            cancellable, stub_uploaded, task);
    return;
  }
  if (stub->fail_with)
    g_task_return_error(task, g_steal_pointer(&stub->fail_with));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static gboolean
stub_send_finish(GAsyncResult *result, gchar **out_server, GError **error, gpointer data)
{
  StubGroups *stub = data;
  if (out_server)
    *out_server = g_steal_pointer(&stub->fail_server);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static gchar *
stub_describe(const GError *error, const gchar *host, gpointer data)
{
  (void)host;
  (void)data;
  return g_strdup_printf("group: %s", error->message);
}

static GhAttachmentTransfer *
stub_lookup(GhMessage *message, guint index, gpointer data)
{
  (void)message;
  StubGroups *stub = data;
  stub->lookups++;
  return index == 0 ? stub->transfer : NULL;
}

static void
stub_download(GhAttachmentTransfer *transfer, gpointer data)
{
  (void)transfer;
  ((StubGroups *)data)->downloads++;
}

static void
stub_cancel(GhAttachmentTransfer *transfer, gpointer data)
{
  (void)transfer;
  (void)data;
}

static const GhAttachmentUiGroups stub_groups = {
  .can_send = stub_can_send,
  .dup_policy_servers = stub_dup_policy_servers,
  .send_async = stub_send_async,
  .send_finish = stub_send_finish,
  .describe = stub_describe,
  .lookup = stub_lookup,
  .download = stub_download,
  .cancel = stub_cancel,
  .watch = stub_watch,
};

static gboolean
saved_toast(gpointer data)
{
  Fixture *f = data;
  const gchar *t = gh_attachment_ui_get_last_toast(f->s.window);
  return t && g_str_has_prefix(t, "Saved ");
}

/* A decrypted kind-9 group message from key 2 with one file (as the MLS
 * layer describes it) and one rejected reference. */
static GhMessage *
group_file_message(Fixture *f)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 9);
  nostr_event_set_pubkey(event, stack_hex[2]);
  nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC - 30);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("h", STUB_GROUP, NULL)));
  gchar id[65] = { 0 };
  g_assert_cmpint(nostr_event_compute_id(event, id), ==, NOSTR_EVENT_VALIDATION_OK);
  free(event->id);
  event->id = strdup(id);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_mls(stack_hex[f->s.key], STUB_GROUP, json, &error);
  free(json);
  g_assert_no_error(error);
  g_autoptr(GPtrArray) files =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_message_attachment_free);
  GhMessageAttachment *a = g_new0(GhMessageAttachment, 1);
  a->media_type = g_strdup("image/png");
  a->filename = g_strdup("../../.config/autostart/x.png");
  g_ptr_array_add(files, a);
  gh_message_set_attachments(message, files, 1);
  return message;
}

static gboolean
label_with(GtkWidget *widget, const gchar *prefix)
{
  if (GTK_IS_LABEL(widget) && gtk_widget_get_visible(widget) &&
      g_str_has_prefix(gtk_label_get_text(GTK_LABEL(widget)), prefix))
    return TRUE;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c))
    if (label_with(c, prefix))
      return TRUE;
  return FALSE;
}

/* An encrypted group's conversation gets the attach button and the same
 * sheet; Send hands the prepared (metadata-free) file to the group
 * delegate, never to the NIP-17 outbox or the attachment servers; the
 * delegate's consent request shows the consent page; its message's card
 * asks the delegate for its transfer, fetches nothing until Download, says
 * a rejected reference, and Save As suggests the sender's name only after
 * it is made safe. */
static void
test_group_delegate(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  StubGroups stub = { 0 };
  stub.transfer = gh_attachment_transfer_new_described("stub/0", "image/png",
                                                       "../../.config/autostart/x.png");
  stub.group = g_simple_action_new("group", NULL);
  gh_attachment_ui_set_groups(f.s.window, &stub_groups, &stub, NULL);
  g_autoptr(GhMessage) message = group_file_message(&f);
  g_autoptr(GError) error = NULL;
  gh_conversation_store_add_message(f.s.model, message, &error);
  g_assert_no_error(error);
  GhConversation *group = gh_conversation_store_lookup(f.s.model,
                                                       gh_message_get_room_id(message));
  g_assert_nonnull(group);
  GhComposer *composer = send_stack_composer(&f.s);
  send_stack_select(&f.s, group);
  g_assert_true(gh_composer_get_can_attach(composer));
  guint pubs_before = pubs ? pubs->len : 0;

  /* The attach button follows the shown group as it changes (left,
   * removed, leaving, offline: its notify), not only when another
   * conversation is shown (W25 re-review R2); a change the group doesn't
   * notify is the delegate's to say (gh_attachment_ui_groups_changed()). */
  g_simple_action_set_enabled(stub.group, FALSE);
  g_assert_false(gh_composer_get_can_attach(composer));
  g_simple_action_set_enabled(stub.group, TRUE);
  g_assert_true(gh_composer_get_can_attach(composer));
  stub.ended = TRUE;
  g_assert_true(gh_composer_get_can_attach(composer));
  gh_attachment_ui_groups_changed(f.s.window);
  g_assert_false(gh_composer_get_can_attach(composer));
  stub.ended = FALSE;
  gh_attachment_ui_groups_changed(f.s.window);
  g_assert_true(gh_composer_get_can_attach(composer));

  /* Send: the stripped bytes go to the delegate. */
  g_autoptr(GBytes) jpeg = make_jpeg(32 * 1024, 7);
  g_assert_true(bytes_contain(jpeg, GPS_TEXT));
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "IMG_2042.jpg", "image/jpeg");
  GhAttachmentSheet *sheet = wait_page(&f, "preview");
  g_assert_cmpstr(gh_attachment_sheet_get_metadata_text(sheet), ==,
                  "Location and camera data removed");
  g_assert_cmpuint(stub.sends, ==, 0);
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(stub.sends, ==, 1);
  g_assert_false(bytes_contain(stub.file, GPS_TEXT));
  g_assert_cmpstr(stub.mime, ==, "image/jpeg");
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  g_assert_cmpuint(pubs ? pubs->len : 0, ==, pubs_before);

  /* A server that wants a known account: the same consent page. */
  stub.fail_with = g_error_new_literal(GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED,
                                       "known accounts only");
  stub.fail_server = g_strdup(blossom_fixture_url(f.blossom));
  gh_attachment_ui_offer_bytes(f.s.window, jpeg, "IMG_2043.jpg", "image/jpeg");
  sheet = wait_page(&f, "preview");
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  wait_page(&f, "consent");
  g_assert_cmpuint(stub.sends, ==, 2);
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.cancel", NULL);
  gh_test_spin_until(sheet_closed, &f);

  /* The card: the delegate's transfer, nothing fetched, the rejected
   * reference said. */
  GhAttachmentCard *card = card_of(&f, message);
  g_assert_true(gh_attachment_card_get_transfer(card) == stub.transfer);
  g_assert_cmpuint(stub.downloads, ==, 0);
  g_assert_true(label_with(GTK_WIDGET(f.s.window), "1 attached file can't be read"));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.download", NULL));
  g_assert_cmpuint(stub.downloads, ==, 1);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);

  /* Save As: the sender's name, made safe. */
  g_autoptr(GBytes) plain = g_bytes_new_static("\x89PNG\r\n\x1a\n", 8);
  gh_attachment_transfer_succeed(stub.transfer, plain, FALSE, FALSE);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.save", NULL));
  /* The write is asynchronous: its toast first, so the next one is ours. */
  gh_test_spin_until(saved_toast, &f);
  g_assert_cmpuint(f.save_asked, ==, 1);
  g_assert_cmpstr(f.save_name, ==, "_.._.config_autostart_x.png");

  /* A file under a GVfs FUSE mount is a remote location (SMB, SFTP...)
   * under a native path: refused before anything is read, here as in a
   * NIP-17 conversation. */
  g_autofree gchar *share = g_build_filename(g_get_user_runtime_dir(), "gvfs",
                                             "smb-share:server=nas,share=photos", NULL);
  g_assert_cmpint(g_mkdir_with_parents(share, 0700), ==, 0);
  g_autofree gchar *remote = g_build_filename(share, "holiday.jpg", NULL);
  g_assert_true(g_file_set_contents(remote, "\xff\xd8\xff", 3, NULL));
  g_autoptr(GFile) remote_file = g_file_new_for_path(remote);
  guint sends = stub.sends;
  gh_attachment_ui_offer_file(f.s.window, remote_file);
  gh_test_run_until_idle();
  g_assert_cmpstr(gh_attachment_ui_get_last_toast(f.s.window), ==,
                  "Only files on this device can be sent");
  g_assert_null(gh_attachment_ui_get_sheet(f.s.window));
  g_assert_cmpuint(stub.sends, ==, sends);

  gh_attachment_ui_set_groups(f.s.window, NULL, NULL, NULL);
  g_assert_false(gh_composer_get_can_attach(composer));
  /* No delegate, no watch: the group's notify asks nothing any more. */
  guint asked = stub.can_sends;
  g_simple_action_set_enabled(stub.group, FALSE);
  g_assert_cmpuint(stub.can_sends, ==, asked);
  fixture_clear(&f);
  g_clear_object(&stub.group);
  g_clear_object(&stub.transfer);
  g_clear_pointer(&stub.file, g_bytes_unref);
  g_free(stub.name);
  g_free(stub.mime);
  g_strfreev(stub.servers);
}

/* The MLS sheet gives its delegate the same first-use choice. This delegate
 * uploads through the real local Blossom fixture; the MDK case separately
 * covers MLS sealing and its on-wire message. */
static void
test_group_first_use_suggested_send(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  Socks5Fixture *socks = first_use_proxy(&f);
  g_autofree gchar *server =
    g_strdup_printf("http://" FIRST_USE_ONION ":%u", blossom_fixture_port(f.blossom));
  StubGroups stub = { .upload_attachments = f.attachments };
  gh_attachment_ui_set_groups(f.s.window, &stub_groups, &stub, NULL);
  g_autoptr(GhMessage) message = group_file_message(&f);
  g_autoptr(GError) error = NULL;
  gh_conversation_store_add_message(f.s.model, message, &error);
  g_assert_no_error(error);
  GhConversation *group = gh_conversation_store_lookup(f.s.model,
                                                       gh_message_get_room_id(message));
  g_assert_nonnull(group);
  send_stack_select(&f.s, group);
  g_auto(GStrv) empty = g_settings_get_strv(f.s.settings, "blossom-servers");
  g_assert_null(empty[0]);
  g_autoptr(GBytes) png = make_png(200);
  gh_attachment_ui_offer_bytes(f.s.window, png, "photo.png", "image/png");
  GhAttachmentSheet *sheet = wait_page(&f, "servers");
  gh_attachment_sheet_set_suggestion_for_test(sheet, 0, server);
  g_signal_emit_by_name(button_labeled(GTK_WIDGET(sheet), "Fixture server"), "clicked");
  wait_page(&f, "preview");
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", NULL));
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(stub.sends, ==, 1);
  g_assert_cmpstr(stub.servers[0], ==, server);
  g_assert_null(stub.servers[1]);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  gh_attachment_ui_set_groups(f.s.window, NULL, NULL, NULL);
  fixture_clear(&f);
  socks5_fixture_free(socks);
  g_clear_pointer(&stub.file, g_bytes_unref);
  g_free(stub.name);
  g_free(stub.mime);
  g_strfreev(stub.servers);
}

/* The account chose B, but the group's verified 0x800b policy names A.
 * The sheet shows A read-only and the same-session PUT must go only to A. */
static void
test_group_policy_overrides_selected_server(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, FALSE);
  BlossomFixture *selected = blossom_fixture_new();
  const gchar *policy_url = blossom_fixture_url(f.blossom);
  const gchar *selected_url = blossom_fixture_url(selected);
  const gchar *account_servers[] = { selected_url, NULL };
  const gchar *policy_servers[] = { policy_url, NULL };
  g_assert_true(g_settings_set_strv(f.s.settings, "blossom-servers", account_servers));
  StubGroups stub = { .upload_attachments = f.attachments,
                      .policy_servers = g_strdupv((gchar **)policy_servers) };
  gh_attachment_ui_set_groups(f.s.window, &stub_groups, &stub, NULL);
  g_autoptr(GhMessage) message = group_file_message(&f);
  g_autoptr(GError) error = NULL;
  gh_conversation_store_add_message(f.s.model, message, &error);
  g_assert_no_error(error);
  GhConversation *group = gh_conversation_store_lookup(f.s.model,
                                                       gh_message_get_room_id(message));
  g_assert_nonnull(group);
  send_stack_select(&f.s, group);
  g_autoptr(GBytes) png = make_png(200);
  gh_attachment_ui_offer_bytes(f.s.window, png, "photo.png", "image/png");
  GhAttachmentSheet *sheet = wait_page(&f, "preview");
  const gchar *note = gh_attachment_sheet_get_server_note(sheet);
  g_assert_nonnull(strstr(note, policy_url));
  g_assert_null(strstr(note, selected_url));
  g_assert_nonnull(strstr(note, "Group Info"));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  g_assert_cmpuint(blossom_fixture_count(selected, NULL), ==, 0);
  gtk_widget_activate_action(GTK_WIDGET(sheet), "sheet.send", NULL);
  gh_test_spin_until(sheet_closed, &f);
  g_assert_cmpuint(stub.sends, ==, 1);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  g_assert_cmpuint(blossom_fixture_count(selected, "PUT"), ==, 0);
  g_assert_cmpstr(stub.servers[0], ==, policy_url);
  g_assert_null(stub.servers[1]);
  gh_attachment_ui_set_groups(f.s.window, NULL, NULL, NULL);
  fixture_clear(&f);
  blossom_fixture_free(selected);
  g_clear_pointer(&stub.file, g_bytes_unref);
  g_free(stub.name);
  g_free(stub.mime);
  g_strfreev(stub.servers);
  g_strfreev(stub.policy_servers);
}

#if GROUNDHOG_TEST_MLS_FILES
/* The application's own delegate (gh-mls-attachment-ui.c) on a real window,
 * under fatal-criticals: it installs and follows without a CRITICAL (W25
 * re-review R2: its follower was once connected with a non-GObject, so it
 * never followed), twice too; without an MLS service no group conversation
 * offers the attach button, before or after a groups change. */
static void
test_mls_delegate_attach(void)
{
  Fixture f;
  fixture_init(&f);
  fixture_up(&f, TRUE);
  GhMlsAttachments *files = gh_mls_attachments_new(f.attachments);
  gh_mls_attachment_ui_attach(f.s.window, files);
  g_autoptr(GhMessage) message = group_file_message(&f);
  g_autoptr(GError) error = NULL;
  gh_conversation_store_add_message(f.s.model, message, &error);
  g_assert_no_error(error);
  GhConversation *group = gh_conversation_store_lookup(f.s.model,
                                                       gh_message_get_room_id(message));
  g_assert_nonnull(group);
  GhComposer *composer = send_stack_composer(&f.s);
  send_stack_select(&f.s, group);
  g_assert_false(gh_composer_get_can_attach(composer));
  gh_attachment_ui_groups_changed(f.s.window);
  g_assert_false(gh_composer_get_can_attach(composer));
  gh_mls_attachment_ui_attach(f.s.window, files);
  g_assert_false(gh_composer_get_can_attach(composer));
  g_object_unref(files);   /* the window keeps its own */
  fixture_clear(&f);
}
#endif

static gchar *
test_env_up(void)
{
  gchar *root = g_dir_make_tmp("groundhog-attachment-ui-xdg-XXXXXX", NULL);
  g_assert_nonnull(root);
  static const gchar *const vars[] = { "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                                       "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    g_autofree gchar *dir = g_build_filename(root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
  }
  return root;
}

int
main(int argc, char **argv)
{
  if (!nostrc_test_bus_available()) {
    g_printerr("groundhog-attachment-ui test skipped: dbus-daemon is not installed\n");
    return 77;
  }
  xdg_root = test_env_up();
  if (!stack_gtk_and_bus_up(&bus)) {
    g_printerr("groundhog-attachment-ui test skipped: no graphical display\n");
    gh_test_remove_tree(xdg_root);
    return 77;
  }
  gh_test_signer_up(&bus, &signer);
  groundhog_register_resource();
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  stack_keys_init();
#define ADD(path, func) nostrc_test_bus_add_func("/groundhog/attachment-ui/" path, func)
  ADD("attach-send", test_attach_send);
  ADD("first-use-server", test_first_use_server);
  ADD("suggested-encrypted-media-servers", test_suggested_encrypted_media_servers);
  ADD("first-use-suggested-send", test_first_use_suggested_send);
  ADD("live-server-choice", test_live_server_choice);
  ADD("opaque-server-refusal", test_opaque_server_refusal);
  ADD("consent", test_consent);
  ADD("drop-and-paste", test_drop_and_paste);
  ADD("card-download-and-save", test_card_download_and_save);
  ADD("card-cancel-and-errors", test_card_cancel_and_errors);
  ADD("preferences", test_preferences);
  ADD("group-delegate", test_group_delegate);
  ADD("group-first-use-suggested-send", test_group_first_use_suggested_send);
  ADD("group-policy-overrides-selected-server", test_group_policy_overrides_selected_server);
#if GROUNDHOG_TEST_MLS_FILES
  ADD("mls-delegate-attach", test_mls_delegate_attach);
#endif
#undef ADD
  int status = g_test_run();
  stack_keys_clear();
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
  gh_test_remove_tree(xdg_root);
  return status;
}
