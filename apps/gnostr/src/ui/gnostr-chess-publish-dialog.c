/**
 * GnostrChessPublishDialog - Publish Chess Game to Nostr Dialog
 *
 * Publishes completed chess games as NIP-64 (kind 64) events.
 * The dialog:
 * 1. Shows a summary of the completed game
 * 2. Exports PGN from the session
 * 3. Creates and signs a kind 64 event
 * 4. Publishes to configured write relays
 */

#include "gnostr-chess-publish-dialog.h"
#include "../ipc/gnostr-signer-service.h"
#include "../util/relays.h"
#include "../util/nip64_chess.h"
#include <glib/gi18n.h>
#include <json.h>
/* Use gobject relay wrapper for GLib integration */
#include "nostr_relay.h"

struct _GnostrChessPublishDialog {
    AdwDialog parent_instance;

    /* Display widgets */
    GtkWidget *lbl_result;
    GtkWidget *lbl_white;
    GtkWidget *lbl_black;
    GtkWidget *lbl_event_type;

    /* Status widgets */
    GtkWidget *status_box;
    GtkWidget *spinner;
    GtkWidget *lbl_status;

    /* Action buttons */
    GtkWidget *btn_cancel;
    GtkWidget *btn_publish;

    /* Game data */
    GnostrChessSession *session;  /* Not owned */
    gchar *result_string;
    gchar *result_reason;
    gchar *white_name;
    gchar *black_name;

    /* State */
    gboolean is_publishing;
    GCancellable *cancellable;
};

G_DEFINE_TYPE(GnostrChessPublishDialog, gnostr_chess_publish_dialog, ADW_TYPE_DIALOG)

enum {
    SIGNAL_PUBLISHED,
    SIGNAL_PUBLISH_FAILED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void on_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_publish_clicked(GtkButton *btn, gpointer user_data);
static void set_publishing_state(GnostrChessPublishDialog *self, gboolean publishing, const gchar *status);
static void show_toast(GnostrChessPublishDialog *self, const gchar *message);

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gnostr_chess_publish_dialog_dispose(GObject *object)
{
    GnostrChessPublishDialog *self = GNOSTR_CHESS_PUBLISH_DIALOG(object);

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }

    G_OBJECT_CLASS(gnostr_chess_publish_dialog_parent_class)->dispose(object);
}

static void
gnostr_chess_publish_dialog_finalize(GObject *object)
{
    GnostrChessPublishDialog *self = GNOSTR_CHESS_PUBLISH_DIALOG(object);

    g_clear_pointer(&self->result_string, g_free);
    g_clear_pointer(&self->result_reason, g_free);
    g_clear_pointer(&self->white_name, g_free);
    g_clear_pointer(&self->black_name, g_free);

    G_OBJECT_CLASS(gnostr_chess_publish_dialog_parent_class)->finalize(object);
}

