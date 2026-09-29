#include "gh-status.h"

#include <glib/gi18n.h>

struct _GhStatus {
  GObject parent_instance;
  gboolean account_active;
  gboolean network_available;
  gboolean tor_unreachable;
  GhStatusSigner signer;
  GhStatusInbox inbox;
  gchar *inbox_error;
  GhStatusStore store;
  gchar *store_error;
  GhStatusBanner banner;
};

enum {
  PROP_0,
  PROP_ACCOUNT_ACTIVE,
  PROP_NETWORK_AVAILABLE,
  PROP_TOR_UNREACHABLE,
  PROP_SIGNER,
  PROP_INBOX,
  PROP_STORE,
  PROP_BANNER,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhStatus, gh_status, G_TYPE_OBJECT)

#define DEFINE_ENUM_TYPE(func, name, ...)                                         \
  GType func(void)                                                                \
  {                                                                               \
    static gsize type = 0;                                                        \
    if (g_once_init_enter(&type)) {                                               \
      static const GEnumValue values[] = { __VA_ARGS__, { 0, NULL, NULL } };      \
      g_once_init_leave(&type, g_enum_register_static(g_intern_static_string(name), \
                                                      values));                   \
    }                                                                             \
    return type;                                                                  \
  }

DEFINE_ENUM_TYPE(gh_status_signer_get_type, "GhStatusSigner",
  { GH_STATUS_SIGNER_UNKNOWN, "GH_STATUS_SIGNER_UNKNOWN", "unknown" },
  { GH_STATUS_SIGNER_AVAILABLE, "GH_STATUS_SIGNER_AVAILABLE", "available" },
  { GH_STATUS_SIGNER_UNAVAILABLE, "GH_STATUS_SIGNER_UNAVAILABLE", "unavailable" },
  { GH_STATUS_SIGNER_NO_BUS, "GH_STATUS_SIGNER_NO_BUS", "no-bus" })

DEFINE_ENUM_TYPE(gh_status_inbox_get_type, "GhStatusInbox",
  { GH_STATUS_INBOX_INACTIVE, "GH_STATUS_INBOX_INACTIVE", "inactive" },
  { GH_STATUS_INBOX_NO_SOURCES, "GH_STATUS_INBOX_NO_SOURCES", "no-sources" },
  { GH_STATUS_INBOX_LOOKING, "GH_STATUS_INBOX_LOOKING", "looking" },
  { GH_STATUS_INBOX_LOOKUP_FAILED, "GH_STATUS_INBOX_LOOKUP_FAILED", "lookup-failed" },
  { GH_STATUS_INBOX_MISSING, "GH_STATUS_INBOX_MISSING", "missing" },
  { GH_STATUS_INBOX_CONNECTING, "GH_STATUS_INBOX_CONNECTING", "connecting" },
  { GH_STATUS_INBOX_BACKFILLING, "GH_STATUS_INBOX_BACKFILLING", "backfilling" },
  { GH_STATUS_INBOX_LIVE, "GH_STATUS_INBOX_LIVE", "live" },
  { GH_STATUS_INBOX_UNREACHABLE, "GH_STATUS_INBOX_UNREACHABLE", "unreachable" },
  { GH_STATUS_INBOX_ERROR, "GH_STATUS_INBOX_ERROR", "error" })

DEFINE_ENUM_TYPE(gh_status_store_get_type, "GhStatusStore",
  { GH_STATUS_STORE_NONE, "GH_STATUS_STORE_NONE", "none" },
  { GH_STATUS_STORE_OPENING, "GH_STATUS_STORE_OPENING", "opening" },
  { GH_STATUS_STORE_OPEN, "GH_STATUS_STORE_OPEN", "open" },
  { GH_STATUS_STORE_EPHEMERAL, "GH_STATUS_STORE_EPHEMERAL", "ephemeral" },
  { GH_STATUS_STORE_LOCKED, "GH_STATUS_STORE_LOCKED", "locked" },
  { GH_STATUS_STORE_UNAVAILABLE, "GH_STATUS_STORE_UNAVAILABLE", "unavailable" },
  { GH_STATUS_STORE_KEY_MISSING, "GH_STATUS_STORE_KEY_MISSING", "key-missing" },
  { GH_STATUS_STORE_CORRUPT, "GH_STATUS_STORE_CORRUPT", "corrupt" },
  { GH_STATUS_STORE_ERROR, "GH_STATUS_STORE_ERROR", "error" })

