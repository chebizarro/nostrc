#include "gh-attachments.h"

#include "gh-net-http.h"
#include "gh-net-session.h"
#include "gh-store-blossom.h"
#include "gh-store-media.h"

#include <glib/gi18n.h>
#include <string.h>

/* One transfer of the current store and its download in flight, if any. */
typedef struct {
  GhAttachmentTransfer *transfer;
  GCancellable *cancellable; /* while DOWNLOADING: this download's */
  guint64 ready_seq;         /* when it became READY (the held-bytes order) */
  guint64 cost;              /* READY: its plaintext and, for a photo, its decoded pixels */
} Entry;

struct _GhAttachments {
  GObject parent_instance;
  GSettings *settings;
  GhNetHttp *http;
  GhBlossomSignAsyncFunc sign_async;
  GhBlossomSignFinishFunc sign_finish;
  gpointer sign_data;
  gboolean allow_private_hosts; /* tests only */
  guint64 held_max;             /* GH_ATTACHMENTS_HELD_MAX, lower in tests */

  GhStore *store;           /* borrowed: the account's open store */
  GhBlossomClient *client;  /* its account's */
  guint64 generation;       /* moves at every store change */
  GHashTable *entries;      /* rumor id -> Entry */
  guint64 ready_seq;
  guint downloads_started;
};

enum { SIGNAL_RESET, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAttachments, gh_attachments, G_TYPE_OBJECT)

static void
entry_free(gpointer data)
{
  Entry *entry = data;
  if (entry->cancellable)
    g_cancellable_cancel(entry->cancellable);
  g_clear_object(&entry->cancellable);
  g_object_unref(entry->transfer);
  g_free(entry);
}

static Entry *
entry_of(GhAttachments *self, GhAttachmentTransfer *transfer)
{
  Entry *entry = g_hash_table_lookup(self->entries, gh_attachment_transfer_get_rumor_id(transfer));
  return entry && entry->transfer == transfer ? entry : NULL;
}

/* Every download stops and every transfer goes back to IDLE, its plaintext
 * dropped (wiped) now; with forget the table lets go of them too (a card
 * still bound to one shows Download, and asks again). */
static void
reset_transfers(GhAttachments *self, gboolean forget)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->entries);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Entry *entry = value;
    if (entry->cancellable)
      g_cancellable_cancel(entry->cancellable);
    g_clear_object(&entry->cancellable);
    gh_attachment_transfer_reset(entry->transfer);
  }
  if (forget)
    g_hash_table_remove_all(self->entries);
}

/* ---- errors ------------------------------------------------------------------------ */

static gboolean
network_error(const GError *error)
{
  if (error->domain == G_RESOLVER_ERROR)
    return TRUE;
  if (error->domain == G_TLS_ERROR)
    return TRUE;
  if (error->domain != G_IO_ERROR)
    return FALSE;
  switch (error->code) {
  case G_IO_ERROR_CONNECTION_REFUSED:
  case G_IO_ERROR_HOST_NOT_FOUND:
  case G_IO_ERROR_HOST_UNREACHABLE:
  case G_IO_ERROR_NETWORK_UNREACHABLE:
  case G_IO_ERROR_TIMED_OUT:
  case G_IO_ERROR_BROKEN_PIPE:
  case G_IO_ERROR_NOT_CONNECTED:
  case G_IO_ERROR_PROXY_FAILED:
  case G_IO_ERROR_PROXY_AUTH_FAILED:
  case G_IO_ERROR_PROXY_NEED_AUTH:
  case G_IO_ERROR_PROXY_NOT_ALLOWED:
  case G_IO_ERROR_PARTIAL_INPUT:
    return TRUE;
  default:
    return FALSE;
  }
}

static gboolean
proxy_error(const GError *error)
{
  return error->domain == G_IO_ERROR &&
         (error->code == G_IO_ERROR_PROXY_FAILED || error->code == G_IO_ERROR_PROXY_AUTH_FAILED ||
          error->code == G_IO_ERROR_PROXY_NEED_AUTH || error->code == G_IO_ERROR_PROXY_NOT_ALLOWED);
}