static void
gnostr_chess_publish_dialog_class_init(GnostrChessPublishDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_chess_publish_dialog_dispose;
    object_class->finalize = gnostr_chess_publish_dialog_finalize;

    /**
     * GnostrChessPublishDialog::published:
     * @self: The dialog
     * @event_id: The published event ID (hex)
     *
     * Emitted when the game is successfully published.
     */
    signals[SIGNAL_PUBLISHED] = g_signal_new(
        "published",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GnostrChessPublishDialog::publish-failed:
     * @self: The dialog
     * @error_message: Error description
     *
     * Emitted when publishing fails.
     */
    signals[SIGNAL_PUBLISH_FAILED] = g_signal_new(
        "publish-failed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_chess_publish_dialog_init(GnostrChessPublishDialog *self)
{
    self->session = NULL;
    self->is_publishing = FALSE;
    self->result_string = NULL;
    self->result_reason = NULL;
    self->white_name = NULL;
    self->black_name = NULL;

    /* Set dialog properties */
    adw_dialog_set_title(ADW_DIALOG(self), _("Publish Game to Nostr?"));
    adw_dialog_set_content_width(ADW_DIALOG(self), 380);
    adw_dialog_set_content_height(ADW_DIALOG(self), 360);

    /* Create main content box */
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Header bar */
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), TRUE);
    gtk_box_append(GTK_BOX(content), header);

    /* Main preferences page */
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);

    /* Result section */
    GtkWidget *result_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(result_group), _("Result"));

    self->lbl_result = gtk_label_new(_("Game in progress"));
    gtk_label_set_xalign(GTK_LABEL(self->lbl_result), 0.0);
    gtk_widget_add_css_class(self->lbl_result, "title-3");
    gtk_widget_set_margin_top(self->lbl_result, 4);
    gtk_widget_set_margin_bottom(self->lbl_result, 8);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(result_group), self->lbl_result);

    gtk_box_append(GTK_BOX(page), result_group);

    /* Game details section */
    GtkWidget *details_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(details_group), _("Game Details"));

    /* Event type row */
    GtkWidget *event_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(event_row), _("Event"));
    self->lbl_event_type = gtk_label_new(_("GNostr Chess Game"));
    gtk_widget_add_css_class(self->lbl_event_type, "dim-label");
    adw_action_row_add_suffix(ADW_ACTION_ROW(event_row), self->lbl_event_type);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(details_group), event_row);

    /* White player row */
    GtkWidget *white_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(white_row), _("White"));
    self->lbl_white = gtk_label_new(_("Human"));
    gtk_widget_add_css_class(self->lbl_white, "dim-label");
    adw_action_row_add_suffix(ADW_ACTION_ROW(white_row), self->lbl_white);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(details_group), white_row);

    /* Black player row */
    GtkWidget *black_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(black_row), _("Black"));
    self->lbl_black = gtk_label_new(_("AI (Intermediate)"));
    gtk_widget_add_css_class(self->lbl_black, "dim-label");
    adw_action_row_add_suffix(ADW_ACTION_ROW(black_row), self->lbl_black);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(details_group), black_row);

    gtk_box_append(GTK_BOX(page), details_group);

    /* Status box (hidden by default) */
    self->status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(self->status_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(self->status_box, 12);
    gtk_widget_set_visible(self->status_box, FALSE);

    self->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(self->status_box), self->spinner);

    self->lbl_status = gtk_label_new(_("Publishing..."));
    gtk_widget_add_css_class(self->lbl_status, "dim-label");
    gtk_box_append(GTK_BOX(self->status_box), self->lbl_status);

    gtk_box_append(GTK_BOX(page), self->status_box);

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(page), spacer);

    /* Action buttons */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(button_box, 16);

    self->btn_cancel = gtk_button_new_with_label(_("Cancel"));
    g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
    gtk_box_append(GTK_BOX(button_box), self->btn_cancel);

    self->btn_publish = gtk_button_new_with_label(_("Publish"));
    gtk_widget_add_css_class(self->btn_publish, "suggested-action");
    g_signal_connect(self->btn_publish, "clicked", G_CALLBACK(on_publish_clicked), self);
    gtk_box_append(GTK_BOX(button_box), self->btn_publish);

    gtk_box_append(GTK_BOX(page), button_box);

    gtk_box_append(GTK_BOX(content), page);
    adw_dialog_set_child(ADW_DIALOG(self), content);
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void
set_publishing_state(GnostrChessPublishDialog *self, gboolean publishing, const gchar *status)
{
    self->is_publishing = publishing;

    gtk_widget_set_visible(self->status_box, publishing);
    gtk_widget_set_sensitive(self->btn_publish, !publishing);
    gtk_widget_set_sensitive(self->btn_cancel, !publishing);
    gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), publishing);

    if (status) {
        gtk_label_set_text(GTK_LABEL(self->lbl_status), status);
    }
}

static void
show_toast(GnostrChessPublishDialog *self, const gchar *message)
{
    GtkWidget *parent = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_APPLICATION_WINDOW);
    if (parent && ADW_IS_APPLICATION_WINDOW(parent)) {
        AdwToast *toast = adw_toast_new(message);
        adw_toast_set_timeout(toast, 3);
        /* Find toast overlay in the window */
        GtkWidget *overlay = gtk_widget_get_first_child(parent);
        while (overlay) {
            if (ADW_IS_TOAST_OVERLAY(overlay)) {
                adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), toast);
                return;
            }
            overlay = gtk_widget_get_next_sibling(overlay);
        }
    }
    g_message("Chess: %s", message);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GnostrChessPublishDialog *self = GNOSTR_CHESS_PUBLISH_DIALOG(user_data);

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
    }

    adw_dialog_close(ADW_DIALOG(self));
}

/* Context for async publish operation */
typedef struct {
    GnostrChessPublishDialog *self;  /* weak ref */
    gchar *pgn_content;               /* owned */
} PublishContext;

static void
publish_context_free(PublishContext *ctx)
{
    if (!ctx) return;
    g_clear_pointer(&ctx->pgn_content, g_free);
    g_free(ctx);
}

