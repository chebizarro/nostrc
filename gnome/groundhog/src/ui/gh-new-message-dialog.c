#include "gh-new-message-dialog.h"
#include "gh-net-session.h"

#include "gh-recipient.h"

#include <glib/gi18n.h>
#include <string.h>

#define MAX_CONTACT_ROWS 50

/* ---- GhNewMessageItem ------------------------------------------------------- */

struct _GhNewMessageItem {
  GObject parent_instance;
  GhNewMessageItemKind kind;
  gchar *title;
  gchar *subtitle;
  gchar *avatar_text;   /* NULL: no avatar */
  gboolean show_initials;
  gchar *icon_name;     /* NULL: no icon */
  gboolean busy;
  gchar *status;
  gboolean enabled;
  gchar *pubkey;        /* CONTACT, PUBKEY, RECIPIENT, PERSON */
  gchar *address;       /* LOOKUP: the NIP-05 address */
};

enum {
  ITEM_PROP_0,
  ITEM_PROP_TITLE,
  ITEM_PROP_SUBTITLE,
  ITEM_PROP_AVATAR_TEXT,
  ITEM_PROP_SHOW_AVATAR,
  ITEM_PROP_SHOW_INITIALS,
  ITEM_PROP_ICON_NAME,
  ITEM_PROP_SHOW_ICON,
  ITEM_PROP_BUSY,
  ITEM_PROP_STATUS,
  ITEM_PROP_HAS_STATUS,
  ITEM_PROP_ENABLED,
  ITEM_N_PROPS
};
static GParamSpec *item_props[ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(GhNewMessageItem, gh_new_message_item, G_TYPE_OBJECT)

static GhNewMessageItem *
item_new(GhNewMessageItemKind kind, const gchar *title, const gchar *subtitle)
{
  GhNewMessageItem *self = g_object_new(GH_TYPE_NEW_MESSAGE_ITEM, NULL);
  self->kind = kind;
  self->title = g_strdup(title);
  self->subtitle = g_strdup(subtitle);
  self->enabled = TRUE;
  return self;
}

static void
item_set_string(GhNewMessageItem *self, gchar **field, const gchar *value, guint prop,
                guint derived)
{
  if (g_strcmp0(*field, value) == 0)
    return;
  g_free(*field);
  *field = g_strdup(value);
  g_object_notify_by_pspec(G_OBJECT(self), item_props[prop]);
  if (derived)
    g_object_notify_by_pspec(G_OBJECT(self), item_props[derived]);
}

static void
item_set_subtitle(GhNewMessageItem *self, const gchar *subtitle)
{
  item_set_string(self, &self->subtitle, subtitle, ITEM_PROP_SUBTITLE, 0);
}

static void
item_set_status(GhNewMessageItem *self, const gchar *status)
{
  item_set_string(self, &self->status, status, ITEM_PROP_STATUS, ITEM_PROP_HAS_STATUS);
}

static void
item_set_busy(GhNewMessageItem *self, gboolean busy)
{
  if (self->busy == busy)
    return;
  self->busy = busy;
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_BUSY]);
}

static void
item_set_enabled(GhNewMessageItem *self, gboolean enabled)
{
  if (self->enabled == enabled)
    return;
  self->enabled = enabled;
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_ENABLED]);
}

GhNewMessageItemKind
gh_new_message_item_get_kind(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), GH_NEW_MESSAGE_ITEM_CONTACT);
  return self->kind;
}

const gchar *
gh_new_message_item_get_title(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), NULL);
  return self->title;
}

const gchar *
gh_new_message_item_get_subtitle(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), NULL);
  return self->subtitle;
}

const gchar *
gh_new_message_item_get_status(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), NULL);
  return self->status;
}

const gchar *
gh_new_message_item_get_pubkey(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), NULL);
  return self->pubkey;
}

gboolean
gh_new_message_item_get_busy(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), FALSE);
  return self->busy;
}

gboolean
gh_new_message_item_get_enabled(GhNewMessageItem *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_ITEM(self), FALSE);
  return self->enabled;
}

