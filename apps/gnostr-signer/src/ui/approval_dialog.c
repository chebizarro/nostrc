/**
 * GnostrApprovalDialog - Modern AdwDialog-based approval dialog for event signing requests
 *
 * Features:
 * - Request header with event type icon
 * - Event metadata display (Event Type, From app, Identity, Timestamp)
 * - Event content preview with truncation and expand option
 * - Approve button (primary/suggested style)
 * - Deny button (destructive style)
 * - Remember decision with TTL options
 * - Identity selector for multiple accounts
 */

#include <adwaita.h>
#include <gtk/gtk.h>
#include <time.h>
#include "../accounts_store.h"

// Callback signature for decision results
// decision: TRUE=Approve, FALSE=Deny; remember: TRUE to persist policy;
// selected_identity may be NULL; ttl_seconds: 0=Forever or no TTL
typedef void (*GnostrApprovalCallback)(gboolean decision, gboolean remember,
                                       const char *selected_identity,
                                       guint64 ttl_seconds, gpointer user_data);

#define GNOSTR_TYPE_APPROVAL_DIALOG (gnostr_approval_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrApprovalDialog, gnostr_approval_dialog, GNOSTR,
                     APPROVAL_DIALOG, AdwDialog)

struct _GnostrApprovalDialog {
  AdwDialog parent_instance;

  /* Template widgets */
  GtkImage *header_icon;
  GtkLabel *header_title;
  AdwActionRow *row_event_type;
  AdwActionRow *row_app;
  AdwActionRow *row_identity;
  AdwActionRow *row_timestamp;
  GtkImage *event_type_icon;
  GtkLabel *content_preview;
  GtkButton *btn_expand;
  GtkFrame *content_frame;
  GtkBox *identity_selector_box;
  GtkDropDown *identity_dropdown;
  GtkCheckButton *chk_remember;
  GtkDropDown *ttl_dropdown;
  GtkButton *btn_deny;
  GtkButton *btn_approve;

  /* State */
  GnostrApprovalCallback callback;
  gpointer user_data;
  GtkStringList *identity_model;
  gchar *full_content;
  gboolean content_expanded;

  /* Session integration */
  gchar *client_pubkey;    /* Client's public key for session management */
};

G_DEFINE_FINAL_TYPE(GnostrApprovalDialog, gnostr_approval_dialog, ADW_TYPE_DIALOG)

/* Content preview length before truncation */
#define PREVIEW_MAX_CHARS 200

static const char *get_event_type_name(int kind) {
  switch (kind) {
  case 0:
    return "Metadata";
  case 1:
    return "Short Text Note";
  case 2:
    return "Recommend Relay";
  case 3:
    return "Contacts";
  case 4:
    return "Encrypted Direct Message";
  case 5:
    return "Event Deletion";
  case 6:
    return "Repost";
  case 7:
    return "Reaction";
  case 8:
    return "Badge Award";
  case 40:
    return "Channel Creation";
  case 41:
    return "Channel Metadata";
  case 42:
    return "Channel Message";
  case 43:
    return "Channel Hide Message";
  case 44:
    return "Channel Mute User";
  case 1984:
    return "Reporting";
  case 9734:
    return "Zap Request";
  case 9735:
    return "Zap";
  case 10000:
    return "Mute List";
  case 10001:
    return "Pin List";
  case 10002:
    return "Relay List Metadata";
  case 22242:
    return "Client Authentication";
  case 24133:
    return "Nostr Connect";
  case 30000:
    return "Categorized People List";
  case 30001:
    return "Categorized Bookmark List";
  case 30008:
    return "Profile Badges";
  case 30009:
    return "Badge Definition";
  case 30023:
    return "Long-form Content";
  case 30078:
    return "Application-specific Data";
  default:
    if (kind >= 10000 && kind < 20000)
      return "Replaceable Event";
    if (kind >= 20000 && kind < 30000)
      return "Ephemeral Event";
    if (kind >= 30000 && kind < 40000)
      return "Parameterized Replaceable Event";
    return "Unknown Event";
  }
}

static const char *get_event_type_icon(int kind) {
  switch (kind) {
  case 0:
    return "user-info-symbolic";
  case 1:
    return "edit-symbolic";
  case 3:
    return "contact-new-symbolic";
  case 4:
    return "mail-send-symbolic";
  case 5:
    return "user-trash-symbolic";
  case 6:
    return "emblem-shared-symbolic";
  case 7:
    return "emblem-favorite-symbolic";
  case 9734:
  case 9735:
    return "starred-symbolic";
  case 22242:
    return "dialog-password-symbolic";
  case 24133:
    return "network-server-symbolic";
  case 30023:
    return "x-office-document-symbolic";
  default:
    return "mail-unread-symbolic";
  }
}

