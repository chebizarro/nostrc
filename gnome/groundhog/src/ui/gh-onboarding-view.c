#include "gh-onboarding-view.h"
#include "gh-relay-list-setup.h"
#include "gh-identity.h"
#include "gh-signer.h"

#include <glib/gi18n.h>
#include <nostr-event.h>
#include <stdlib.h>
#include <string.h>

/* The signer test signs an event of a kind in NIP-01's ephemeral range
 * (20000-29999, which relays do not store) that no NIP assigns, and never
 * sends it anywhere. */
#define SIGNER_TEST_KIND 27001
#define SIGNER_TEST_TEXT "Groundhog signer test. This is never published."

/* ---- GhOnboardingItem ------------------------------------------------------- */

struct _GhOnboardingItem {
  GObject parent_instance;
  gchar *key;
  gchar *title;
  gchar *subtitle;
  gchar *icon_name;
  gchar *accessible_description;
  gboolean checked;
  gboolean checkable; /* a choice: its state is part of the description */
};

enum {
  ITEM_PROP_0,
  ITEM_PROP_KEY,
  ITEM_PROP_TITLE,
  ITEM_PROP_SUBTITLE,
  ITEM_PROP_HAS_SUBTITLE,
  ITEM_PROP_CHECKED,
  ITEM_PROP_CHECKABLE,
  ITEM_PROP_ICON_NAME,
  ITEM_PROP_HAS_ICON,
  ITEM_PROP_ACCESSIBLE_DESCRIPTION,
  ITEM_N_PROPS
};
static GParamSpec *item_props[ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(GhOnboardingItem, gh_onboarding_item, G_TYPE_OBJECT)

static void
item_update_description(GhOnboardingItem *self)
{
  const gchar *subtitle = self->subtitle ? self->subtitle : "";
  g_autofree gchar *description =
    self->checkable ? g_strdup_printf("%s %s", self->checked ? _("Ticked.") : _("Not ticked."),
                                      subtitle)
                    : g_strdup(subtitle);
  if (g_strcmp0(description, self->accessible_description) == 0)
    return;
  g_free(self->accessible_description);
  self->accessible_description = g_steal_pointer(&description);
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_ACCESSIBLE_DESCRIPTION]);
}

static void
item_set_subtitle(GhOnboardingItem *self, const gchar *subtitle)
{
  subtitle = subtitle ? subtitle : "";
  if (g_strcmp0(self->subtitle, subtitle) == 0)
    return;
  gboolean had = self->subtitle && *self->subtitle;
  g_free(self->subtitle);
  self->subtitle = g_strdup(subtitle);
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_SUBTITLE]);
  if (had != (*subtitle != '\0'))
    g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_HAS_SUBTITLE]);
  item_update_description(self);
}

static void
item_set_checked(GhOnboardingItem *self, gboolean checked)
{
  if (self->checked == !!checked)
    return;
  self->checked = !!checked;
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_CHECKED]);
  item_update_description(self);
}

static void
item_set_icon_name(GhOnboardingItem *self, const gchar *icon_name)
{
  icon_name = icon_name ? icon_name : "";
  if (g_strcmp0(self->icon_name, icon_name) == 0)
    return;
  gboolean had = self->icon_name && *self->icon_name;
  g_free(self->icon_name);
  self->icon_name = g_strdup(icon_name);
  g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_ICON_NAME]);
  if (had != (*icon_name != '\0'))
    g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_HAS_ICON]);
}

static GhOnboardingItem *
item_new(const gchar *key, const gchar *title, gboolean checkable)
{
  GhOnboardingItem *self = g_object_new(GH_TYPE_ONBOARDING_ITEM, NULL);
  self->key = g_strdup(key);
  self->title = g_strdup(title);
  self->checkable = checkable;
  item_update_description(self);
  return self;
}

const gchar *
gh_onboarding_item_get_key(GhOnboardingItem *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_ITEM(self), NULL);
  return self->key;
}

const gchar *
gh_onboarding_item_get_title(GhOnboardingItem *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_ITEM(self), NULL);
  return self->title;
}

const gchar *
gh_onboarding_item_get_subtitle(GhOnboardingItem *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_ITEM(self), NULL);
  return self->subtitle;
}

gboolean
gh_onboarding_item_get_checked(GhOnboardingItem *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_ITEM(self), FALSE);
  return self->checked;
}

const gchar *
gh_onboarding_item_get_icon_name(GhOnboardingItem *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_ITEM(self), NULL);
  return self->icon_name;
}

static void
gh_onboarding_item_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhOnboardingItem *self = GH_ONBOARDING_ITEM(object);
  switch (id) {
  case ITEM_PROP_KEY:
    g_value_set_string(value, self->key);
    break;
  case ITEM_PROP_TITLE:
    g_value_set_string(value, self->title);
    break;
  case ITEM_PROP_SUBTITLE:
    g_value_set_string(value, self->subtitle);
    break;
  case ITEM_PROP_HAS_SUBTITLE:
    g_value_set_boolean(value, self->subtitle && *self->subtitle);
    break;
  case ITEM_PROP_CHECKED:
    g_value_set_boolean(value, self->checked);
    break;
  case ITEM_PROP_CHECKABLE:
    g_value_set_boolean(value, self->checkable);
    break;
  case ITEM_PROP_ICON_NAME:
    g_value_set_string(value, self->icon_name);
    break;
  case ITEM_PROP_HAS_ICON:
    g_value_set_boolean(value, self->icon_name && *self->icon_name);
    break;
  case ITEM_PROP_ACCESSIBLE_DESCRIPTION:
    g_value_set_string(value, self->accessible_description);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_onboarding_item_finalize(GObject *object)
{
  GhOnboardingItem *self = GH_ONBOARDING_ITEM(object);
  g_free(self->key);
  g_free(self->title);
  g_free(self->subtitle);
  g_free(self->icon_name);
  g_free(self->accessible_description);
  G_OBJECT_CLASS(gh_onboarding_item_parent_class)->finalize(object);
}

static void
gh_onboarding_item_class_init(GhOnboardingItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  object_class->get_property = gh_onboarding_item_get_property;
  object_class->finalize = gh_onboarding_item_finalize;
  item_props[ITEM_PROP_KEY] = g_param_spec_string("key", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_SUBTITLE] = g_param_spec_string("subtitle", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_HAS_SUBTITLE] = g_param_spec_boolean("has-subtitle", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_CHECKED] = g_param_spec_boolean("checked", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_CHECKABLE] = g_param_spec_boolean("checkable", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_ICON_NAME] = g_param_spec_string("icon-name", NULL, NULL, NULL, ro);
  item_props[ITEM_PROP_HAS_ICON] = g_param_spec_boolean("has-icon", NULL, NULL, FALSE, ro);
  item_props[ITEM_PROP_ACCESSIBLE_DESCRIPTION] =
    g_param_spec_string("accessible-description", NULL, NULL, NULL, ro);
  g_object_class_install_properties(object_class, ITEM_N_PROPS, item_props);
}

static void
gh_onboarding_item_init(GhOnboardingItem *self)
{
  (void)self;
}

/* ---- GhOnboardingRow ------------------------------------------------------ */

/* A row of the onboarding lists (data/ui/gh-onboarding-row.blp): the
 * template binds its title, subtitle, check and icon to the item. Here it
 * only says whether a choice is ticked to assistive technology, since the
 * check itself is presentation. */
#define GH_TYPE_ONBOARDING_ROW (gh_onboarding_row_get_type())
G_DECLARE_FINAL_TYPE(GhOnboardingRow, gh_onboarding_row, GH, ONBOARDING_ROW, AdwActionRow)

struct _GhOnboardingRow {
  AdwActionRow parent_instance;
  GhOnboardingItem *item;
};

enum { ROW_PROP_0, ROW_PROP_ITEM, ROW_N_PROPS };
static GParamSpec *row_props[ROW_N_PROPS];

G_DEFINE_FINAL_TYPE(GhOnboardingRow, gh_onboarding_row, ADW_TYPE_ACTION_ROW)

static void
row_update_description(GhOnboardingRow *self)
{
  if (self->item)
    gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                   self->item->accessible_description ?
                                     self->item->accessible_description : "",
                                   -1);
}