gchar *
gh_attachments_describe_error(const GError *error, GhAttachmentsDirection direction,
                              gboolean tor, gboolean tor_unreachable, const gchar *host,
                              gboolean *out_can_retry)
{
  g_return_val_if_fail(error != NULL, NULL);
  gboolean download = direction == GH_ATTACHMENTS_DOWNLOAD;
  gboolean retry = TRUE;
  gchar *text = NULL;
  const gchar *where = host && *host ? host : NULL;
  if (g_error_matches(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED)) {
    retry = FALSE;
    text = g_strdup(_("This file was changed or damaged, so Groundhog didn't open it."));
  } else if (g_error_matches(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_TOO_LARGE) ||
             g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE) ||
             g_error_matches(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE) ||
             (error->domain == GH_NET_HTTP_ERROR && error->code == 413)) {
    retry = FALSE;
    gulong mb = GH_BLOSSOM_MAX_FILE_SIZE / (1024 * 1024);
    text = download
      ? g_strdup_printf(_("This file is larger than Groundhog accepts (%lu MB), so it wasn't "
                          "downloaded."), mb)
      : g_strdup_printf(_("Files can be at most %lu MB."), mb);
  } else if (g_error_matches(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_EMPTY)) {
    retry = FALSE;
    text = g_strdup(_("This file is empty."));
  } else if (!download && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)) {
    retry = FALSE;
    text = g_strdup(_("This image is damaged, so Groundhog won't send it: its hidden details "
                      "couldn't be removed."));
  } else if (g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER)) {
    text = g_strdup(_("Choose an attachment server to send files."));
  } else if (g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED)) {
    text = where ? g_strdup_printf(_("%s refused the upload, even from your account."), where)
                 : g_strdup(_("The server refused the upload, even from your account."));
  } else if (g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER)) {
    text = g_strdup(_("Nostr Signer didn't sign the upload, so nothing was sent."));
  } else if (g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_BAD_ANSWER)) {
    text = g_strdup(_("The server answered with a different file, so nothing was sent."));
  } else if (g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_REFUSED)) {
    /* Its message is "<host>: <the server's reason>", printable and cut. */
    text = g_strdup_printf(_("The server refused the file (%s)."), error->message);
  } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
    /* The download address policy, or a .onion outside Tor mode. */
    retry = FALSE;
    gboolean onion = where && g_str_has_suffix(where, ".onion");
    if (onion)
      text = g_strdup(download ? _("This file is on a Tor onion service. Choose Tor in Network "
                                   "settings to download it.")
                               : _("This server is a Tor onion service. Choose Tor in Network "
                                   "settings to use it."));
    else
      text = g_strdup(download ? _("This file's address isn't a public attachment server, so "
                                   "Groundhog won't download it.")
                               : _("Groundhog can't reach this server in the current network "
                                   "setting."));
  } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
    retry = FALSE;
    text = g_strdup(download ? _("This file's address can't be downloaded safely.")
                             : _("This server's address can't be used: only https:// "
                                 "addresses are."));
  } else if (download && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
                          (error->domain == GH_NET_HTTP_ERROR &&
                           (error->code == 404 || error->code == 410)))) {
    retry = FALSE;
    text = g_strdup(_("The server no longer has this file."));
  } else if (download && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED)) {
    /* GhNetHttp: another HTTP status, or a redirect (never followed). */
    text = g_strdup(_("The server that holds this file refused the download."));
  } else if (error->domain == GH_NET_HTTP_ERROR) {
    text = download ? g_strdup_printf(_("The server refused the download (HTTP %d)."), error->code)
                    : g_strdup_printf(_("The server refused the file (HTTP %d)."), error->code);
  } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED)) {
    text = g_strdup(_("The network setting changed before the server answered."));
  } else if (tor && (tor_unreachable || proxy_error(error) ||
                     g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED))) {
    /* Tor mode never connects directly (charter §4.2): nothing went out. */
    text = g_strdup(download ? _("Can't reach Tor, so nothing was downloaded. Groundhog doesn't "
                                 "connect without it.")
                             : _("Can't reach Tor, so nothing was sent. Groundhog doesn't "
                                 "connect without it."));
  } else if (network_error(error)) {
    if (download)
      text = tor ? g_strdup(_("Can't reach the server that holds this file through Tor."))
                 : g_strdup(_("Can't reach the server that holds this file."));
    else if (where)
      text = tor ? g_strdup_printf(_("Can't reach %s through Tor, so nothing was sent."), where)
                 : g_strdup_printf(_("Can't reach %s, so nothing was sent."), where);
    else
      text = g_strdup(_("Can't reach the attachment server, so nothing was sent."));
  } else {
    text = g_strdup(download ? _("The file couldn't be downloaded.")
                             : _("The file couldn't be sent."));
  }
  if (out_can_retry)
    *out_can_retry = retry;
  return text;
}

