/**
 * GnostrReportDialog - NIP-56 Report Dialog
 *
 * Dialog for reporting content/users per NIP-56.
 * Creates kind 1984 events with p-tag (user), e-tag (event), and report type tag.
 */

#include "gnostr-report-dialog.h"
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include "../util/relays.h"
#include <glib/gi18n.h>
#include "nostr_json.h"
#include <json.h>
#include "nostr_relay.h"
#include "nostr-event.h"
#include "../util/utils.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-report-dialog.ui"

/* NIP-56 kind for reporting */
#define NOSTR_KIND_REPORTING 1984

struct _GnostrReportDialog {
  GtkWindow parent_instance;

  /* Template children */
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
  GtkWidget *reason_list;
  GtkWidget *row_nudity;
  GtkWidget *row_malware;
  GtkWidget *row_profanity;
  GtkWidget *row_illegal;
  GtkWidget *row_spam;
  GtkWidget *row_impersonation;
  GtkWidget *row_other;
  GtkWidget *entry_comment;
  GtkWidget *status_box;
  GtkWidget *spinner;
  GtkWidget *lbl_status;
  GtkWidget *btn_submit;
  GtkWidget *lbl_submit_button;

  /* State */
  gchar *event_id_hex;
  gchar *pubkey_hex;
  GnostrReportType selected_type;
  gboolean is_processing;

  /* Async context */
  GCancellable *cancellable;

  /* nostrc-05yz (harden-8): Disposed flag to prevent async callbacks from
   * accessing template widgets after dispose. */
  gboolean disposed;
};

G_DEFINE_TYPE(GnostrReportDialog, gnostr_report_dialog, GTK_TYPE_WINDOW)

/* Signals */
enum {
  SIGNAL_REPORT_SENT,
  SIGNAL_REPORT_FAILED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_submit_clicked(GtkButton *btn, gpointer user_data);
static void on_reason_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void set_processing(GnostrReportDialog *self, gboolean processing, const gchar *status);

const gchar *gnostr_report_type_to_string(GnostrReportType type) {
  switch (type) {
    case GNOSTR_REPORT_TYPE_NUDITY:       return "nudity";
    case GNOSTR_REPORT_TYPE_MALWARE:      return "malware";
    case GNOSTR_REPORT_TYPE_PROFANITY:    return "profanity";
    case GNOSTR_REPORT_TYPE_ILLEGAL:      return "illegal";
    case GNOSTR_REPORT_TYPE_SPAM:         return "spam";
    case GNOSTR_REPORT_TYPE_IMPERSONATION: return "impersonation";
    case GNOSTR_REPORT_TYPE_OTHER:
    default:                              return "other";
  }
}

static gboolean hide_toast_timeout_cb(gpointer user_data) {
  gtk_revealer_set_reveal_child(GTK_REVEALER(user_data), FALSE);
  return G_SOURCE_REMOVE;
}

static void show_toast(GnostrReportDialog *self, const gchar *msg) {
  if (!self->toast_label || !self->toast_revealer) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  g_timeout_add_full(G_PRIORITY_DEFAULT, 3000, hide_toast_timeout_cb,
                     g_object_ref(self->toast_revealer), g_object_unref);
}

static void gnostr_report_dialog_dispose(GObject *obj) {
  GnostrReportDialog *self = GNOSTR_REPORT_DIALOG(obj);

  /* nostrc-05yz (harden-8): Mark disposed FIRST to prevent async callbacks
   * from accessing template widgets after dispose begins. */
  self->disposed = TRUE;

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_REPORT_DIALOG);
  G_OBJECT_CLASS(gnostr_report_dialog_parent_class)->dispose(obj);
}

static void gnostr_report_dialog_finalize(GObject *obj) {
  GnostrReportDialog *self = GNOSTR_REPORT_DIALOG(obj);

  g_clear_pointer(&self->event_id_hex, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);

  G_OBJECT_CLASS(gnostr_report_dialog_parent_class)->finalize(obj);
}