static void
gh_new_message_item_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhNewMessageItem *self = GH_NEW_MESSAGE_ITEM(object);
  switch (id) {
  case ITEM_PROP_TITLE:
    g_value_set_string(value, self->title);
    break;
  case ITEM_PROP_SUBTITLE:
    g_value_set_string(value, self->subtitle);
    break;
  case ITEM_PROP_AVATAR_TEXT:
    g_value_set_string(value, self->avatar_text);
    break;
  case ITEM_PROP_SHOW_AVATAR:
    g_value_set_boolean(value, self->avatar_text != NULL);
    break;
  case ITEM_PROP_SHOW_INITIALS:
    g_value_set_boolean(value, self->show_initials);
    break;
  case ITEM_PROP_ICON_NAME:
    g_value_set_string(value, self->icon_name);
    break;
  case ITEM_PROP_SHOW_ICON:
    g_value_set_boolean(value, self->icon_name != NULL);
    break;
  case ITEM_PROP_BUSY:
    g_value_set_boolean(value, self->busy);
    break;
  case ITEM_PROP_STATUS:
    g_value_set_string(value, self->status);
    break;
  case ITEM_PROP_HAS_STATUS:
    g_value_set_boolean(value, self->status && *self->status);
    break;
  case ITEM_PROP_ENABLED:
    g_value_set_boolean(value, self->enabled);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_new_message_item_finalize(GObject *object)
{
  GhNewMessageItem *self = GH_NEW_MESSAGE_ITEM(object);
  g_free(self->title);
  g_free(self->subtitle);
  g_free(self->avatar_text);
  g_free(self->icon_name);
  g_free(self->status);
  g_free(self->pubkey);
  g_free(self->address);
  G_OBJECT_CLASS(gh_new_message_item_parent_class)->finalize(object);
}

static void
gh_new_message_item_class_init(GhNewMessageItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  object_class->get_property = gh_new_message_item_get_property;
  object_class->finalize = gh_new_message_item_finalize;
  item_props[ITEM_PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_SUBTITLE] = g_param_spec_string("subtitle", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_AVATAR_TEXT] = g_param_spec_string("avatar-text", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_SHOW_AVATAR] = g_param_spec_boolean("show-avatar", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_SHOW_INITIALS] =
    g_param_spec_boolean("show-initials", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_ICON_NAME] = g_param_spec_string("icon-name", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_SHOW_ICON] = g_param_spec_boolean("show-icon", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_BUSY] = g_param_spec_boolean("busy", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_STATUS] = g_param_spec_string("status", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_HAS_STATUS] = g_param_spec_boolean("has-status", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_ENABLED] = g_param_spec_boolean("enabled", NULL, NULL, TRUE, ro);
  g_object_class_install_properties(object_class, ITEM_N_PROPS, item_props);
}

static void
gh_new_message_item_init(GhNewMessageItem *self)
{
  self->enabled = TRUE;
}

/* ---- GhNewMessageRow (data/ui/gh-new-message-row.blp) ------------------------- */

#define GH_TYPE_NEW_MESSAGE_ROW (gh_new_message_row_get_type())
G_DECLARE_FINAL_TYPE(GhNewMessageRow, gh_new_message_row, GH, NEW_MESSAGE_ROW, AdwActionRow)

struct _GhNewMessageRow {
  AdwActionRow parent_instance;
  GhNewMessageItem *item;
};

enum { ROW_PROP_0, ROW_PROP_ITEM, ROW_N_PROPS };
static GParamSpec *row_props[ROW_N_PROPS];

G_DEFINE_FINAL_TYPE(GhNewMessageRow, gh_new_message_row, ADW_TYPE_ACTION_ROW)

/* The suffix status and busy state are part of what the row says. */
static void
row_update_description(GhNewMessageRow *self)
{
  GhNewMessageItem *item = self->item;
  if (!item)
    return;
  g_autofree gchar *description =
    item->busy ? g_strdup(_("Working…"))
               : g_strdup(item->status && *item->status ? item->status : "");
  if (item->subtitle && *item->subtitle) {
    g_autofree gchar *both = *description ? g_strdup_printf("%s. %s", item->subtitle, description)
                                          : g_strdup(item->subtitle);
    g_free(description);
    description = g_steal_pointer(&both);
  }
  gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                 description, -1);
}

static void
gh_new_message_row_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == ROW_PROP_ITEM)
    g_value_set_object(value, GH_NEW_MESSAGE_ROW(object)->item);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gh_new_message_row_set_property(GObject *object, guint id, const GValue *value,
                                GParamSpec *pspec)
{
  GhNewMessageRow *self = GH_NEW_MESSAGE_ROW(object);
  if (id != ROW_PROP_ITEM) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    return;
  }
  self->item = g_value_dup_object(value);
  if (!self->item)
    return;
  static const gchar *const watched[] = { "notify::subtitle", "notify::status", "notify::busy" };
  for (guint i = 0; i < G_N_ELEMENTS(watched); i++)
    g_signal_connect_object(self->item, watched[i], G_CALLBACK(row_update_description), self,
                            G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  row_update_description(self);
}

static void
gh_new_message_row_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_NEW_MESSAGE_ROW);
  g_clear_object(&GH_NEW_MESSAGE_ROW(object)->item);
  G_OBJECT_CLASS(gh_new_message_row_parent_class)->dispose(object);
}

static void
gh_new_message_row_class_init(GhNewMessageRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_new_message_row_get_property;
  object_class->set_property = gh_new_message_row_set_property;
  object_class->dispose = gh_new_message_row_dispose;
  row_props[ROW_PROP_ITEM] = g_param_spec_object("item", NULL, NULL, GH_TYPE_NEW_MESSAGE_ITEM,
    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, ROW_N_PROPS, row_props);
  g_type_ensure(GH_TYPE_NEW_MESSAGE_ITEM);
  gtk_widget_class_set_template_from_resource(GTK_WIDGET_CLASS(klass),
                                              "/org/nostr/Groundhog/ui/gh-new-message-row.ui");
}

static void
gh_new_message_row_init(GhNewMessageRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

static GtkWidget *
create_suggestion_row(gpointer item, gpointer data)
{
  (void)data;
  return g_object_new(GH_TYPE_NEW_MESSAGE_ROW, "item", item, "activatable", TRUE, NULL);
}

static GtkWidget *
create_person_row(gpointer item, gpointer data)
{
  (void)data;
  GtkWidget *row = g_object_new(GH_TYPE_NEW_MESSAGE_ROW, "item", item, "activatable", FALSE,
                                NULL);
  /* The npub subtitle is read in groups of four: keep its columns. */
  gtk_widget_add_css_class(row, "groundhog-npub-row");
  return row;
}

/* ---- GhRecipientChip (data/ui/gh-recipient-chip.blp) ------------------------ */

#define GH_TYPE_RECIPIENT_CHIP (gh_recipient_chip_get_type())
G_DECLARE_FINAL_TYPE(GhRecipientChip, gh_recipient_chip, GH, RECIPIENT_CHIP, GtkBox)

struct _GhRecipientChip {
  GtkBox parent_instance;
  GtkButton *remove_button;
  GhNewMessageItem *item;
};

G_DEFINE_FINAL_TYPE(GhRecipientChip, gh_recipient_chip, GTK_TYPE_BOX)

static void
gh_recipient_chip_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == ROW_PROP_ITEM)
    g_value_set_object(value, GH_RECIPIENT_CHIP(object)->item);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gh_recipient_chip_set_property(GObject *object, guint id, const GValue *value,
                               GParamSpec *pspec)
{
  GhRecipientChip *self = GH_RECIPIENT_CHIP(object);
  if (id != ROW_PROP_ITEM) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    return;
  }
  self->item = g_value_dup_object(value);
  if (!self->item)
    return;
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->remove_button), "s", self->item->pubkey);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->remove_button), "new-message.remove");
  g_autofree gchar *label = g_strdup_printf(_("Remove %s"), self->item->title);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->remove_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void
gh_recipient_chip_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_RECIPIENT_CHIP);
  g_clear_object(&GH_RECIPIENT_CHIP(object)->item);
  G_OBJECT_CLASS(gh_recipient_chip_parent_class)->dispose(object);
}

