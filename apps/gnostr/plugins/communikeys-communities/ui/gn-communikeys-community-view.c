#include "gn-communikeys-community-view.h"
#include "gn-communikeys-composer.h"
#include "../model/gn-communikeys-message-item.h"
#include "../model/gn-communikeys-section-item.h"
#include "../model/gn-communikeys-targeted-item.h"

#include <string.h>

struct _GnCommunikeysCommunityView {
  GtkBox parent_instance;
  GnCommunikeysCommunityService *service;
  GnCommunikeysCommunityItem *item;
  gchar *pubkey;
  GtkBox *overview;
  GnCommunikeysComposer *composer;
  GtkLabel *composer_status;
  GtkEntry *target_reference;
  GtkSpinButton *target_kind;
  GtkButton *target_publish;
  GtkLabel *target_status;
  GCancellable *chat_cancellable;
  GCancellable *target_cancellable;
  gulong service_updated;
};

G_DEFINE_TYPE(GnCommunikeysCommunityView, gn_communikeys_community_view,
              GTK_TYPE_BOX)

static GtkWidget *left_label(const char *text, const char *css_class) {
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_hexpand(label, TRUE);
  if (css_class) gtk_widget_add_css_class(label, css_class);
  return label;
}

static gchar *short_pubkey(const char *pubkey) {
  if (!pubkey) return g_strdup("unknown");
  return strlen(pubkey) > 16
    ? g_strdup_printf("%.16s…", pubkey) : g_strdup(pubkey);
}

static void clear_box(GtkBox *box) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
    gtk_box_remove(box, child);
}

static gchar *assignment_summary(GnCommunikeysSectionItem *section) {
  GString *summary = g_string_new(NULL);
  guint n = gn_communikeys_section_item_get_assignment_count(section);
  for (guint i = 0; i < n; i++) {
    int kind = 0;
    const char *subtype = NULL;
    gn_communikeys_section_item_get_assignment(
      section, i, &kind, &subtype);
    if (i) g_string_append(summary, " · ");
    if (subtype && *subtype)
      g_string_append_printf(summary, "kind %d / %s", kind, subtype);
    else
      g_string_append_printf(summary, "kind %d", kind);
  }
  return g_string_free(summary, FALSE);
}

static void rebuild_overview(GnCommunikeysCommunityView *self) {
  clear_box(self->overview);
  if (!self->item) {
    gtk_box_append(self->overview,
      left_label("The community definition is no longer available.",
                 "dim-label"));
    return;
  }

  GListModel *sections =
    gn_communikeys_community_item_get_sections(self->item);
  guint n = g_list_model_get_n_items(sections);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysSectionItem) section =
      g_list_model_get_item(sections, i);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_margin_start(card, 12);
    gtk_widget_set_margin_end(card, 12);
    gtk_widget_set_margin_top(card, 6);
    gtk_widget_set_margin_bottom(card, 6);
    GtkWidget *name = left_label(
      gn_communikeys_section_item_get_name(section), "heading");
    gtk_widget_set_margin_top(name, 10);
    gtk_widget_set_margin_start(name, 12);
    gtk_widget_set_margin_end(name, 12);
    gtk_box_append(GTK_BOX(card), name);

    g_autofree gchar *assignments = assignment_summary(section);
    GtkWidget *assignment = left_label(assignments, "dim-label");
    gtk_widget_set_margin_start(assignment, 12);
    gtk_widget_set_margin_end(assignment, 12);
    gtk_box_append(GTK_BOX(card), assignment);

    g_autofree gchar *coordinate = g_strdup_printf(
      "ACL: 30000:%s:%s%s%s",
      gn_communikeys_section_item_get_acl_publisher(section),
      gn_communikeys_section_item_get_acl_identifier(section),
      gn_communikeys_section_item_get_acl_relay(section) ? " · " : "",
      gn_communikeys_section_item_get_acl_relay(section)
        ? gn_communikeys_section_item_get_acl_relay(section) : "");
    GtkWidget *acl = left_label(coordinate, "dim-label");
    gtk_widget_set_margin_start(acl, 12);
    gtk_widget_set_margin_end(acl, 12);
    gtk_box_append(GTK_BOX(card), acl);

    g_autofree gchar *state = g_strdup_printf(
      "%s · %u member%s",
      gn_communikeys_section_item_get_acl_status(section),
      gn_communikeys_section_item_get_member_count(section),
      gn_communikeys_section_item_get_member_count(section) == 1 ? "" : "s");
    GtkWidget *state_label = left_label(state, NULL);
    gtk_widget_set_margin_start(state_label, 12);
    gtk_widget_set_margin_end(state_label, 12);
    gtk_box_append(GTK_BOX(card), state_label);

    guint members = gn_communikeys_section_item_get_member_count(section);
    if (members) {
      GString *member_text = g_string_new("Members: ");
      for (guint j = 0; j < members; j++) {
        g_autofree gchar *member = short_pubkey(
          gn_communikeys_section_item_get_member(section, j));
        if (j) g_string_append(member_text, ", ");
        g_string_append(member_text, member);
      }
      GtkWidget *member_label = left_label(member_text->str, "dim-label");
      gtk_widget_set_margin_start(member_label, 12);
      gtk_widget_set_margin_end(member_label, 12);
      gtk_box_append(GTK_BOX(card), member_label);
      g_string_free(member_text, TRUE);
    }

    guint badges = gn_communikeys_section_item_get_badge_count(section);
    if (badges) {
      GString *badge_text = g_string_new(
        "Engagement badges (never publishing permission): ");
      for (guint j = 0; j < badges; j++) {
        if (j) g_string_append(badge_text, ", ");
        g_string_append(badge_text,
          gn_communikeys_section_item_get_badge(section, j));
      }
      GtkWidget *badge_label = left_label(badge_text->str, "dim-label");
      gtk_widget_set_margin_start(badge_label, 12);
      gtk_widget_set_margin_end(badge_label, 12);
      gtk_widget_set_margin_bottom(badge_label, 10);
      gtk_box_append(GTK_BOX(card), badge_label);
      g_string_free(badge_text, TRUE);
    } else {
      gtk_widget_set_margin_bottom(state_label, 10);
    }
    gtk_box_append(self->overview, card);
  }
}