static void
gh_onboarding_row_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == ROW_PROP_ITEM)
    g_value_set_object(value, GH_ONBOARDING_ROW(object)->item);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gh_onboarding_row_set_property(GObject *object, guint id, const GValue *value,
                               GParamSpec *pspec)
{
  GhOnboardingRow *self = GH_ONBOARDING_ROW(object);
  if (id != ROW_PROP_ITEM) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    return;
  }
  self->item = g_value_dup_object(value);
  if (self->item) {
    /* After the template's subtitle binding, which sets the description
     * from the subtitle. */
    g_signal_connect_object(self->item, "notify::accessible-description",
                            G_CALLBACK(row_update_description), self,
                            G_CONNECT_SWAPPED | G_CONNECT_AFTER);
    row_update_description(self);
  }
}

static void
gh_onboarding_row_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_ONBOARDING_ROW);
  g_clear_object(&GH_ONBOARDING_ROW(object)->item);
  G_OBJECT_CLASS(gh_onboarding_row_parent_class)->dispose(object);
}

static void
gh_onboarding_row_class_init(GhOnboardingRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->get_property = gh_onboarding_row_get_property;
  object_class->set_property = gh_onboarding_row_set_property;
  object_class->dispose = gh_onboarding_row_dispose;
  row_props[ROW_PROP_ITEM] = g_param_spec_object("item", NULL, NULL, GH_TYPE_ONBOARDING_ITEM,
    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, ROW_N_PROPS, row_props);
  g_type_ensure(GH_TYPE_ONBOARDING_ITEM);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-onboarding-row.ui");
}

static void
gh_onboarding_row_init(GhOnboardingRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

/* A list box row for an item; choices and identities are activatable. */
static GtkWidget *
create_choice_row(gpointer item, gpointer data)
{
  (void)data;
  return g_object_new(GH_TYPE_ONBOARDING_ROW, "item", item, "activatable", TRUE, NULL);
}

static GtkWidget *
create_result_row(gpointer item, gpointer data)
{
  (void)data;
  return g_object_new(GH_TYPE_ONBOARDING_ROW, "item", item, "activatable", FALSE, NULL);
}

/* ---- GhOnboardingView ----------------------------------------------------- */

typedef enum { ORIGIN_CURRENT, ORIGIN_SUGGESTED, ORIGIN_CUSTOM } Origin;

/* The account-state pages reused from gh-account-ui.blp, by stack name. */
static const struct {
  const gchar *id;
  const gchar *name;
} account_pages[] = {
  { "account_discovering", "discovering" },
  { "account_store_unavailable", "unavailable" },
  { "account_none", "none" },
};

struct _GhOnboardingView {
  AdwBin parent_instance;

  AdwNavigationView *navigation;
  AdwNavigationPage *welcome_page;
  AdwNavigationPage *account_page;
  AdwNavigationPage *signer_page;
  AdwNavigationPage *inbox_page;
  AdwNavigationPage *confirm_page;
  AdwNavigationPage *publish_page;
  AdwNavigationPage *done_page;
  GtkWidget *start_button;
  GtkStack *account_stack;
  GtkListBox *identity_list;
  AdwActionRow *signer_status;
  GtkImage *signer_icon;
  GtkWidget *signer_test;
  GtkSpinner *signer_spinner;
  GtkLabel *signer_result;
  GtkLabel *current_note;
  GtkListBox *relay_list;
  AdwEntryRow *custom_entry;
  GtkLabel *custom_error;
  GtkWidget *keep_button;
  AdwActionRow *confirm_targets;
  GtkWidget *discovery_group;
  AdwSwitchRow *discovery_switch;
  GtkWidget *relay_list_group;
  AdwSwitchRow *relay_list_switch;
  GtkWidget *relay_list_later_group;
  AdwActionRow *relay_list_later_row;
  GtkWidget *relay_list_later_button;
  GhRelayListSetup *later;   /* the relay list offered on the result page (R2) */
  GtkWidget *publish_button;
  GtkImage *publish_icon;
  GtkLabel *publish_title;
  GtkLabel *publish_description;
  GtkListBox *result_list;
  GtkWidget *publish_continue;
  GtkWidget *publish_retry;
  GtkWidget *publish_later;
  AdwStatusPage *done_status;
  GtkWidget *start_conversation_button;
  GtkWidget *done_button;
  GtkLabel *check_note;

  GhInboxSetupConfig config; /* accounts, account_relays, settings are owned refs */
  GListStore *identities;    /* GhOnboardingItem, key npub */
  GListStore *relays;        /* GhOnboardingItem, key URL */
  GListStore *results;       /* GhOnboardingItem, key URL */
  GHashTable *origins;       /* URL -> Origin */
  GHashTable *reviewed;      /* URL -> GhInboxSuggestion (borrowed from suggestions) */
  GPtrArray *suggestions;    /* GhInboxSuggestion */
  GhInboxSetup *setup;       /* the confirmed publication (or the plan's) */
  GhInboxProbe *probe;       /* Check Privacy */
  GCancellable *signer_test_cancellable;
  gchar *signer_test_pubkey;
  gboolean kept_current;     /* finished with the published list unchanged */
};

enum { SIGNAL_FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhOnboardingView, gh_onboarding_view, ADW_TYPE_BIN)

static const gchar *
host_of(const gchar *url)
{
  return g_str_has_prefix(url, "wss://") ? url + strlen("wss://") : url;
}

/* An npub as "npub1abcd efgh … wxyz": enough to recognize, short to read. */
static gchar *
short_npub(const gchar *npub)
{
  gsize length = strlen(npub);
  if (length <= 20)
    return g_strdup(npub);
  return g_strdup_printf("%.9s %.4s … %s", npub, npub + 9, npub + length - 4);
}

static void
announce(GhOnboardingView *self, const gchar *text)
{
  if (text && *text)
    gtk_accessible_announce(GTK_ACCESSIBLE(self), text, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

static void
push(GhOnboardingView *self, const gchar *tag)
{
  AdwNavigationPage *visible = adw_navigation_view_get_visible_page(self->navigation);
  if (visible && g_strcmp0(adw_navigation_page_get_tag(visible), tag) == 0)
    return;
  adw_navigation_view_push_by_tag(self->navigation, tag);
}

static void
finish(GhOnboardingView *self)
{
  if (self->probe) {
    gh_inbox_probe_cancel(self->probe);
    g_clear_pointer(&self->probe, gh_inbox_probe_unref);
  }
  g_signal_emit(self, signals[SIGNAL_FINISHED], 0);
}

static GtkWindow *
window_of(GhOnboardingView *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static void
toast(GhOnboardingView *self, const gchar *text)
{
  GtkWindow *window = window_of(self);
  if (GH_IS_WINDOW(window))
    adw_toast_overlay_add_toast(gh_window_get_toasts(GH_WINDOW(window)), adw_toast_new(text));
  announce(self, text);
}

/* Plain-language copy for a signer failure; NULL for a cancellation. */
static gchar *
signer_error_text(const GError *error)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED))
    return NULL;
  if (error->domain == GH_SIGNER_ERROR) {
    switch (error->code) {
    case GH_SIGNER_ERROR_DENIED:
      return g_strdup(_("You declined the request in Nostr Signer. That's fine: nothing was "
                        "sent, and Groundhog will ask again when it needs to."));
    case GH_SIGNER_ERROR_TIMED_OUT:
      return g_strdup(_("Nostr Signer didn't get an answer in time. Try again when you're "
                        "ready to approve."));
    case GH_SIGNER_ERROR_UNAVAILABLE:
      return g_strdup(_("Nostr Signer isn't running, so it couldn't answer."));
    case GH_SIGNER_ERROR_NO_APPROVER:
      return g_strdup(_("Nostr Signer has no way to ask you right now. Make sure it can show "
                        "its approval window."));
    case GH_SIGNER_ERROR_KEY_MISMATCH:
      return g_strdup(_("Nostr Signer answered with a different key than this account's."));
    case GH_SIGNER_ERROR_INVALID_RESULT:
      return g_strdup(_("Nostr Signer's answer didn't check out, so Groundhog ignored it."));
    default:
      break;
    }
  }
  return g_strdup_printf(_("Nostr Signer couldn't finish: %s"), error->message);
}

/* ---- account ------------------------------------------------------------- */

static void
update_identities(GhOnboardingView *self)
{
  GhAccountController *accounts = self->config.accounts;
  GhAccountState state = gh_account_controller_get_state(accounts);
  GPtrArray *identities = gh_account_controller_get_identities(accounts);
  const gchar *active = gh_account_controller_get_active_npub(accounts);
  const gchar *page = "list";
  if (state == GH_ACCOUNT_STATE_DISCOVERING && !identities)
    page = "discovering";
  else if (state == GH_ACCOUNT_STATE_STORE_UNAVAILABLE)
    page = "unavailable";
  else if (state == GH_ACCOUNT_STATE_NO_IDENTITIES)
    page = "none";
  gtk_stack_set_visible_child_name(self->account_stack, page);

  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    g_autofree gchar *npub = short_npub(info->npub);
    const gchar *title = info->label && *info->label ? info->label : npub;
    GhOnboardingItem *item = item_new(info->npub, title, FALSE);
    gboolean is_active = g_strcmp0(info->npub, active) == 0;
    g_autofree gchar *subtitle =
      is_active ? g_strdup_printf(_("%s · Chosen"), npub) : g_strdup(npub);
    item_set_subtitle(item, subtitle);
    item_set_icon_name(item, is_active ? "object-select-symbolic" : NULL);
    g_ptr_array_add(items, item);
  }
  g_list_store_splice(self->identities, 0, g_list_model_get_n_items(G_LIST_MODEL(self->identities)),
                      items->pdata, items->len);
  gboolean ready = state == GH_ACCOUNT_STATE_ACTIVE;
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.account-continue", ready);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.test-signer",
                                ready && !self->signer_test_cancellable);
}

