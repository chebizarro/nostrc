#include "gh-mls-attachments.h"

#include "gh-attachment.h"
#include "gh-store-media.h"

#include <glib/gi18n.h>
#include <string.h>

/* One file of an MLS message and its download in flight, if any. */
typedef struct {
  GhAttachmentTransfer *transfer;
  GhMlsAttachment *attachment; /* NULL: it can't be opened (no source epoch) */
  gchar *group_id;             /* lowercase hex */
  gchar *file_id;              /* the cache identity, or NULL */
  GCancellable *cancellable;   /* while DOWNLOADING */
  guint64 ready_seq;
  guint64 cost;
} Entry;

struct _GhMlsAttachments {
  GObject parent_instance;
  GhAttachments *attachments;
  GhMlsService *service;   /* borrowed: the last one seen */
  GhMlsAttachmentsServiceFunc service_func;   /* nullable: asked at every use */
  gpointer service_data;
  guint64 generation;
  GHashTable *entries;     /* "<rumor id>/<index>" -> Entry */
  guint64 ready_seq;
  guint downloads_started;
};

G_DEFINE_FINAL_TYPE(GhMlsAttachments, gh_mls_attachments, G_TYPE_OBJECT)

static void
entry_free(gpointer data)
{
  Entry *entry = data;
  if (entry->cancellable)
    g_cancellable_cancel(entry->cancellable);
  g_clear_object(&entry->cancellable);
  g_object_unref(entry->transfer);
  g_clear_pointer(&entry->attachment, gh_mls_attachment_free);
  g_free(entry->group_id);
  g_free(entry->file_id);
  g_free(entry);
}

static Entry *
entry_of(GhMlsAttachments *self, GhAttachmentTransfer *transfer)
{
  Entry *entry = g_hash_table_lookup(self->entries, gh_attachment_transfer_get_rumor_id(transfer));
  return entry && entry->transfer == transfer ? entry : NULL;
}

/* Every download stops; with forget, every transfer is let go too. */
static void
reset(GhMlsAttachments *self, gboolean forget)
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
  self->generation++;
}

static GhStore *
store_of(GhMlsAttachments *self)
{
  return gh_attachments_get_store(self->attachments);
}

static GhBlossomClient *
client_of(GhMlsAttachments *self)
{
  return gh_attachments_get_client(self->attachments);
}

static void
on_attachments_reset(GhMlsAttachments *self)
{
  reset(self, TRUE);
}

static void on_service_gone(gpointer data, GObject *where);

/* The service is watched, not held: when it goes (a store closing, an
 * account switch) every transfer of it goes too, even if a later service
 * happens to be allocated at the same address. */
void
gh_mls_attachments_set_service(GhMlsAttachments *self, GhMlsService *service)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  g_return_if_fail(!service || GH_IS_MLS_SERVICE(service));
  if (self->service == service)
    return;
  reset(self, TRUE);
  if (self->service)
    g_object_weak_unref(G_OBJECT(self->service), on_service_gone, self);
  self->service = service;
  if (service)
    g_object_weak_ref(G_OBJECT(service), on_service_gone, self);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhMlsAttachments *self = data;
  (void)where;
  self->service = NULL;
  reset(self, TRUE);
}

void
gh_mls_attachments_set_service_func(GhMlsAttachments *self, GhMlsAttachmentsServiceFunc func,
                                    gpointer data)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  self->service_func = func;
  self->service_data = data;
}

/* The open store's service now: a new one (an account switch) lets go of
 * every transfer of the previous one first. */
static GhMlsService *
service_of(GhMlsAttachments *self)
{
  if (self->service_func)
    gh_mls_attachments_set_service(self, self->service_func(self->service_data));
  return self->service;
}

GhMlsService *
gh_mls_attachments_get_service(GhMlsAttachments *self)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), NULL);
  return service_of(self);
}

gboolean
gh_mls_attachments_get_tor(GhMlsAttachments *self)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), FALSE);
  return gh_attachments_get_tor(self->attachments);
}

/* ---- errors ------------------------------------------------------------------------ */