static void update_publish_controls(GnCommunikeysCommunityView *self) {
  const char *user =
    gn_communikeys_community_service_get_current_pubkey(self->service);
  gboolean can_kind_9 = user &&
    gn_communikeys_community_service_author_can_publish(
      self->service, self->pubkey, 9, user);
  gboolean can_kind_11 = user &&
    gn_communikeys_community_service_author_can_publish(
      self->service, self->pubkey, 11, user);
  gboolean chat = can_kind_9 || can_kind_11;
  gn_communikeys_composer_set_allowed_kinds(
    self->composer, can_kind_9, can_kind_11);
  gn_communikeys_composer_set_send_sensitive(self->composer, chat);
  gtk_label_set_text(
    self->composer_status,
    !user ? "Sign in to publish."
          : chat ? "Verified section ACL permits publishing."
                 : "Publishing disabled: no verified matching ACL membership.");
  gtk_widget_set_sensitive(GTK_WIDGET(self->target_publish), user != NULL);
}

static void on_service_updated(GnCommunikeysCommunityService *service,
                               const char *pubkey, guint flags,
                               gpointer user_data) {
  (void)flags;
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(user_data);
  if (g_strcmp0(pubkey, self->pubkey) != 0) return;
  g_clear_object(&self->item);
  self->item = gn_communikeys_community_service_lookup_community(
    service, self->pubkey);
  rebuild_overview(self);
  update_publish_controls(self);
}

static void on_message_setup(GtkSignalListItemFactory *factory,
                             GtkListItem *list_item,
                             gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_box_append(GTK_BOX(box), left_label(NULL, "caption-heading"));
  gtk_box_append(GTK_BOX(box), left_label(NULL, NULL));
  gtk_box_append(GTK_BOX(box), left_label(NULL, "dim-label"));
  gtk_list_item_set_child(list_item, box);
}
static void on_message_bind(GtkSignalListItemFactory *factory,
                            GtkListItem *list_item,
                            gpointer user_data) {
  (void)factory;
  (void)user_data;
  GnCommunikeysMessageItem *item = gtk_list_item_get_item(list_item);
  GtkWidget *box = gtk_list_item_get_child(list_item);
  GtkWidget *author = gtk_widget_get_first_child(box);
  GtkWidget *content = gtk_widget_get_next_sibling(author);
  GtkWidget *meta = gtk_widget_get_next_sibling(content);
  g_autofree gchar *short_author = short_pubkey(
    gn_communikeys_message_item_get_pubkey(item));
  gtk_label_set_text(GTK_LABEL(author), short_author);
  gtk_label_set_text(GTK_LABEL(content),
                     gn_communikeys_message_item_get_content(item));
  g_autofree gchar *details = g_strdup_printf(
    "%s · kind %d",
    gn_communikeys_message_item_get_section_name(item),
    gn_communikeys_message_item_get_kind(item));
  gtk_label_set_text(GTK_LABEL(meta), details);
}