static void on_remember_toggled(GtkCheckButton *btn, gpointer user_data) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(user_data);
  gboolean active = gtk_check_button_get_active(btn);
  gtk_widget_set_sensitive(GTK_WIDGET(self->ttl_dropdown), active);
}

static void on_expand_clicked(GtkButton *btn, gpointer user_data) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(user_data);
  (void)btn;

  self->content_expanded = !self->content_expanded;

  if (self->content_expanded) {
    gtk_label_set_text(self->content_preview, self->full_content);
    gtk_label_set_ellipsize(self->content_preview, PANGO_ELLIPSIZE_NONE);
    gtk_label_set_lines(self->content_preview, -1);
    gtk_button_set_label(self->btn_expand, "Show Less");
  } else {
    /* Re-truncate */
    if (self->full_content && strlen(self->full_content) > PREVIEW_MAX_CHARS) {
      gchar *truncated = g_strndup(self->full_content, PREVIEW_MAX_CHARS);
      gchar *display = g_strdup_printf("%s...", truncated);
      gtk_label_set_text(self->content_preview, display);
      g_free(truncated);
      g_free(display);
    }
    gtk_label_set_ellipsize(self->content_preview, PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(self->content_preview, 3);
    gtk_button_set_label(self->btn_expand, "Show More");
  }
}

static void do_finish(GnostrApprovalDialog *self, gboolean decision) {
  gboolean remember = gtk_check_button_get_active(self->chk_remember);
  guint64 ttl_seconds = 0;

  if (remember) {
    guint idx = gtk_drop_down_get_selected(self->ttl_dropdown);
    switch ((int)idx) {
    case 0:
      ttl_seconds = 600;
      break; /* 10 minutes */
    case 1:
      ttl_seconds = 3600;
      break; /* 1 hour */
    case 2:
      ttl_seconds = 86400;
      break; /* 24 hours */
    case 3:
    default:
      ttl_seconds = 0;
      break; /* Forever */
    }
  }

  const char *selected = NULL;
  if (self->identity_model) {
    guint idx = gtk_drop_down_get_selected(self->identity_dropdown);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->identity_model));
    if (n > 0) {
      if (idx >= n)
        idx = 0;
      selected = gtk_string_list_get_string(self->identity_model, idx);
    }
  }

  if (self->callback) {
    self->callback(decision, remember, selected, ttl_seconds, self->user_data);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_approve_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  do_finish(GNOSTR_APPROVAL_DIALOG(user_data), TRUE);
}

static void on_deny_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  do_finish(GNOSTR_APPROVAL_DIALOG(user_data), FALSE);
}

static void on_dialog_closed(AdwDialog *dialog) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(dialog);

  /* Treat close as denial if not already handled */
  if (self->callback) {
    self->callback(FALSE, FALSE, NULL, 0, self->user_data);
    self->callback = NULL; /* Prevent double-call */
  }

  ADW_DIALOG_CLASS(gnostr_approval_dialog_parent_class)->closed(dialog);
}

static void gnostr_approval_dialog_dispose(GObject *object) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(object);

  g_clear_pointer(&self->full_content, g_free);
  g_clear_pointer(&self->client_pubkey, g_free);
  g_clear_object(&self->identity_model);

  G_OBJECT_CLASS(gnostr_approval_dialog_parent_class)->dispose(object);
}

static void gnostr_approval_dialog_class_init(GnostrApprovalDialogClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  AdwDialogClass *dialog_class = ADW_DIALOG_CLASS(klass);

  object_class->dispose = gnostr_approval_dialog_dispose;
  dialog_class->closed = on_dialog_closed;

  gtk_widget_class_set_template_from_resource(
      widget_class, "/org/gnostr/signer/ui/approval-dialog.ui");

  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       header_icon);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       header_title);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       row_event_type);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       row_app);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       row_identity);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       row_timestamp);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       event_type_icon);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       content_preview);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       btn_expand);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       content_frame);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       identity_selector_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       identity_dropdown);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       chk_remember);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       ttl_dropdown);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       btn_deny);
  gtk_widget_class_bind_template_child(widget_class, GnostrApprovalDialog,
                                       btn_approve);
}

/* Callback for Ctrl+A keyboard shortcut (Approve) */
static gboolean on_shortcut_approve(GtkWidget *widget, GVariant *args, gpointer user_data) {
  (void)widget; (void)args; (void)user_data;
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(widget);
  if (gtk_widget_get_sensitive(GTK_WIDGET(self->btn_approve))) {
    do_finish(self, TRUE);
  }
  return TRUE;
}