gboolean
gh_attachments_get_tor(GhAttachments *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), FALSE);
  if (!self->settings)
    return FALSE;
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(self->settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key(schema, "network-mode"))
    return FALSE;
  g_autofree gchar *mode = g_settings_get_string(self->settings, "network-mode");
  return g_str_equal(mode, "tor");
}

gchar *
gh_attachments_describe(GhAttachments *self, const GError *error,
                        GhAttachmentsDirection direction, const gchar *host,
                        gboolean *out_can_retry)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), NULL);
  gboolean tor = gh_attachments_get_tor(self);
  g_autoptr(GhNetSession) session = tor ? gh_net_session_dup_default() : NULL;
  gboolean unreachable = session &&
                         gh_net_session_get_tor_state(session) == GH_NET_TOR_UNREACHABLE;
  return gh_attachments_describe_error(error, direction, tor, unreachable, host, out_can_retry);
}

/* ---- the store and its client ------------------------------------------------------- */

void
gh_attachments_set_store(GhAttachments *self, GhStore *store)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  if (store == self->store)
    return;
  /* The previous store's downloads are cancelled before it closes. */
  reset_transfers(self, TRUE);
  g_clear_object(&self->client);
  self->store = store;
  self->generation++;
  if (store) {
    self->client = gh_blossom_client_new(self->settings, self->http);
    if (self->allow_private_hosts)
      gh_blossom_client_set_allow_private_hosts(self->client, TRUE);
    const gchar *account = gh_store_get_account_pubkey(store);
    if (self->sign_async && account)
      gh_blossom_client_set_account_signer(self->client, account, self->sign_async,
                                           self->sign_finish, self->sign_data, NULL);
    /* After the signer: setting it clears every consent. */
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) consents = gh_store_blossom_dup_consents(store, &error);
    if (!consents)
      g_message("Groundhog could not read the attachment server consents: %s", error->message);
    for (guint i = 0; consents && consents[i]; i++)
      gh_blossom_client_set_account_consent(self->client, consents[i], TRUE);
  }
  g_signal_emit(self, signals[SIGNAL_RESET], 0);
}

GhStore *
gh_attachments_get_store(GhAttachments *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), NULL);
  return self->store;
}

GhBlossomClient *
gh_attachments_get_client(GhAttachments *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), NULL);
  return self->client;
}

void
gh_attachments_set_held_max(GhAttachments *self, guint64 held_max)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  g_return_if_fail(held_max > 0 && held_max <= GH_ATTACHMENTS_HELD_MAX);
  self->held_max = held_max;
}

void
gh_attachments_set_allow_private_hosts(GhAttachments *self, gboolean allow)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  self->allow_private_hosts = !!allow;
  if (self->client)
    gh_blossom_client_set_allow_private_hosts(self->client, self->allow_private_hosts);
}

/* ---- received files -------------------------------------------------------------- */