static void
on_identity_activate(GhOnboardingView *self, GtkListBoxRow *row)
{
  g_autoptr(GhOnboardingItem) item = g_list_model_get_item(G_LIST_MODEL(self->identities),
                                                           gtk_list_box_row_get_index(row));
  g_autoptr(GError) error = NULL;
  if (!item)
    return;
  /* Writes only org.nostr.Groundhog current-npub (UX-6). */
  if (!gh_account_controller_select(self->config.accounts, item->key, &error))
    toast(self, error->message);
}

/* ---- signer ----------------------------------------------------------------- */

static void
update_signer_status(GhOnboardingView *self)
{
  const gchar *title, *subtitle, *icon;
  switch (gh_account_controller_get_signer_availability(self->config.accounts)) {
  case GH_SIGNER_AVAILABILITY_RUNNING:
    title = _("Nostr Signer is running");
    subtitle = _("It's ready to ask you whenever Groundhog needs a signature.");
    icon = "emblem-ok-symbolic";
    break;
  case GH_SIGNER_AVAILABILITY_ACTIVATABLE:
    title = _("Nostr Signer is installed");
    subtitle = _("It starts by itself when Groundhog needs it.");
    icon = "emblem-ok-symbolic";
    break;
  case GH_SIGNER_AVAILABILITY_ABSENT:
    title = _("Nostr Signer isn't available");
    subtitle = _("Install or start Nostr Signer. Until then you can look around, but not "
                 "send or unlock messages.");
    icon = "dialog-warning-symbolic";
    break;
  case GH_SIGNER_AVAILABILITY_NO_BUS:
    title = _("Nostr Signer can't be reached");
    subtitle = _("This session has no message bus to reach the signer on.");
    icon = "dialog-warning-symbolic";
    break;
  case GH_SIGNER_AVAILABILITY_UNKNOWN:
  default:
    title = _("Looking for Nostr Signer…");
    subtitle = "";
    icon = "dialog-password-symbolic";
    break;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->signer_status), title);
  adw_action_row_set_subtitle(self->signer_status, subtitle);
  gtk_image_set_from_icon_name(self->signer_icon, icon);
}

static void
signer_test_show(GhOnboardingView *self, gboolean running, const gchar *text)
{
  gtk_widget_set_visible(GTK_WIDGET(self->signer_spinner), running);
  gtk_spinner_set_spinning(self->signer_spinner, running);
  gtk_label_set_text(self->signer_result, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(self->signer_result), text && *text);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.test-signer",
                                !running && gh_account_controller_get_state(self->config.accounts) ==
                                              GH_ACCOUNT_STATE_ACTIVE);
  announce(self, text);
}

/* One test run: its view and the cancellable it was started with. A later
 * run has a new cancellable, so a stale answer never ends it. */
typedef struct {
  GhOnboardingView *view; /* a reference */
  GCancellable *cancellable;
} SignerTest;

static void
signer_test_free(SignerTest *test)
{
  g_object_unref(test->view);
  g_object_unref(test->cancellable);
  g_free(test);
}

static gboolean
signer_test_is_current(SignerTest *test)
{
  return test->cancellable == test->view->signer_test_cancellable &&
         !g_cancellable_is_cancelled(test->cancellable);
}

/* Ends the run with text, or with the error's explanation. */
static void
signer_test_end(SignerTest *test, const GError *error, const gchar *text)
{
  GhOnboardingView *self = test->view;
  if (signer_test_is_current(test)) {
    g_clear_object(&self->signer_test_cancellable);
    g_autofree gchar *message = error ? signer_error_text(error) : NULL;
    signer_test_show(self, FALSE, message ? message : text);
  }
  signer_test_free(test);
}

static void
on_test_decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  SignerTest *test = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *plaintext = gh_account_controller_nip44_finish(result, &error);
  (void)source;
  if (plaintext && !g_str_equal(plaintext, SIGNER_TEST_TEXT))
    g_set_error_literal(&error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT, "mismatch");
  signer_test_end(test, error,
                  _("It works: Nostr Signer signed, locked and unlocked a test message."));
}

static void
on_test_encrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  SignerTest *test = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *ciphertext = gh_account_controller_nip44_finish(result, &error);
  (void)source;
  if (!ciphertext || !signer_test_is_current(test)) {
    signer_test_end(test, error, NULL);
    return;
  }
  GhOnboardingView *self = test->view;
  gh_account_controller_nip44_decrypt_with_cancellable_async(self->config.accounts, ciphertext,
    self->signer_test_pubkey, test->cancellable, on_test_decrypted, test);
}

static void
on_test_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  SignerTest *test = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  (void)source;
  if (!signed_json || !signer_test_is_current(test)) {
    signer_test_end(test, error, NULL);
    return;
  }
  GhOnboardingView *self = test->view;
  gh_account_controller_nip44_encrypt_with_cancellable_async(self->config.accounts,
    SIGNER_TEST_TEXT, self->signer_test_pubkey, test->cancellable, on_test_encrypted, test);
}

static gchar *
signer_test_event(const gchar *pubkey_hex)
{
  NostrEvent *event = nostr_event_new();
  if (!event)
    return NULL;
  nostr_event_set_kind(event, SIGNER_TEST_KIND);
  nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC);
  nostr_event_set_content(event, SIGNER_TEST_TEXT);
  nostr_event_set_pubkey(event, pubkey_hex);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *json = raw ? g_strdup(raw) : NULL;
  free(raw);
  return json;
}

static void
signer_test_cancel(GhOnboardingView *self)
{
  if (!self->signer_test_cancellable)
    return;
  g_cancellable_cancel(self->signer_test_cancellable);
  g_clear_object(&self->signer_test_cancellable);
  if (self->signer_spinner)
    signer_test_show(self, FALSE, NULL);
}

static void
test_signer_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(widget);
  (void)name;
  (void)parameter;
  g_free(self->signer_test_pubkey);
  self->signer_test_pubkey =
    gh_identity_pubkey_hex(gh_account_controller_get_active_npub(self->config.accounts));
  g_autofree gchar *json = self->signer_test_pubkey ? signer_test_event(self->signer_test_pubkey)
                                                    : NULL;
  if (!json || self->signer_test_cancellable)
    return;
  self->signer_test_cancellable = g_cancellable_new();
  SignerTest *test = g_new0(SignerTest, 1);
  test->view = g_object_ref(self);
  test->cancellable = g_object_ref(self->signer_test_cancellable);
  signer_test_show(self, TRUE, _("Waiting for you to approve in Nostr Signer…"));
  gh_account_controller_sign_with_cancellable_async(self->config.accounts, json,
    test->cancellable, on_test_signed, test);
}

/* ---- inbox: choosing message relays ------------------------------------ */

static const gchar *
reviewed_text(GhInboxPrivateReads reads)
{
  switch (reads) {
  case GH_INBOX_PRIVATE_READS_YES:
    return _("Asked for sign-in before handing out messages when it was reviewed.");
  case GH_INBOX_PRIVATE_READS_NO:
    return _("Hands out sealed messages without sign-in.");
  case GH_INBOX_PRIVATE_READS_UNKNOWN:
  default:
    return _("Not known yet whether it asks for sign-in.");
  }
}