/* Callback for Ctrl+D keyboard shortcut (Deny) */
static gboolean on_shortcut_deny(GtkWidget *widget, GVariant *args, gpointer user_data) {
  (void)widget; (void)args; (void)user_data;
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(widget);
  do_finish(self, FALSE);
  return TRUE;
}

static void gnostr_approval_dialog_init(GnostrApprovalDialog *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->content_expanded = FALSE;
  self->full_content = NULL;
  self->identity_model = NULL;
  self->callback = NULL;
  self->user_data = NULL;

  /* Setup TTL dropdown model */
  GtkStringList *ttl_model = gtk_string_list_new(NULL);
  gtk_string_list_append(ttl_model, "10 minutes");
  gtk_string_list_append(ttl_model, "1 hour");
  gtk_string_list_append(ttl_model, "24 hours");
  gtk_string_list_append(ttl_model, "Forever");
  gtk_drop_down_set_model(self->ttl_dropdown, G_LIST_MODEL(ttl_model));
  gtk_drop_down_set_selected(self->ttl_dropdown, 0);
  g_object_unref(ttl_model);

  /* Connect signals */
  g_signal_connect(self->chk_remember, "toggled",
                   G_CALLBACK(on_remember_toggled), self);
  g_signal_connect(self->btn_expand, "clicked", G_CALLBACK(on_expand_clicked),
                   self);
  g_signal_connect(self->btn_approve, "clicked", G_CALLBACK(on_approve_clicked),
                   self);
  g_signal_connect(self->btn_deny, "clicked", G_CALLBACK(on_deny_clicked),
                   self);

  /* Setup keyboard shortcuts using GtkShortcutController */
  GtkEventController *shortcut_ctrl = gtk_shortcut_controller_new();
  gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl), GTK_SHORTCUT_SCOPE_LOCAL);

  /* Ctrl+A: Approve */
  GtkShortcutTrigger *approve_trigger = gtk_shortcut_trigger_parse_string("<Primary>a");
  GtkShortcutAction *approve_action = gtk_callback_action_new(on_shortcut_approve, NULL, NULL);
  GtkShortcut *approve_shortcut = gtk_shortcut_new(approve_trigger, approve_action);
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl), approve_shortcut);

  /* Ctrl+D: Deny */
  GtkShortcutTrigger *deny_trigger = gtk_shortcut_trigger_parse_string("<Primary>d");
  GtkShortcutAction *deny_action = gtk_callback_action_new(on_shortcut_deny, NULL, NULL);
  GtkShortcut *deny_shortcut = gtk_shortcut_new(deny_trigger, deny_action);
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl), deny_shortcut);

  gtk_widget_add_controller(GTK_WIDGET(self), shortcut_ctrl);
}

/**
 * gnostr_approval_dialog_new:
 *
 * Creates a new approval dialog.
 *
 * Returns: (transfer full): a new #GnostrApprovalDialog
 */
GnostrApprovalDialog *gnostr_approval_dialog_new(void) {
  return g_object_new(GNOSTR_TYPE_APPROVAL_DIALOG, NULL);
}

/**
 * gnostr_approval_dialog_set_event_type:
 * @self: a #GnostrApprovalDialog
 * @kind: the Nostr event kind number
 *
 * Sets the event type display based on the kind number.
 */
void gnostr_approval_dialog_set_event_type(GnostrApprovalDialog *self, int kind) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  const char *type_name = get_event_type_name(kind);
  const char *icon_name = get_event_type_icon(kind);

  gchar *subtitle = g_strdup_printf("%s (kind %d)", type_name, kind);
  adw_action_row_set_subtitle(self->row_event_type, subtitle);
  g_free(subtitle);

  gtk_image_set_from_icon_name(self->event_type_icon, icon_name);
  gtk_image_set_from_icon_name(self->header_icon, icon_name);
}

/**
 * gnostr_approval_dialog_set_app_name:
 * @self: a #GnostrApprovalDialog
 * @app_name: the requesting application name
 *
 * Sets the requesting application name.
 */
void gnostr_approval_dialog_set_app_name(GnostrApprovalDialog *self,
                                         const char *app_name) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  adw_action_row_set_subtitle(self->row_app,
                              app_name ? app_name : "Unknown Application");

  gchar *title = g_strdup_printf("%s requests signature",
                                 app_name ? app_name : "Application");
  gtk_label_set_text(self->header_title, title);
  g_free(title);
}

/**
 * gnostr_approval_dialog_set_identity:
 * @self: a #GnostrApprovalDialog
 * @identity_npub: the identity npub string
 *
 * Sets the identity display.
 */
