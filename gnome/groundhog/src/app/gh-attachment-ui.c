#include "gh-attachment-ui.h"

#include "gh-attachment-card.h"
#include "gh-composer.h"
#include "gh-conversation-view.h"
#include "gh-nip17-envelope.h"
#include "gh-outbox.h"
#include "gh-preferences-dialog.h"

#include <glib/gi18n.h>
#include <string.h>

#define ATTACHMENT_UI_DATA "groundhog-attachment-ui"

/* The file being offered in the sheet. */
typedef struct {
  GhAttachmentSheet *sheet;       /* a reference */
  GhAttachmentPrepared *prepared; /* what is encrypted: metadata removed */
  GStrv recipients;               /* the shown conversation's, at the offer */
  GCancellable *upload;           /* while uploading */
  gchar *consent_server;          /* the server that asked for a known account */
} Offer;

typedef struct {
  GtkWidget *window;          /* not owned: the struct is the window's data */
  GhComposer *composer;       /* template children of the window */
  GhConversationView *view;
  GhAccountStore *store;
  GhConversationStore *model;
  GhAttachments *attachments;
  GSettings *settings;
  gboolean allow_onion;
  gboolean destroyed;         /* the window went: nothing more is shown */
  GCancellable *loading;      /* reading a chosen file */
  Offer *offer;
  GhAttachmentUiSaveTarget save_target;
  gpointer save_data;
  gchar *last_toast;
} GhAttachmentUi;

static void close_offer(GhAttachmentUi *ui);

static GhAttachmentUi *
ui_of(GtkWidget *window)
{
  GhAttachmentUi *ui = window ? g_object_get_data(G_OBJECT(window), ATTACHMENT_UI_DATA) : NULL;
  return ui && !ui->destroyed ? ui : NULL;
}

static void
offer_free(Offer *offer)
{
  if (offer->upload)
    g_cancellable_cancel(offer->upload);
  g_clear_object(&offer->upload);
  g_clear_object(&offer->sheet);
  g_clear_pointer(&offer->prepared, gh_attachment_prepared_free);
  g_strfreev(offer->recipients);
  g_free(offer->consent_server);
  g_free(offer);
}

static void
ui_free(gpointer data)
{
  GhAttachmentUi *ui = data;
  if (ui->loading)
    g_cancellable_cancel(ui->loading);
  g_clear_object(&ui->loading);
  g_clear_pointer(&ui->offer, offer_free);
  g_clear_object(&ui->store);
  g_clear_object(&ui->model);
  g_clear_object(&ui->attachments);
  g_clear_object(&ui->settings);
  g_free(ui->last_toast);
  g_free(ui);
}

static void
toast(GhAttachmentUi *ui, const gchar *text)
{
  g_free(ui->last_toast);
  ui->last_toast = g_strdup(text);
  adw_toast_overlay_add_toast(gh_window_get_toasts(GH_WINDOW(ui->window)), adw_toast_new(text));
}

/* ---- where a file can go ------------------------------------------------------------- */

/* The shown conversation's other participants, one or a NIP-17 room of up
 * to GH_NIP17_MAX_SEND_RECIPIENTS, or the account itself in a note to self;
 * NULL for a relay group (a file there would be public: charter §6 scope)
 * or a room too large to send to (as gh-send-ui.c). */
static GStrv
recipients_of(GhAttachmentUi *ui, GhConversation *conversation)
{
  if (!conversation || gh_conversation_get_backend(conversation) != GH_CONVERSATION_BACKEND_NIP17)
    return NULL;
  const gchar *const *peers = gh_conversation_get_peers(conversation);
  if (!peers || !peers[0]) {
    const gchar *account = gh_conversation_store_get_account(ui->model);
    if (!account)
      return NULL;
    const gchar *const self[] = { account, NULL };
    return g_strdupv((gchar **)self);
  }
  if (g_strv_length((gchar **)peers) > GH_NIP17_MAX_SEND_RECIPIENTS)
    return NULL;
  return g_strdupv((gchar **)peers);
}