GhAttachmentTransfer *
gh_attachments_lookup(GhAttachments *self, GhMessage *message)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), NULL);
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  if (!self->store)
    return NULL;
  const gchar *rumor_id = gh_message_get_rumor_id(message);
  Entry *entry = g_hash_table_lookup(self->entries, rumor_id);
  if (entry)
    return entry->transfer;
  g_autoptr(GhNip17File) file = gh_message_dup_file(message);
  if (!file)
    return NULL;
  entry = g_new0(Entry, 1);
  entry->transfer = gh_attachment_transfer_new(rumor_id, file);
  g_hash_table_insert(self->entries, g_strdup(rumor_id), entry);
  return entry->transfer;
}

/* Lets go of the least recently finished READY transfers (not keep) until
 * at most the bound is held: their plaintext and, for photos, the pixels a
 * card decodes from it (4 bytes each), which a large photo makes far more. */
static void
bound_held(GhAttachments *self, Entry *keep)
{
  for (;;) {
    guint64 held = 0;
    Entry *oldest = NULL;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, self->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Entry *entry = value;
      if (!gh_attachment_transfer_get_plaintext(entry->transfer))
        continue;
      held += entry->cost;
      if (entry != keep && (!oldest || entry->ready_seq < oldest->ready_seq))
        oldest = entry;
    }
    if (held <= self->held_max || !oldest)
      return;
    gh_attachment_transfer_reset(oldest->transfer);
  }
}

static void
ready(GhAttachments *self, Entry *entry, GBytes *plaintext, gboolean from_cache)
{
  guint width = 0, height = 0;
  gboolean previewable = gh_attachment_check_preview(plaintext, NULL, &width, &height, NULL);
  entry->cost = g_bytes_get_size(plaintext) + (previewable ? (guint64)width * height * 4 : 0);
  entry->ready_seq = ++self->ready_seq;
  gh_attachment_transfer_succeed(entry->transfer, plaintext, previewable, from_cache);
  bound_held(self, entry);
}

typedef struct {
  GhAttachments *self;
  GhAttachmentTransfer *transfer;
  GCancellable *cancellable;
  guint64 generation;
} DownloadOp;

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer data)
{
  DownloadOp *op = data;
  GhAttachments *self = op->self;
  (void)source;
  gboolean cached = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) plaintext = gh_attachment_download_finish(result, &cached, &error);
  /* Only the download this transfer is waiting for: not one the user
   * cancelled (and maybe started again), nor one of another store. */
  Entry *entry = op->generation == self->generation ? entry_of(self, op->transfer) : NULL;
  if (entry && entry->cancellable == op->cancellable) {
    g_clear_object(&entry->cancellable);
    if (plaintext) {
      ready(self, entry, plaintext, cached);
    } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      gh_attachment_transfer_reset(entry->transfer);
    } else {
      gboolean retry = TRUE;
      g_autoptr(GUri) uri = g_uri_parse(gh_attachment_transfer_get_file(entry->transfer)->url,
                                        G_URI_FLAGS_ENCODED, NULL);
      g_autofree gchar *text = gh_attachments_describe(self, error, GH_ATTACHMENTS_DOWNLOAD,
                                                       uri ? g_uri_get_host(uri) : NULL, &retry);
      /* The error names the server and may quote it: debug only (PD-10). */
      g_debug("Attachment: a download failed: %s", error->message);
      gh_attachment_transfer_fail(entry->transfer, text, retry);
    }
  }
  g_object_unref(op->cancellable);
  g_object_unref(op->transfer);
  g_object_unref(op->self);
  g_free(op);
}

void
gh_attachments_download(GhAttachments *self, GhAttachmentTransfer *transfer)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(transfer));
  Entry *entry = entry_of(self, transfer);
  GhAttachmentState state = gh_attachment_transfer_get_state(transfer);
  if (!entry || !self->client ||
      (state != GH_ATTACHMENT_STATE_IDLE && state != GH_ATTACHMENT_STATE_FAILED))
    return;
  entry->cancellable = g_cancellable_new();
  gh_attachment_transfer_start(transfer);
  self->downloads_started++;
  DownloadOp *op = g_new0(DownloadOp, 1);
  op->self = g_object_ref(self);
  op->transfer = g_object_ref(transfer);
  op->cancellable = g_object_ref(entry->cancellable);
  op->generation = self->generation;
  /* A read-only store serves what it has but keeps nothing new. */
  gh_attachment_download_async(self->client, self->store, gh_attachment_transfer_get_file(transfer),
                               entry->cancellable, on_downloaded, op);
}