gchar *
gh_mls_attachments_describe(GhMlsAttachments *self, const GError *error,
                            GhAttachmentsDirection direction, const gchar *host,
                            gboolean *out_can_retry)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self) && error != NULL, NULL);
  gboolean retry = FALSE;
  const gchar *text = NULL;
  if (error->domain == GH_MLS_MEDIA_ERROR) {
    switch ((GhMlsMediaError)error->code) {
    case GH_MLS_MEDIA_ERROR_DAMAGED:
      text = _("This file was changed or damaged, so Groundhog didn't open it.");
      break;
    case GH_MLS_MEDIA_ERROR_NO_KEY:
      text = _("This file can't be opened any more: this device no longer has the key of the "
               "group's state it was sent in.");
      break;
    case GH_MLS_MEDIA_ERROR_UNAVAILABLE:
      retry = TRUE;
      text = _("None of the servers that could hold this file has it.");
      break;
    case GH_MLS_MEDIA_ERROR_NOT_LOCAL:
      text = _("Only files on this device can be sent");
      break;
    case GH_MLS_MEDIA_ERROR_TOO_LARGE:
      break;   /* the attachments' words below */
    case GH_MLS_MEDIA_ERROR_UNSUPPORTED:
      text = direction == GH_ATTACHMENTS_DOWNLOAD
               ? _("This file uses a kind of encryption Groundhog can't open.")
               : _("A group picture must be a JPEG or PNG photo.");
      break;
    case GH_MLS_MEDIA_ERROR_INVALID:
      text = _("This file's reference isn't valid, so it can't be opened.");
      break;
    case GH_MLS_MEDIA_ERROR_EPOCH_CHANGED:
    case GH_MLS_MEDIA_ERROR_FAILED:
    default:
      retry = TRUE;
      text = direction == GH_ATTACHMENTS_DOWNLOAD ? _("The file couldn't be opened.")
                                                  : _("The file couldn't be sent.");
    }
  } else if (error->domain == GH_MLS_SERVICE_ERROR) {
    switch ((GhMlsServiceError)error->code) {
    case GH_MLS_SERVICE_ERROR_UNSUPPORTED:
      text = _("This group doesn't accept files or pictures Groundhog can send.");
      break;
    case GH_MLS_SERVICE_ERROR_EPOCH_CHANGED:
      retry = TRUE;
      text = _("The group kept changing while the file was uploading, so it wasn't sent.");
      break;
    case GH_MLS_SERVICE_ERROR_SERVERS_CHANGED:
      retry = TRUE;
      text = _("The group's servers changed, so nothing was uploaded. Check where it goes and "
               "try again.");
      break;
    case GH_MLS_SERVICE_ERROR_NOT_ADMIN:
      text = _("Only a group admin can change the group's picture.");
      break;
    case GH_MLS_SERVICE_ERROR_BUSY:
      retry = TRUE;
      text = _("Another change of this group is still being sent. Try again in a moment.");
      break;
    default:
      retry = TRUE;
      text = _("The group couldn't be reached to send this.");
    }
  } else if (direction == GH_ATTACHMENTS_UPLOAD &&
             g_error_matches(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER) &&
             host && g_str_equal(host, "group")) {
    text = _("This group doesn't name a server for its pictures, so one can't be set here.");
  }
  if (text) {
    if (out_can_retry)
      *out_can_retry = retry;
    return g_strdup(text);
  }
  return gh_attachments_describe(self->attachments, error, direction, host, out_can_retry);
}

/* ---- received files ------------------------------------------------------------ */

static gchar *
entry_key(GhMessage *message, guint index)
{
  return g_strdup_printf("%s/%s/%u", gh_message_get_group_id(message),
                         gh_message_get_rumor_id(message), index);
}

GhAttachmentTransfer *
gh_mls_attachments_lookup(GhMlsAttachments *self, GhMessage *message, guint index)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), NULL);
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  if (!service_of(self) || !store_of(self) || !gh_message_is_mls(message))
    return NULL;
  const GhMessageAttachment *described = gh_message_get_attachment(message, index);
  if (!described || !gh_mls_service_lookup(self->service, gh_message_get_group_id(message)))
    return NULL;
  g_autofree gchar *key = entry_key(message, index);
  Entry *entry = g_hash_table_lookup(self->entries, key);
  if (entry)
    return entry->transfer;
  entry = g_new0(Entry, 1);
  entry->transfer = gh_attachment_transfer_new_described(key, described->media_type,
                                                         described->filename);
  entry->group_id = g_strdup(gh_message_get_group_id(message));
  guint64 epoch = 0;
  if (described->file_id && gh_message_get_mls_epoch(message, &epoch)) {
    /* The reference again, matched by identity (never trusting the order). */
    g_autoptr(GPtrArray) parsed =
      gh_mls_attachments_from_inner_event(gh_message_get_rumor_json(message), epoch, NULL);
    for (guint i = 0; i < parsed->len && !entry->attachment; i++) {
      g_autofree gchar *id = gh_mls_attachment_dup_file_id(g_ptr_array_index(parsed, i),
                                                           entry->group_id);
      if (g_strcmp0(id, described->file_id) == 0)
        entry->attachment = gh_mls_attachment_copy(g_ptr_array_index(parsed, i));
    }
    entry->file_id = g_strdup(described->file_id);
  }
  if (!entry->attachment)
    gh_attachment_transfer_fail(entry->transfer,
                                _("This file arrived before Groundhog could open files in "
                                  "encrypted groups, so it can't be opened here."), FALSE);
  g_hash_table_insert(self->entries, g_strdup(key), entry);
  return entry->transfer;
}

/* As GhAttachments: the least recently finished READY transfers are let go
 * beyond the bound (the newest kept). */
static void
bound_held(GhMlsAttachments *self, Entry *keep)
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
    if (held <= GH_ATTACHMENTS_HELD_MAX || !oldest)
      return;
    gh_attachment_transfer_reset(oldest->transfer);
  }
}

static void
ready(GhMlsAttachments *self, Entry *entry, GBytes *plaintext, gboolean from_cache)
{
  guint width = 0, height = 0;
  gboolean previewable = gh_attachment_check_preview(plaintext, NULL, &width, &height, NULL);
  entry->cost = g_bytes_get_size(plaintext) + (previewable ? (guint64)width * height * 4 : 0);
  entry->ready_seq = ++self->ready_seq;
  gh_attachment_transfer_succeed(entry->transfer, plaintext, previewable, from_cache);
  bound_held(self, entry);
}