static const gchar *
probe_text(GhInboxProbeResult result)
{
  switch (result) {
  case GH_INBOX_PROBE_PRIVATE:
    return _("Checked just now: asks for sign-in before handing out your sealed messages.");
  case GH_INBOX_PROBE_OPEN:
    return _("Checked just now: hands out sealed messages without sign-in. They stay "
             "unreadable, but others can see how many you get.");
  case GH_INBOX_PROBE_REFUSED:
    return _("Checked just now: it wouldn't answer the privacy check.");
  case GH_INBOX_PROBE_UNREACHABLE:
    return _("Couldn't check it: the relay didn't answer.");
  case GH_INBOX_PROBE_PENDING:
  default:
    return _("Checking…");
  }
}

static GhOnboardingItem *
find_item(GListStore *store, const gchar *key, guint *position)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhOnboardingItem) item = g_list_model_get_item(G_LIST_MODEL(store), i);
    if (g_str_equal(item->key, key)) {
      if (position)
        *position = i;
      return item; /* borrowed: the store keeps it */
    }
  }
  return NULL;
}

/* The relay's own line, and what a check found (NULL: nothing checked). */
static void
relay_describe(GhOnboardingView *self, GhOnboardingItem *item, const gchar *checked)
{
  Origin origin = GPOINTER_TO_UINT(g_hash_table_lookup(self->origins, item->key));
  const GhInboxSuggestion *suggestion = g_hash_table_lookup(self->reviewed, item->key);
  const gchar *what = origin == ORIGIN_CURRENT ? _("In your current settings.")
                    : origin == ORIGIN_CUSTOM  ? _("Added by you.")
                    : suggestion               ? suggestion->description
                                               : "";
  const gchar *reads = checked ? checked
                     : suggestion ? reviewed_text(suggestion->private_reads)
                                  : reviewed_text(GH_INBOX_PRIVATE_READS_UNKNOWN);
  /* A suggestion's title is its name; its address is always shown too. */
  g_autofree gchar *subtitle =
    origin == ORIGIN_SUGGESTED ? g_strdup_printf("%s · %s\n%s", host_of(item->key), what, reads)
                               : g_strdup_printf("%s\n%s", what, reads);
  item_set_subtitle(item, subtitle);
}

static GStrv
chosen_relays(GhOnboardingView *self)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhOnboardingItem) item = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
    if (item->checked)
      g_strv_builder_add(builder, item->key);
  }
  return g_strv_builder_end(builder);
}

static void
update_inbox_actions(GhOnboardingView *self)
{
  g_auto(GStrv) chosen = chosen_relays(self);
  guint n = g_strv_length(chosen);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.inbox-continue",
                                n > 0 && n <= GH_INBOX_SETUP_MAX_INBOX_RELAYS);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.check-relays",
                                n > 0 && n <= GH_INBOX_SETUP_MAX_INBOX_RELAYS && !self->probe);
}

static GhOnboardingItem *
add_relay(GhOnboardingView *self, const gchar *url, const gchar *title, Origin origin,
          gboolean checked)
{
  GhOnboardingItem *item = find_item(self->relays, url, NULL);
  if (item) {
    if (checked)
      item_set_checked(item, TRUE);
    return item;
  }
  g_autoptr(GhOnboardingItem) created = item_new(url, title ? title : host_of(url), TRUE);
  g_hash_table_insert(self->origins, g_strdup(url), GUINT_TO_POINTER(origin));
  item_set_checked(created, checked);
  relay_describe(self, created, NULL);
  g_list_store_append(self->relays, created);
  return created;
}

/* The discovered 10050 list, if the account has one (and discovery ran). */
static const gchar *const *
current_inbox(GhOnboardingView *self)
{
  GhAccountRelays *relays = self->config.account_relays;
  GhAccountController *accounts = self->config.accounts;
  if (!relays || gh_account_controller_get_state(accounts) != GH_ACCOUNT_STATE_ACTIVE ||
      gh_account_relays_get_generation(relays) != gh_account_controller_get_generation(accounts))
    return NULL;
  return gh_account_relays_get_inbox_relays(relays);
}

static void
update_current_inbox(GhOnboardingView *self)
{
  const gchar *const *current = current_inbox(self);
  gboolean any = FALSE;
  for (guint i = 0; current && current[i]; i++) {
    g_autofree gchar *url = gh_inbox_setup_normalize_url(current[i], NULL);
    if (!url)
      continue;
    any = TRUE;
    if (!find_item(self->relays, url, NULL))
      add_relay(self, url, NULL, ORIGIN_CURRENT, TRUE);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->current_note), any);
  gtk_widget_set_visible(self->keep_button, any);
  update_inbox_actions(self);
}

static void
prepare_inbox(GhOnboardingView *self)
{
  if (g_list_model_get_n_items(G_LIST_MODEL(self->relays)) == 0 && self->suggestions)
    for (guint i = 0; i < self->suggestions->len; i++) {
      const GhInboxSuggestion *suggestion = g_ptr_array_index(self->suggestions, i);
      /* D4: suggestions start unticked; the user chooses. */
      add_relay(self, suggestion->url, suggestion->name, ORIGIN_SUGGESTED, FALSE);
    }
  update_current_inbox(self);
  gtk_editable_set_text(GTK_EDITABLE(self->custom_entry), "");
  gtk_widget_set_visible(GTK_WIDGET(self->custom_error), FALSE);
}

static void
on_relay_activate(GhOnboardingView *self, GtkListBoxRow *row)
{
  g_autoptr(GhOnboardingItem) item = g_list_model_get_item(G_LIST_MODEL(self->relays),
                                                           gtk_list_box_row_get_index(row));
  if (!item)
    return;
  item_set_checked(item, !item->checked);
  update_inbox_actions(self);
}

static void
show_custom_error(GhOnboardingView *self, const gchar *message)
{
  gtk_label_set_text(self->custom_error, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->custom_error), message != NULL);
  if (message) {
    gtk_widget_add_css_class(GTK_WIDGET(self->custom_entry), "error");
    announce(self, message);
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->custom_entry), "error");
  }
}

static void
on_custom_apply(GhOnboardingView *self)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *url =
    gh_inbox_setup_normalize_url(gtk_editable_get_text(GTK_EDITABLE(self->custom_entry)), &error);
  if (!url) {
    show_custom_error(self, error->message);
    return;
  }
  show_custom_error(self, NULL);
  add_relay(self, url, NULL, ORIGIN_CUSTOM, TRUE);
  gtk_editable_set_text(GTK_EDITABLE(self->custom_entry), "");
  update_inbox_actions(self);
}

static void
on_custom_changed(GhOnboardingView *self)
{
  if (gtk_widget_get_visible(GTK_WIDGET(self->custom_error)))
    show_custom_error(self, NULL);
}

static void
on_check_result(GhInboxProbe *probe, const gchar *url, GhInboxProbeResult result,
                const gchar *detail, gpointer data)
{
  GhOnboardingView *self = data;
  (void)detail;
  if (probe != self->probe)
    return;
  GhOnboardingItem *item = find_item(self->relays, url, NULL);
  if (item)
    relay_describe(self, item, probe_text(result));
  if (gh_inbox_probe_is_complete(probe)) {
    g_clear_pointer(&self->probe, gh_inbox_probe_unref);
    update_inbox_actions(self);
    announce(self, _("The privacy check is done."));
  }
}

/* "Check Privacy": the user's explicit go-ahead to contact the ticked
 * relays, unauthenticated, for the check only. */
static void
check_relays_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(widget);
  (void)name;
  (void)parameter;
  g_auto(GStrv) chosen = chosen_relays(self);
  GhAccountController *accounts = self->config.accounts;
  if (!chosen[0] || self->probe ||
      gh_account_controller_get_state(accounts) != GH_ACCOUNT_STATE_ACTIVE)
    return;
  self->probe = gh_inbox_probe_new(gh_account_controller_get_generation(accounts),
                                   self->config.probe_transport,
                                   self->config.probe_auth_transport,
                                   self->config.probe_transport_data, on_check_result, self);
  for (guint i = 0; chosen[i]; i++) {
    GhOnboardingItem *item = find_item(self->relays, chosen[i], NULL);
    if (item)
      relay_describe(self, item, probe_text(GH_INBOX_PROBE_PENDING));
    gh_inbox_probe_add_url(self->probe, chosen[i], NULL);
  }
  update_inbox_actions(self);
  g_autoptr(GError) error = NULL;
  if (!gh_inbox_probe_start(self->probe, &error)) {
    g_clear_pointer(&self->probe, gh_inbox_probe_unref);
    update_inbox_actions(self);
    toast(self, error->message);
  }
}

/* ---- confirm and publish ------------------------------------------------------ */