static void on_target_setup(GtkSignalListItemFactory *factory,
                            GtkListItem *list_item,
                            gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_box_append(GTK_BOX(box), left_label(NULL, "caption-heading"));
  gtk_box_append(GTK_BOX(box), left_label(NULL, NULL));
  gtk_box_append(GTK_BOX(box), left_label(NULL, "dim-label"));
  gtk_list_item_set_child(list_item, box);
}
static void on_target_bind(GtkSignalListItemFactory *factory,
                           GtkListItem *list_item,
                           gpointer user_data) {
  (void)factory;
  (void)user_data;
  GnCommunikeysTargetedItem *item = gtk_list_item_get_item(list_item);
  GtkWidget *box = gtk_list_item_get_child(list_item);
  GtkWidget *heading = gtk_widget_get_first_child(box);
  GtkWidget *content = gtk_widget_get_next_sibling(heading);
  GtkWidget *meta = gtk_widget_get_next_sibling(content);
  g_autofree gchar *author = short_pubkey(
    gn_communikeys_targeted_item_get_author(item));
  g_autofree gchar *title = g_strdup_printf(
    "%s targeted kind %d",
    author, gn_communikeys_targeted_item_get_original_kind(item));
  gtk_label_set_text(GTK_LABEL(heading), title);
  gtk_label_set_text(GTK_LABEL(content),
    gn_communikeys_targeted_item_get_original_content(item));
  g_autofree gchar *details = g_strdup_printf(
    "%s · %u communit%s · %s",
    gn_communikeys_targeted_item_get_section_name(item),
    gn_communikeys_targeted_item_get_target_count(item),
    gn_communikeys_targeted_item_get_target_count(item) == 1 ? "y" : "ies",
    gn_communikeys_targeted_item_get_reference(item));
  gtk_label_set_text(GTK_LABEL(meta), details);
}

static void set_status(GtkLabel *label, const char *message,
                       gboolean error) {
  gtk_label_set_text(label, message ? message : "");
  if (error) gtk_widget_add_css_class(GTK_WIDGET(label), "error");
  else gtk_widget_remove_css_class(GTK_WIDGET(label), "error");
}

static void on_chat_done(GObject *source, GAsyncResult *result,
                         gpointer user_data) {
  (void)source;
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(user_data);
  g_autoptr(GError) error = NULL;
  gboolean ok =
    gn_communikeys_community_service_publish_exclusive_finish(
      self->service, result, &error);
  g_clear_object(&self->chat_cancellable);
  update_publish_controls(self);
  if (ok) {
    gn_communikeys_composer_clear(self->composer);
    set_status(self->composer_status, "Message published.", FALSE);
  } else {
    set_status(self->composer_status,
               error ? error->message : "Failed to publish message", TRUE);
  }
  g_object_unref(self);
}

static void on_send_requested(GnCommunikeysComposer *composer,
                              int kind, const char *content,
                              gpointer user_data) {
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(user_data);
  if (self->chat_cancellable)
    g_cancellable_cancel(self->chat_cancellable);
  g_clear_object(&self->chat_cancellable);
  self->chat_cancellable = g_cancellable_new();
  gn_communikeys_composer_set_send_sensitive(composer, FALSE);
  set_status(self->composer_status, "Requesting signature…", FALSE);
  gn_communikeys_community_service_publish_exclusive_async(
    self->service, self->pubkey, kind, content,
    self->chat_cancellable, on_chat_done, g_object_ref(self));
}