/* Callback when signing completes */
static void
on_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
    PublishContext *ctx = (PublishContext*)user_data;
    (void)source;

    if (!ctx || !GNOSTR_IS_CHESS_PUBLISH_DIALOG(ctx->self)) {
        publish_context_free(ctx);
        return;
    }

    GnostrChessPublishDialog *self = ctx->self;
    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

    if (!ok || !signed_event_json) {
        gchar *msg = g_strdup_printf(_("Signing failed: %s"),
                                     error ? error->message : _("unknown error"));
        show_toast(self, msg);
        g_signal_emit(self, signals[SIGNAL_PUBLISH_FAILED], 0, msg);
        g_free(msg);
        g_clear_error(&error);
        set_publishing_state(self, FALSE, NULL);
        publish_context_free(ctx);
        return;
    }

    g_debug("[NIP-64] Signed chess event: %.100s...", signed_event_json);

    /* Parse the signed event */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json);
    if (parse_rc != 1) {
        show_toast(self, _("Failed to parse signed event"));
        g_signal_emit(self, signals[SIGNAL_PUBLISH_FAILED], 0, "Parse error");
        nostr_event_free(event);
        g_free(signed_event_json);
        set_publishing_state(self, FALSE, NULL);
        publish_context_free(ctx);
        return;
    }

    /* Get write relays and publish */
    set_publishing_state(self, TRUE, _("Publishing to relays..."));

    GPtrArray *write_relays = gnostr_get_write_relay_urls();
    if (!write_relays || write_relays->len == 0) {
        show_toast(self, _("No write relays configured"));
        g_signal_emit(self, signals[SIGNAL_PUBLISH_FAILED], 0, "No relays");
        nostr_event_free(event);
        g_free(signed_event_json);
        if (write_relays) g_ptr_array_unref(write_relays);
        set_publishing_state(self, FALSE, NULL);
        publish_context_free(ctx);
        return;
    }

    /* Publish to each write relay */
    guint success_count = 0;
    guint fail_count = 0;

    for (guint i = 0; i < write_relays->len; i++) {
        const char *url = (const char*)g_ptr_array_index(write_relays, i);
        if (!url || !*url) continue;

        GNostrRelay *relay = gnostr_relay_new(url);
        if (!relay) {
            fail_count++;
            continue;
        }

        GError *conn_err = NULL;
        if (!gnostr_relay_connect(relay, &conn_err)) {
            g_clear_error(&conn_err);
            g_object_unref(relay);
            fail_count++;
            continue;
        }

        GError *pub_err = NULL;
        if (gnostr_relay_publish(relay, event, &pub_err)) {
            success_count++;
        } else {
            g_clear_error(&pub_err);
            fail_count++;
        }
        g_object_unref(relay);
    }

    g_debug("[NIP-64] Published chess game to %u/%u relays", success_count, write_relays->len);

    /* Get event ID for signal */
    char *event_id = nostr_event_get_id(event);

    /* Cleanup */
    g_ptr_array_unref(write_relays);
    nostr_event_free(event);
    g_free(signed_event_json);

    if (success_count > 0) {
        show_toast(self, _("Chess game published to Nostr!"));
        g_signal_emit(self, signals[SIGNAL_PUBLISHED], 0, event_id ? event_id : "");

        /* Close dialog after short delay */
        g_timeout_add(1000, (GSourceFunc)adw_dialog_close, ADW_DIALOG(self));
    } else {
        show_toast(self, _("Failed to publish to relays"));
        g_signal_emit(self, signals[SIGNAL_PUBLISH_FAILED], 0, "Relay publish failed");
        set_publishing_state(self, FALSE, NULL);
    }

    g_free(event_id);
    publish_context_free(ctx);
}