typedef struct {
  GhMlsAttachments *self;
  GhAttachmentTransfer *transfer;
  GCancellable *cancellable;
  guint64 generation;
  gchar *host;   /* the first server tried, for the error words */
} DownloadOp;

static void
download_op_free(DownloadOp *op)
{
  g_object_unref(op->cancellable);
  g_object_unref(op->transfer);
  g_object_unref(op->self);
  g_free(op->host);
  g_free(op);
}

static void
on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  DownloadOp *op = data;
  GhMlsAttachments *self = op->self;
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) ciphertext = gh_mls_media_fetch_finish(result, &error);
  Entry *entry = op->generation == self->generation ? entry_of(self, op->transfer) : NULL;
  if (!entry || entry->cancellable != op->cancellable) {
    download_op_free(op);
    return;
  }
  g_clear_object(&entry->cancellable);
  g_autoptr(GBytes) plaintext = NULL;
  if (ciphertext && self->service)
    plaintext = gh_mls_media_open(gh_mls_service_get_marmot(self->service), entry->group_id,
                                  entry->attachment, ciphertext, &error);
  if (plaintext) {
    GhStore *store = store_of(self);
    g_autoptr(GError) put_error = NULL;
    if (store && entry->file_id && !gh_store_is_read_only(store) &&
        !gh_store_media_put_id(store, entry->file_id,
                               gh_attachment_transfer_get_media_type(entry->transfer), plaintext,
                               &put_error))
      g_debug("Attachment: a group file was not kept in the encrypted cache: %s",
              put_error->message);
    ready(self, entry, plaintext, FALSE);
  } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    gh_attachment_transfer_reset(entry->transfer);
  } else {
    gboolean retry = TRUE;
    /* The error may name a server or quote it: debug only (PD-10). */
    g_debug("Attachment: a group file download failed: %s",
            error ? error->message : "no service");
    g_autofree gchar *text = error ? gh_mls_attachments_describe(self, error,
                                                                 GH_ATTACHMENTS_DOWNLOAD,
                                                                 op->host, &retry)
                                   : g_strdup(_("The file couldn't be opened."));
    gh_attachment_transfer_fail(entry->transfer, text, retry);
  }
  download_op_free(op);
}

static GStrv
fallback_urls(GhMlsAttachments *self, Entry *entry)
{
  GhMlsGroup *group = gh_mls_service_lookup(self->service, entry->group_id);
  MarmotGroupComponents components;
  GStrv urls = NULL;
  /* A legacy group has no media policy: the file's own locators only. */
  if (group && gh_mls_service_get_components(self->service, group, &components, NULL)) {
    g_autofree gchar *sha = gh_mls_attachment_dup_ciphertext_sha256(entry->attachment);
    guint8 hash[32];
    for (guint i = 0; i < 32; i++)
      hash[i] = (guint8)(g_ascii_xdigit_value(sha[2 * i]) << 4 |
                         g_ascii_xdigit_value(sha[2 * i + 1]));
    urls = gh_mls_media_fallback_urls(&components, hash);
    marmot_group_components_clear(&components);
  }
  return urls;
}

static gchar *
host_of_url(const gchar *url)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL) : NULL;
  return uri && g_uri_get_host(uri) ? g_strdup(g_uri_get_host(uri)) : NULL;
}

/* The hosts, in order and once each, of the URLs this client may contact
 * (public hosts only, as GhBlossomClient enforces for downloads). */
static GStrv
public_hosts(GhMlsAttachments *self, const gchar *const *urls)
{
  g_autoptr(GStrvBuilder) hosts = g_strv_builder_new();
  GhBlossomClient *client = client_of(self);
  g_auto(GStrv) usable = client ? gh_blossom_client_dup_public_servers(client, urls)
                                : g_new0(gchar *, 1);
  g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; usable[i]; i++) {
    gchar *host = host_of_url(usable[i]);
    gboolean dup = !host;
    for (guint k = 0; !dup && k < seen->len; k++)
      dup = g_str_equal(g_ptr_array_index(seen, k), host);
    if (dup) {
      g_free(host);
      continue;
    }
    g_strv_builder_add(hosts, host);
    g_ptr_array_add(seen, host);
  }
  return g_strv_builder_end(hosts);
}

gchar *
gh_mls_describe_hosts(const gchar *const *hosts)
{
  guint n = hosts ? g_strv_length((gchar **)hosts) : 0;
  if (n == 0)
    return NULL;
  if (n == 1)
    return g_strdup(hosts[0]);
  g_autofree gchar *head = g_strjoinv(", ", (gchar **)hosts);
  /* "a, b, c" -> "a, b or c" */
  gchar *last_comma = g_strrstr(head, ", ");
  *last_comma = '\0';
  /* TRANSLATORS: the last of several servers: "a.example, b.example or c.example". */
  return g_strdup_printf(_("%s or %s"), head, last_comma + 2);
}