static const gchar *
role_text(GhInboxSetupRole roles)
{
  if (roles & GH_INBOX_SETUP_ROLE_INBOX)
    return _("message relay");
  if (roles & GH_INBOX_SETUP_ROLE_WRITE)
    return _("where you publish");
  return _("where you're looked up");
}

static gboolean
discovery_is_empty(GhOnboardingView *self)
{
  g_auto(GStrv) urls = g_settings_get_strv(self->config.settings, "discovery-relays");
  return urls[0] == NULL;
}

static void clear_later(GhOnboardingView *self);

static void
replace_setup(GhOnboardingView *self)
{
  clear_later(self);
  if (self->setup) {
    g_signal_handlers_disconnect_by_data(self->setup, self);
    g_object_run_dispose(G_OBJECT(self->setup));
    g_clear_object(&self->setup);
  }
  self->setup = gh_inbox_setup_new(&self->config);
}

/* Fills the confirm page from the plan; FALSE (with a toast) if the choice
 * cannot be published. */
static gboolean
prepare_confirm(GhOnboardingView *self)
{
  g_auto(GStrv) chosen = chosen_relays(self);
  gboolean ask_discovery = discovery_is_empty(self);
  gtk_widget_set_visible(self->discovery_group, ask_discovery);
  replace_setup(self);
  /* Encrypted groups find people through their kind-10002 relay list
   * (nostrc-0bdg): offered only when the account has none at all, or one
   * without a relay it publishes to (then extended, not replaced). */
  GhRelayListOffer offer = gh_inbox_setup_get_relay_list_offer(self->setup);
  gtk_widget_set_visible(self->relay_list_group, offer != GH_RELAY_LIST_OFFER_NONE);
  if (offer == GH_RELAY_LIST_OFFER_ADD_WRITE) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->relay_list_switch),
                                  _("Add These Relays to Your Relay List"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->relay_list_switch),
      _("Your relay list names no relay you publish to, so people can't find your "
        "encrypted-group keys. This adds these relays to it as relays you publish to and keeps "
        "everything else in it. Nostr Signer asks once more."));
  } else {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->relay_list_switch),
                                  _("Let People Invite You to Encrypted Groups"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->relay_list_switch),
      _("Also publishes a relay list naming these relays as where you publish, so people can "
        "find your encrypted-group keys there. Nostr Signer asks once more. Anyone can see this "
        "list. Groundhog first checks that none of these relays holds a relay list of yours, and "
        "never changes one you already have."));
  }
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) plan = gh_inbox_setup_plan(self->setup, (const gchar *const *)chosen,
    ask_discovery && adw_switch_row_get_active(self->discovery_switch), &error);
  if (!plan) {
    toast(self, error->message);
    return FALSE;
  }
  GString *targets = g_string_new(NULL);
  for (guint i = 0; i < plan->len; i++) {
    const GhInboxSetupRelay *relay = g_ptr_array_index(plan, i);
    g_string_append_printf(targets, "%s%s (%s)", i ? "\n" : "", host_of(relay->url),
                           role_text(relay->roles));
  }
  adw_action_row_set_subtitle(self->confirm_targets, targets->str);
  g_string_free(targets, TRUE);
  return TRUE;
}

static const gchar *
outcome_icon(GhRelayPublishOutcome outcome)
{
  switch (outcome) {
  case GH_RELAY_PUBLISH_ACCEPTED: return "emblem-ok-symbolic";
  case GH_RELAY_PUBLISH_PENDING: return "content-loading-symbolic";
  case GH_RELAY_PUBLISH_REJECTED:
  case GH_RELAY_PUBLISH_AUTH_REQUIRED:
  case GH_RELAY_PUBLISH_CONNECTION_FAILED:
  case GH_RELAY_PUBLISH_CANCELLED:
  default:
    return "dialog-warning-symbolic";
  }
}

/* The relay's own words, kept short; relay text is shown as plain text. */
static gchar *
relay_words(const gchar *message)
{
  if (!message || !*message)
    return NULL;
  if (g_utf8_strlen(message, -1) <= 120)
    return g_strdup(message);
  g_autofree gchar *head = g_utf8_substring(message, 0, 119);
  return g_strconcat(head, "…", NULL);
}

static gchar *
outcome_text(const GhInboxSetupRelay *relay)
{
  g_autofree gchar *words = relay_words(relay->message);
  switch (relay->outcome) {
  case GH_RELAY_PUBLISH_PENDING:
    return g_strdup(_("Sending your list…"));
  case GH_RELAY_PUBLISH_ACCEPTED:
    return g_strdup(_("Kept your list."));
  case GH_RELAY_PUBLISH_AUTH_REQUIRED:
    return g_strdup(_("Didn't keep your list: it asked you to sign in, and that didn't work."));
  case GH_RELAY_PUBLISH_CONNECTION_FAILED:
    return g_strdup(_("Couldn't connect, so your list may not be there."));
  case GH_RELAY_PUBLISH_CANCELLED:
    return g_strdup(_("Stopped before it answered."));
  case GH_RELAY_PUBLISH_REJECTED:
  default:
    if (relay->prefix == GH_RELAY_OK_PREFIX_RATE_LIMITED)
      return g_strdup(_("Too busy right now. Try again later."));
    return words ? g_strdup_printf(_("Didn't keep your list. It said: “%s”"), words)
                 : g_strdup(_("Didn't keep your list."));
  }
}

static void
update_results(GhOnboardingView *self)
{
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
  guint n = self->setup ? gh_inbox_setup_get_n_relays(self->setup) : 0;
  for (guint i = 0; i < n; i++) {
    const GhInboxSetupRelay *relay = gh_inbox_setup_get_relay(self->setup, i);
    g_autofree gchar *outcome = outcome_text(relay);
    g_autofree gchar *subtitle = relay->probed
      ? g_strdup_printf("%s · %s\n%s", role_text(relay->roles), outcome, probe_text(relay->probe))
      : g_strdup_printf("%s · %s", role_text(relay->roles), outcome);
    GhOnboardingItem *item = item_new(relay->url, host_of(relay->url), FALSE);
    item_set_subtitle(item, subtitle);
    item_set_icon_name(item, outcome_icon(relay->outcome));
    g_ptr_array_add(items, item);
  }
  g_list_store_splice(self->results, 0, g_list_model_get_n_items(G_LIST_MODEL(self->results)),
                      items->pdata, items->len);
}

/* ---- the relay list on the result page (nostrc-0bdg R2) ---------------------- */

/* A first run has no discovery relay (PD-13): only once Publish adopted the
 * message relays as discovery relays and Groundhog looked there does it
 * know whether the account has a relay list. Then the result page offers
 * it, with the same consent and the same check of every relay. */
static void
update_later_offer(GhOnboardingView *self)
{
  if (!self->relay_list_later_group)
    return;
  gboolean show = FALSE, button = FALSE;
  const gchar *subtitle = NULL;
  g_autofree gchar *failure = NULL;
  GhInboxSetup *setup = self->setup;
  if (self->later) {
    show = TRUE;
    switch (gh_relay_list_setup_get_state(self->later)) {
    case GH_RELAY_LIST_SETUP_CHECKING:
      subtitle = _("Checking that these relays hold no relay list of yours…");
      break;
    case GH_RELAY_LIST_SETUP_SIGNING:
      subtitle = _("Approve the request in Nostr Signer to publish your relay list.");
      break;
    case GH_RELAY_LIST_SETUP_PUBLISHING:
      subtitle = _("Publishing your relay list…");
      break;
    case GH_RELAY_LIST_SETUP_DONE:
      subtitle = _("People can now invite you to encrypted groups.");
      break;
    case GH_RELAY_LIST_SETUP_SKIPPED:
      subtitle = _("You already have a relay list, so it was left as it is.");
      break;
    case GH_RELAY_LIST_SETUP_FAILED:
    case GH_RELAY_LIST_SETUP_IDLE:
    default: {
      const GError *error = gh_relay_list_setup_get_error(self->later);
      failure = g_strdup_printf(_("Your relay list wasn't published, so people can't invite you "
                                  "to encrypted groups yet. %s"),
                                error ? error->message : "");
      subtitle = failure;
      break;
    }
    }
  } else if (setup && self->config.offer_relay_list &&
             gh_inbox_setup_get_state(setup) == GH_INBOX_SETUP_DONE &&
             gh_inbox_setup_get_relay_list_state(setup) == GH_INBOX_SETUP_RELAY_LIST_NONE) {
    GhRelayListOffer offer = gh_inbox_setup_get_relay_list_offer(setup);
    GhAccountRelays *relays = self->config.account_relays;
    if (offer == GH_RELAY_LIST_OFFER_CREATE) {
      show = button = TRUE;
      subtitle = _("People can't invite you to encrypted groups yet: you have no relay list "
                   "saying where you publish. Publish one naming your message relays? Nostr "
                   "Signer asks once more. Anyone can see this list.");
      gtk_button_set_label(GTK_BUTTON(self->relay_list_later_button), _("_Publish Relay List"));
    } else if (offer == GH_RELAY_LIST_OFFER_ADD_WRITE) {
      show = button = TRUE;
      subtitle = _("Your relay list names no relay you publish to, so people can't invite you "
                   "to encrypted groups. Add your message relays to it as relays you publish "
                   "to? Everything else in it is kept. Nostr Signer asks once more.");
      gtk_button_set_label(GTK_BUTTON(self->relay_list_later_button), _("_Add My Relays"));
    } else if (relays &&
               gh_account_relays_get_state(relays) == GH_ACCOUNT_RELAYS_DISCOVERING) {
      show = TRUE;
      subtitle = _("Checking whether you already have a relay list…");
    }
  }
  gtk_widget_set_visible(self->relay_list_later_group, show);
  gtk_widget_set_visible(self->relay_list_later_button, button);
  if (subtitle)
    adw_action_row_set_subtitle(self->relay_list_later_row, subtitle);
}