static void
gh_recipient_chip_class_init(GhRecipientChipClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->get_property = gh_recipient_chip_get_property;
  object_class->set_property = gh_recipient_chip_set_property;
  object_class->dispose = gh_recipient_chip_dispose;
  g_object_class_install_property(object_class, ROW_PROP_ITEM,
    g_param_spec_object("item", NULL, NULL, GH_TYPE_NEW_MESSAGE_ITEM,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_type_ensure(GH_TYPE_NEW_MESSAGE_ITEM);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-recipient-chip.ui");
  gtk_widget_class_bind_template_child(widget_class, GhRecipientChip, remove_button);
}

static void
gh_recipient_chip_init(GhRecipientChip *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

static GtkWidget *
create_chip(gpointer item, gpointer data)
{
  (void)data;
  return g_object_new(GH_TYPE_RECIPIENT_CHIP, "item", item, NULL);
}

/* ---- GhNewMessageDialog ------------------------------------------------------- */

struct _GhNewMessageDialog {
  AdwDialog parent_instance;
  AdwNavigationView *navigation;
  AdwNavigationPage *confirm_page;
  GtkSearchEntry *entry;
  GtkFlowBox *chips;
  GtkLabel *limit_label;
  GtkLabel *input_error;
  GtkListBox *results;
  GtkListBox *people;
  AdwActionRow *check_row;
  GtkSpinner *check_spinner;
  GtkLabel *start_note;

  GhNewMessageConfig config;
  GListStore *suggestions;
  GListStore *recipients;
  GListStore *people_items;
  GCancellable *cancellable; /* lookups and checks; cancelled on close */
  guint checks_pending;
  guint check_round; /* reset_cancellable() starts a new one */
};

enum { SIGNAL_STARTED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhNewMessageDialog, gh_new_message_dialog, ADW_TYPE_DIALOG)

static const gchar *
account_of(GhNewMessageDialog *self)
{
  return gh_conversation_store_get_account(self->config.conversations);
}

static const gchar *
name_of(GhNewMessageDialog *self, const gchar *pubkey)
{
  const gchar *name = self->config.display_name
                        ? self->config.display_name(self->config.names_data, pubkey) : NULL;
  return name && *name ? name : NULL;
}

static const gchar *
claimed_nip05_of(GhNewMessageDialog *self, const gchar *pubkey)
{
  const gchar *nip05 = self->config.claimed_nip05
                         ? self->config.claimed_nip05(self->config.names_data, pubkey) : NULL;
  return nip05 && *nip05 ? nip05 : NULL;
}

static gchar *
settings_string(GSettings *settings, const gchar *key, const gchar *fallback)
{
  if (!settings)
    return g_strdup(fallback);
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key(schema, key))
    return g_strdup(fallback);
  return g_settings_get_string(settings, key);
}

/* Whether the user's default protocol for new DMs is Marmot (MLS).
 * Falls back to TRUE (Marmot) when the setting is absent or unrecognised,
 * matching the schema default. The caller still falls back to NIP-17 when
 * the peer has no usable KeyPackage. */
static gboolean
default_dm_is_marmot(GhNewMessageDialog *self)
{
  g_autofree gchar *protocol = settings_string(self->config.settings,
                                                "default-dm-protocol", "marmot");
  return g_strcmp0(protocol, "nip17") != 0;
}

/* The host names of the configured discovery relays, ", "-joined; NULL
 * when there are none. */
static gchar *
discovery_hosts(GhNewMessageDialog *self)
{
  GSettings *settings = self->config.settings;
  if (!settings)
    return NULL;
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key(schema, "discovery-relays"))
    return NULL;
  g_auto(GStrv) relays = g_settings_get_strv(settings, "discovery-relays");
  g_autoptr(GPtrArray) hosts = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; relays && relays[i]; i++) {
    g_autoptr(GUri) uri = g_uri_parse(relays[i], G_URI_FLAGS_NONE, NULL);
    const gchar *host = uri ? g_uri_get_host(uri) : NULL;
    if (host && *host && !g_ptr_array_find_with_equal_func(hosts, host, g_str_equal, NULL))
      g_ptr_array_add(hosts, g_strdup(host));
  }
  if (!hosts->len)
    return NULL;
  g_ptr_array_add(hosts, NULL);
  return g_strjoinv(", ", (gchar **)hosts->pdata);
}

static gboolean
is_recipient(GhNewMessageDialog *self, const gchar *pubkey)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->recipients));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->recipients), i);
    if (g_strcmp0(item->pubkey, pubkey) == 0)
      return TRUE;
  }
  return FALSE;
}

static gboolean
recipients_full(GhNewMessageDialog *self)
{
  return g_list_model_get_n_items(G_LIST_MODEL(self->recipients)) >= GH_CONVERSATION_MAX_PEERS;
}

static void
show_error(GhNewMessageDialog *self, const gchar *message)
{
  gtk_label_set_text(self->input_error, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->input_error), message != NULL);
  if (message)
    gtk_accessible_announce(GTK_ACCESSIBLE(self), message,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

/* A person row: their cached name if any, else the short npub; the avatar
 * shows initials of a name only (never of an npub). */
static GhNewMessageItem *
person_item(GhNewMessageDialog *self, GhNewMessageItemKind kind, const gchar *pubkey,
            const gchar *label)
{
  const gchar *name = name_of(self, pubkey);
  g_autofree gchar *short_npub = gh_recipient_npub_short(pubkey);
  const gchar *title = label ? label : name ? name : short_npub;
  GhNewMessageItem *item = item_new(kind, title, NULL);
  item->pubkey = g_strdup(pubkey);
  item->avatar_text = g_strdup(name ? name : title);
  item->show_initials = name != NULL;
  return item;
}

/* ---- suggestions ------------------------------------------------------------ */

/* The peers of accepted conversations, most recent first, each once. */
static GPtrArray *
contacts(GhNewMessageDialog *self)
{
  GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
  GListModel *rooms = G_LIST_MODEL(self->config.conversations);
  guint n = g_list_model_get_n_items(rooms);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhConversation) room = g_list_model_get_item(rooms, i);
    if (gh_conversation_get_backend(room) != GH_CONVERSATION_BACKEND_NIP17 ||
        gh_conversation_get_is_request(room))
      continue;
    for (const gchar *const *peer = gh_conversation_get_peers(room); peer && *peer; peer++)
      if (!g_ptr_array_find_with_equal_func(found, *peer, g_str_equal, NULL))
        g_ptr_array_add(found, g_strdup(*peer));
  }
  return found;
}