gchar *
gh_mls_download_note(const gchar *const *hosts, gboolean tor)
{
  g_autofree gchar *where = gh_mls_describe_hosts(hosts);
  guint n = hosts ? g_strv_length((gchar **)hosts) : 0;
  if (!where)
    return g_strdup(_("No server this file names can be contacted, so it can't be downloaded"));
  if (n == 1)
    return tor ? g_strdup_printf(_("Downloads the encrypted file from %s through Tor"), where)
               : g_strdup_printf(_("Downloads the encrypted file from %s, which can see your IP "
                                   "address"), where);
  return tor ? g_strdup_printf(_("Downloads the encrypted file from the first of %s that has it, "
                                 "through Tor"), where)
             : g_strdup_printf(_("Downloads the encrypted file from the first of %s that has it; "
                                 "each server asked can see your IP address"), where);
}

gchar *
gh_mls_picture_upload_note(const gchar *const *hosts, gboolean tor)
{
  g_autofree gchar *where = gh_mls_describe_hosts(hosts);
  guint n = hosts ? g_strv_length((gchar **)hosts) : 0;
  if (!where)
    return NULL;
  if (n == 1)
    return tor ? g_strdup_printf(_("The picture is encrypted on this device, then uploaded to %s "
                                   "through Tor. Everyone in the group will see it."), where)
               : g_strdup_printf(_("The picture is encrypted on this device, then uploaded to %s, "
                                   "which can see your IP address. Everyone in the group will see "
                                   "it."), where);
  return tor ? g_strdup_printf(_("The picture is encrypted on this device, then uploaded to the "
                                 "first of %s that takes it, through Tor. Everyone in the group "
                                 "will see it."), where)
             : g_strdup_printf(_("The picture is encrypted on this device, then uploaded to the "
                                 "first of %s that takes it; each server asked can see your IP "
                                 "address. Everyone in the group will see it."), where);
}

void
gh_mls_attachments_download(GhMlsAttachments *self, GhAttachmentTransfer *transfer)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(transfer));
  Entry *entry = entry_of(self, transfer);
  GhAttachmentState state = gh_attachment_transfer_get_state(transfer);
  if (!entry || !entry->attachment || !service_of(self) || !client_of(self) ||
      (state != GH_ATTACHMENT_STATE_IDLE && state != GH_ATTACHMENT_STATE_FAILED))
    return;
  self->downloads_started++;
  gh_attachment_transfer_start(transfer);
  /* This very file from the encrypted cache: no network. */
  GhStore *store = store_of(self);
  if (store && entry->file_id) {
    g_autoptr(GBytes) cached = gh_store_media_get_id(store, entry->file_id, NULL, NULL);
    if (cached && g_bytes_get_size(cached) > 0) {
      ready(self, entry, cached, TRUE);
      return;
    }
  }
  entry->cancellable = g_cancellable_new();
  DownloadOp *op = g_new0(DownloadOp, 1);
  op->self = g_object_ref(self);
  op->transfer = g_object_ref(transfer);
  op->cancellable = g_object_ref(entry->cancellable);
  op->generation = self->generation;
  g_auto(GStrv) fallbacks = fallback_urls(self, entry);
  g_auto(GStrv) own = gh_mls_attachment_dup_blossom_urls(entry->attachment);
  op->host = host_of_url(own[0] ? own[0] : fallbacks ? fallbacks[0] : NULL);
  gh_mls_media_fetch_with_fallbacks_async(client_of(self), entry->attachment,
                                          (const gchar *const *)fallbacks, entry->cancellable,
                                          on_fetched, op);
}

void
gh_mls_attachments_cancel(GhMlsAttachments *self, GhAttachmentTransfer *transfer)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(transfer));
  Entry *entry = entry_of(self, transfer);
  if (!entry || !entry->cancellable)
    return;
  g_cancellable_cancel(entry->cancellable);
  g_clear_object(&entry->cancellable);
  gh_attachment_transfer_reset(transfer);
}

gchar *
gh_mls_attachments_download_note(GhMlsAttachments *self, GhAttachmentTransfer *transfer)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), NULL);
  Entry *entry = transfer ? entry_of(self, transfer) : NULL;
  if (!entry || !entry->attachment)
    return NULL;
  /* Every server Download may ask, in order: the sender's locators, then
   * the group's media servers (W25 review L3). */
  g_auto(GStrv) own = gh_mls_attachment_dup_blossom_urls(entry->attachment);
  g_auto(GStrv) fallbacks = service_of(self) ? fallback_urls(self, entry) : NULL;
  g_autoptr(GStrvBuilder) all = g_strv_builder_new();
  g_strv_builder_addv(all, (const char **)own);
  if (fallbacks)
    g_strv_builder_addv(all, (const char **)fallbacks);
  g_auto(GStrv) urls = g_strv_builder_end(all);
  g_auto(GStrv) hosts = public_hosts(self, (const gchar *const *)urls);
  return gh_mls_download_note((const gchar *const *)hosts,
                              gh_attachments_get_tor(self->attachments));
}

guint
gh_mls_attachments_get_downloads_started(GhMlsAttachments *self)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), 0);
  return self->downloads_started;
}

/* ---- sent files ------------------------------------------------------------------ */

gchar *
gh_mls_attachments_neutral_name(const gchar *mime, const gchar *name)
{
  if (g_strcmp0(mime, "image/jpeg") == 0)
    return g_strdup("photo.jpg");
  if (g_strcmp0(mime, "image/png") == 0)
    return g_strdup("photo.png");
  const gchar *dot = name ? strrchr(name, '.') : NULL;
  gsize n = dot ? strlen(dot + 1) : 0;
  gboolean ok = n >= 1 && n <= 8;
  for (gsize i = 0; ok && i < n; i++)
    ok = g_ascii_isalnum(dot[1 + i]);
  if (!ok)
    return g_strdup("file");
  g_autofree gchar *ext = g_ascii_strdown(dot + 1, -1);
  return g_strconcat("file.", ext, NULL);
}