static GhOutbox *
outbox_of(GhAttachmentUi *ui)
{
  GhAccountStoreState state = gh_account_store_get_state(ui->store);
  GObject *object = state == GH_ACCOUNT_STORE_OPEN || state == GH_ACCOUNT_STORE_EPHEMERAL
                      ? gh_account_store_get_outbox(ui->store) : NULL;
  return GH_IS_OUTBOX(object) ? GH_OUTBOX(object) : NULL;
}

/* The attach button shows only where a file can be sent right now. */
static void
update_can_attach(GhAttachmentUi *ui)
{
  g_auto(GStrv) recipients = recipients_of(ui, gh_conversation_view_get_conversation(ui->view));
  gboolean can = recipients && outbox_of(ui) && gh_attachments_get_client(ui->attachments);
  gh_composer_set_can_attach(ui->composer, can);
}

static GStrv
servers(GhAttachmentUi *ui)
{
  GStrv list = ui->settings ? g_settings_get_strv(ui->settings, "blossom-servers") : NULL;
  return list ? list : g_new0(gchar *, 1);
}

/* ---- the sheet's copy -------------------------------------------------------------- */

static gchar *
host_of(const gchar *url)
{
  g_autofree gchar *host = NULL;
  g_autofree gchar *normalized = gh_blossom_client_normalize_server(url, &host);
  return normalized ? g_steal_pointer(&host) : NULL;
}

#define HOUR_S (G_GINT64_CONSTANT(3600))
#define DAY_S  (24 * HOUR_S)
#define WEEK_S (7 * DAY_S)

static gchar *
describe_timer(gint64 seconds)
{
  if (seconds % WEEK_S == 0) {
    gulong weeks = (gulong)(seconds / WEEK_S);
    return g_strdup_printf(g_dngettext(NULL, "%lu week", "%lu weeks", weeks), weeks);
  }
  if (seconds % DAY_S == 0) {
    gulong days = (gulong)(seconds / DAY_S);
    return g_strdup_printf(g_dngettext(NULL, "%lu day", "%lu days", days), days);
  }
  gulong hours = (gulong)((seconds + HOUR_S - 1) / HOUR_S);
  return g_strdup_printf(g_dngettext(NULL, "%lu hour", "%lu hours", hours), hours);
}

/* What the first server learns in the current network mode, and what
 * outlives a disappearing message (charter §2.2 "Attachment server"). */
static void
update_notes(GhAttachmentUi *ui)
{
  if (!ui->offer)
    return;
  g_auto(GStrv) list = servers(ui);
  g_autofree gchar *host = list[0] ? host_of(list[0]) : NULL;
  gboolean tor = gh_attachments_get_tor(ui->attachments);
  g_autofree gchar *server_note = NULL;
  if (!host)
    server_note = g_strdup("");
  else if (tor)
    server_note = g_strdup_printf(
      _("The file is encrypted on this device, then uploaded to %s through Tor. The server keeps "
        "the encrypted file and can see its size and when it's uploaded, but not your IP address "
        "or what the file contains."), host);
  else
    server_note = g_strdup_printf(
      _("The file is encrypted on this device, then uploaded to %s. The server keeps the "
        "encrypted file and can see its size, when it's uploaded and your IP address, but not "
        "what the file contains."), host);
  gint64 timer = gh_composer_get_disappearing_timer(ui->composer);
  g_autofree gchar *duration = timer > 0 ? describe_timer(timer) : NULL;
  g_autofree gchar *timer_note =
    duration ? g_strdup_printf(_("The message disappears after %s, but the server keeps the "
                                 "encrypted file until it deletes it."), duration)
             : NULL;
  gh_attachment_sheet_set_notes(ui->offer->sheet, server_note, timer_note);
}

/* ---- sending ------------------------------------------------------------------------ */