static gboolean
contact_matches(GhNewMessageDialog *self, const gchar *pubkey, const gchar *needle)
{
  if (!needle || !*needle)
    return TRUE;
  g_autofree gchar *npub = gh_recipient_npub(pubkey);
  const gchar *haystacks[] = { name_of(self, pubkey), claimed_nip05_of(self, pubkey), npub };
  g_autofree gchar *folded_needle = g_utf8_casefold(needle, -1);
  for (guint i = 0; i < G_N_ELEMENTS(haystacks); i++) {
    if (!haystacks[i])
      continue;
    g_autofree gchar *folded = g_utf8_casefold(haystacks[i], -1);
    if (strstr(folded, folded_needle))
      return TRUE;
  }
  return FALSE;
}

static void
add_contact_rows(GhNewMessageDialog *self, GPtrArray *rows, const gchar *needle,
                 const gchar *exact_nip05)
{
  g_autoptr(GPtrArray) people = contacts(self);
  const gboolean full = recipients_full(self);
  guint added = 0;
  for (guint i = 0; i < people->len && added < MAX_CONTACT_ROWS; i++) {
    const gchar *pubkey = g_ptr_array_index(people, i);
    if (is_recipient(self, pubkey))
      continue;
    const gchar *claimed = claimed_nip05_of(self, pubkey);
    gboolean match = exact_nip05 ? claimed && g_ascii_strcasecmp(claimed, exact_nip05) == 0
                                 : contact_matches(self, pubkey, needle);
    if (!match)
      continue;
    GhNewMessageItem *item = person_item(self, GH_NEW_MESSAGE_ITEM_CONTACT, pubkey, NULL);
    g_autofree gchar *short_npub = gh_recipient_npub_short(pubkey);
    /* A claimed address is what the profile says, not verified (PD-2); a
     * contact without a cached name is already titled by its npub. */
    g_autofree gchar *unverified = claimed ? g_strdup_printf(_("%s (not verified)"), claimed)
                                           : NULL;
    item_set_subtitle(item, claimed ? unverified : name_of(self, pubkey) ? short_npub : NULL);
    item_set_enabled(item, !full);
    g_ptr_array_add(rows, item);
    added++;
  }
}

static GhNewMessageItem *
note_to_self_item(void)
{
  GhNewMessageItem *item = item_new(GH_NEW_MESSAGE_ITEM_NOTE_TO_SELF, _("Note to Self"),
                                    _("A private conversation with only you in it"));
  item->icon_name = g_strdup("user-bookmarks-symbolic");
  return item;
}

static GhNewMessageItem *
lookup_item(GhNewMessageDialog *self, const GhRecipientInput *input)
{
  g_autofree gchar *title = g_strdup_printf(_("Look up %s"), input->nip05);
  GhNewMessageItem *item = item_new(GH_NEW_MESSAGE_ITEM_LOOKUP, title, NULL);
  item->address = g_strdup(input->nip05);
  item->icon_name = g_strdup("system-search-symbolic");
  g_autofree gchar *mode = settings_string(self->config.settings, "network-mode", "system");
  gboolean tor = gh_net_mode_from_string(mode) == GH_NET_MODE_TOR;
  g_autofree gchar *subtitle = NULL;
  if (!self->config.nip05) {
    subtitle = g_strdup(_("Looking up addresses isn't available in this version"));
    item->enabled = FALSE;
  } else if (!tor && gh_net_host_is_onion(input->nip05_domain)) {
    subtitle = g_strdup(_(".onion addresses can only be reached through Tor"));
    item->enabled = FALSE;
  } else if (tor) {
    /* G09: the lookup goes through Tor (GhNetHttp in Tor mode). */
    /* TRANSLATORS: both %s are the same domain. */
    subtitle = g_strdup_printf(_("Connects to %s through Tor. %s learns whom you looked up, "
                                 "not your IP address"), input->nip05_domain, input->nip05_domain);
  } else {
    /* PD-2: the row names who learns what before anything is contacted. */
    subtitle = g_strdup_printf(_("Connects to %s, which learns your IP address and whom you "
                                 "looked up"), input->nip05_domain);
  }
  item_set_subtitle(item, subtitle);
  if (recipients_full(self))
    item->enabled = FALSE;
  return item;
}