typedef struct {
  guint64 generation;
  GhMlsGroup *group;
  GBytes *file;
  gchar *name;      /* neutral */
  gchar *mime;
  gchar *caption;
  GStrv selected_servers;
  GStrv servers;
  guint tries;
  GhMlsMediaSealed *sealed;
  gchar *server;
} SendOp;

static void
send_op_free(gpointer data)
{
  SendOp *op = data;
  g_clear_object(&op->group);
  g_bytes_unref(op->file);
  g_free(op->name);
  g_free(op->mime);
  g_free(op->caption);
  g_strfreev(op->selected_servers);
  g_strfreev(op->servers);
  g_clear_pointer(&op->sealed, gh_mls_media_sealed_free);
  g_free(op->server);
  g_free(op);
}

static void send_try(GTask *task);

/* A group's verified 0x800b endpoints override the sheet/account choice.
 * Re-read them for an epoch retry: a Commit may have changed the policy. */
static gboolean
refresh_send_servers(GhMlsAttachments *self, SendOp *op, GError **error)
{
  MarmotGroupComponents components;
  g_autoptr(GError) components_error = NULL;
  if (!gh_mls_service_get_components(self->service, op->group, &components,
                                     &components_error)) {
    if (!g_error_matches(components_error, GH_MLS_SERVICE_ERROR,
                         GH_MLS_SERVICE_ERROR_UNSUPPORTED)) {
      g_propagate_error(error, g_steal_pointer(&components_error));
      return FALSE;
    }
    g_clear_pointer(&op->servers, g_strfreev);
    op->servers = op->selected_servers ? g_strdupv(op->selected_servers) : NULL;
    return TRUE; /* An older-format group has no 0x800b policy. */
  }
  gboolean has_policy = components.has_media_policy;
  gboolean allowed = gh_mls_media_policy_allows_blossom(&components);
  g_auto(GStrv) named = has_policy ? gh_mls_media_dup_servers(&components) : NULL;
  marmot_group_components_clear(&components);
  if (!allowed) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED,
                        "The group's media policy doesn't allow Blossom servers");
    return FALSE;
  }
  GStrv next = has_policy
    ? gh_blossom_client_dup_public_servers(client_of(self), (const gchar *const *)named)
    : (op->selected_servers ? g_strdupv(op->selected_servers) : NULL);
  if (has_policy && !next[0]) {
    g_strfreev(next);
    g_set_error_literal(error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER,
                        "The group names no usable Blossom media server");
    return FALSE;
  }
  g_strfreev(op->servers);
  op->servers = next;
  return TRUE;
}

static gboolean
send_stale(GTask *task)
{
  GhMlsAttachments *self = g_task_get_source_object(task);
  SendOp *op = g_task_get_task_data(task);
  if (op->generation == self->generation && service_of(self))
    return FALSE;
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                          "The account changed before the file was sent");
  g_object_unref(task);
  return TRUE;
}

/* The sender's own card shows the file from the cache at once. */
static void
remember_sent(GhMlsAttachments *self, GhMessage *message, GBytes *plaintext)
{
  const GhMessageAttachment *a = gh_message_get_attachment(message, 0);
  GhStore *store = store_of(self);
  if (!a || !a->file_id || !store)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_store_media_put_id(store, a->file_id, a->media_type, plaintext, &error))
    g_debug("Attachment: the sent group file was not kept in the encrypted cache: %s",
            error->message);
  GhAttachmentTransfer *transfer = gh_mls_attachments_lookup(self, message, 0);
  Entry *entry = transfer ? entry_of(self, transfer) : NULL;
  if (entry && gh_attachment_transfer_get_state(transfer) != GH_ATTACHMENT_STATE_DOWNLOADING)
    ready(self, entry, plaintext, TRUE);
}

static void
on_send_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GhMlsAttachments *self = g_task_get_source_object(task);
  SendOp *op = g_task_get_task_data(task);
  (void)source;
  GError *error = NULL;
  g_clear_pointer(&op->server, g_free);
  g_autoptr(GhMlsAttachment) uploaded = gh_mls_media_upload_finish_full(result, &op->server,
                                                                        &error);
  if (send_stale(task)) {
    g_clear_error(&error);
    return;
  }
  if (!uploaded) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  g_auto(GStrv) imeta = gh_mls_attachment_dup_imeta(uploaded, &error);
  g_autoptr(GPtrArray) tags = g_ptr_array_new();
  if (imeta)
    g_ptr_array_add(tags, imeta);
  GhMessage *message = imeta ? gh_mls_service_send_with_imeta(
                                 self->service, op->group, op->caption, tags,
                                 gh_mls_attachment_get_source_epoch(uploaded), &error)
                             : NULL;
  if (!message && g_error_matches(error, GH_MLS_SERVICE_ERROR,
                                  GH_MLS_SERVICE_ERROR_EPOCH_CHANGED) &&
      op->tries < GH_MLS_ATTACHMENTS_SEND_TRIES) {
    /* A Commit came during the upload: the file is bound to the old epoch,
     * so it is sealed and uploaded again; nothing stale was sent. */
    g_clear_error(&error);
    send_try(task);
    return;
  }
  if (!message) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  remember_sent(self, message, op->sealed->plaintext);
  g_task_return_pointer(task, message, g_object_unref);
  g_object_unref(task);
}