static void start_upload(GhAttachmentUi *ui);

typedef struct {
  GtkWidget *window;       /* a reference */
  GCancellable *upload;    /* which upload: the offer's, if it is still that */
} UploadOp;

/* The upload of the offer finished: consent, a server, an error, or the
 * message queued. */
static void
upload_done(GhAttachmentUi *ui, Offer *offer, const GhNip17File *file, const gchar *server,
            const GError *upload_error)
{
  g_autofree gchar *host = server ? host_of(server) : NULL;
  if (!file) {
    if (g_error_matches(upload_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      gh_attachment_sheet_show_preview(offer->sheet, NULL);
    } else if (g_error_matches(upload_error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED) &&
               server && !gh_attachments_get_consent(ui->attachments, server)) {
      /* Charter §6 step 4: the account signs only with this server's consent. */
      g_free(offer->consent_server);
      offer->consent_server = g_strdup(server);
      gh_attachment_sheet_show_consent(offer->sheet, host ? host : server);
    } else if (g_error_matches(upload_error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER)) {
      gh_attachment_sheet_show_servers(offer->sheet, NULL);
    } else {
      /* The error names the server and may quote it: debug only (PD-10). */
      g_debug("Attachment: an upload failed: %s", upload_error->message);
      g_autofree gchar *text = gh_attachments_describe(ui->attachments, upload_error,
                                                       GH_ATTACHMENTS_UPLOAD, host, NULL);
      gh_attachment_sheet_show_preview(offer->sheet, text);
    }
    return;
  }
  g_autoptr(GError) error = NULL;
  GhOutbox *outbox = outbox_of(ui);
  g_autoptr(GhOutboxItem) item =
    outbox ? gh_outbox_send_file_room(outbox, (const gchar *const *)offer->recipients, file,
                                      &error)
           : NULL;
  if (!item) {
    if (error)
      g_message("Groundhog could not queue a file: %s", error->message);
    gh_attachment_sheet_show_preview(
      offer->sheet,
      g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL)
        ? _("Storage is full, so the file was uploaded but not sent.")
        : _("The file was uploaded, but it couldn't be queued for sending."));
    return;
  }
  /* T-enqueue committed: the send UI shows the message; its card shows the
   * file from the encrypted cache. */
  gh_attachments_remember_sent(ui->attachments, gh_outbox_item_get_rumor_id(item), file,
                               offer->prepared->plaintext);
  close_offer(ui);
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  UploadOp *op = data;
  GhAttachmentUi *ui = ui_of(op->window);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *server = NULL;
  g_autoptr(GhNip17File) file =
    gh_attachments_upload_finish(GH_ATTACHMENTS(source), result, &server, &error);
  Offer *offer = ui ? ui->offer : NULL;
  /* Not when cancelled, closed or offered again meanwhile. */
  if (offer && offer->upload == op->upload) {
    g_clear_object(&offer->upload);
    upload_done(ui, offer, file, server, error);
  }
  g_object_unref(op->upload);
  g_object_unref(op->window);
  g_free(op);
}

static void
start_upload(GhAttachmentUi *ui)
{
  Offer *offer = ui->offer;
  if (!offer || offer->upload)
    return;
  g_auto(GStrv) list = servers(ui);
  if (!list[0]) {
    gh_attachment_sheet_show_servers(offer->sheet, NULL);
    return;
  }
  g_autofree gchar *host = host_of(list[0]);
  g_autofree gchar *text = g_strdup_printf(_("Encrypting and uploading to %s…"),
                                           host ? host : list[0]);
  gh_attachment_sheet_show_sending(offer->sheet, text);
  offer->upload = g_cancellable_new();
  UploadOp *op = g_new0(UploadOp, 1);
  op->window = g_object_ref(ui->window);
  op->upload = g_object_ref(offer->upload);
  gh_attachments_upload_async(ui->attachments, offer->prepared->plaintext, offer->prepared->mime,
                              offer->upload, on_uploaded, op);
}