static void gnostr_report_dialog_class_init(GnostrReportDialogClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_report_dialog_dispose;
  gclass->finalize = gnostr_report_dialog_finalize;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, toast_revealer);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, toast_label);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, reason_list);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_nudity);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_malware);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_profanity);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_illegal);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_spam);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_impersonation);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, row_other);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, entry_comment);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, status_box);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, lbl_status);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, btn_submit);
  gtk_widget_class_bind_template_child(wclass, GnostrReportDialog, lbl_submit_button);

  /* Bind template callbacks */
  gtk_widget_class_bind_template_callback(wclass, on_cancel_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_submit_clicked);

  /**
   * GnostrReportDialog::report-sent:
   * @self: the report dialog
   * @event_id: the event that was reported
   * @report_type: the type of report (e.g., "spam", "nudity")
   */
  signals[SIGNAL_REPORT_SENT] = g_signal_new(
    "report-sent",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * GnostrReportDialog::report-failed:
   * @self: the report dialog
   * @error_message: error description
   */
  signals[SIGNAL_REPORT_FAILED] = g_signal_new(
    "report-failed",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_report_dialog_init(GnostrReportDialog *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->selected_type = GNOSTR_REPORT_TYPE_SPAM;  /* Default selection */
  self->is_processing = FALSE;

  /* Connect list row activation */
  g_signal_connect(self->reason_list, "row-activated",
                   G_CALLBACK(on_reason_row_activated), self);

  /* Select spam by default */
  if (GTK_IS_LIST_BOX(self->reason_list) && GTK_IS_LIST_BOX_ROW(self->row_spam)) {
    gtk_list_box_select_row(GTK_LIST_BOX(self->reason_list),
                            GTK_LIST_BOX_ROW(self->row_spam));
  }
}

GnostrReportDialog *gnostr_report_dialog_new(GtkWindow *parent) {
  GnostrReportDialog *self = g_object_new(GNOSTR_TYPE_REPORT_DIALOG,
                                          "transient-for", parent,
                                          "modal", TRUE,
                                          NULL);
  return self;
}

void gnostr_report_dialog_set_target(GnostrReportDialog *self,
                                      const gchar *event_id_hex,
                                      const gchar *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_REPORT_DIALOG(self));

  g_clear_pointer(&self->event_id_hex, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);

  self->event_id_hex = g_strdup(event_id_hex);
  self->pubkey_hex = g_strdup(pubkey_hex);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrReportDialog *self = GNOSTR_REPORT_DIALOG(user_data);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
  }

  gtk_window_close(GTK_WINDOW(self));
}

static void on_reason_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  GnostrReportDialog *self = GNOSTR_REPORT_DIALOG(user_data);

  if (row == GTK_LIST_BOX_ROW(self->row_nudity)) {
    self->selected_type = GNOSTR_REPORT_TYPE_NUDITY;
  } else if (row == GTK_LIST_BOX_ROW(self->row_malware)) {
    self->selected_type = GNOSTR_REPORT_TYPE_MALWARE;
  } else if (row == GTK_LIST_BOX_ROW(self->row_profanity)) {
    self->selected_type = GNOSTR_REPORT_TYPE_PROFANITY;
  } else if (row == GTK_LIST_BOX_ROW(self->row_illegal)) {
    self->selected_type = GNOSTR_REPORT_TYPE_ILLEGAL;
  } else if (row == GTK_LIST_BOX_ROW(self->row_spam)) {
    self->selected_type = GNOSTR_REPORT_TYPE_SPAM;
  } else if (row == GTK_LIST_BOX_ROW(self->row_impersonation)) {
    self->selected_type = GNOSTR_REPORT_TYPE_IMPERSONATION;
  } else if (row == GTK_LIST_BOX_ROW(self->row_other)) {
    self->selected_type = GNOSTR_REPORT_TYPE_OTHER;
  }
}

static void set_processing(GnostrReportDialog *self, gboolean processing, const gchar *status) {
  self->is_processing = processing;

  if (GTK_IS_WIDGET(self->status_box)) {
    gtk_widget_set_visible(self->status_box, processing);
  }
  if (GTK_IS_WIDGET(self->btn_submit)) {
    gtk_widget_set_sensitive(self->btn_submit, !processing);
  }
  if (GTK_IS_SPINNER(self->spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), processing);
  }
  if (GTK_IS_LABEL(self->lbl_status) && status) {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), status);
  }
}

/* Context for async report operation */
typedef struct {
  GnostrReportDialog *self;  /* weak ref */
  gchar *event_id_hex;       /* owned */
  gchar *report_type;        /* owned */
} ReportContext;

static void report_context_free(ReportContext *ctx) {
  if (!ctx) return;
  g_clear_pointer(&ctx->event_id_hex, g_free);
  g_clear_pointer(&ctx->report_type, g_free);
  g_free(ctx);
}

static void report_publish_done(guint success_count, guint fail_count, gpointer user_data);