static void
rebuild_suggestions(GhNewMessageDialog *self)
{
  g_autoptr(GhRecipientInput) input =
    gh_recipient_input_parse(gtk_editable_get_text(GTK_EDITABLE(self->entry)));
  g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
  const gboolean full = recipients_full(self);
  const gboolean alone = g_list_model_get_n_items(G_LIST_MODEL(self->recipients)) == 0;
  const gchar *error = NULL;
  switch (input->kind) {
  case GH_RECIPIENT_INPUT_EMPTY:
    if (alone)
      g_ptr_array_add(rows, note_to_self_item());
    add_contact_rows(self, rows, NULL, NULL);
    break;
  case GH_RECIPIENT_INPUT_PUBKEY:
    if (g_strcmp0(input->pubkey, account_of(self)) == 0) {
      if (alone)
        g_ptr_array_add(rows, note_to_self_item());
      else
        error = _("That's your own npub: you are always in your conversations.");
    } else if (is_recipient(self, input->pubkey)) {
      error = _("Already added.");
    } else {
      GhNewMessageItem *item = person_item(self, GH_NEW_MESSAGE_ITEM_PUBKEY, input->pubkey, NULL);
      g_autofree gchar *short_npub = gh_recipient_npub_short(input->pubkey);
      item_set_subtitle(item, name_of(self, input->pubkey) ? short_npub
                                                           : _("Add to the conversation"));
      item_set_enabled(item, !full);
      g_ptr_array_add(rows, item);
    }
    break;
  case GH_RECIPIENT_INPUT_NIP05:
    /* People you talk to whose profile claims the address: no network. */
    add_contact_rows(self, rows, NULL, input->nip05);
    g_ptr_array_add(rows, lookup_item(self, input));
    break;
  case GH_RECIPIENT_INPUT_SECRET:
    error = _("That's a secret key. Keep it private: Groundhog never needs it. Ask for their npub "
              "instead.");
    break;
  case GH_RECIPIENT_INPUT_OTHER_ENTITY:
    error = _("That points to a note or an event, not a person. Paste an npub or nprofile, or "
              "enter an address like name@example.com.");
    break;
  case GH_RECIPIENT_INPUT_INVALID:
    error = _("That npub or nostr: address isn't valid. Check that all of it was copied.");
    break;
  case GH_RECIPIENT_INPUT_TEXT:
    add_contact_rows(self, rows, input->text, NULL);
    break;
  }
  /* An emptied entry keeps the message that explains why it was emptied. */
  if (input->kind != GH_RECIPIENT_INPUT_EMPTY || error)
    show_error(self, error);
  g_list_store_splice(self->suggestions, 0,
                      g_list_model_get_n_items(G_LIST_MODEL(self->suggestions)),
                      rows->pdata, rows->len);
  gtk_widget_set_visible(GTK_WIDGET(self->results), rows->len > 0);
  /* Never keep a secret key in the entry, or on screen (this rebuilds the
   * suggestions for the empty entry). */
  if (input->kind == GH_RECIPIENT_INPUT_SECRET)
    gtk_editable_set_text(GTK_EDITABLE(self->entry), "");
}

static void
update_recipients(GhNewMessageDialog *self)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->recipients));
  gtk_widget_set_visible(GTK_WIDGET(self->chips), n > 0);
  gtk_widget_set_visible(GTK_WIDGET(self->limit_label), n >= GH_CONVERSATION_MAX_PEERS);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-message.next", n > 0);
  rebuild_suggestions(self);
}

/* FALSE when the limit or a duplicate refuses it. */
static gboolean
add_recipient(GhNewMessageDialog *self, const gchar *pubkey, const gchar *label)
{
  if (!gh_recipient_is_pubkey(pubkey) || g_strcmp0(pubkey, account_of(self)) == 0 ||
      is_recipient(self, pubkey))
    return FALSE;
  if (recipients_full(self)) {
    gtk_widget_set_visible(GTK_WIDGET(self->limit_label), TRUE);
    gtk_accessible_announce(GTK_ACCESSIBLE(self), gtk_label_get_text(self->limit_label),
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    return FALSE;
  }
  g_autoptr(GhNewMessageItem) item = person_item(self, GH_NEW_MESSAGE_ITEM_RECIPIENT, pubkey,
                                                  label);
  g_list_store_append(self->recipients, item);
  g_autofree gchar *added = g_strdup_printf(_("Added %s"), item->title);
  gtk_accessible_announce(GTK_ACCESSIBLE(self), added,
                          GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  gtk_editable_set_text(GTK_EDITABLE(self->entry), "");
  update_recipients(self);
  return TRUE;
}

/* ---- starting ---------------------------------------------------------------- */

static void
start(GhNewMessageDialog *self, gboolean note_to_self)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  guint n = note_to_self ? 0 : g_list_model_get_n_items(G_LIST_MODEL(self->recipients));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->recipients), i);
    g_strv_builder_add(builder, item->pubkey);
  }
  g_auto(GStrv) peers = g_strv_builder_end(builder);
  /* A one-to-one conversation: the configured protocol decides whether to
   * create a Marmot (MLS) DM or a NIP-17 room. When the peer has no usable
   * KeyPackage, the Marmot path falls back to NIP-17 (the caller of this
   * function handles the fallback once the create-group flow is wired). */
  gboolean want_marmot = !note_to_self && g_strv_length(peers) == 1
                         && default_dm_is_marmot(self);
  (void)want_marmot; /* wired by the Marmot DM creation flow */
  g_autoptr(GError) error = NULL;
  GhConversation *room = gh_conversation_store_open_room(self->config.conversations,
                                                         (const gchar *const *)peers, &error);
  if (!room) {
    g_message("Groundhog could not start a conversation: %s", error->message);
    show_error(self, g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED)
                       ? _("The conversation couldn't be started. Choose an account first.")
                       : _("The conversation couldn't be started."));
    adw_navigation_view_pop_to_tag(self->navigation, "pick");
    return;
  }
  /* Closing may drop the presenter's reference before the signal. */
  g_autoptr(GhNewMessageDialog) keep = g_object_ref(self);
  g_autoptr(GhConversation) started = g_object_ref(room);
  g_cancellable_cancel(self->cancellable);
  g_signal_emit(self, signals[SIGNAL_STARTED], 0, started);
  adw_dialog_close(ADW_DIALOG(self));
}

/* ---- NIP-05 lookup (consent row) -------------------------------------------- */

typedef struct {
  GhNewMessageDialog *self;
  GhNewMessageItem *item;
  guint round; /* the check round it belongs to */
} Pending;

static Pending *
pending_new(GhNewMessageDialog *self, GhNewMessageItem *item)
{
  Pending *pending = g_new0(Pending, 1);
  pending->self = g_object_ref(self);
  pending->item = g_object_ref(item);
  pending->round = self->check_round;
  return pending;
}

static void
pending_free(Pending *pending)
{
  g_object_unref(pending->self);
  g_object_unref(pending->item);
  g_free(pending);
}