static void on_target_done(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
  (void)source;
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(user_data);
  g_autoptr(GError) error = NULL;
  gboolean ok = gn_communikeys_community_service_publish_target_finish(
    self->service, result, &error);
  g_clear_object(&self->target_cancellable);
  gtk_widget_set_sensitive(GTK_WIDGET(self->target_publish), TRUE);
  if (ok) {
    gtk_editable_set_text(GTK_EDITABLE(self->target_reference), "");
    set_status(self->target_status, "Targeting record published.", FALSE);
  } else {
    set_status(self->target_status,
               error ? error->message : "Failed to publish target", TRUE);
  }
  g_object_unref(self);
}

static void on_publish_target(GtkButton *button, gpointer user_data) {
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(user_data);
  const char *reference =
    gtk_editable_get_text(GTK_EDITABLE(self->target_reference));
  const char *author =
    gn_communikeys_community_service_get_current_pubkey(self->service);
  if (!reference || !*reference || !author) {
    set_status(self->target_status,
               "Enter an event ID or address and sign in.", TRUE);
    return;
  }
  g_autofree gchar *identifier = g_uuid_string_random();
  nostr_communikeys_target_t target = {
    .pubkey = self->pubkey,
    .relay = (char *)gn_communikeys_community_item_get_main_relay(self->item)
  };
  nostr_communikeys_targeted_publication_t publication = {
    .identifier = identifier,
    .reference_type = strchr(reference, ':')
      ? NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS
      : NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
    .reference = (char *)reference,
    .reference_relay = NULL,
    .reference_author = (char *)author,
    .original_kind = gtk_spin_button_get_value_as_int(self->target_kind),
    .original_author = (char *)author,
    .targets = &target,
    .targets_len = 1
  };
  if (self->target_cancellable)
    g_cancellable_cancel(self->target_cancellable);
  g_clear_object(&self->target_cancellable);
  self->target_cancellable = g_cancellable_new();
  gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
  set_status(self->target_status, "Verifying original and ACL…", FALSE);
  gn_communikeys_community_service_publish_target_async(
    self->service, &publication, self->target_cancellable,
    on_target_done, g_object_ref(self));
}

static GtkWidget *model_page(GListModel *model,
                             GCallback setup, GCallback bind,
                             const char *empty_text) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", setup, NULL);
  g_signal_connect(factory, "bind", bind, NULL);
  GtkNoSelection *selection =
    gtk_no_selection_new(model ? g_object_ref(model) : NULL);
  GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(box), scroll);
  if (!model || g_list_model_get_n_items(model) == 0) {
    GtkWidget *empty = left_label(empty_text, "dim-label");
    gtk_widget_set_margin_start(empty, 12);
    gtk_widget_set_margin_end(empty, 12);
    gtk_widget_set_margin_top(empty, 12);
    gtk_box_prepend(GTK_BOX(box), empty);
  }
  return box;
}