static void
send_try(GTask *task)
{
  GhMlsAttachments *self = g_task_get_source_object(task);
  SendOp *op = g_task_get_task_data(task);
  if (send_stale(task))
    return;
  GError *error = NULL;
  if (!refresh_send_servers(self, op, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  op->tries++;
  g_clear_pointer(&op->sealed, gh_mls_media_sealed_free);
  op->sealed = gh_mls_media_seal(gh_mls_service_get_marmot(self->service),
                                 gh_mls_group_get_group_id(op->group), op->file, op->mime,
                                 op->name, GH_BLOSSOM_MAX_FILE_SIZE, &error);
  if (!op->sealed) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  gh_mls_media_upload_on_servers_async(client_of(self), op->sealed,
                                       (const gchar *const *)op->servers,
                                       g_task_get_cancellable(task), on_send_uploaded, task);
}

void
gh_mls_attachments_send_async(GhMlsAttachments *self, GhMlsGroup *group, GBytes *file,
                              const gchar *name, const gchar *mime_hint, const gchar *caption,
                              GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer user_data)
{
  gh_mls_attachments_send_on_servers_async(self, group, file, name, mime_hint, caption, NULL,
                                           cancellable, callback, user_data);
}

void
gh_mls_attachments_send_on_servers_async(GhMlsAttachments *self, GhMlsGroup *group,
                                          GBytes *file, const gchar *name,
                                          const gchar *mime_hint, const gchar *caption,
                                          const gchar *const *servers,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  g_return_if_fail(GH_IS_MLS_GROUP(group) && file != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_attachments_send_async);
  SendOp *op = g_new0(SendOp, 1);
  op->generation = self->generation;
  op->group = g_object_ref(group);
  op->file = g_bytes_ref(file);
  op->mime = g_strdup(mime_hint);
  op->caption = g_strdup(caption ? caption : "");
  op->selected_servers = servers ? g_strdupv((gchar **)servers) : NULL;
  g_task_set_task_data(task, op, send_op_free);
  if (!service_of(self) || !client_of(self)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "Files need the account's message storage, which isn't open");
    g_object_unref(task);
    return;
  }
  /* The name every member and the server sees says no more than the type. */
  g_autoptr(GhAttachmentPrepared) probe = gh_attachment_prepare(file, mime_hint,
                                                                GH_BLOSSOM_MAX_FILE_SIZE, NULL);
  op->name = gh_mls_attachments_neutral_name(probe ? probe->mime : mime_hint, name);
  send_try(task);
}

GhMessage *
gh_mls_attachments_send_finish(GhMlsAttachments *self, GAsyncResult *result, gchar **out_server,
                               GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  if (out_server) {
    SendOp *op = g_task_get_task_data(G_TASK(result));
    *out_server = op ? g_strdup(op->server) : NULL;
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- the group picture ------------------------------------------------------------ */

GhMlsPictureState
gh_mls_picture_state_for_source(MarmotGroupAvatarSource source)
{
  switch (source) {
  case MARMOT_GROUP_AVATAR_URL:
    return GH_MLS_PICTURE_WEB;   /* it wins over an encrypted picture */
  case MARMOT_GROUP_AVATAR_URL_PLACEHOLDER:
    return GH_MLS_PICTURE_WEB_UNVERIFIED;
  case MARMOT_GROUP_AVATAR_BLOSSOM:
    return GH_MLS_PICTURE_AVAILABLE;
  case MARMOT_GROUP_AVATAR_NONE:
  default:
    return GH_MLS_PICTURE_NONE;
  }
}

gboolean
gh_mls_picture_may_load(GhMlsPictureState state, gboolean remote_images)
{
  /* An unverified URL never; a verified one only under the remote-image
   * policy; an encrypted picture on the user's request. */
  return (state == GH_MLS_PICTURE_WEB && remote_images) || state == GH_MLS_PICTURE_AVAILABLE;
}

GhMlsPictureState
gh_mls_attachments_get_picture(GhMlsAttachments *self, GhMlsGroup *group, GBytes **out_picture,
                               GStrv *out_hosts)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), GH_MLS_PICTURE_NONE);
  if (out_picture)
    *out_picture = NULL;
  if (out_hosts)
    *out_hosts = NULL;
  if (!service_of(self) || !GH_IS_MLS_GROUP(group))
    return GH_MLS_PICTURE_NONE;
  MarmotGroupComponents c;
  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_get_components(self->service, group, &c, &error))
    return g_error_matches(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED)
             ? GH_MLS_PICTURE_UNSUPPORTED : GH_MLS_PICTURE_NONE;
  GhMlsPictureState state = gh_mls_picture_state_for_source(c.avatar_source);
  const gchar *gid = gh_mls_group_get_group_id(group);
  GhStore *store = store_of(self);
  g_autofree gchar *picture_id = c.image.present ? gh_mls_media_picture_id(&c.image) : NULL;
  /* A picture the group replaced or removed is not kept. */
  if (store && !gh_store_is_read_only(store))
    (void)gh_store_group_image_forget(store, gid, picture_id, NULL);
  if (state == GH_MLS_PICTURE_AVAILABLE) {
    g_autoptr(GBytes) cached = store && picture_id
      ? gh_store_group_image_get(store, gid, picture_id, NULL, NULL) : NULL;
    g_auto(GStrv) urls = gh_mls_media_picture_urls(&c);
    /* Only servers Show Picture may contact (public hosts). */
    g_auto(GStrv) hosts = public_hosts(self, (const gchar *const *)urls);
    if (cached) {
      state = GH_MLS_PICTURE_READY;
      if (out_picture)
        *out_picture = g_steal_pointer(&cached);
    } else if (!hosts[0]) {
      state = GH_MLS_PICTURE_NO_SERVER;
    } else if (out_hosts) {
      *out_hosts = g_steal_pointer(&hosts);
    }
  }
  marmot_group_components_clear(&c);
  return state;
}