/* ---- the sheet's signals ------------------------------------------------------------ */

static void
on_sheet_send(GhAttachmentSheet *sheet, GtkWidget *window)
{
  GhAttachmentUi *ui = ui_of(window);
  if (ui && ui->offer && ui->offer->sheet == sheet)
    start_upload(ui);
}

static void
on_sheet_server(GhAttachmentSheet *sheet, const gchar *text, GtkWidget *window)
{
  GhAttachmentUi *ui = ui_of(window);
  if (!ui || !ui->offer || ui->offer->sheet != sheet)
    return;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) current = servers(ui);
  g_auto(GStrv) list = gh_preferences_server_list_add((const gchar *const *)current, text,
                                                      ui->allow_onion, &error);
  if (!list && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
    gh_attachment_sheet_show_servers(sheet, error->message);
    return;
  }
  if (list && (!ui->settings ||
               !g_settings_set_strv(ui->settings, "blossom-servers", (const gchar *const *)list))) {
    gh_attachment_sheet_show_servers(sheet, _("This setting can't be changed"));
    return;
  }
  update_notes(ui);
  gh_attachment_sheet_show_preview(sheet, NULL);
}

static void
on_sheet_consent(GhAttachmentSheet *sheet, GtkWidget *window)
{
  GhAttachmentUi *ui = ui_of(window);
  if (!ui || !ui->offer || ui->offer->sheet != sheet || !ui->offer->consent_server)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_attachments_set_consent(ui->attachments, ui->offer->consent_server, TRUE, &error)) {
    g_message("Groundhog could not keep an upload consent: %s", error->message);
    gh_attachment_sheet_show_preview(sheet, _("Your choice couldn't be saved, so nothing was "
                                              "uploaded."));
    return;
  }
  start_upload(ui);
}

/* Cancel while sending returns to the preview; "Don't Upload" closes. */
static void
on_sheet_cancel(GhAttachmentSheet *sheet, GtkWidget *window)
{
  GhAttachmentUi *ui = ui_of(window);
  if (!ui || !ui->offer || ui->offer->sheet != sheet)
    return;
  if (ui->offer->upload) {
    g_cancellable_cancel(ui->offer->upload);
    g_clear_object(&ui->offer->upload);
    gh_attachment_sheet_show_preview(sheet, NULL);
    return;
  }
  close_offer(ui);
}

static void
on_sheet_closed(GhAttachmentSheet *sheet, GtkWidget *window)
{
  GhAttachmentUi *ui = g_object_get_data(G_OBJECT(window), ATTACHMENT_UI_DATA);
  if (ui && ui->offer && ui->offer->sheet == sheet)
    g_clear_pointer(&ui->offer, offer_free); /* cancels an upload */
}

static void
close_offer(GhAttachmentUi *ui)
{
  if (!ui->offer)
    return;
  g_autoptr(GhAttachmentSheet) sheet = g_object_ref(ui->offer->sheet);
  adw_dialog_force_close(ADW_DIALOG(sheet)); /* "closed" frees the offer */
  g_clear_pointer(&ui->offer, offer_free);
}

/* ---- offering a file ---------------------------------------------------------------- */