static void
publish_relay_list_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(widget);
  (void)name;
  (void)parameter;
  GhInboxSetup *setup = self->setup;
  if (self->later || !setup)
    return;
  g_autoptr(GPtrArray) write = g_ptr_array_new();
  g_autoptr(GPtrArray) targets = g_ptr_array_new();
  for (guint i = 0; i < gh_inbox_setup_get_n_relays(setup); i++) {
    const GhInboxSetupRelay *relay = gh_inbox_setup_get_relay(setup, i);
    if (relay->roles & GH_INBOX_SETUP_ROLE_INBOX)
      g_ptr_array_add(write, (gpointer)relay->url);
    g_ptr_array_add(targets, (gpointer)relay->url);
  }
  g_auto(GStrv) discovery = g_settings_get_strv(self->config.settings, "discovery-relays");
  for (guint i = 0; discovery[i]; i++)
    g_ptr_array_add(targets, discovery[i]);
  g_ptr_array_add(write, NULL);
  g_ptr_array_add(targets, NULL);
  self->later = gh_relay_list_setup_new(&self->config);
  g_signal_connect_object(self->later, "changed", G_CALLBACK(update_later_offer), self,
                          G_CONNECT_SWAPPED);
  g_autoptr(GError) error = NULL;
  if (!gh_relay_list_setup_start(self->later, (const gchar *const *)write->pdata,
                                 (const gchar *const *)targets->pdata, &error)) {
    g_signal_handlers_disconnect_by_data(self->later, self);
    g_clear_object(&self->later);
    toast(self, error->message);
  }
  update_later_offer(self);
}

static void
clear_later(GhOnboardingView *self)
{
  if (!self->later)
    return;
  g_signal_handlers_disconnect_by_data(self->later, self);
  g_object_run_dispose(G_OBJECT(self->later));
  g_clear_object(&self->later);
}

/* What became of the encrypted-groups relay list, as a sentence ("" when it
 * was not asked for). Honest about a failure: nobody can invite
 * the account to encrypted groups then (nostrc-0bdg). */
static const gchar *
relay_list_text(GhInboxSetup *setup)
{
  switch (gh_inbox_setup_get_relay_list_state(setup)) {
  case GH_INBOX_SETUP_RELAY_LIST_DONE:
    return _("People can now invite you to encrypted groups.");
  case GH_INBOX_SETUP_RELAY_LIST_FAILED:
    return _("Your relay list wasn't published, so people can't invite you to encrypted "
             "groups yet. You can try again from Preferences.");
  case GH_INBOX_SETUP_RELAY_LIST_SKIPPED:
    return _("You already have a relay list, so it was left as it is.");
  case GH_INBOX_SETUP_RELAY_LIST_NONE:
  case GH_INBOX_SETUP_RELAY_LIST_WAITING:
  case GH_INBOX_SETUP_RELAY_LIST_CHECKING:
  case GH_INBOX_SETUP_RELAY_LIST_SIGNING:
  case GH_INBOX_SETUP_RELAY_LIST_PUBLISHING:
  default:
    return "";
  }
}

static void
on_setup_changed(GhOnboardingView *self)
{
  GhInboxSetup *setup = self->setup;
  GhInboxSetupState state = gh_inbox_setup_get_state(setup);
  const gchar *icon, *title;
  g_autofree gchar *description = NULL;
  switch (state) {
  case GH_INBOX_SETUP_SIGNING:
    icon = "dialog-password-symbolic";
    title = _("Waiting for Nostr Signer");
    description = g_strdup(_("Approve the request in Nostr Signer to publish your list. "
                             "Nothing has been sent yet."));
    break;
  case GH_INBOX_SETUP_DONE: {
    guint accepted = gh_inbox_setup_get_n_accepted(setup);
    guint total = gh_inbox_setup_get_n_relays(setup);
    icon = "emblem-ok-symbolic";
    title = _("Your Message Relays Are Set Up");
    g_autofree gchar *kept = g_strdup_printf(g_dngettext(NULL, "%u of %u relay kept your list.",
                                                         "%u of %u relays kept your list.",
                                                         total),
                                             accepted, total);
    const gchar *relay_list = relay_list_text(setup);
    description = *relay_list ? g_strjoin(" ", kept, relay_list, NULL) : g_strdup(kept);
    break;
  }
  case GH_INBOX_SETUP_FAILED: {
    const GError *error = gh_inbox_setup_get_error(setup);
    icon = "dialog-warning-symbolic";
    if (error && error->domain == GH_SIGNER_ERROR) {
      title = _("Nothing Was Published");
      description = signer_error_text(error);
    } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      title = _("Publishing Stopped");
      description = g_strdup(error->message);
    } else {
      title = _("Your List Wasn't Published");
      description = g_strdup(_("No relay kept your list. The details are below; you can try "
                               "again, choose other relays, or set this up later."));
    }
    break;
  }
  case GH_INBOX_SETUP_PUBLISHING:
  case GH_INBOX_SETUP_IDLE:
  default:
    icon = "emblem-synchronizing-symbolic";
    title = _("Publishing…");
    description = g_strdup(_("Sending your list and checking your message relays."));
    break;
  }
  gtk_image_set_from_icon_name(self->publish_icon, icon);
  gtk_label_set_text(self->publish_title, title);
  gtk_label_set_text(self->publish_description, description);
  update_results(self);
  update_later_offer(self);
  gboolean done = state == GH_INBOX_SETUP_DONE, failed = state == GH_INBOX_SETUP_FAILED;
  gtk_widget_set_visible(self->publish_continue, done);
  gtk_widget_set_visible(self->publish_retry, failed);
  gtk_widget_set_visible(self->publish_later, failed);
  if (done || failed) {
    announce(self, title);
    gtk_widget_grab_focus(done ? self->publish_continue : self->publish_retry);
  }
}

static void
publish_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(widget);
  (void)name;
  (void)parameter;
  /* The plan shown is the one published: rebuild it with the switch's
   * current value, then start. */
  if (!prepare_confirm(self))
    return;
  g_auto(GStrv) chosen = chosen_relays(self);
  gboolean adopt = gtk_widget_get_visible(self->discovery_group) &&
                   adw_switch_row_get_active(self->discovery_switch);
  g_signal_connect_object(self->setup, "changed", G_CALLBACK(on_setup_changed), self,
                          G_CONNECT_SWAPPED);
  gboolean relay_list = gtk_widget_get_visible(self->relay_list_group) &&
                        adw_switch_row_get_active(self->relay_list_switch);
  g_autoptr(GError) error = NULL;
  if (!gh_inbox_setup_start_full(self->setup, (const gchar *const *)chosen, adopt, relay_list,
                                 &error)) {
    toast(self, error->message);
    return;
  }
  self->kept_current = FALSE;
  on_setup_changed(self);
  push(self, "publish");
}

/* ---- navigation actions ----------------------------------------------------- */

/* [Start a Conversation] only while the window can open New Message
 * (win.new-message, charter G18): a read-only account has no way to send. */