typedef struct {
  guint64 generation;
  GhMlsGroup *group;
  MarmotGroupComponents components;
  gboolean has_components;
} PictureOp;

static void
picture_op_free(gpointer data)
{
  PictureOp *op = data;
  g_clear_object(&op->group);
  if (op->has_components)
    marmot_group_components_clear(&op->components);   /* wipes the picture's keys */
  g_free(op);
}

static void
on_picture_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GhMlsAttachments *self = g_task_get_source_object(task);
  PictureOp *op = g_task_get_task_data(task);
  (void)source;
  GError *error = NULL;
  g_autoptr(GBytes) ciphertext = gh_mls_media_fetch_finish(result, &error);
  g_autoptr(GBytes) picture = ciphertext
    ? gh_mls_media_open_picture(&op->components.image, ciphertext, &error) : NULL;
  if (picture && op->generation != self->generation) {
    g_clear_pointer(&picture, g_bytes_unref);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "The account changed");
  }
  if (!picture) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  GhStore *store = store_of(self);
  g_autofree gchar *picture_id = gh_mls_media_picture_id(&op->components.image);
  g_autoptr(GError) put_error = NULL;
  if (store && !gh_store_is_read_only(store) &&
      !gh_store_group_image_put(store, gh_mls_group_get_group_id(op->group), picture_id,
                                op->components.image.media_type, picture, &put_error))
    g_debug("Group picture: not kept in the encrypted store: %s", put_error->message);
  g_task_return_pointer(task, g_steal_pointer(&picture), (GDestroyNotify)g_bytes_unref);
  g_object_unref(task);
}