void
gh_attachments_cancel(GhAttachments *self, GhAttachmentTransfer *transfer)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(transfer));
  Entry *entry = entry_of(self, transfer);
  if (!entry || !entry->cancellable)
    return;
  g_cancellable_cancel(entry->cancellable);
  g_clear_object(&entry->cancellable);
  gh_attachment_transfer_reset(transfer);
}

guint
gh_attachments_get_downloads_started(GhAttachments *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), 0);
  return self->downloads_started;
}

/* ---- sent files ---------------------------------------------------------------------- */

typedef struct {
  guint64 generation;
  gchar *server;
} UploadOp;

static void
upload_op_free(gpointer data)
{
  UploadOp *op = data;
  g_free(op->server);
  g_free(op);
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GhAttachments *self = g_task_get_source_object(task);
  UploadOp *op = g_task_get_task_data(task);
  (void)source;
  GError *error = NULL;
  GhNip17File *file = gh_attachment_upload_finish_full(result, &op->server, &error);
  if (file && op->generation != self->generation) {
    /* Uploaded for an account that is no longer open: never sent. */
    gh_nip17_file_free(file);
    file = NULL;
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "The account changed before the file was sent");
  }
  if (file)
    g_task_return_pointer(task, file, (GDestroyNotify)gh_nip17_file_free);
  else
    g_task_return_error(task, error);
  g_object_unref(task);
}

void
gh_attachments_upload_async(GhAttachments *self, GBytes *file, const gchar *mime,
                            GCancellable *cancellable, GAsyncReadyCallback callback,
                            gpointer user_data)
{
  gh_attachments_upload_on_servers_async(self, NULL, file, mime, cancellable, callback,
                                          user_data);
}

void
gh_attachments_upload_on_servers_async(GhAttachments *self, const gchar *const *servers,
                                       GBytes *file, const gchar *mime,
                                       GCancellable *cancellable, GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  g_return_if_fail(file != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_attachments_upload_async);
  UploadOp *op = g_new0(UploadOp, 1);
  op->generation = self->generation;
  g_task_set_task_data(task, op, upload_op_free);
  if (!self->client) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "Attachments need the account's message storage, which isn't open");
    g_object_unref(task);
    return;
  }
  gh_attachment_upload_on_servers_async(self->client, servers, file, mime, cancellable,
                                         on_uploaded, task);
}

GhNip17File *
gh_attachments_upload_finish(GhAttachments *self, GAsyncResult *result, gchar **out_server,
                             GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  if (out_server) {
    UploadOp *op = g_task_get_task_data(G_TASK(result));
    *out_server = op ? g_strdup(op->server) : NULL;
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}

static gboolean
is_plaintext_of(const GhNip17File *file, GBytes *plaintext)
{
  if (!file->ox[0])
    return FALSE;
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(plaintext, &size);
  g_autofree gchar *ox = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
  return ox && g_ascii_strcasecmp(ox, file->ox) == 0;
}

void
gh_attachments_remember_sent(GhAttachments *self, const gchar *rumor_id,
                             const GhNip17File *file, GBytes *plaintext)
{
  g_return_if_fail(GH_IS_ATTACHMENTS(self));
  g_return_if_fail(rumor_id != NULL && file != NULL && plaintext != NULL);
  if (!self->store || !is_plaintext_of(file, plaintext))
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_store_media_put(self->store, file, file->file_type, plaintext, &error))
    g_debug("Attachment: the sent file was not kept in the encrypted cache: %s", error->message);
  Entry *entry = g_hash_table_lookup(self->entries, rumor_id);
  if (!entry) {
    entry = g_new0(Entry, 1);
    entry->transfer = gh_attachment_transfer_new(rumor_id, file);
    g_hash_table_insert(self->entries, g_strdup(rumor_id), entry);
  }
  if (gh_attachment_transfer_get_state(entry->transfer) != GH_ATTACHMENT_STATE_DOWNLOADING)
    ready(self, entry, plaintext, TRUE);
}