static void gn_communikeys_community_view_dispose(GObject *object) {
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(object);
  if (self->chat_cancellable) g_cancellable_cancel(self->chat_cancellable);
  if (self->target_cancellable) g_cancellable_cancel(self->target_cancellable);
  g_clear_object(&self->chat_cancellable);
  g_clear_object(&self->target_cancellable);
  if (self->service && self->service_updated)
    g_signal_handler_disconnect(self->service, self->service_updated);
  self->service_updated = 0;
  g_clear_object(&self->service);
  g_clear_object(&self->item);
  G_OBJECT_CLASS(gn_communikeys_community_view_parent_class)->dispose(object);
}
static void gn_communikeys_community_view_finalize(GObject *object) {
  GnCommunikeysCommunityView *self =
    GN_COMMUNIKEYS_COMMUNITY_VIEW(object);
  g_free(self->pubkey);
  G_OBJECT_CLASS(gn_communikeys_community_view_parent_class)->finalize(object);
}
static void gn_communikeys_community_view_class_init(
    GnCommunikeysCommunityViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_communikeys_community_view_dispose;
  object_class->finalize = gn_communikeys_community_view_finalize;
}
static void gn_communikeys_community_view_init(
    GnCommunikeysCommunityView *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

GnCommunikeysCommunityView *gn_communikeys_community_view_new(
    GnCommunikeysCommunityService *service,
    GnCommunikeysCommunityItem *item) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_ITEM(item), NULL);
  GnCommunikeysCommunityView *self =
    g_object_new(GN_TYPE_COMMUNIKEYS_COMMUNITY_VIEW, NULL);
  self->service = g_object_ref(service);
  self->item = g_object_ref(item);
  self->pubkey = g_strdup(
    gn_communikeys_community_item_get_pubkey(item));

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_margin_start(header, 12);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 10);
  gtk_widget_set_margin_bottom(header, 8);
  g_autofree gchar *title = short_pubkey(self->pubkey);
  gtk_box_append(GTK_BOX(header), left_label(title, "title-2"));
  gtk_box_append(GTK_BOX(header), left_label(
    gn_communikeys_community_item_get_description(item), NULL));
  gtk_box_append(GTK_BOX(header), left_label(
    gn_communikeys_community_item_get_main_relay(item)
      ? gn_communikeys_community_item_get_main_relay(item)
      : "No publication relay", "dim-label"));
  gtk_box_append(GTK_BOX(self), header);

  GtkStack *stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  GtkWidget *switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), stack);
  gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), switcher);

  self->overview = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *overview_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(overview_scroll),
                                GTK_WIDGET(self->overview));
  gtk_stack_add_titled(stack, overview_scroll, "sections", "Sections");

  GListModel *messages = gn_communikeys_community_service_get_messages(
    service, self->pubkey);
  GtkWidget *chat = model_page(
    messages, G_CALLBACK(on_message_setup), G_CALLBACK(on_message_bind),
    "No authorized kind-9/11 publications.");
  self->composer_status = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->composer_status, TRUE);
  gtk_widget_set_margin_start(GTK_WIDGET(self->composer_status), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self->composer_status), 12);
  gtk_box_append(GTK_BOX(chat), GTK_WIDGET(self->composer_status));
  self->composer = gn_communikeys_composer_new();
  g_signal_connect(self->composer, "send-requested",
                   G_CALLBACK(on_send_requested), self);
  gtk_box_append(GTK_BOX(chat), GTK_WIDGET(self->composer));
  gtk_stack_add_titled(stack, chat, "chat", "Chat");

  GListModel *targets = gn_communikeys_community_service_get_targets(
    service, self->pubkey);
  GtkWidget *target_page = model_page(
    targets, G_CALLBACK(on_target_setup), G_CALLBACK(on_target_bind),
    "No verified targeted publications.");
  GtkWidget *editor = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(editor, 12);
  gtk_widget_set_margin_end(editor, 12);
  gtk_widget_set_margin_top(editor, 6);
  gtk_widget_set_margin_bottom(editor, 6);
  self->target_reference = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(
    self->target_reference, "Original event ID or address");
  gtk_widget_set_hexpand(GTK_WIDGET(self->target_reference), TRUE);
  gtk_box_append(GTK_BOX(editor), GTK_WIDGET(self->target_reference));
  GtkAdjustment *kind_adjustment =
    gtk_adjustment_new(1, 0, 65535, 1, 10, 0);
  self->target_kind = GTK_SPIN_BUTTON(
    gtk_spin_button_new(kind_adjustment, 1, 0));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->target_kind),
                              "Original publication kind");
  gtk_box_append(GTK_BOX(editor), GTK_WIDGET(self->target_kind));
  self->target_publish = GTK_BUTTON(
    gtk_button_new_with_label("Target"));
  gtk_widget_add_css_class(GTK_WIDGET(self->target_publish),
                           "suggested-action");
  g_signal_connect(self->target_publish, "clicked",
                   G_CALLBACK(on_publish_target), self);
  gtk_box_append(GTK_BOX(editor), GTK_WIDGET(self->target_publish));
  gtk_box_append(GTK_BOX(target_page), editor);
  self->target_status = GTK_LABEL(gtk_label_new(
    "The original, author, kind, targets, and section ACL are verified before signing."));
  gtk_label_set_wrap(self->target_status, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->target_status), "dim-label");
  gtk_widget_set_margin_start(GTK_WIDGET(self->target_status), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self->target_status), 12);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->target_status), 8);
  gtk_box_append(GTK_BOX(target_page), GTK_WIDGET(self->target_status));
  gtk_stack_add_titled(stack, target_page, "targets", "Targeted");
  gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(stack));

  self->service_updated = g_signal_connect(
    service, "community-updated", G_CALLBACK(on_service_updated), self);
  rebuild_overview(self);
  update_publish_controls(self);
  return self;
}