static void
on_publish_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GnostrChessPublishDialog *self = GNOSTR_CHESS_PUBLISH_DIALOG(user_data);

    if (self->is_publishing)
        return;

    if (!self->session) {
        show_toast(self, _("No game session available"));
        return;
    }

    /* Check if signer is available */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        show_toast(self, _("Signer not available. Please log in."));
        return;
    }

    set_publishing_state(self, TRUE, _("Signing event..."));

    /* Export PGN from session */
    gchar *pgn = gnostr_chess_session_export_pgn(self->session);
    if (!pgn || !*pgn) {
        show_toast(self, _("No game data to publish"));
        set_publishing_state(self, FALSE, NULL);
        g_free(pgn);
        return;
    }

    g_debug("[NIP-64] Exporting PGN:\n%s", pgn);

    /* Build unsigned kind 64 event JSON */
    NostrJsonBuilder *builder = nostr_json_builder_new();
    nostr_json_builder_begin_object(builder);

    nostr_json_builder_set_key(builder, "kind");
    nostr_json_builder_add_int(builder, NOSTR_KIND_CHESS);

    nostr_json_builder_set_key(builder, "created_at");
    nostr_json_builder_add_int64(builder, (int64_t)time(NULL));

    nostr_json_builder_set_key(builder, "content");
    nostr_json_builder_add_string(builder, pgn);

    /* Build tags array */
    nostr_json_builder_set_key(builder, "tags");
    nostr_json_builder_begin_array(builder);

    /* ["t", "chess"] - topic tag */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "t");
    nostr_json_builder_add_string(builder, "chess");
    nostr_json_builder_end_array(builder);

    /* ["subject", "Chess Game"] */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "subject");
    nostr_json_builder_add_string(builder, "Chess Game");
    nostr_json_builder_end_array(builder);

    nostr_json_builder_end_array(builder);  /* end tags */
    nostr_json_builder_end_object(builder);

    char *event_json = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

    if (!event_json) {
        show_toast(self, _("Failed to create event"));
        set_publishing_state(self, FALSE, NULL);
        g_free(pgn);
        return;
    }

    g_debug("[NIP-64] Unsigned event: %s", event_json);

    /* Create async context */
    PublishContext *ctx = g_new0(PublishContext, 1);
    ctx->self = self;
    ctx->pgn_content = pgn;  /* Transfer ownership */

    /* Sign the event */
    gnostr_sign_event_async(
        event_json,
        "",         /* current_user: ignored */
        "gnostr",   /* app_id: ignored */
        NULL,       /* cancellable */
        on_sign_complete,
        ctx
    );

    g_free(event_json);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrChessPublishDialog *
gnostr_chess_publish_dialog_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHESS_PUBLISH_DIALOG, NULL);
}

void
gnostr_chess_publish_dialog_present(GnostrChessPublishDialog *self, GtkWidget *parent)
{
    g_return_if_fail(GNOSTR_IS_CHESS_PUBLISH_DIALOG(self));

    /* Reset state */
    set_publishing_state(self, FALSE, NULL);

    adw_dialog_present(ADW_DIALOG(self), parent);
}

void
gnostr_chess_publish_dialog_set_session(GnostrChessPublishDialog *self,
                                         GnostrChessSession *session)
{
    g_return_if_fail(GNOSTR_IS_CHESS_PUBLISH_DIALOG(self));

    self->session = session;  /* Not owned, caller maintains reference */
}

void
gnostr_chess_publish_dialog_set_result_info(GnostrChessPublishDialog *self,
                                             const gchar *result,
                                             const gchar *reason,
                                             const gchar *white_name,
                                             const gchar *black_name)
{
    g_return_if_fail(GNOSTR_IS_CHESS_PUBLISH_DIALOG(self));

    g_free(self->result_string);
    g_free(self->result_reason);
    g_free(self->white_name);
    g_free(self->black_name);

    self->result_string = g_strdup(result);
    self->result_reason = g_strdup(reason);
    self->white_name = g_strdup(white_name);
    self->black_name = g_strdup(black_name);

    /* Update labels */
    if (reason && *reason) {
        gtk_label_set_text(GTK_LABEL(self->lbl_result), reason);
    } else if (result && *result) {
        /* Format result string nicely */
        if (g_strcmp0(result, "1-0") == 0) {
            gtk_label_set_text(GTK_LABEL(self->lbl_result), _("White wins"));
        } else if (g_strcmp0(result, "0-1") == 0) {
            gtk_label_set_text(GTK_LABEL(self->lbl_result), _("Black wins"));
        } else if (g_strcmp0(result, "1/2-1/2") == 0) {
            gtk_label_set_text(GTK_LABEL(self->lbl_result), _("Draw"));
        } else {
            gtk_label_set_text(GTK_LABEL(self->lbl_result), result);
        }
    }

    if (white_name && *white_name) {
        gtk_label_set_text(GTK_LABEL(self->lbl_white), white_name);
    }

    if (black_name && *black_name) {
        gtk_label_set_text(GTK_LABEL(self->lbl_black), black_name);
    }
}
