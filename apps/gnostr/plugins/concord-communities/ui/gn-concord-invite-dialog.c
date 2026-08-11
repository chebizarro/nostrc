#include "gn-concord-invite-dialog.h"

#include <nip_concord.h>

#include <string.h>

struct _GnConcordInviteDialog {
  GtkWindow parent_instance;
  GnConcordCommunityService *service;
  gchar *community_id;
  GtkEntry *label_entry;
  GtkWidget *create_button;
  GtkWidget *link_list;
  GtkLabel *status;
  GCancellable *cancellable;
  gulong service_updated;
};

G_DEFINE_TYPE(GnConcordInviteDialog, gn_concord_invite_dialog, GTK_TYPE_WINDOW)

static void refresh_links(GnConcordInviteDialog *self);

static void set_status(GnConcordInviteDialog *self, const char *message,
                       gboolean error) {
  gtk_label_set_text(self->status, message ? message : "");
  if (error) gtk_widget_add_css_class(GTK_WIDGET(self->status), "error");
  else gtk_widget_remove_css_class(GTK_WIDGET(self->status), "error");
}

/* CORD-05 §5: minting and retiring are gated by CREATE_INVITE, which the
 * Control Plane fold resolves. Showing the gate beats failing at it. */
static gboolean may_create(GnConcordInviteDialog *self) {
  const char *me =
    gn_concord_community_service_get_current_pubkey(self->service);
  if (!me) return FALSE;
  guint64 permissions = gn_concord_community_service_get_permissions(
    self->service, self->community_id, me);
  return (permissions & CONCORD_PERM_CREATE_INVITE) != 0;
}

static void copy_to_clipboard(GtkWidget *widget, const char *text) {
  GdkClipboard *clipboard = gtk_widget_get_clipboard(widget);
  if (clipboard && text) gdk_clipboard_set_text(clipboard, text);
}

/* --- minting --- */

static void on_invite_created(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(user_data);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *url = gn_concord_community_service_create_invite_finish(
    self->service, result, &error);
  g_clear_object(&self->cancellable);
  gtk_widget_set_sensitive(self->create_button, may_create(self));

  if (url) {
    /* The fragment is the token's only written-down form outside the Invite
     * List, so the link goes straight to the clipboard: a user who never
     * copies it has minted a link nobody can follow. */
    copy_to_clipboard(GTK_WIDGET(self), url);
    gtk_editable_set_text(GTK_EDITABLE(self->label_entry), "");
    set_status(self, "Link created and copied. Anyone holding it can join "
                     "until you retire it.", FALSE);
  } else {
    set_status(self, error ? error->message : "The link could not be created",
               TRUE);
  }
  refresh_links(self);
  g_object_unref(self);
}

static void on_create_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(user_data);
  const char *label = gtk_editable_get_text(GTK_EDITABLE(self->label_entry));

  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();
  gtk_widget_set_sensitive(self->create_button, FALSE);
  set_status(self, "Publishing the bundle, then listing it in this "
                   "Community's Registry…", FALSE);
  gn_concord_community_service_create_invite_async(
    self->service, self->community_id, label && *label ? label : NULL, 0,
    self->cancellable, on_invite_created, g_object_ref(self));
}

/* --- retiring --- */

static void on_invite_revoked(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(user_data);
  g_autoptr(GError) error = NULL;
  gboolean ok = gn_concord_community_service_revoke_invite_finish(
    self->service, result, &error);
  g_clear_object(&self->cancellable);
  set_status(self,
             ok ? "Link retired. Its coordinate now holds a tombstone, so "
                  "anyone following it finds the grave instead of keys."
                : (error ? error->message : "The link could not be retired"),
             !ok);
  refresh_links(self);
  g_object_unref(self);
}

static void on_revoke_clicked(GtkButton *button, gpointer user_data) {
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(user_data);
  const char *token = g_object_get_data(G_OBJECT(button), "concord-token");
  if (!token) return;

  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();
  set_status(self, "Publishing the revocation tombstone…", FALSE);
  gn_concord_community_service_revoke_invite_async(
    self->service, self->community_id, token, self->cancellable,
    on_invite_revoked, g_object_ref(self));
}

static void on_copy_clicked(GtkButton *button, gpointer user_data) {
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(user_data);
  const char *url = g_object_get_data(G_OBJECT(button), "concord-url");
  copy_to_clipboard(GTK_WIDGET(self), url);
  set_status(self, "Link copied.", FALSE);
}

/* --- the live list --- */