/* Callback when unified signer service completes signing */
static void on_sign_report_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  ReportContext *ctx = (ReportContext*)user_data;
  (void)source;
  if (!ctx || !GNOSTR_IS_REPORT_DIALOG(ctx->self)) {
    report_context_free(ctx);
    return;
  }
  GnostrReportDialog *self = ctx->self;

  /* nostrc-05yz (harden-8): Bail if dialog was disposed while async was in-flight */
  if (self->disposed) {
    report_context_free(ctx);
    return;
  }

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    char *msg = g_strdup_printf("Report signing failed: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_signal_emit(self, signals[SIGNAL_REPORT_FAILED], 0, msg);
    g_free(msg);
    g_clear_error(&error);
    set_processing(self, FALSE, NULL);
    report_context_free(ctx);
    return;
  }

  g_debug("[NIP-56] Signed report event: %.100s...", signed_event_json);

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    show_toast(self, "Failed to parse signed report event");
    g_signal_emit(self, signals[SIGNAL_REPORT_FAILED], 0, "Failed to parse signed event");
    nostr_event_free(event);
    g_free(signed_event_json);
    set_processing(self, FALSE, NULL);
    report_context_free(ctx);
    return;
  }

  /* Get write relays and publish */
  GPtrArray *write_relays = gnostr_get_write_relay_urls();
  if (!write_relays || write_relays->len == 0) {
    show_toast(self, "No write relays configured");
    g_signal_emit(self, signals[SIGNAL_REPORT_FAILED], 0, "No write relays configured");
    nostr_event_free(event);
    g_free(signed_event_json);
    if (write_relays) g_ptr_array_unref(write_relays);
    set_processing(self, FALSE, NULL);
    report_context_free(ctx);
    return;
  }

  g_free(signed_event_json);

  /* hq-gflmf: Move connect+publish loop to background thread */
  gnostr_publish_to_relays_async(event, write_relays,
    report_publish_done, ctx);
}

/* Proper GSourceFunc wrapper for gtk_window_close (ABI-safe) */
static gboolean close_window_timeout_cb(gpointer data) {
  gtk_window_close(GTK_WINDOW(data));
  return G_SOURCE_REMOVE;
}

static void report_publish_done(guint success_count, guint fail_count, gpointer user_data) {
  ReportContext *ctx = (ReportContext *)user_data;
  if (!ctx) return;

  g_debug("[NIP-56] Published report to %u relays, failed %u", success_count, fail_count);

  if (GNOSTR_IS_REPORT_DIALOG(ctx->self)) {
    GnostrReportDialog *self = ctx->self;

    /* nostrc-05yz (harden-8): Bail if dialog was disposed while publish was in-flight */
    if (self->disposed) {
      report_context_free(ctx);
      return;
    }

    if (success_count > 0) {
      show_toast(self, "Report submitted successfully");
      g_signal_emit(self, signals[SIGNAL_REPORT_SENT], 0, ctx->event_id_hex, ctx->report_type);
      g_timeout_add_full(G_PRIORITY_DEFAULT, 1500, close_window_timeout_cb,
                         g_object_ref(GTK_WINDOW(self)), g_object_unref);
    } else {
      show_toast(self, "Failed to submit report");
    }
    set_processing(self, FALSE, NULL);
  }

  report_context_free(ctx);
}

static void on_submit_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrReportDialog *self = GNOSTR_REPORT_DIALOG(user_data);

  if (self->is_processing) return;

  /* Validate we have at least a pubkey to report */
  if (!self->pubkey_hex || strlen(self->pubkey_hex) != 64) {
    show_toast(self, "Invalid target for report");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available");
    return;
  }

  set_processing(self, TRUE, "Submitting report...");

  /* Get selected report type string */
  const gchar *report_type = gnostr_report_type_to_string(self->selected_type);

  /* Get optional comment */
  const gchar *comment = "";
  if (GTK_IS_EDITABLE(self->entry_comment)) {
    comment = gtk_editable_get_text(GTK_EDITABLE(self->entry_comment));
    if (!comment) comment = "";
  }

  /* Build unsigned kind 1984 report event JSON per NIP-56 */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NOSTR_KIND_REPORTING);

  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, comment);

  /* Build tags array per NIP-56:
   * - ["p", "<pubkey>", "<report-type>"] - report user with reason
   * - ["e", "<event-id>", "<report-type>"] - report event with reason (optional)
   */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* p-tag: ["p", "<pubkey>", "<report-type>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "p");
  gnostr_json_builder_add_string(builder, self->pubkey_hex);
  gnostr_json_builder_add_string(builder, report_type);
  gnostr_json_builder_end_array(builder);

  /* e-tag if event ID provided: ["e", "<event-id>", "<report-type>"] */
  if (self->event_id_hex && strlen(self->event_id_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, self->event_id_hex);
    gnostr_json_builder_add_string(builder, report_type);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to serialize report event");
    set_processing(self, FALSE, NULL);
    return;
  }

  g_debug("[NIP-56] Unsigned report event: %s", event_json);

  /* Create async context */
  ReportContext *ctx = g_new0(ReportContext, 1);
  ctx->self = self;
  ctx->event_id_hex = g_strdup(self->event_id_hex);
  ctx->report_type = g_strdup(report_type);

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_report_complete,
    ctx
  );
  g_free(event_json);
}