static gchar *
lookup_failure(GhNewMessageItem *item, const GError *error)
{
  g_autofree gchar *local = NULL, *domain = NULL;
  gh_recipient_parse_nip05(item->address, &local, &domain);
  if (g_error_matches(error, GH_NIP05_ERROR, GH_NIP05_ERROR_NOT_FOUND) ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return g_strdup_printf(_("%s doesn't list %s"), domain, item->address);
  if (g_error_matches(error, GH_NIP05_ERROR, GH_NIP05_ERROR_RESPONSE))
    return g_strdup_printf(_("%s gave an answer Groundhog can't use"), domain);
  if (error->domain == GH_NIP05_ERROR)
    return g_strdup(error->message);
  return g_strdup_printf(_("Couldn't reach %s"), domain);
}

static void
on_looked_up(GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *pending = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *pubkey = gh_nip05_lookup_finish(GH_NIP05(source), result, &error);
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    pending_free(pending);
    return;
  }
  GhNewMessageDialog *self = pending->self;
  GhNewMessageItem *item = pending->item;
  item_set_busy(item, FALSE);
  if (!pubkey) {
    g_autofree gchar *why = lookup_failure(item, error);
    item_set_subtitle(item, why);
    gtk_accessible_announce(GTK_ACCESSIBLE(self), why, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  } else if (g_strcmp0(pubkey, account_of(self)) == 0) {
    item_set_subtitle(item, _("That's your own address"));
  } else if (!add_recipient(self, pubkey, item->address)) {
    item_set_subtitle(item, is_recipient(self, pubkey) ? _("Already added") : NULL);
  }
  pending_free(pending);
}

static void
look_up(GhNewMessageDialog *self, GhNewMessageItem *item)
{
  if (item->busy || !item->enabled || !self->config.nip05)
    return;
  g_autofree gchar *local = NULL, *domain = NULL;
  if (!gh_recipient_parse_nip05(item->address, &local, &domain))
    return;
  item_set_busy(item, TRUE);
  g_autofree gchar *asking = g_strdup_printf(_("Asking %s…"), domain);
  item_set_subtitle(item, asking);
  gh_nip05_lookup_async(self->config.nip05, item->address, self->cancellable, on_looked_up,
                        pending_new(self, item));
}

/* ---- the confirm page and the inbox check (consent row) ---------------------- */

static void
update_check_row(GhNewMessageDialog *self, gboolean done)
{
  g_autofree gchar *hosts = discovery_hosts(self);
  g_autofree gchar *subtitle = NULL;
  gboolean possible = self->config.inboxes && hosts;
  if (!self->config.inboxes)
    subtitle = g_strdup(_("Checking isn't available in this version"));
  else if (!hosts)
    subtitle = g_strdup(_("Add a discovery relay in Preferences to check this"));
  else if (done)
    subtitle = g_strdup_printf(_("Checked with %s. Choose to check again."), hosts);
  else
    subtitle = g_strdup_printf(_("Asks your discovery relays (%s) for their message relays. "
                                 "Those relays learn whom you asked about."), hosts);
  adw_action_row_set_subtitle(self->check_row, subtitle);
  gtk_widget_set_sensitive(GTK_WIDGET(self->check_row), possible && !self->checks_pending);
  gtk_widget_set_visible(GTK_WIDGET(self->check_spinner), self->checks_pending > 0);
  gtk_spinner_set_spinning(self->check_spinner, self->checks_pending > 0);
}

static const gchar *
inbox_status_text(const GhInboxResult *result)
{
  switch (result->status) {
  case GH_INBOX_FOUND:
    return _("Can receive private messages");
  case GH_INBOX_EMPTY:
  case GH_INBOX_NOT_FOUND:
    return _("Hasn't set up private messaging yet");
  case GH_INBOX_UNREACHABLE:
    return _("Couldn't reach your discovery relays");
  case GH_INBOX_NO_SOURCES:
  default:
    return _("No discovery relays to ask");
  }
}

static void
on_checked(GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *pending = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhInboxResult) inbox =
    gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source), result, &error);
  GhNewMessageDialog *self = pending->self;
  GhNewMessageItem *item = pending->item;
  /* A cancelled round's late answer counts for nothing: the count is the
   * current round's (reset_cancellable() zeroed it for the next one). */
  if (pending->round == self->check_round &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    if (self->checks_pending)
      self->checks_pending--;
    item_set_busy(item, FALSE);
    item_set_status(item, inbox ? inbox_status_text(inbox) : _("Couldn't check"));
    if (!self->checks_pending) {
      update_check_row(self, TRUE);
      gtk_accessible_announce(GTK_ACCESSIBLE(self), _("Check finished"),
                              GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    }
  }
  pending_free(pending);
}

static void
on_check_activated(GhNewMessageDialog *self)
{
  if (!self->config.inboxes || self->checks_pending)
    return;
  g_autofree gchar *hosts = discovery_hosts(self);
  if (!hosts)
    return;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->people_items));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->people_items), i);
    item_set_busy(item, TRUE);
    item_set_status(item, _("Checking…"));
    self->checks_pending++;
  }
  update_check_row(self, FALSE);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->people_items), i);
    gh_inbox_resolver_resolve_async(self->config.inboxes, item->pubkey, self->cancellable,
                                    on_checked, pending_new(self, item));
  }
}

static void
reset_cancellable(GhNewMessageDialog *self)
{
  g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();
  self->checks_pending = 0;
  self->check_round++;
}

static void
on_next(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhNewMessageDialog *self = GH_NEW_MESSAGE_DIALOG(widget);
  (void)action;
  (void)parameter;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->recipients));
  if (!n)
    return;
  /* A check made for other recipients no longer applies. */
  reset_cancellable(self);
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhNewMessageItem) chosen = g_list_model_get_item(G_LIST_MODEL(self->recipients), i);
    GhNewMessageItem *item = person_item(self, GH_NEW_MESSAGE_ITEM_PERSON, chosen->pubkey,
                                         chosen->title);
    g_autofree gchar *grouped = gh_recipient_npub_grouped(chosen->pubkey);
    item_set_subtitle(item, grouped);
    item_set_status(item, _("Not checked"));
    g_ptr_array_add(items, item);
  }
  g_list_store_splice(self->people_items, 0,
                      g_list_model_get_n_items(G_LIST_MODEL(self->people_items)),
                      items->pdata, items->len);
  update_check_row(self, FALSE);
  /* A conversation the user blocked is unblocked by starting it (G18,
   * gh_conversation_store_open_room()): said before, not after. */
  g_autoptr(GStrvBuilder) chosen_keys = g_strv_builder_new();
  for (guint i = 0; i < items->len; i++)
    g_strv_builder_add(chosen_keys, ((GhNewMessageItem *)g_ptr_array_index(items, i))->pubkey);
  g_auto(GStrv) peers = g_strv_builder_end(chosen_keys);
  gboolean blocked = gh_conversation_store_is_blocked(self->config.conversations,
                                                      (const gchar *const *)peers);
  /* Honest state (P4, W17): a room message is encrypted separately for each
   * person, and each of them sees who else is in the conversation (the
   * rumor names everyone). */
  const gchar *note = n > 1
    ? _("Nothing is sent until you write a message. Each message is encrypted separately "
        "for each person, and everyone in the conversation can see who else is in it.")
    : _("Nothing is sent until you write a message.");
  g_autofree gchar *text = blocked
    ? g_strconcat(_("You blocked this conversation. Starting it unblocks it: their new "
                    "messages show and notify again. Messages they sent while it was blocked "
                    "aren't shown."), "\n\n", note, NULL)
    : g_strdup(note);
  gtk_label_set_text(self->start_note, text);
  adw_navigation_view_push(self->navigation, self->confirm_page);
}