DEFINE_ENUM_TYPE(gh_status_banner_get_type, "GhStatusBanner",
  { GH_STATUS_BANNER_NONE, "GH_STATUS_BANNER_NONE", "none" },
  { GH_STATUS_BANNER_STORE_LOCKED, "GH_STATUS_BANNER_STORE_LOCKED", "store-locked" },
  { GH_STATUS_BANNER_STORE_UNAVAILABLE, "GH_STATUS_BANNER_STORE_UNAVAILABLE",
    "store-unavailable" },
  { GH_STATUS_BANNER_STORE_KEY_MISSING, "GH_STATUS_BANNER_STORE_KEY_MISSING",
    "store-key-missing" },
  { GH_STATUS_BANNER_STORE_CORRUPT, "GH_STATUS_BANNER_STORE_CORRUPT", "store-corrupt" },
  { GH_STATUS_BANNER_STORE_ERROR, "GH_STATUS_BANNER_STORE_ERROR", "store-error" },
  { GH_STATUS_BANNER_OFFLINE, "GH_STATUS_BANNER_OFFLINE", "offline" },
  { GH_STATUS_BANNER_TOR_UNREACHABLE, "GH_STATUS_BANNER_TOR_UNREACHABLE", "tor-unreachable" },
  { GH_STATUS_BANNER_SIGNER_UNAVAILABLE, "GH_STATUS_BANNER_SIGNER_UNAVAILABLE",
    "signer-unavailable" },
  { GH_STATUS_BANNER_SIGNER_NO_BUS, "GH_STATUS_BANNER_SIGNER_NO_BUS", "signer-no-bus" },
  { GH_STATUS_BANNER_INBOX_ERROR, "GH_STATUS_BANNER_INBOX_ERROR", "inbox-error" },
  { GH_STATUS_BANNER_NO_RELAYS, "GH_STATUS_BANNER_NO_RELAYS", "no-relays" },
  { GH_STATUS_BANNER_LOOKUP_FAILED, "GH_STATUS_BANNER_LOOKUP_FAILED", "lookup-failed" },
  { GH_STATUS_BANNER_INBOX_MISSING, "GH_STATUS_BANNER_INBOX_MISSING", "inbox-missing" },
  { GH_STATUS_BANNER_INBOX_UNREACHABLE, "GH_STATUS_BANNER_INBOX_UNREACHABLE",
    "inbox-unreachable" },
  { GH_STATUS_BANNER_STORE_EPHEMERAL, "GH_STATUS_BANNER_STORE_EPHEMERAL", "store-ephemeral" },
  { GH_STATUS_BANNER_STORE_OPENING, "GH_STATUS_BANNER_STORE_OPENING", "store-opening" },
  { GH_STATUS_BANNER_LOOKING, "GH_STATUS_BANNER_LOOKING", "looking" },
  { GH_STATUS_BANNER_CONNECTING, "GH_STATUS_BANNER_CONNECTING", "connecting" },
  { GH_STATUS_BANNER_BACKFILLING, "GH_STATUS_BANNER_BACKFILLING", "backfilling" })

/* Copy for each banner (charter §7.15 where it names one). The signer copy
 * differs from the charter's "you can read, but not send": messages are
 * unlocked through the signer and not yet stored (G05), so without it
 * nothing new can be read either. */