void gnostr_approval_dialog_set_identity(GnostrApprovalDialog *self,
                                         const char *identity_npub) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  if (identity_npub && *identity_npub) {
    /* Truncate long npub for display */
    if (strlen(identity_npub) > 20) {
      gchar *truncated = g_strdup_printf("%.12s...%.8s", identity_npub,
                                         identity_npub + strlen(identity_npub) - 8);
      adw_action_row_set_subtitle(self->row_identity, truncated);
      g_free(truncated);
    } else {
      adw_action_row_set_subtitle(self->row_identity, identity_npub);
    }
  } else {
    adw_action_row_set_subtitle(self->row_identity, "Not specified");
  }
}

/**
 * gnostr_approval_dialog_set_timestamp:
 * @self: a #GnostrApprovalDialog
 * @timestamp: Unix timestamp, or 0 for current time
 *
 * Sets the timestamp display.
 */
void gnostr_approval_dialog_set_timestamp(GnostrApprovalDialog *self,
                                          guint64 timestamp) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  time_t t = (timestamp > 0) ? (time_t)timestamp : time(NULL);
  struct tm *tm_info = localtime(&t);
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);

  adw_action_row_set_subtitle(self->row_timestamp, buffer);
}

/**
 * gnostr_approval_dialog_set_content:
 * @self: a #GnostrApprovalDialog
 * @content: the event content to preview
 *
 * Sets the content preview. Long content will be truncated with an expand option.
 */
void gnostr_approval_dialog_set_content(GnostrApprovalDialog *self,
                                        const char *content) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  g_clear_pointer(&self->full_content, g_free);

  if (!content || !*content) {
    gtk_label_set_text(self->content_preview, "(No content)");
    gtk_widget_set_visible(GTK_WIDGET(self->btn_expand), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->content_frame), FALSE);
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(self->content_frame), TRUE);
  self->full_content = g_strdup(content);

  if (strlen(content) > PREVIEW_MAX_CHARS) {
    gchar *truncated = g_strndup(content, PREVIEW_MAX_CHARS);
    gchar *display = g_strdup_printf("%s...", truncated);
    gtk_label_set_text(self->content_preview, display);
    g_free(truncated);
    g_free(display);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_expand), TRUE);
  } else {
    gtk_label_set_text(self->content_preview, content);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_expand), FALSE);
  }
}

/**
 * gnostr_approval_dialog_set_accounts:
 * @self: a #GnostrApprovalDialog
 * @as: the accounts store
 * @selected_npub: the initially selected npub, or NULL
 *
 * Populates the identity dropdown with available accounts.
 */
void gnostr_approval_dialog_set_accounts(GnostrApprovalDialog *self,
                                         AccountsStore *as,
                                         const char *selected_npub) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  g_clear_object(&self->identity_model);
  self->identity_model = gtk_string_list_new(NULL);

  guint selected_idx = 0;
  guint count = 0;

  if (as) {
    GPtrArray *items = accounts_store_list(as);
    if (items) {
      for (guint i = 0; i < items->len; i++) {
        AccountEntry *e = g_ptr_array_index(items, i);
        gtk_string_list_append(self->identity_model, e->id);
        if (selected_npub && g_strcmp0(selected_npub, e->id) == 0) {
          selected_idx = count;
        }
        count++;
        g_free(e->id);
        g_free(e->label);
        g_free(e);
      }
      g_ptr_array_free(items, TRUE);
    }
  }

  /* If no accounts but we have a selected npub, add it */
  if (count == 0 && selected_npub && *selected_npub) {
    gtk_string_list_append(self->identity_model, selected_npub);
    count = 1;
    selected_idx = 0;
  }

  gtk_drop_down_set_model(self->identity_dropdown,
                          G_LIST_MODEL(self->identity_model));
  gtk_drop_down_set_selected(self->identity_dropdown, selected_idx);

  /* Show selector only if multiple accounts */
  gtk_widget_set_visible(GTK_WIDGET(self->identity_selector_box), count > 1);

  /* Disable approve if no identity available */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), count > 0);
}

/**
 * gnostr_approval_dialog_set_callback:
 * @self: a #GnostrApprovalDialog
 * @callback: the callback function
 * @user_data: user data for the callback
 *
 * Sets the callback to be invoked when the user makes a decision.
 */
void gnostr_approval_dialog_set_callback(GnostrApprovalDialog *self,
                                         GnostrApprovalCallback callback,
                                         gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  self->callback = callback;
  self->user_data = user_data;
}