static void
on_start(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  start(GH_NEW_MESSAGE_DIALOG(widget), FALSE);
}

static void
on_remove(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhNewMessageDialog *self = GH_NEW_MESSAGE_DIALOG(widget);
  (void)action;
  const gchar *pubkey = g_variant_get_string(parameter, NULL);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->recipients));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhNewMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->recipients), i);
    if (g_strcmp0(item->pubkey, pubkey) != 0)
      continue;
    g_autofree gchar *removed = g_strdup_printf(_("Removed %s"), item->title);
    g_list_store_remove(self->recipients, i);
    gtk_accessible_announce(GTK_ACCESSIBLE(self), removed,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    update_recipients(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->entry));
    return;
  }
}

/* What choosing a row does. Only the LOOKUP row contacts anything. */
static void
activate_item(GhNewMessageDialog *self, GhNewMessageItem *item)
{
  if (!item->enabled)
    return;
  switch (item->kind) {
  case GH_NEW_MESSAGE_ITEM_NOTE_TO_SELF:
    start(self, TRUE);
    break;
  case GH_NEW_MESSAGE_ITEM_CONTACT:
  case GH_NEW_MESSAGE_ITEM_PUBKEY:
    if (add_recipient(self, item->pubkey, NULL))
      gtk_widget_grab_focus(GTK_WIDGET(self->entry));
    break;
  case GH_NEW_MESSAGE_ITEM_LOOKUP:
    look_up(self, item);
    break;
  default:
    break;
  }
}

static void
on_row_activated(GhNewMessageDialog *self, GtkListBoxRow *row)
{
  gint index = gtk_list_box_row_get_index(row);
  if (index < 0)
    return;
  g_autoptr(GhNewMessageItem) item =
    g_list_model_get_item(G_LIST_MODEL(self->suggestions), (guint)index);
  if (item)
    activate_item(self, item);
}

/* Enter adds an exact or a local match; it never looks anything up. */
static void
on_entry_activate(GhNewMessageDialog *self)
{
  if (!*gtk_editable_get_text(GTK_EDITABLE(self->entry)))
    return;
  g_autoptr(GhNewMessageItem) first = g_list_model_get_item(G_LIST_MODEL(self->suggestions), 0);
  if (first && (first->kind == GH_NEW_MESSAGE_ITEM_PUBKEY ||
                first->kind == GH_NEW_MESSAGE_ITEM_CONTACT))
    activate_item(self, first);
}

GListModel *
gh_new_message_dialog_get_suggestions(GhNewMessageDialog *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_DIALOG(self), NULL);
  return G_LIST_MODEL(self->suggestions);
}

GListModel *
gh_new_message_dialog_get_recipients(GhNewMessageDialog *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_DIALOG(self), NULL);
  return G_LIST_MODEL(self->recipients);
}

GListModel *
gh_new_message_dialog_get_people(GhNewMessageDialog *self)
{
  g_return_val_if_fail(GH_IS_NEW_MESSAGE_DIALOG(self), NULL);
  return G_LIST_MODEL(self->people_items);
}

GhNewMessageDialog *
gh_new_message_dialog_new(const GhNewMessageConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(config->conversations), NULL);
  g_return_val_if_fail(!config->inboxes || GH_IS_INBOX_RESOLVER(config->inboxes), NULL);
  g_return_val_if_fail(!config->nip05 || GH_IS_NIP05(config->nip05), NULL);
  g_return_val_if_fail(!config->settings || G_IS_SETTINGS(config->settings), NULL);
  GhNewMessageDialog *self = g_object_new(GH_TYPE_NEW_MESSAGE_DIALOG, NULL);
  self->config = *config;
  g_object_ref(config->conversations);
  if (config->inboxes)
    g_object_ref(config->inboxes);
  if (config->nip05)
    g_object_ref(config->nip05);
  if (config->settings)
    g_object_ref(config->settings);
  /* The template says larger groups aren't available; only a build that
   * creates encrypted groups points to them (charter §7.9, P4). */
  if (config->encrypted_groups)
    gtk_label_set_text(self->limit_label,
                       _("A private conversation can include up to 10 people. "
                         "For larger groups, create an encrypted group."));
  update_recipients(self);
  return self;
}

static void
gh_new_message_dialog_closed(AdwDialog *dialog)
{
  GhNewMessageDialog *self = GH_NEW_MESSAGE_DIALOG(dialog);
  /* Nothing asked for continues once the dialog is gone. */
  g_cancellable_cancel(self->cancellable);
  if (ADW_DIALOG_CLASS(gh_new_message_dialog_parent_class)->closed)
    ADW_DIALOG_CLASS(gh_new_message_dialog_parent_class)->closed(dialog);
}