void
gh_mls_attachments_fetch_picture_async(GhMlsAttachments *self, GhMlsGroup *group,
                                       GCancellable *cancellable, GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_attachments_fetch_picture_async);
  PictureOp *op = g_new0(PictureOp, 1);
  op->generation = self->generation;
  op->group = GH_IS_MLS_GROUP(group) ? g_object_ref(group) : NULL;
  g_task_set_task_data(task, op, picture_op_free);
  GError *error = NULL;
  if (!service_of(self) || !client_of(self) || !op->group) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "The group's storage isn't open");
    g_object_unref(task);
    return;
  }
  if (!gh_mls_service_get_components(self->service, group, &op->components, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  op->has_components = TRUE;
  /* Only the encrypted picture that shows: never a URL avatar. */
  if (op->components.avatar_source != MARMOT_GROUP_AVATAR_BLOSSOM) {
    g_task_return_new_error(task, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_UNAVAILABLE,
                            "The group has no encrypted picture to show");
    g_object_unref(task);
    return;
  }
  gh_mls_media_fetch_picture_async(client_of(self), &op->components, cancellable,
                                   on_picture_fetched, task);
}

GBytes *
gh_mls_attachments_fetch_picture_finish(GhMlsAttachments *self, GAsyncResult *result,
                                        GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct {
  guint64 generation;
  GhMlsGroup *group;
  MarmotGroupBlossomImage image;
  GBytes *ciphertext;
} SetPictureOp;

static void
set_picture_op_free(gpointer data)
{
  SetPictureOp *op = data;
  g_clear_object(&op->group);
  marmot_group_blossom_image_clear(&op->image);
  g_clear_pointer(&op->ciphertext, g_bytes_unref);
  g_free(op);
}

static void
on_picture_committed(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GhMlsAttachments *self = g_task_get_source_object(task);
  SetPictureOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  /* The admin sees the new picture at once (decrypted from what was sent). */
  GhStore *store = store_of(self);
  if (op->ciphertext && store && !gh_store_is_read_only(store) &&
      op->generation == self->generation) {
    g_autoptr(GBytes) picture = gh_mls_media_open_picture(&op->image, op->ciphertext, NULL);
    g_autofree gchar *picture_id = gh_mls_media_picture_id(&op->image);
    if (picture)
      (void)gh_store_group_image_put(store, gh_mls_group_get_group_id(op->group), picture_id,
                                     op->image.media_type, picture, NULL);
  }
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void
commit_picture(GTask *task)
{
  GhMlsAttachments *self = g_task_get_source_object(task);
  SetPictureOp *op = g_task_get_task_data(task);
  if (op->generation != self->generation || !service_of(self)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The account changed");
    g_object_unref(task);
    return;
  }
  gh_mls_service_set_image_async(self->service, op->group,
                                 op->image.present ? &op->image : NULL,
                                 g_task_get_cancellable(task), on_picture_committed, task);
}

static void
on_picture_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  g_autofree gchar *url = gh_blossom_client_upload_finish(GH_BLOSSOM_CLIENT(source), result,
                                                          NULL, &error);
  if (!url) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  commit_picture(task);
}

void
gh_mls_attachments_set_picture_async(GhMlsAttachments *self, GhMlsGroup *group, GBytes *file,
                                     const gchar *mime_hint,
                                     const gchar *const *confirmed_hosts,
                                     GCancellable *cancellable, GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(self));
  g_return_if_fail(GH_IS_MLS_GROUP(group));
  g_return_if_fail(!file || confirmed_hosts);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_attachments_set_picture_async);
  SetPictureOp *op = g_new0(SetPictureOp, 1);
  op->generation = self->generation;
  op->group = g_object_ref(group);
  g_task_set_task_data(task, op, set_picture_op_free);
  GError *error = NULL;
  if (!service_of(self) || !client_of(self)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "The group's storage isn't open");
    g_object_unref(task);
    return;
  }
  if (!file) {
    commit_picture(task);   /* remove */
    return;
  }
  MarmotGroupComponents c;
  if (!gh_mls_service_get_components(self->service, group, &c, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  /* 0x8002 names no server: members look on the group's media servers, so
   * the picture goes there (MDK uploads it to its default media server),
   * public hosts only (W25 review M1; the upload filters again). */
  g_auto(GStrv) named = gh_mls_media_dup_servers(&c);
  marmot_group_components_clear(&c);
  g_auto(GStrv) servers = gh_blossom_client_dup_public_servers(client_of(self),
                                                               (const gchar *const *)named);
  /* Exactly the servers the admin was shown, or nothing (W25 re-review R4):
   * a Commit may have changed them while the confirmation was open. */
  g_auto(GStrv) hosts = public_hosts(self, (const gchar *const *)named);
  if (!g_strv_equal((const gchar *const *)hosts, confirmed_hosts)) {
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_SERVERS_CHANGED,
                            "The group's media servers changed since they were confirmed");
    g_object_unref(task);
    return;
  }
  if (!servers[0]) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER,
                            "The group names no media server for its picture");
    g_object_unref(task);
    return;
  }
  op->ciphertext = gh_mls_media_seal_picture(file, mime_hint, &op->image, &error);
  if (!op->ciphertext) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  g_autofree gchar *sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, op->ciphertext);
  gh_blossom_client_upload_keyed_async(client_of(self), (const gchar *const *)servers,
                                       op->ciphertext, sha, op->image.image_upload_key,
                                       cancellable, on_picture_uploaded, task);
}

GStrv
gh_mls_attachments_dup_picture_upload_hosts(GhMlsAttachments *self, GhMlsGroup *group)
{
  g_return_val_if_fail(GH_IS_MLS_ATTACHMENTS(self), NULL);
  MarmotGroupComponents c;
  if (!service_of(self) || !GH_IS_MLS_GROUP(group) ||
      !gh_mls_service_get_components(self->service, group, &c, NULL))
    return g_new0(gchar *, 1);
  g_auto(GStrv) named = gh_mls_media_dup_servers(&c);
  marmot_group_components_clear(&c);
  return public_hosts(self, (const gchar *const *)named);
}

gboolean
gh_mls_attachments_set_picture_finish(GhMlsAttachments *self, GAsyncResult *result,
                                      GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- GObject -------------------------------------------------------------------------- */

GhMlsAttachments *
gh_mls_attachments_new(GhAttachments *attachments)
{
  g_return_val_if_fail(GH_IS_ATTACHMENTS(attachments), NULL);
  GhMlsAttachments *self = g_object_new(GH_TYPE_MLS_ATTACHMENTS, NULL);
  self->attachments = g_object_ref(attachments);
  g_signal_connect_object(attachments, "reset", G_CALLBACK(on_attachments_reset), self,
                          G_CONNECT_SWAPPED);
  return self;
}

static void
gh_mls_attachments_dispose(GObject *object)
{
  GhMlsAttachments *self = GH_MLS_ATTACHMENTS(object);
  if (self->entries)
    reset(self, TRUE);
  if (self->service)
    g_object_weak_unref(G_OBJECT(self->service), on_service_gone, self);
  self->service = NULL;
  g_clear_object(&self->attachments);
  G_OBJECT_CLASS(gh_mls_attachments_parent_class)->dispose(object);
}

static void
gh_mls_attachments_finalize(GObject *object)
{
  GhMlsAttachments *self = GH_MLS_ATTACHMENTS(object);
  g_clear_pointer(&self->entries, g_hash_table_unref);
  G_OBJECT_CLASS(gh_mls_attachments_parent_class)->finalize(object);
}

static void
gh_mls_attachments_class_init(GhMlsAttachmentsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_mls_attachments_dispose;
  object_class->finalize = gh_mls_attachments_finalize;
}

static void
gh_mls_attachments_init(GhMlsAttachments *self)
{
  self->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
}
