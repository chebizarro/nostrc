#include "gh-blocked-page.h"

#include <glib/gi18n.h>
#include <string.h>

struct _GhBlockedPage {
  AdwNavigationPage parent_instance;
  GtkStack *stack;
  GtkListBox *list;
  AdwStatusPage *error_page;
  GhBlockedBackend backend;
  gpointer data;
};

G_DEFINE_FINAL_TYPE(GhBlockedPage, gh_blocked_page, ADW_TYPE_NAVIGATION_PAGE)

/* A row's Unblock button (borrowed: the row's own child). */
#define ROW_BUTTON "groundhog-unblock-button"

void
gh_blocked_conversation_free(GhBlockedConversation *conversation)
{
  if (!conversation)
    return;
  g_free(conversation->room_id);
  g_strfreev(conversation->npubs);
  g_free(conversation);
}

/* "npub1abcde…wxyz", as the conversation list abbreviates. */
static gchar *
short_npub(const gchar *npub)
{
  gsize length = strlen(npub);
  return length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4) : g_strdup(npub);
}

static GtkWidget *
create_row(GhBlockedConversation *conversation)
{
  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-blocked-row.ui");
  GtkWidget *row = GTK_WIDGET(g_object_ref_sink(gtk_builder_get_object(builder, "row")));
  GtkWidget *button = GTK_WIDGET(gtk_builder_get_object(builder, "unblock_button"));

  g_autoptr(GPtrArray) shorts = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; conversation->npubs && conversation->npubs[i]; i++)
    g_ptr_array_add(shorts, short_npub(conversation->npubs[i]));
  g_ptr_array_add(shorts, NULL);
  g_autofree gchar *title = g_strjoinv(", ", (gchar **)shorts->pdata);
  g_autofree gchar *full = conversation->npubs ? g_strjoinv(", ", conversation->npubs)
                                               : g_strdup("");
  g_autofree gchar *subtitle =
    conversation->has_messages
      ? g_strdup(full)
      /* TRANSLATORS: the blocked people's npubs, then why nothing comes back. */
      : g_strdup_printf(_("%s\nIts messages were deleted when you blocked it"), full);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(button), "s", conversation->room_id);
  g_object_set_data(G_OBJECT(row), ROW_BUTTON, button);
  /* "Unblock" alone says nothing about whom in a list read out row by row. */
  g_autofree gchar *label = g_strdup_printf(_("Unblock %s"), title);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                 -1);
  return row;
}

static void
set_state(GhBlockedPage *self, const gchar *state)
{
  gtk_stack_set_visible_child_name(self->stack, state);
}

void
gh_blocked_page_refresh(GhBlockedPage *self)
{
  g_return_if_fail(GH_IS_BLOCKED_PAGE(self));
  gtk_list_box_remove_all(self->list);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) blocked = self->backend.list(self->data, &error);
  if (!blocked) {
    adw_status_page_set_description(self->error_page, error ? error->message : NULL);
    set_state(self, "error");
    return;
  }
  for (guint i = 0; i < blocked->len; i++) {
    g_autoptr(GtkWidget) row = create_row(g_ptr_array_index(blocked, i));
    gtk_list_box_append(self->list, row);
  }
  set_state(self, blocked->len ? "list" : "empty");
}

const gchar *
gh_blocked_page_get_state(GhBlockedPage *self)
{
  g_return_val_if_fail(GH_IS_BLOCKED_PAGE(self), NULL);
  return gtk_stack_get_visible_child_name(self->stack);
}

GtkListBox *
gh_blocked_page_get_list(GhBlockedPage *self)
{
  g_return_val_if_fail(GH_IS_BLOCKED_PAGE(self), NULL);
  return self->list;
}

static void
toast(GhBlockedPage *self, const gchar *text)
{
  GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_PREFERENCES_DIALOG);
  if (!dialog)
    return;
  AdwToast *toast = adw_toast_new(text);
  adw_toast_set_use_markup(toast, FALSE);
  adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog), toast);
}

/* The Unblock button of the row at index, else of the last row; NULL when
 * the list is empty. */