/**
 * gnostr_show_approval_dialog:
 * @parent: the parent widget
 * @identity_npub: the requesting identity npub
 * @app_name: the requesting application name
 * @preview: the event content preview
 * @as: the accounts store
 * @cb: callback for the decision
 * @user_data: user data for callback
 *
 * Convenience function to show an approval dialog.
 * This is the legacy API maintained for compatibility.
 */
void gnostr_show_approval_dialog(GtkWidget *parent, const char *identity_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as, GnostrApprovalCallback cb,
                                 gpointer user_data) {
  GnostrApprovalDialog *dialog = gnostr_approval_dialog_new();

  gnostr_approval_dialog_set_app_name(dialog, app_name);
  gnostr_approval_dialog_set_identity(dialog, identity_npub);
  gnostr_approval_dialog_set_content(dialog, preview);
  gnostr_approval_dialog_set_timestamp(dialog, 0);
  gnostr_approval_dialog_set_event_type(dialog, 1); /* Default to text note */
  gnostr_approval_dialog_set_accounts(dialog, as, identity_npub);
  gnostr_approval_dialog_set_callback(dialog, cb, user_data);

  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

/**
 * gnostr_show_approval_dialog_full:
 * @parent: the parent widget
 * @identity_npub: the requesting identity npub
 * @app_name: the requesting application name
 * @content: the event content
 * @event_kind: the Nostr event kind
 * @timestamp: the event timestamp (0 for current time)
 * @as: the accounts store
 * @cb: callback for the decision
 * @user_data: user data for callback
 *
 * Full-featured approval dialog with all event metadata.
 */
void gnostr_show_approval_dialog_full(GtkWidget *parent,
                                      const char *identity_npub,
                                      const char *app_name, const char *content,
                                      int event_kind, guint64 timestamp,
                                      AccountsStore *as,
                                      GnostrApprovalCallback cb,
                                      gpointer user_data) {
  GnostrApprovalDialog *dialog = gnostr_approval_dialog_new();

  gnostr_approval_dialog_set_app_name(dialog, app_name);
  gnostr_approval_dialog_set_identity(dialog, identity_npub);
  gnostr_approval_dialog_set_content(dialog, content);
  gnostr_approval_dialog_set_timestamp(dialog, timestamp);
  gnostr_approval_dialog_set_event_type(dialog, event_kind);
  gnostr_approval_dialog_set_accounts(dialog, as, identity_npub);
  gnostr_approval_dialog_set_callback(dialog, cb, user_data);

  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

/**
 * gnostr_approval_dialog_set_client_pubkey:
 * @self: a #GnostrApprovalDialog
 * @client_pubkey: the client's public key (hex format)
 *
 * Sets the client public key for session management integration.
 */
void gnostr_approval_dialog_set_client_pubkey(GnostrApprovalDialog *self,
                                              const char *client_pubkey) {
  g_return_if_fail(GNOSTR_IS_APPROVAL_DIALOG(self));

  g_free(self->client_pubkey);
  self->client_pubkey = g_strdup(client_pubkey);
}

/**
 * gnostr_show_approval_dialog_with_session:
 *
 * Shows approval dialog with session management integration.
 * If an active session exists for the client+identity, this may
 * auto-approve based on session state.
 *
 * Returns: %TRUE if dialog was shown, %FALSE if auto-approved by session
 */
gboolean gnostr_show_approval_dialog_with_session(GtkWidget *parent,
                                                  const char *client_pubkey,
                                                  const char *identity_npub,
                                                  const char *app_name,
                                                  const char *content,
                                                  int event_kind,
                                                  guint64 timestamp,
                                                  AccountsStore *as,
                                                  GnostrApprovalCallback cb,
                                                  gpointer user_data) {
  /* Include client_session.h to check for existing sessions */
  /* Note: This creates a dependency - the bunker_service should
   * call this instead of directly showing approval dialogs */

  /* For now, just show the dialog with client pubkey set */
  GnostrApprovalDialog *dialog = gnostr_approval_dialog_new();

  gnostr_approval_dialog_set_client_pubkey(dialog, client_pubkey);
  gnostr_approval_dialog_set_app_name(dialog, app_name);
  gnostr_approval_dialog_set_identity(dialog, identity_npub);
  gnostr_approval_dialog_set_content(dialog, content);
  gnostr_approval_dialog_set_timestamp(dialog, timestamp);
  gnostr_approval_dialog_set_event_type(dialog, event_kind);
  gnostr_approval_dialog_set_accounts(dialog, as, identity_npub);
  gnostr_approval_dialog_set_callback(dialog, cb, user_data);

  adw_dialog_present(ADW_DIALOG(dialog), parent);

  return TRUE;  /* Dialog was shown */
}