static void
offer_prepared(GhAttachmentUi *ui, GhAttachmentPrepared *prepared, const gchar *name,
               GStrv recipients)
{
  close_offer(ui);
  Offer *offer = g_new0(Offer, 1);
  offer->prepared = prepared;
  offer->recipients = recipients;
  offer->sheet = g_object_ref_sink(gh_attachment_sheet_new());
  ui->offer = offer;

  /* The photo only after the decode guard (AT-4), with GTK's own loaders. */
  g_autoptr(GdkTexture) thumbnail = NULL;
  if (gh_attachment_check_preview(prepared->plaintext, NULL, NULL, NULL, NULL))
    thumbnail = gdk_texture_new_from_bytes(prepared->plaintext, NULL);
  g_autofree gchar *kind = gh_attachment_card_describe_type(prepared->mime);
  gh_attachment_sheet_set_file(offer->sheet, name, g_bytes_get_size(prepared->plaintext), kind,
                               thumbnail ? GDK_PAINTABLE(thumbnail) : NULL);
  gh_attachment_sheet_set_metadata_removed(offer->sheet, !prepared->may_have_metadata);
  update_notes(ui);
  g_signal_connect_object(offer->sheet, "send", G_CALLBACK(on_sheet_send), ui->window, 0);
  g_signal_connect_object(offer->sheet, "server-chosen", G_CALLBACK(on_sheet_server), ui->window,
                          0);
  g_signal_connect_object(offer->sheet, "consent", G_CALLBACK(on_sheet_consent), ui->window, 0);
  g_signal_connect_object(offer->sheet, "cancel", G_CALLBACK(on_sheet_cancel), ui->window, 0);
  g_signal_connect_object(offer->sheet, "closed", G_CALLBACK(on_sheet_closed), ui->window, 0);
  g_auto(GStrv) list = servers(ui);
  /* D6: no server is ever chosen for the user. */
  if (list[0])
    gh_attachment_sheet_show_preview(offer->sheet, NULL);
  else
    gh_attachment_sheet_show_servers(offer->sheet, NULL);
  adw_dialog_present(ADW_DIALOG(offer->sheet), ui->window);
}

void
gh_attachment_ui_offer_bytes(GhWindow *window, GBytes *bytes, const gchar *name,
                             const gchar *mime)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(bytes != NULL);
  GhAttachmentUi *ui = ui_of(GTK_WIDGET(window));
  if (!ui)
    return;
  g_auto(GStrv) recipients = recipients_of(ui, gh_conversation_view_get_conversation(ui->view));
  if (!recipients || !outbox_of(ui)) {
    toast(ui, _("Files can't be sent in this conversation"));
    return;
  }
  g_autoptr(GError) error = NULL;
  GhAttachmentPrepared *prepared =
    gh_attachment_prepare(bytes, mime, GH_BLOSSOM_MAX_FILE_SIZE, &error);
  if (!prepared) {
    g_autofree gchar *text = gh_attachments_describe(ui->attachments, error,
                                                     GH_ATTACHMENTS_UPLOAD, NULL, NULL);
    toast(ui, text);
    return;
  }
  offer_prepared(ui, prepared, name, g_steal_pointer(&recipients));
}

typedef struct {
  GtkWidget *window;         /* a reference */
  GCancellable *cancellable; /* this load's (a newer offer cancels it) */
  gchar *name;
  gchar *mime;
} LoadOp;

static void
load_op_free(LoadOp *op)
{
  g_object_unref(op->window);
  g_object_unref(op->cancellable);
  g_free(op->name);
  g_free(op->mime);
  g_free(op);
}

static void
on_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  LoadOp *op = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_file_load_bytes_finish(G_FILE(source), result, NULL, &error);
  GhAttachmentUi *ui = ui_of(op->window);
  if (ui && bytes)
    gh_attachment_ui_offer_bytes(GH_WINDOW(op->window), bytes, op->name, op->mime);
  else if (ui && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    toast(ui, _("The file couldn't be read"));
  load_op_free(op);
}

static void
on_file_info(GObject *source, GAsyncResult *result, gpointer data)
{
  LoadOp *op = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), result, &error);
  GhAttachmentUi *ui = ui_of(op->window);
  if (!ui || !info) {
    if (ui && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      toast(ui, _("The file couldn't be read"));
    load_op_free(op);
    return;
  }
  /* D6: nothing larger than the limit is even read. */
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
    toast(ui, _("Only files can be sent, not folders"));
    load_op_free(op);
    return;
  }
  if (g_file_info_get_size(info) > GH_BLOSSOM_MAX_FILE_SIZE) {
    g_autofree gchar *text =
      g_strdup_printf(_("This file is too large: files can be at most %lu MB"),
                      (gulong)(GH_BLOSSOM_MAX_FILE_SIZE / (1024 * 1024)));
    toast(ui, text);
    load_op_free(op);
    return;
  }
  op->name = g_strdup(g_file_info_get_display_name(info));
  const gchar *type = g_file_info_get_content_type(info);
  op->mime = type ? g_content_type_get_mime_type(type) : NULL;
  g_file_load_bytes_async(G_FILE(source), op->cancellable, on_loaded, op);
}