static void
sync_start_conversation(GhOnboardingView *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  gboolean available = G_IS_ACTION_GROUP(root) &&
                       g_action_group_get_action_enabled(G_ACTION_GROUP(root), "new-message");
  gtk_widget_set_visible(self->start_conversation_button, available);
}

static void
show_done(GhOnboardingView *self)
{
  const gchar *description =
    self->kept_current ? _("Your message relays stay as they were. People can reach you privately.")
                       : _("People can now reach you privately.");
  adw_status_page_set_description(self->done_status, description);
  sync_start_conversation(self);
  push(self, "done");
}

static void
nav_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(widget);
  (void)parameter;
  if (g_str_equal(name, "onboarding.start")) {
    update_identities(self);
    push(self, "account");
  } else if (g_str_equal(name, "onboarding.refresh")) {
    gh_account_controller_refresh(self->config.accounts);
  } else if (g_str_equal(name, "onboarding.read-only")) {
    g_autoptr(GError) error = NULL;
    if (gh_account_controller_select(self->config.accounts, "", &error))
      finish(self);
    else
      toast(self, error->message);
  } else if (g_str_equal(name, "onboarding.account-continue")) {
    update_signer_status(self);
    push(self, "signer");
  } else if (g_str_equal(name, "onboarding.signer-continue")) {
    prepare_inbox(self);
    push(self, "inbox");
  } else if (g_str_equal(name, "onboarding.inbox-continue")) {
    if (prepare_confirm(self))
      push(self, "confirm");
  } else if (g_str_equal(name, "onboarding.keep-current")) {
    self->kept_current = TRUE;
    show_done(self);
  } else if (g_str_equal(name, "onboarding.retry")) {
    adw_navigation_view_pop_to_tag(self->navigation, "confirm");
    prepare_confirm(self);
  } else if (g_str_equal(name, "onboarding.publish-continue")) {
    show_done(self);
  } else if (g_str_equal(name, "onboarding.later") || g_str_equal(name, "onboarding.finish")) {
    finish(self);
  } else if (g_str_equal(name, "onboarding.start-conversation")) {
    /* Back to the conversations first, so New Message opens over them. */
    finish(self);
    gtk_widget_activate_action(GTK_WIDGET(self), "win.new-message", NULL);
  }
}


/* Keyboard and screen-reader focus lands on each page's one next step
 * (charter §7.14), never on whatever widget comes first. */
static void
on_page_shown(AdwNavigationPage *page, gpointer data)
{
  GhOnboardingView *self = data;
  GtkWidget *target = NULL;
  if (page == self->welcome_page)
    target = self->start_button;
  else if (page == self->account_page)
    target = GTK_WIDGET(self->identity_list);
  else if (page == self->signer_page)
    target = self->signer_test;
  else if (page == self->inbox_page)
    target = GTK_WIDGET(self->relay_list);
  else if (page == self->confirm_page)
    target = self->publish_button;
  else if (page == self->done_page)
    target = gtk_widget_get_visible(self->start_conversation_button)
               ? self->start_conversation_button : self->done_button;
  if (target && gtk_widget_get_sensitive(target))
    gtk_widget_grab_focus(target);
}

static void
on_accounts_changed(GhOnboardingView *self)
{
  if (!self->identity_list)
    return;
  /* A switch revokes the old generation: its test and check end. */
  if (self->signer_test_cancellable &&
      gh_account_controller_get_state(self->config.accounts) != GH_ACCOUNT_STATE_ACTIVE)
    signer_test_cancel(self);
  update_identities(self);
  update_signer_status(self);
}

static void
on_account_relays_changed(GhOnboardingView *self)
{
  if (self->relay_list)
    update_current_inbox(self);
  update_later_offer(self);
}

static void
account_refresh_activated(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  gh_account_controller_refresh(GH_ONBOARDING_VIEW(data)->config.accounts);
}

/* ---- public ----------------------------------------------------------------- */

void
gh_onboarding_view_show_welcome(GhOnboardingView *self)
{
  g_return_if_fail(GH_IS_ONBOARDING_VIEW(self));
  const gchar *tags[] = { "welcome" };
  adw_navigation_view_replace_with_tags(self->navigation, tags, G_N_ELEMENTS(tags));
  update_identities(self);
}

void
gh_onboarding_view_show_inbox(GhOnboardingView *self)
{
  g_return_if_fail(GH_IS_ONBOARDING_VIEW(self));
  if (gh_account_controller_get_state(self->config.accounts) != GH_ACCOUNT_STATE_ACTIVE) {
    gh_onboarding_view_show_welcome(self);
    return;
  }
  prepare_inbox(self);
  const gchar *tags[] = { "inbox" };
  adw_navigation_view_replace_with_tags(self->navigation, tags, G_N_ELEMENTS(tags));
}

const gchar *
gh_onboarding_view_get_page(GhOnboardingView *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_VIEW(self), NULL);
  AdwNavigationPage *page = adw_navigation_view_get_visible_page(self->navigation);
  return page ? adw_navigation_page_get_tag(page) : NULL;
}

GListModel *
gh_onboarding_view_get_identities(GhOnboardingView *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_VIEW(self), NULL);
  return G_LIST_MODEL(self->identities);
}

GListModel *
gh_onboarding_view_get_relays(GhOnboardingView *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_VIEW(self), NULL);
  return G_LIST_MODEL(self->relays);
}

GListModel *
gh_onboarding_view_get_results(GhOnboardingView *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_VIEW(self), NULL);
  return G_LIST_MODEL(self->results);
}

GhInboxSetup *
gh_onboarding_view_get_setup(GhOnboardingView *self)
{
  g_return_val_if_fail(GH_IS_ONBOARDING_VIEW(self), NULL);
  return self->setup;
}

/* What Check Privacy reveals in the current network mode (P9, and W16
 * review #5: in Tor mode the relays don't see the IP address either). */
static void
sync_check_note(GhOnboardingView *self)
{
  g_autofree gchar *mode = g_settings_get_string(self->config.settings, "network-mode");
  gtk_label_set_text(self->check_note,
                     g_str_equal(mode, "tor")
                       ? _("Checking connects to the ticked relays through Tor. They see neither "
                           "your IP address nor your account.")
                       : _("Checking connects to the ticked relays. They see your IP address, "
                           "not your account."));
}

GhOnboardingView *
gh_onboarding_view_new(const GhInboxSetupConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(!config->account_relays || GH_IS_ACCOUNT_RELAYS(config->account_relays),
                       NULL);
  GhOnboardingView *self = g_object_new(GH_TYPE_ONBOARDING_VIEW, NULL);
  self->config = *config;
  g_object_ref(self->config.accounts);
  g_object_ref(self->config.settings);
  if (self->config.account_relays)
    g_object_ref(self->config.account_relays);
  g_signal_connect_object(self->config.settings, "changed::network-mode",
                          G_CALLBACK(sync_check_note), self, G_CONNECT_SWAPPED);
  sync_check_note(self);

  g_autoptr(GError) error = NULL;
  self->suggestions = gh_inbox_setup_load_suggestions(&error);
  if (!self->suggestions)
    g_warning("Groundhog relay suggestions unavailable: %s", error->message);
  for (guint i = 0; self->suggestions && i < self->suggestions->len; i++) {
    GhInboxSuggestion *suggestion = g_ptr_array_index(self->suggestions, i);
    g_hash_table_insert(self->reviewed, suggestion->url, suggestion);
  }

  /* The empty, unavailable and discovering states are gh-account-ui.blp's
   * own pages; their buttons run this view's account.refresh. */
  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-account-ui.ui");
  for (guint i = 0; i < G_N_ELEMENTS(account_pages); i++)
    gtk_stack_add_named(self->account_stack,
                        GTK_WIDGET(gtk_builder_get_object(builder, account_pages[i].id)),
                        account_pages[i].name);
  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  g_autoptr(GSimpleAction) refresh = g_simple_action_new("refresh", NULL);
  g_signal_connect(refresh, "activate", G_CALLBACK(account_refresh_activated), self);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(refresh));
  gtk_widget_insert_action_group(GTK_WIDGET(self), "account", G_ACTION_GROUP(group));

  g_signal_connect_object(self->config.accounts, "changed", G_CALLBACK(on_accounts_changed), self,
                          G_CONNECT_SWAPPED);
  if (self->config.account_relays)
    g_signal_connect_object(self->config.account_relays, "changed",
                            G_CALLBACK(on_account_relays_changed), self, G_CONNECT_SWAPPED);
  update_identities(self);
  update_signer_status(self);
  update_inbox_actions(self);
  return self;
}