static GtkWidget *
unblock_button_near(GhBlockedPage *self, gint index)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(self->list, index);
  for (gint i = index - 1; !row && i >= 0; i--)
    row = gtk_list_box_get_row_at_index(self->list, i);
  return row ? g_object_get_data(G_OBJECT(row), ROW_BUTTON) : NULL;
}

static void
unblock_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhBlockedPage *self = GH_BLOCKED_PAGE(widget);
  (void)name;
  const gchar *room_id = g_variant_get_string(parameter, NULL);
  /* Where keyboard focus goes once the row is gone: the next row. */
  gint index = -1;
  GtkRoot *root = gtk_widget_get_root(widget);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  GtkWidget *row = focus ? gtk_widget_get_ancestor(focus, GTK_TYPE_LIST_BOX_ROW) : NULL;
  if (row && gtk_widget_get_parent(row) == GTK_WIDGET(self->list))
    index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row));

  g_autoptr(GError) error = NULL;
  if (!self->backend.unblock(self->data, room_id, &error)) {
    g_message("Groundhog could not unblock a conversation: %s",
              error ? error->message : "unknown error");
    toast(self, _("Groundhog couldn’t save this change"));
    return;
  }
  toast(self, _("Conversation unblocked"));
  gh_blocked_page_refresh(self);
  GtkWidget *next = index >= 0 ? unblock_button_near(self, index) : NULL;
  if (next)
    gtk_widget_grab_focus(next);
}

static void
gh_blocked_page_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_BLOCKED_PAGE);
  G_OBJECT_CLASS(gh_blocked_page_parent_class)->dispose(object);
}

static void
gh_blocked_page_class_init(GhBlockedPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->dispose = gh_blocked_page_dispose;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-blocked-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhBlockedPage, stack);
  gtk_widget_class_bind_template_child(widget_class, GhBlockedPage, list);
  gtk_widget_class_bind_template_child(widget_class, GhBlockedPage, error_page);
  gtk_widget_class_install_action(widget_class, "blocked.unblock", "s", unblock_action);
}

static void
gh_blocked_page_init(GhBlockedPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

GhBlockedPage *
gh_blocked_page_new(const GhBlockedBackend *backend, gpointer data)
{
  g_return_val_if_fail(backend != NULL && backend->list && backend->unblock, NULL);
  GhBlockedPage *self = g_object_new(GH_TYPE_BLOCKED_PAGE, NULL);
  self->backend = *backend;
  self->data = data;
  gh_blocked_page_refresh(self);
  return self;
}

/* ---- Preferences › Privacy ---------------------------------------------------- */

#define ATTACH_DATA "groundhog-blocked"

typedef struct {
  GhBlockedBackend backend;
  gpointer data;
  GDestroyNotify destroy;
} BlockedAttach;

static void
attach_free(gpointer data)
{
  BlockedAttach *attach = data;
  if (attach->destroy)
    attach->destroy(attach->data);
  g_free(attach);
}

static void
on_blocked_row(AdwActionRow *row, GhPreferencesDialog *dialog)
{
  (void)row;
  BlockedAttach *attach = g_object_get_data(G_OBJECT(dialog), ATTACH_DATA);
  adw_preferences_dialog_push_subpage(
    ADW_PREFERENCES_DIALOG(dialog),
    ADW_NAVIGATION_PAGE(gh_blocked_page_new(&attach->backend, attach->data)));
}

void
gh_blocked_page_attach(GhPreferencesDialog *dialog, const GhBlockedBackend *backend,
                       gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_PREFERENCES_DIALOG(dialog));
  g_return_if_fail(backend != NULL && backend->list && backend->unblock);
  g_return_if_fail(g_object_get_data(G_OBJECT(dialog), ATTACH_DATA) == NULL);
  BlockedAttach *attach = g_new0(BlockedAttach, 1);
  attach->backend = *backend;
  attach->data = data;
  attach->destroy = destroy;
  g_object_set_data_full(G_OBJECT(dialog), ATTACH_DATA, attach, attach_free);
  GObject *group = gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG,
                                                 "blocked_group");
  GObject *row = gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG,
                                               "blocked_row");
  g_signal_connect_object(row, "activated", G_CALLBACK(on_blocked_row), dialog, 0);
  gtk_widget_set_visible(GTK_WIDGET(group), TRUE);
}