static GtkWidget *build_row(GnConcordInviteDialog *self,
                            const GnConcordInviteLink *link) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(row, 8);
  gtk_widget_set_margin_end(row, 8);
  gtk_widget_set_margin_top(row, 6);
  gtk_widget_set_margin_bottom(row, 6);

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text, TRUE);

  GtkWidget *name =
    gtk_label_new(link->label && *link->label ? link->label : "Unnamed link");
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_widget_add_css_class(name, "heading");
  gtk_box_append(GTK_BOX(text), name);

  GtkWidget *url = gtk_label_new(link->url);
  gtk_label_set_xalign(GTK_LABEL(url), 0);
  gtk_label_set_ellipsize(GTK_LABEL(url), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_selectable(GTK_LABEL(url), TRUE);
  gtk_widget_add_css_class(url, "dim-label");
  gtk_box_append(GTK_BOX(text), url);
  gtk_box_append(GTK_BOX(row), text);

  GtkWidget *copy = gtk_button_new_with_label("Copy");
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  g_object_set_data_full(G_OBJECT(copy), "concord-url", g_strdup(link->url),
                         g_free);
  g_signal_connect(copy, "clicked", G_CALLBACK(on_copy_clicked), self);
  gtk_box_append(GTK_BOX(row), copy);

  GtkWidget *revoke = gtk_button_new_with_label("Retire");
  gtk_widget_set_valign(revoke, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(revoke, "destructive-action");
  gtk_widget_set_sensitive(revoke, may_create(self));
  g_object_set_data_full(G_OBJECT(revoke), "concord-token",
                         g_strdup(link->token), g_free);
  g_signal_connect(revoke, "clicked", G_CALLBACK(on_revoke_clicked), self);
  gtk_box_append(GTK_BOX(row), revoke);

  return row;
}

static void refresh_links(GnConcordInviteDialog *self) {
  GtkWidget *child = gtk_widget_get_first_child(self->link_list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->link_list), child);
    child = next;
  }

  g_autoptr(GPtrArray) links =
    gn_concord_community_service_get_invites(self->service,
                                             self->community_id);
  if (!links || links->len == 0) {
    GtkWidget *empty = gtk_label_new(
      "No live links. A Community with none is Private: membership grows by "
      "personal handoff until you mint one.");
    gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
    gtk_label_set_xalign(GTK_LABEL(empty), 0);
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_widget_set_margin_start(empty, 8);
    gtk_widget_set_margin_end(empty, 8);
    gtk_widget_set_margin_top(empty, 8);
    gtk_box_append(GTK_BOX(self->link_list), empty);
    return;
  }

  for (guint i = 0; i < links->len; i++)
    gtk_box_append(GTK_BOX(self->link_list),
                   build_row(self, g_ptr_array_index(links, i)));
}

static void on_service_updated(GnConcordCommunityService *service,
                               const char *community_id, guint flags,
                               gpointer user_data) {
  (void)service;
  (void)flags;
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(user_data);
  if (g_strcmp0(community_id, self->community_id) != 0) return;
  refresh_links(self);
}

/* --- lifecycle --- */

static void gn_concord_invite_dialog_dispose(GObject *object) {
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(object);
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  if (self->service && self->service_updated)
    g_signal_handler_disconnect(self->service, self->service_updated);
  self->service_updated = 0;
  g_clear_object(&self->service);
  G_OBJECT_CLASS(gn_concord_invite_dialog_parent_class)->dispose(object);
}

static void gn_concord_invite_dialog_finalize(GObject *object) {
  GnConcordInviteDialog *self = GN_CONCORD_INVITE_DIALOG(object);
  g_free(self->community_id);
  G_OBJECT_CLASS(gn_concord_invite_dialog_parent_class)->finalize(object);
}

static void gn_concord_invite_dialog_class_init(
    GnConcordInviteDialogClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_concord_invite_dialog_dispose;
  object_class->finalize = gn_concord_invite_dialog_finalize;
}

static void gn_concord_invite_dialog_init(GnConcordInviteDialog *self) {
  gtk_window_set_title(GTK_WINDOW(self), "Invite links");
  gtk_window_set_default_size(GTK_WINDOW(self), 560, 420);
  gtk_window_set_modal(GTK_WINDOW(self), TRUE);
}

GnConcordInviteDialog *gn_concord_invite_dialog_new(
    GnConcordCommunityService *service, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(service), NULL);
  g_return_val_if_fail(community_id != NULL, NULL);

  GnConcordInviteDialog *self =
    g_object_new(GN_TYPE_CONCORD_INVITE_DIALOG, NULL);
  self->service = g_object_ref(service);
  self->community_id = g_strdup(community_id);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *mint = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(mint, 12);
  gtk_widget_set_margin_end(mint, 12);
  gtk_widget_set_margin_top(mint, 12);
  self->label_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->label_entry,
                                 "Name this link (\"Conf 2026\")");
  gtk_widget_set_hexpand(GTK_WIDGET(self->label_entry), TRUE);
  gtk_box_append(GTK_BOX(mint), GTK_WIDGET(self->label_entry));

  self->create_button = gtk_button_new_with_label("Create link");
  gtk_widget_add_css_class(self->create_button, "suggested-action");
  g_signal_connect(self->create_button, "clicked",
                   G_CALLBACK(on_create_clicked), self);
  gtk_box_append(GTK_BOX(mint), self->create_button);
  gtk_box_append(GTK_BOX(content), mint);

  self->status = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->status, TRUE);
  gtk_label_set_xalign(self->status, 0);
  gtk_widget_set_margin_start(GTK_WIDGET(self->status), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self->status), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self->status), 6);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->status));

  self->link_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->link_list);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_widget_set_margin_top(scroll, 8);
  gtk_box_append(GTK_BOX(content), scroll);

  gtk_window_set_child(GTK_WINDOW(self), content);

  self->service_updated = g_signal_connect(
    service, "community-updated", G_CALLBACK(on_service_updated), self);

  gtk_widget_set_sensitive(self->create_button, may_create(self));
  if (!gn_concord_community_service_get_current_pubkey(service))
    set_status(self, "Sign in to mint an invite link.", FALSE);
  else if (!may_create(self))
    set_status(self,
               "You do not hold CREATE_INVITE in this Community, so you "
               "cannot mint or retire its links.", FALSE);
  refresh_links(self);
  return self;
}