static void
gh_onboarding_view_dispose(GObject *object)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(object);
  signer_test_cancel(self);
  if (self->probe) {
    gh_inbox_probe_cancel(self->probe);
    g_clear_pointer(&self->probe, gh_inbox_probe_unref);
  }
  clear_later(self);
  if (self->setup) {
    g_signal_handlers_disconnect_by_data(self->setup, self);
    g_object_run_dispose(G_OBJECT(self->setup));
    g_clear_object(&self->setup);
  }
  if (self->identity_list)
    gtk_list_box_bind_model(self->identity_list, NULL, NULL, NULL, NULL);
  if (self->relay_list)
    gtk_list_box_bind_model(self->relay_list, NULL, NULL, NULL, NULL);
  if (self->result_list)
    gtk_list_box_bind_model(self->result_list, NULL, NULL, NULL, NULL);
  /* navigation is both the bin's child and a bound template child: detach
   * it here, or dispose_template would unparent it behind AdwBin's back. */
  adw_bin_set_child(ADW_BIN(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_ONBOARDING_VIEW);
  g_clear_object(&self->identities);
  g_clear_object(&self->relays);
  g_clear_object(&self->results);
  g_clear_object(&self->config.account_relays);
  g_clear_object(&self->config.settings);
  g_clear_object(&self->config.accounts);
  G_OBJECT_CLASS(gh_onboarding_view_parent_class)->dispose(object);
}

static void
gh_onboarding_view_finalize(GObject *object)
{
  GhOnboardingView *self = GH_ONBOARDING_VIEW(object);
  g_hash_table_unref(self->origins);
  g_hash_table_unref(self->reviewed);
  g_clear_pointer(&self->suggestions, g_ptr_array_unref);
  g_free(self->signer_test_pubkey);
  G_OBJECT_CLASS(gh_onboarding_view_parent_class)->finalize(object);
}

static void
gh_onboarding_view_class_init(GhOnboardingViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gh_onboarding_view_dispose;
  object_class->finalize = gh_onboarding_view_finalize;
  signals[SIGNAL_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(klass),
                                          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  g_type_ensure(GH_TYPE_ONBOARDING_ITEM);
  g_type_ensure(GH_TYPE_ONBOARDING_ROW);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-onboarding-view.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhOnboardingView, name)
  BIND(navigation);
  BIND(welcome_page);
  BIND(account_page);
  BIND(signer_page);
  BIND(inbox_page);
  BIND(confirm_page);
  BIND(publish_page);
  BIND(done_page);
  BIND(start_button);
  BIND(account_stack);
  BIND(identity_list);
  BIND(signer_status);
  BIND(signer_icon);
  BIND(signer_test);
  BIND(signer_spinner);
  BIND(signer_result);
  BIND(current_note);
  BIND(relay_list);
  BIND(custom_entry);
  BIND(custom_error);
  BIND(keep_button);
  BIND(confirm_targets);
  BIND(discovery_group);
  BIND(discovery_switch);
  BIND(relay_list_group);
  BIND(relay_list_switch);
  BIND(relay_list_later_group);
  BIND(relay_list_later_row);
  BIND(relay_list_later_button);
  BIND(publish_button);
  BIND(publish_icon);
  BIND(publish_title);
  BIND(publish_description);
  BIND(result_list);
  BIND(publish_continue);
  BIND(publish_retry);
  BIND(publish_later);
  BIND(done_status);
  BIND(start_conversation_button);
  BIND(done_button);
  BIND(check_note);
#undef BIND
  /* Reachable by name (gtk_widget_get_template_child) for tests. */
  gtk_widget_class_bind_template_child_full(widget_class, "account_continue", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "inbox_continue", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "check_button", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "inbox_footer", FALSE, 0);

  static const gchar *const navigation_actions[] = {
    "onboarding.start", "onboarding.refresh", "onboarding.read-only",
    "onboarding.account-continue", "onboarding.signer-continue", "onboarding.inbox-continue",
    "onboarding.keep-current", "onboarding.retry", "onboarding.publish-continue",
    "onboarding.later", "onboarding.finish", "onboarding.start-conversation",
  };
  for (guint i = 0; i < G_N_ELEMENTS(navigation_actions); i++)
    gtk_widget_class_install_action(widget_class, navigation_actions[i], NULL, nav_action);
  gtk_widget_class_install_action(widget_class, "onboarding.test-signer", NULL,
                                  test_signer_action);
  gtk_widget_class_install_action(widget_class, "onboarding.check-relays", NULL,
                                  check_relays_action);
  gtk_widget_class_install_action(widget_class, "onboarding.publish", NULL, publish_action);
  gtk_widget_class_install_action(widget_class, "onboarding.publish-relay-list", NULL,
                                  publish_relay_list_action);
}

static void
gh_onboarding_view_init(GhOnboardingView *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->identities = g_list_store_new(GH_TYPE_ONBOARDING_ITEM);
  self->relays = g_list_store_new(GH_TYPE_ONBOARDING_ITEM);
  self->results = g_list_store_new(GH_TYPE_ONBOARDING_ITEM);
  self->origins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->reviewed = g_hash_table_new(g_str_hash, g_str_equal);
  gtk_list_box_bind_model(self->identity_list, G_LIST_MODEL(self->identities), create_choice_row,
                          NULL, NULL);
  gtk_list_box_bind_model(self->relay_list, G_LIST_MODEL(self->relays), create_choice_row, NULL,
                          NULL);
  gtk_list_box_bind_model(self->result_list, G_LIST_MODEL(self->results), create_result_row,
                          NULL, NULL);
  g_signal_connect_swapped(self->identity_list, "row-activated", G_CALLBACK(on_identity_activate),
                           self);
  g_signal_connect_swapped(self->relay_list, "row-activated", G_CALLBACK(on_relay_activate),
                           self);
  g_signal_connect_swapped(self->custom_entry, "apply", G_CALLBACK(on_custom_apply), self);
  g_signal_connect_swapped(self->custom_entry, "changed", G_CALLBACK(on_custom_changed), self);
  AdwNavigationPage *pages[] = { self->welcome_page, self->account_page, self->signer_page,
                                 self->inbox_page, self->confirm_page, self->done_page };
  for (guint i = 0; i < G_N_ELEMENTS(pages); i++)
    g_signal_connect(pages[i], "shown", G_CALLBACK(on_page_shown), self);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.account-continue", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "onboarding.test-signer", FALSE);
}

/* ---- window --------------------------------------------------------------------- */

static void
show_onboarding(GhOnboardingView *view)
{
  GtkWidget *stack = gtk_widget_get_parent(GTK_WIDGET(view));
  if (GTK_IS_STACK(stack))
    gtk_stack_set_visible_child(GTK_STACK(stack), GTK_WIDGET(view));
}

static void
on_finished(GhOnboardingView *view)
{
  GtkWidget *stack = gtk_widget_get_parent(GTK_WIDGET(view));
  if (GTK_IS_STACK(stack))
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "main");
}

static void
on_setup_inbox(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GhOnboardingView *view = data;
  (void)action;
  (void)parameter;
  gh_onboarding_view_show_inbox(view);
  show_onboarding(view);
}

GhOnboardingView *
gh_onboarding_attach(GhWindow *window, const GhInboxSetupConfig *config)
{
  g_return_val_if_fail(GH_IS_WINDOW(window), NULL);
  GhOnboardingView *view = gh_onboarding_view_new(config);
  gtk_stack_add_named(gh_window_get_root_stack(window), GTK_WIDGET(view), "onboarding");
  g_signal_connect(view, "finished", G_CALLBACK(on_finished), NULL);
  g_signal_connect_object(window, "action-enabled-changed::new-message",
                          G_CALLBACK(sync_start_conversation), view, G_CONNECT_SWAPPED);
  g_signal_connect_object(window, "action-added::new-message",
                          G_CALLBACK(sync_start_conversation), view, G_CONNECT_SWAPPED);
  sync_start_conversation(view);
  /* The banners' [Set Up] (GH_STATUS_ACTION_SETUP_INBOX). */
  g_autoptr(GSimpleAction) setup = g_simple_action_new("setup-inbox", NULL);
  g_signal_connect_object(setup, "activate", G_CALLBACK(on_setup_inbox), view, 0);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(setup));
  /* First run: no Groundhog account has been chosen yet. */
  g_autofree gchar *current = g_settings_get_string(config->settings, "current-npub");
  if (!*current) {
    gh_onboarding_view_show_welcome(view);
    show_onboarding(view);
  }
  return view;
}