void
gh_attachment_ui_offer_file(GhWindow *window, GFile *file)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(G_IS_FILE(file));
  GhAttachmentUi *ui = ui_of(GTK_WIDGET(window));
  if (!ui)
    return;
  if (ui->loading)
    g_cancellable_cancel(ui->loading);
  g_clear_object(&ui->loading);
  ui->loading = g_cancellable_new();
  LoadOp *op = g_new0(LoadOp, 1);
  op->window = g_object_ref(GTK_WIDGET(window));
  op->cancellable = g_object_ref(ui->loading);
  g_file_query_info_async(file,
                          G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                          G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                          G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                          G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, op->cancellable,
                          on_file_info, op);
}

static void
on_file_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data; /* a reference */
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file && ui_of(window))
    gh_attachment_ui_offer_file(GH_WINDOW(window), file);
  g_object_unref(window);
}

static void
on_attach_requested(GhComposer *composer, GtkWidget *window)
{
  (void)composer;
  if (!ui_of(window))
    return;
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Attach File"));
  gtk_file_dialog_set_accept_label(dialog, _("_Attach"));
  gtk_file_dialog_set_modal(dialog, TRUE);
  gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, on_file_chosen, g_object_ref(window));
}

static void
on_attach_file(GhComposer *composer, GFile *file, GtkWidget *window)
{
  (void)composer;
  gh_attachment_ui_offer_file(GH_WINDOW(window), file);
}

/* A pasted or dropped image: a new PNG of its pixels, so nothing but the
 * picture is sent. */
static void
on_attach_texture(GhComposer *composer, GdkTexture *texture, GtkWidget *window)
{
  (void)composer;
  g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(texture);
  gh_attachment_ui_offer_bytes(GH_WINDOW(window), png, _("Pasted image.png"), "image/png");
}

/* ---- received files: the cards' provider -------------------------------------------------- */

static GhAttachmentTransfer *
card_lookup(GhMessage *message, gpointer data)
{
  GhAttachmentUi *ui = data;
  return ui->destroyed ? NULL : gh_attachments_lookup(ui->attachments, message);
}

static void
card_download(GhAttachmentTransfer *transfer, gpointer data)
{
  GhAttachmentUi *ui = data;
  gh_attachments_download(ui->attachments, transfer);
}

static void
card_cancel(GhAttachmentTransfer *transfer, gpointer data)
{
  GhAttachmentUi *ui = data;
  gh_attachments_cancel(ui->attachments, transfer);
}

static gchar *
card_download_note(GhAttachmentTransfer *transfer, gpointer data)
{
  GhAttachmentUi *ui = data;
  g_autoptr(GUri) uri = g_uri_parse(gh_attachment_transfer_get_file(transfer)->url,
                                    G_URI_FLAGS_ENCODED, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!host)
    return NULL;
  return gh_attachments_get_tor(ui->attachments)
    ? g_strdup_printf(_("Downloads the encrypted file from %s through Tor"), host)
    : g_strdup_printf(_("Downloads the encrypted file from %s, which can see your IP address"),
                      host);
}

typedef struct {
  GtkWidget *window; /* a reference */
  GBytes *plaintext;
  GFile *file;
} SaveOp;

static void
save_op_free(SaveOp *op)
{
  g_object_unref(op->window);
  g_bytes_unref(op->plaintext);
  g_clear_object(&op->file);
  g_free(op);
}