/* ---- upload consent ------------------------------------------------------------------- */

gboolean
gh_attachments_set_consent(GhAttachments *self, const gchar *server, gboolean consent,
                           GError **error)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), FALSE);
  g_autofree gchar *normalized = gh_blossom_client_normalize_server(server, NULL);
  if (!normalized) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not an attachment server address");
    return FALSE;
  }
  if (!self->store || !self->client) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "The account's message storage isn't open");
    return FALSE;
  }
  if (!gh_store_blossom_set_consent(self->store, normalized, consent,
                                    g_get_real_time() / G_USEC_PER_SEC, error))
    return FALSE;
  gh_blossom_client_set_account_consent(self->client, normalized, consent);
  return TRUE;
}

gboolean
gh_attachments_get_consent(GhAttachments *self, const gchar *server)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), FALSE);
  return self->client && server && gh_blossom_client_get_account_consent(self->client, server);
}

GStrv
gh_attachments_dup_consents(GhAttachments *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), NULL);
  g_autoptr(GError) error = NULL;
  GStrv consents = self->store ? gh_store_blossom_dup_consents(self->store, &error) : NULL;
  if (!consents && error)
    g_message("Groundhog could not read the attachment server consents: %s", error->message);
  return consents ? consents : g_new0(gchar *, 1);
}

/* ---- the cache ----------------------------------------------------------------------- */

gboolean
gh_attachments_get_cache_size(GhAttachments *self, gint64 *out_bytes, GError **error)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), FALSE);
  g_return_val_if_fail(out_bytes != NULL, FALSE);
  *out_bytes = 0;
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "The account's message storage isn't open");
    return FALSE;
  }
  return gh_store_media_get_total(self->store, out_bytes, error);
}

gboolean
gh_attachments_clear_cache(GhAttachments *self, GError **error)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(self), FALSE);
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "The account's message storage isn't open");
    return FALSE;
  }
  /* In-flight downloads would put their file back: they stop first. */
  reset_transfers(self, FALSE);
  return gh_store_media_prune(self->store, 0, error);
}

/* ---- GObject ---------------------------------------------------------------------------- */

GhAttachments *
gh_attachments_new(const GhAttachmentsConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_NET_HTTP(config->http), NULL);
  g_return_val_if_fail(!config->settings || G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(!config->sign_async || config->sign_finish, NULL);
  GhAttachments *self = g_object_new(GH_TYPE_ATTACHMENTS, NULL);
  self->settings = config->settings ? g_object_ref(config->settings) : NULL;
  self->http = g_object_ref(config->http);
  self->sign_async = config->sign_async;
  self->sign_finish = config->sign_finish;
  self->sign_data = config->sign_data;
  return self;
}

static void
gh_attachments_dispose(GObject *object)
{
  GhAttachments *self = GH_ATTACHMENTS(object);
  if (self->entries)
    reset_transfers(self, TRUE);
  g_clear_object(&self->client);
  self->store = NULL;
  self->generation++;
  G_OBJECT_CLASS(gh_attachments_parent_class)->dispose(object);
}

static void
gh_attachments_finalize(GObject *object)
{
  GhAttachments *self = GH_ATTACHMENTS(object);
  g_clear_pointer(&self->entries, g_hash_table_unref);
  g_clear_object(&self->settings);
  g_clear_object(&self->http);
  G_OBJECT_CLASS(gh_attachments_parent_class)->finalize(object);
}

static void
gh_attachments_class_init(GhAttachmentsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_attachments_dispose;
  object_class->finalize = gh_attachments_finalize;
  /* After every store change: every transfer was let go. */
  signals[SIGNAL_RESET] = g_signal_new("reset", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                       NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_attachments_init(GhAttachments *self)
{
  self->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
  self->held_max = GH_ATTACHMENTS_HELD_MAX;
}