static void
gh_new_message_dialog_dispose(GObject *object)
{
  GhNewMessageDialog *self = GH_NEW_MESSAGE_DIALOG(object);
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  if (self->results) {
    gtk_list_box_bind_model(self->results, NULL, NULL, NULL, NULL);
    gtk_list_box_bind_model(self->people, NULL, NULL, NULL, NULL);
    gtk_flow_box_bind_model(self->chips, NULL, NULL, NULL, NULL);
  }
  /* navigation is both the dialog's child and a bound template child. */
  adw_dialog_set_child(ADW_DIALOG(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_NEW_MESSAGE_DIALOG);
  g_clear_object(&self->config.conversations);
  g_clear_object(&self->config.inboxes);
  g_clear_object(&self->config.nip05);
  g_clear_object(&self->config.settings);
  g_clear_object(&self->suggestions);
  g_clear_object(&self->recipients);
  g_clear_object(&self->people_items);
  g_clear_object(&self->cancellable);
  G_OBJECT_CLASS(gh_new_message_dialog_parent_class)->dispose(object);
}

static void
gh_new_message_dialog_class_init(GhNewMessageDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_new_message_dialog_dispose;
  ADW_DIALOG_CLASS(klass)->closed = gh_new_message_dialog_closed;
  signals[SIGNAL_STARTED] =
    g_signal_new("conversation-started", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                 NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_CONVERSATION);
  g_type_ensure(GH_TYPE_NEW_MESSAGE_ITEM);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-new-message-dialog.ui");
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, navigation);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, confirm_page);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, entry);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, chips);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, limit_label);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, input_error);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, results);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, people);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, check_row);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, check_spinner);
  gtk_widget_class_bind_template_child(widget_class, GhNewMessageDialog, start_note);
  gtk_widget_class_install_action(widget_class, "new-message.next", NULL, on_next);
  gtk_widget_class_install_action(widget_class, "new-message.start", NULL, on_start);
  gtk_widget_class_install_action(widget_class, "new-message.remove", "s", on_remove);
}

static void
gh_new_message_dialog_init(GhNewMessageDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->suggestions = g_list_store_new(GH_TYPE_NEW_MESSAGE_ITEM);
  self->recipients = g_list_store_new(GH_TYPE_NEW_MESSAGE_ITEM);
  self->people_items = g_list_store_new(GH_TYPE_NEW_MESSAGE_ITEM);
  self->cancellable = g_cancellable_new();
  gtk_list_box_bind_model(self->results, G_LIST_MODEL(self->suggestions), create_suggestion_row,
                          NULL, NULL);
  gtk_list_box_bind_model(self->people, G_LIST_MODEL(self->people_items), create_person_row,
                          NULL, NULL);
  gtk_flow_box_bind_model(self->chips, G_LIST_MODEL(self->recipients), create_chip, NULL, NULL);
  g_signal_connect_object(self->results, "row-activated", G_CALLBACK(on_row_activated), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->entry, "search-changed", G_CALLBACK(rebuild_suggestions), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->entry, "activate", G_CALLBACK(on_entry_activate), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->check_row, "activated", G_CALLBACK(on_check_activated), self,
                          G_CONNECT_SWAPPED);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-message.next", FALSE);
}

/* ---- window glue ---------------------------------------------------------------- */

typedef struct {
  GhNewMessageConfig config;
  GhWindow *window;        /* the attachment is the window's data */
  GWeakRef started;        /* the conversation the open dialog started */
  guint focus_idle;
} Attachment;

static void
attachment_free(gpointer data)
{
  Attachment *attachment = data;
  g_clear_handle_id(&attachment->focus_idle, g_source_remove);
  g_weak_ref_clear(&attachment->started);
  g_clear_object(&attachment->config.conversations);
  g_clear_object(&attachment->config.inboxes);
  g_clear_object(&attachment->config.nip05);
  g_clear_object(&attachment->config.settings);
  g_free(attachment);
}

/* The started conversation is on screen: focus its composer when sending is
 * possible (charter §7.14), else the conversation. */
static gboolean
focus_started(gpointer data)
{
  Attachment *attachment = data;
  attachment->focus_idle = 0;
  g_autoptr(GhConversation) started = g_weak_ref_get(&attachment->started);
  g_weak_ref_set(&attachment->started, NULL);
  GhContentPage *content = gh_window_get_content(attachment->window);
  if (started && gh_content_page_get_conversation_shown(content))
    gh_content_page_focus_conversation(content);
  return G_SOURCE_REMOVE;
}

static void
on_started(GhNewMessageDialog *dialog, GhConversation *conversation, Attachment *attachment)
{
  (void)dialog;
  if (gh_window_open_item(attachment->window, conversation))
    g_weak_ref_set(&attachment->started, conversation);
}

/* After the dialog handed focus back to where it was (AdwDialog does that
 * as it closes), move it into the started conversation. */
static void
on_closed(GhNewMessageDialog *dialog, Attachment *attachment)
{
  (void)dialog;
  g_autoptr(GhConversation) started = g_weak_ref_get(&attachment->started);
  if (started && !attachment->focus_idle)
    attachment->focus_idle = g_idle_add(focus_started, attachment);
}

static void
present_dialog(GhWindow *window, gpointer data)
{
  Attachment *attachment = data;
  GhNewMessageDialog *dialog = gh_new_message_dialog_new(&attachment->config);
  g_signal_connect(dialog, "conversation-started", G_CALLBACK(on_started), attachment);
  g_signal_connect_after(dialog, "closed", G_CALLBACK(on_closed), attachment);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
}

static void
sync_enabled(GhWindow *window, GParamSpec *pspec, GhConversationStore *conversations)
{
  (void)pspec;
  gh_window_set_new_message_enabled(window,
                                    gh_conversation_store_get_account(conversations) != NULL);
}

void
gh_new_message_attach(GhWindow *window, const GhNewMessageConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL && GH_IS_CONVERSATION_STORE(config->conversations));
  Attachment *attachment = g_new0(Attachment, 1);
  attachment->config = *config;
  attachment->window = window;
  g_weak_ref_init(&attachment->started, NULL);
  g_object_ref(config->conversations);
  if (config->inboxes)
    g_object_ref(config->inboxes);
  if (config->nip05)
    g_object_ref(config->nip05);
  if (config->settings)
    g_object_ref(config->settings);
  gh_window_set_new_message_handler(window, present_dialog, attachment, attachment_free);
  g_signal_connect_object(config->conversations, "notify::account", G_CALLBACK(sync_enabled),
                          window, G_CONNECT_SWAPPED);
  sync_enabled(window, NULL, config->conversations);
}