static void
on_open_saved(AdwToast *toast_widget, GFile *file)
{
  GtkWidget *window = g_object_get_data(G_OBJECT(toast_widget), "groundhog-window");
  g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_launch(launcher, GTK_IS_WINDOW(window) ? GTK_WINDOW(window) : NULL, NULL,
                           NULL, NULL);
}

static void
on_saved(GObject *source, GAsyncResult *result, gpointer data)
{
  SaveOp *op = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = g_file_replace_contents_finish(G_FILE(source), result, NULL, &error);
  GhAttachmentUi *ui = ui_of(op->window);
  if (ui) {
    g_autofree gchar *name = g_file_get_basename(op->file);
    if (ok) {
      /* Charter §6 step 6: the copy is the user's now, outside the store. */
      g_autofree gchar *text =
        g_strdup_printf(_("Saved “%s”. Saved files aren't protected by Groundhog."), name);
      g_free(ui->last_toast);
      ui->last_toast = g_strdup(text);
      AdwToast *saved = adw_toast_new(text);
      adw_toast_set_button_label(saved, _("_Open"));
      g_object_set_data(G_OBJECT(saved), "groundhog-window", ui->window);
      g_signal_connect_data(saved, "button-clicked", G_CALLBACK(on_open_saved),
                            g_object_ref(op->file), (GClosureNotify)(void (*)(void))g_object_unref,
                            0);
      adw_toast_overlay_add_toast(gh_window_get_toasts(GH_WINDOW(ui->window)), saved);
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      /* The error names the chosen path: debug only (PD-10). */
      g_debug("Attachment: saving failed: %s", error->message);
      g_autofree gchar *text = g_strdup_printf(_("Couldn't save “%s”"), name);
      toast(ui, text);
    }
  }
  save_op_free(op);
}

static void
save_to(SaveOp *op, GFile *file)
{
  op->file = g_object_ref(file);
  gsize size = 0;
  gconstpointer data = g_bytes_get_data(op->plaintext, &size);
  g_file_replace_contents_async(file, data, size, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, on_saved, op);
}

static void
on_save_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
  SaveOp *op = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
  if (!file || !ui_of(op->window)) {
    save_op_free(op);
    return;
  }
  save_to(op, file);
}

/* Save As… (charter §6 step 6): the plaintext is written only where the
 * user chose in the Save dialog (the portal), never anywhere else. */
static void
card_save(GhAttachmentTransfer *transfer, GtkWidget *card, gpointer data)
{
  GhAttachmentUi *ui = data;
  (void)card;
  GBytes *plaintext = gh_attachment_transfer_get_plaintext(transfer);
  if (ui->destroyed || !plaintext)
    return;
  const GhNip17File *file = gh_attachment_transfer_get_file(transfer);
  g_autofree gchar *name = gh_attachment_card_suggest_name(file->file_type, plaintext);
  SaveOp *op = g_new0(SaveOp, 1);
  op->window = g_object_ref(ui->window);
  op->plaintext = g_bytes_ref(plaintext);
  if (ui->save_target) {
    g_autoptr(GFile) target = ui->save_target(name, ui->save_data);
    if (target)
      save_to(op, target);
    else
      save_op_free(op);
    return;
  }
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Save File"));
  gtk_file_dialog_set_initial_name(dialog, name);
  gtk_file_dialog_set_modal(dialog, TRUE);
  gtk_file_dialog_save(dialog, GTK_WINDOW(ui->window), NULL, on_save_chosen, op);
}

static const GhAttachmentCardProvider card_provider = {
  .lookup = card_lookup,
  .download = card_download,
  .cancel = card_cancel,
  .save = card_save,
  .download_note = card_download_note,
};

/* ---- state -------------------------------------------------------------------------- */

static void
on_state_source(GtkWidget *window)
{
  GhAttachmentUi *ui = ui_of(window);
  if (ui)
    update_can_attach(ui);
}