static const struct {
  GhStatusBanner banner;
  const gchar *title;
  gboolean problem;
  const gchar *button; /* NULL: no button */
  const gchar *action;
} banner_copy[] = {
  { GH_STATUS_BANNER_NONE, "", FALSE, NULL, NULL },
  /* §7.15 #14-#16. Starting fresh or resetting damaged storage deletes
   * messages, so it needs a confirmation dialog (Preferences, G17), not a
   * one-click banner button. */
  { GH_STATUS_BANNER_STORE_LOCKED,
    N_("Message storage is locked — unlock your keyring to read and receive messages"), TRUE,
    N_("Unlock"), GH_STATUS_ACTION_STORE_UNLOCK },
  { GH_STATUS_BANNER_STORE_UNAVAILABLE,
    N_("Private storage unavailable — no keyring can protect messages on this device"), TRUE,
    N_("Continue Without Saving Messages"), GH_STATUS_ACTION_STORE_EPHEMERAL },
  { GH_STATUS_BANNER_STORE_KEY_MISSING,
    N_("The key to your saved messages is missing from the keyring"), TRUE,
    N_("Try Again"), GH_STATUS_ACTION_STORE_RETRY },
  { GH_STATUS_BANNER_STORE_CORRUPT,
    N_("Message storage is damaged — showing what could be read"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_STORE_ERROR,
    N_("Can't open message storage"), TRUE, N_("Try Again"), GH_STATUS_ACTION_STORE_RETRY },
  { GH_STATUS_BANNER_OFFLINE,
    N_("Offline — new messages will arrive when you're back online"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_TOR_UNREACHABLE,
    N_("Can't reach Tor — Groundhog won't connect without it"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_SIGNER_UNAVAILABLE,
    N_("Nostr Signer isn't running — messages can't be unlocked or sent"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_SIGNER_NO_BUS,
    N_("No session bus — Nostr Signer can't be reached to unlock messages"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_INBOX_ERROR,
    N_("Can't receive messages on this device"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_NO_RELAYS,
    N_("No relay is set up yet, so Groundhog can't receive messages"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_LOOKUP_FAILED,
    N_("Can't reach your relays to find where your messages arrive"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_INBOX_MISSING,
    N_("Set up private messaging so people can reach you"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_INBOX_UNREACHABLE,
    N_("Can't reach your message relays"), TRUE, NULL, NULL },
  { GH_STATUS_BANNER_STORE_EPHEMERAL,
    N_("Messages aren't saved on this device — they're gone when Groundhog closes"), FALSE,
    NULL, NULL },
  { GH_STATUS_BANNER_STORE_OPENING,
    N_("Opening your messages…"), FALSE, NULL, NULL },
  { GH_STATUS_BANNER_LOOKING,
    N_("Looking for your message relays…"), FALSE, NULL, NULL },
  { GH_STATUS_BANNER_CONNECTING,
    N_("Connecting to your message relays…"), FALSE, NULL, NULL },
  { GH_STATUS_BANNER_BACKFILLING,
    N_("Checking for new messages…"), FALSE, NULL, NULL },
};

G_STATIC_ASSERT(G_N_ELEMENTS(banner_copy) == GH_STATUS_BANNER_LAST + 1);

const gchar *
gh_status_banner_get_title(GhStatusBanner banner)
{
  g_return_val_if_fail(banner <= GH_STATUS_BANNER_LAST, "");
  g_assert(banner_copy[banner].banner == banner);
  return *banner_copy[banner].title ? _(banner_copy[banner].title) : "";
}

const gchar *
gh_status_banner_get_button_label(GhStatusBanner banner)
{
  g_return_val_if_fail(banner <= GH_STATUS_BANNER_LAST, NULL);
  return banner_copy[banner].button ? _(banner_copy[banner].button) : NULL;
}

const gchar *
gh_status_banner_get_action(GhStatusBanner banner)
{
  g_return_val_if_fail(banner <= GH_STATUS_BANNER_LAST, NULL);
  return banner_copy[banner].action;
}

gboolean
gh_status_banner_is_problem(GhStatusBanner banner)
{
  g_return_val_if_fail(banner <= GH_STATUS_BANNER_LAST, FALSE);
  return banner_copy[banner].problem;
}

static GhStatusBanner
inbox_banner(GhStatusInbox inbox)
{
  switch (inbox) {
  case GH_STATUS_INBOX_NO_SOURCES: return GH_STATUS_BANNER_NO_RELAYS;
  case GH_STATUS_INBOX_LOOKING: return GH_STATUS_BANNER_LOOKING;
  case GH_STATUS_INBOX_LOOKUP_FAILED: return GH_STATUS_BANNER_LOOKUP_FAILED;
  case GH_STATUS_INBOX_MISSING: return GH_STATUS_BANNER_INBOX_MISSING;
  case GH_STATUS_INBOX_CONNECTING: return GH_STATUS_BANNER_CONNECTING;
  case GH_STATUS_INBOX_BACKFILLING: return GH_STATUS_BANNER_BACKFILLING;
  case GH_STATUS_INBOX_UNREACHABLE: return GH_STATUS_BANNER_INBOX_UNREACHABLE;
  case GH_STATUS_INBOX_ERROR: return GH_STATUS_BANNER_INBOX_ERROR;
  case GH_STATUS_INBOX_INACTIVE:
  case GH_STATUS_INBOX_LIVE:
  default:
    return GH_STATUS_BANNER_NONE;
  }
}

static GhStatusBanner
store_problem_banner(GhStatusStore store)
{
  switch (store) {
  case GH_STATUS_STORE_LOCKED: return GH_STATUS_BANNER_STORE_LOCKED;
  case GH_STATUS_STORE_UNAVAILABLE: return GH_STATUS_BANNER_STORE_UNAVAILABLE;
  case GH_STATUS_STORE_KEY_MISSING: return GH_STATUS_BANNER_STORE_KEY_MISSING;
  case GH_STATUS_STORE_CORRUPT: return GH_STATUS_BANNER_STORE_CORRUPT;
  case GH_STATUS_STORE_ERROR: return GH_STATUS_BANNER_STORE_ERROR;
  case GH_STATUS_STORE_NONE:
  case GH_STATUS_STORE_OPENING:
  case GH_STATUS_STORE_OPEN:
  case GH_STATUS_STORE_EPHEMERAL:
  default:
    return GH_STATUS_BANNER_NONE;
  }
}

/* Highest priority first: a store that cannot open blocks reading and
 * receiving whatever the network does, and needs the user; no network
 * explains every relay failure below it; without the signer nothing can be
 * unlocked whatever the relays do; a local inbox failure outranks the relay
 * path; the in-memory choice stays visible over progress; progress comes
 * last. */
static GhStatusBanner
compute_banner(GhStatus *self)
{
  if (!self->account_active)
    return GH_STATUS_BANNER_NONE;
  GhStatusBanner store = store_problem_banner(self->store);
  if (store != GH_STATUS_BANNER_NONE)
    return store;
  if (!self->network_available)
    return GH_STATUS_BANNER_OFFLINE;
  if (self->tor_unreachable)
    return GH_STATUS_BANNER_TOR_UNREACHABLE;
  if (self->signer == GH_STATUS_SIGNER_UNAVAILABLE)
    return GH_STATUS_BANNER_SIGNER_UNAVAILABLE;
  if (self->signer == GH_STATUS_SIGNER_NO_BUS)
    return GH_STATUS_BANNER_SIGNER_NO_BUS;
  GhStatusBanner inbox = inbox_banner(self->inbox);
  if (gh_status_banner_is_problem(inbox))
    return inbox;
  if (self->store == GH_STATUS_STORE_EPHEMERAL)
    return GH_STATUS_BANNER_STORE_EPHEMERAL;
  if (self->store == GH_STATUS_STORE_OPENING)
    return GH_STATUS_BANNER_STORE_OPENING;
  return inbox;
}

static void
update(GhStatus *self, GParamSpec *changed)
{
  GhStatusBanner banner = compute_banner(self);
  g_object_freeze_notify(G_OBJECT(self));
  if (changed)
    g_object_notify_by_pspec(G_OBJECT(self), changed);
  if (banner != self->banner) {
    self->banner = banner;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_BANNER]);
  }
  g_object_thaw_notify(G_OBJECT(self));
}

GhStatus *
gh_status_new(void)
{
  return g_object_new(GH_TYPE_STATUS, NULL);
}

void
gh_status_set_account_active(GhStatus *self, gboolean active)
{
  g_return_if_fail(GH_IS_STATUS(self));
  if (self->account_active == !!active)
    return;
  self->account_active = !!active;
  update(self, props[PROP_ACCOUNT_ACTIVE]);
}

void
gh_status_set_network_available(GhStatus *self, gboolean available)
{
  g_return_if_fail(GH_IS_STATUS(self));
  if (self->network_available == !!available)
    return;
  self->network_available = !!available;
  update(self, props[PROP_NETWORK_AVAILABLE]);
}

void
gh_status_set_tor_unreachable(GhStatus *self, gboolean unreachable)
{
  g_return_if_fail(GH_IS_STATUS(self));
  if (self->tor_unreachable == !!unreachable)
    return;
  self->tor_unreachable = !!unreachable;
  update(self, props[PROP_TOR_UNREACHABLE]);
}

void
gh_status_set_signer(GhStatus *self, GhStatusSigner signer)
{
  g_return_if_fail(GH_IS_STATUS(self));
  g_return_if_fail(signer <= GH_STATUS_SIGNER_NO_BUS);
  if (self->signer == signer)
    return;
  self->signer = signer;
  update(self, props[PROP_SIGNER]);
}

void
gh_status_set_inbox(GhStatus *self, GhStatusInbox inbox, const gchar *error)
{
  g_return_if_fail(GH_IS_STATUS(self));
  g_return_if_fail(inbox <= GH_STATUS_INBOX_ERROR);
  if ((inbox != GH_STATUS_INBOX_ERROR && inbox != GH_STATUS_INBOX_UNREACHABLE) ||
      (error && !*error))
    error = NULL;
  if (self->inbox == inbox && g_strcmp0(self->inbox_error, error) == 0)
    return;
  self->inbox = inbox;
  g_free(self->inbox_error);
  self->inbox_error = g_strdup(error);
  /* A new explanation for the same state also notifies "inbox". */
  update(self, props[PROP_INBOX]);
}

void
gh_status_set_store(GhStatus *self, GhStatusStore store, const gchar *error)
{
  g_return_if_fail(GH_IS_STATUS(self));
  g_return_if_fail(store <= GH_STATUS_STORE_ERROR);
  if ((store != GH_STATUS_STORE_ERROR && store != GH_STATUS_STORE_KEY_MISSING &&
       store != GH_STATUS_STORE_CORRUPT) || (error && !*error))
    error = NULL;
  if (self->store == store && g_strcmp0(self->store_error, error) == 0)
    return;
  self->store = store;
  g_free(self->store_error);
  self->store_error = g_strdup(error);
  update(self, props[PROP_STORE]);
}

GhStatusStore
gh_status_get_store(GhStatus *self)
{
  g_return_val_if_fail(GH_IS_STATUS(self), GH_STATUS_STORE_NONE);
  return self->store;
}

const gchar *
gh_status_get_store_error(GhStatus *self)
{
  g_return_val_if_fail(GH_IS_STATUS(self), NULL);
  return self->store_error;
}

GhStatusInbox
gh_status_get_inbox(GhStatus *self)
{
  g_return_val_if_fail(GH_IS_STATUS(self), GH_STATUS_INBOX_INACTIVE);
  return self->inbox;
}

const gchar *
gh_status_get_inbox_error(GhStatus *self)
{
  g_return_val_if_fail(GH_IS_STATUS(self), NULL);
  return self->inbox_error;
}

GhStatusBanner
gh_status_get_banner(GhStatus *self)
{
  g_return_val_if_fail(GH_IS_STATUS(self), GH_STATUS_BANNER_NONE);
  return self->banner;
}

static void
gh_status_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhStatus *self = GH_STATUS(object);
  switch (id) {
  case PROP_ACCOUNT_ACTIVE:
    g_value_set_boolean(value, self->account_active);
    break;
  case PROP_NETWORK_AVAILABLE:
    g_value_set_boolean(value, self->network_available);
    break;
  case PROP_TOR_UNREACHABLE:
    g_value_set_boolean(value, self->tor_unreachable);
    break;
  case PROP_SIGNER:
    g_value_set_enum(value, self->signer);
    break;
  case PROP_INBOX:
    g_value_set_enum(value, self->inbox);
    break;
  case PROP_STORE:
    g_value_set_enum(value, self->store);
    break;
  case PROP_BANNER:
    g_value_set_enum(value, self->banner);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_status_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GhStatus *self = GH_STATUS(object);
  switch (id) {
  case PROP_ACCOUNT_ACTIVE:
    gh_status_set_account_active(self, g_value_get_boolean(value));
    break;
  case PROP_NETWORK_AVAILABLE:
    gh_status_set_network_available(self, g_value_get_boolean(value));
    break;
  case PROP_TOR_UNREACHABLE:
    gh_status_set_tor_unreachable(self, g_value_get_boolean(value));
    break;
  case PROP_SIGNER:
    gh_status_set_signer(self, g_value_get_enum(value));
    break;
  case PROP_INBOX:
    gh_status_set_inbox(self, g_value_get_enum(value), NULL);
    break;
  case PROP_STORE:
    gh_status_set_store(self, g_value_get_enum(value), NULL);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_status_finalize(GObject *object)
{
  g_free(GH_STATUS(object)->inbox_error);
  g_free(GH_STATUS(object)->store_error);
  G_OBJECT_CLASS(gh_status_parent_class)->finalize(object);
}

static void
gh_status_class_init(GhStatusClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  const GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  object_class->get_property = gh_status_get_property;
  object_class->set_property = gh_status_set_property;
  object_class->finalize = gh_status_finalize;
  props[PROP_ACCOUNT_ACTIVE] = g_param_spec_boolean("account-active", NULL, NULL, FALSE, rw);
  props[PROP_NETWORK_AVAILABLE] = g_param_spec_boolean("network-available", NULL, NULL,
                                                       TRUE, rw);
  props[PROP_TOR_UNREACHABLE] = g_param_spec_boolean("tor-unreachable", NULL, NULL, FALSE, rw);
  props[PROP_SIGNER] = g_param_spec_enum("signer", NULL, NULL, GH_TYPE_STATUS_SIGNER,
                                         GH_STATUS_SIGNER_UNKNOWN, rw);
  props[PROP_INBOX] = g_param_spec_enum("inbox", NULL, NULL, GH_TYPE_STATUS_INBOX,
                                        GH_STATUS_INBOX_INACTIVE, rw);
  props[PROP_STORE] = g_param_spec_enum("store", NULL, NULL, GH_TYPE_STATUS_STORE,
                                        GH_STATUS_STORE_NONE, rw);
  props[PROP_BANNER] = g_param_spec_enum("banner", NULL, NULL, GH_TYPE_STATUS_BANNER,
                                         GH_STATUS_BANNER_NONE,
                                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                                         G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gh_status_init(GhStatus *self)
{
  self->network_available = TRUE;
  self->signer = GH_STATUS_SIGNER_UNKNOWN;
  self->inbox = GH_STATUS_INBOX_INACTIVE;
  self->store = GH_STATUS_STORE_NONE;
  self->banner = GH_STATUS_BANNER_NONE;
}