/* Another account's storage (or none): the file offered was the previous
 * account's to send. */
static void
on_attachments_reset(GtkWidget *window)
{
  GhAttachmentUi *ui = ui_of(window);
  if (!ui)
    return;
  close_offer(ui);
  update_can_attach(ui);
}

static void
on_window_destroy(GtkWidget *window)
{
  GhAttachmentUi *ui = g_object_get_data(G_OBJECT(window), ATTACHMENT_UI_DATA);
  if (!ui || ui->destroyed)
    return;
  if (ui->loading)
    g_cancellable_cancel(ui->loading);
  close_offer(ui);
  ui->destroyed = TRUE;
  gh_attachment_card_set_provider(window, NULL, NULL, NULL);
}

/* ---- public ----------------------------------------------------------------------------- */

void
gh_attachment_ui_attach(GhWindow *window, const GhAttachmentUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL);
  g_return_if_fail(GH_IS_ACCOUNT_STORE(config->account_store));
  g_return_if_fail(GH_IS_CONVERSATION_STORE(config->conversations));
  g_return_if_fail(GH_IS_ATTACHMENTS(config->attachments));
  g_return_if_fail(!config->settings || G_IS_SETTINGS(config->settings));
  g_return_if_fail(g_object_get_data(G_OBJECT(window), ATTACHMENT_UI_DATA) == NULL);
  GhContentPage *content = gh_window_get_content(window);
  GtkWidget *view = gh_content_page_get_view(content);
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(view));

  GhAttachmentUi *ui = g_new0(GhAttachmentUi, 1);
  ui->window = GTK_WIDGET(window);
  ui->composer = gh_content_page_get_composer(content);
  ui->view = GH_CONVERSATION_VIEW(view);
  ui->store = g_object_ref(config->account_store);
  ui->model = g_object_ref(config->conversations);
  ui->attachments = g_object_ref(config->attachments);
  ui->settings = config->settings ? g_object_ref(config->settings) : NULL;
  ui->allow_onion = config->allow_onion;
  g_object_set_data_full(G_OBJECT(window), ATTACHMENT_UI_DATA, ui, ui_free);

  /* Handlers on objects that outlive the window stop with it. */
  g_signal_connect_object(ui->composer, "attach-requested", G_CALLBACK(on_attach_requested),
                          window, 0);
  g_signal_connect_object(ui->composer, "attach-file", G_CALLBACK(on_attach_file), window, 0);
  g_signal_connect_object(ui->composer, "attach-texture", G_CALLBACK(on_attach_texture), window,
                          0);
  g_signal_connect_object(ui->view, "notify::conversation", G_CALLBACK(on_state_source), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(ui->store, "changed", G_CALLBACK(on_state_source), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(ui->attachments, "reset", G_CALLBACK(on_attachments_reset), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
  gh_attachment_card_set_provider(GTK_WIDGET(window), &card_provider, ui, NULL);
  update_can_attach(ui);
}

GhAttachmentSheet *
gh_attachment_ui_get_sheet(GhWindow *window)
{
  g_return_val_if_fail(GH_IS_WINDOW(window), NULL);
  GhAttachmentUi *ui = ui_of(GTK_WIDGET(window));
  return ui && ui->offer ? ui->offer->sheet : NULL;
}

void
gh_attachment_ui_set_save_target(GhWindow *window, GhAttachmentUiSaveTarget func, gpointer data)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  GhAttachmentUi *ui = ui_of(GTK_WIDGET(window));
  if (!ui)
    return;
  ui->save_target = func;
  ui->save_data = data;
}

const gchar *
gh_attachment_ui_get_last_toast(GhWindow *window)
{
  g_return_val_if_fail(GH_IS_WINDOW(window), NULL);
  GhAttachmentUi *ui = g_object_get_data(G_OBJECT(window), ATTACHMENT_UI_DATA);
  return ui ? ui->last_toast : NULL;
}
