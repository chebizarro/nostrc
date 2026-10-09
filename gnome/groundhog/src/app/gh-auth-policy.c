#include "gh-auth-policy.h"
#include "gh-account-auth.h"

struct _GhAuthPolicy {
  GObject parent_instance;
  GhAccountController *accounts; /* weak: NULL once the controller is disposed */
  gulong accounts_handler;
  GhAccountAuth *auth;           /* the account signer of `generation`, made on first use */
  guint64 generation;            /* 0 without auth */
  gulong auth_handler;
};

enum { SIGNAL_ACCOUNT_STATE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAuthPolicy, gh_auth_policy, G_TYPE_OBJECT)

/* Charter §4.3, "AUTH identity" column; R1 lists the four ACCOUNT rows. */
static const struct {
  const gchar *name;
  GhRelayAuthMode mode;
} purposes[GH_AUTH_N_PURPOSES] = {
  [GH_AUTH_PURPOSE_OWN_INBOX_READ] = { "own-inbox-read", GH_RELAY_AUTH_ACCOUNT },
  [GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY] = { "own-list-discovery", GH_RELAY_AUTH_EPHEMERAL },
  [GH_AUTH_PURPOSE_OWN_LIST_PUBLISH] = { "own-list-publish", GH_RELAY_AUTH_ACCOUNT },
  [GH_AUTH_PURPOSE_CONTACT_DIRECTORY] = { "contact-directory", GH_RELAY_AUTH_EPHEMERAL },
  [GH_AUTH_PURPOSE_RECIPIENT_WRAP] = { "recipient-wrap", GH_RELAY_AUTH_EPHEMERAL },
  [GH_AUTH_PURPOSE_SELF_WRAP] = { "self-wrap", GH_RELAY_AUTH_ACCOUNT },
  [GH_AUTH_PURPOSE_GROUP] = { "group", GH_RELAY_AUTH_ACCOUNT },
    [GH_AUTH_PURPOSE_MLS_ROUTING] = { "mls-routing", GH_RELAY_AUTH_EPHEMERAL },
    [GH_AUTH_PURPOSE_PUBLIC_POST] = { "public-post", GH_RELAY_AUTH_EPHEMERAL },
};

GhRelayAuthMode
gh_auth_policy_decide(GhAuthPurpose purpose)
{
  if ((guint)purpose >= GH_AUTH_N_PURPOSES)
    return GH_RELAY_AUTH_EPHEMERAL; /* never the account for something unnamed */
  return purposes[purpose].mode;
}

const gchar *
gh_auth_purpose_to_string(GhAuthPurpose purpose)
{
  return (guint)purpose < GH_AUTH_N_PURPOSES ? purposes[purpose].name : NULL;
}

/* Revokes the held account signer: requests in flight are cancelled and
 * nothing more is signed with it. */
static void
drop_auth(GhAuthPolicy *self)
{
  if (!self->auth)
    return;
  GhAccountAuth *auth = g_steal_pointer(&self->auth);
  g_clear_signal_handler(&self->auth_handler, auth);
  self->generation = 0;
  gh_account_auth_revoke(auth);
  g_object_unref(auth);
}

static void
on_accounts_changed(GhAuthPolicy *self)
{
  if (self->auth && (!self->accounts ||
                     !gh_account_controller_is_current(self->accounts, self->generation)))
    drop_auth(self);
}

static void
on_auth_relay_changed(GhAuthPolicy *self, const gchar *url)
{
  g_signal_emit(self, signals[SIGNAL_ACCOUNT_STATE_CHANGED], 0, url);
}

/* The account signer of generation, which must be the active account's
 * current one; made on first use and shared by every caller (R6). */
static GhAccountAuth *
account_auth(GhAuthPolicy *self, guint64 generation, GError **error)
{
  if (!self->accounts ||
      gh_account_controller_get_state(self->accounts) != GH_ACCOUNT_STATE_ACTIVE ||
      !gh_account_controller_is_current(self->accounts, generation)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "Signing in as the account needs the current account session");
    return NULL;
  }
  if (self->auth && self->generation != generation)
    drop_auth(self);
  if (!self->auth) {
    GhAccountAuth *auth = gh_account_auth_new(self->accounts);
    if (!auth || gh_account_auth_get_generation(auth) != generation) {
      g_clear_object(&auth);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                          "The active account cannot sign in to relays");
      return NULL;
    }
    self->auth = auth;
    self->generation = generation;
    self->auth_handler = g_signal_connect_swapped(auth, "relay-changed",
                                                  G_CALLBACK(on_auth_relay_changed), self);
  }
  return self->auth;
}

gboolean
gh_auth_policy_apply_scope(GhAuthPolicy *self, GhRelayScope *scope, GhAuthPurpose purpose,
                           const gchar *url, GError **error)
{
  g_return_val_if_fail(GH_IS_AUTH_POLICY(self), FALSE);
  g_return_val_if_fail(scope != NULL && url != NULL, FALSE);
  GhRelayAuthMode mode = gh_auth_policy_decide(purpose);
  if (mode != GH_RELAY_AUTH_ACCOUNT)
    return gh_relay_scope_set_url_auth(scope, url, mode, error);
  GhAccountAuth *auth = account_auth(self, gh_relay_scope_get_generation(scope), error);
  return auth &&
         gh_relay_scope_set_account_signer(scope, gh_account_auth_get_signer(auth), error) &&
         gh_relay_scope_set_url_auth(scope, url, GH_RELAY_AUTH_ACCOUNT, error);
}

gboolean
gh_auth_policy_apply_publish(GhAuthPolicy *self, GhRelayPublish *publish, GhAuthPurpose purpose,
                             const gchar *url, GError **error)
{
  g_return_val_if_fail(GH_IS_AUTH_POLICY(self), FALSE);
  g_return_val_if_fail(publish != NULL && url != NULL, FALSE);
  GhRelayAuthMode mode = gh_auth_policy_decide(purpose);
  if (mode != GH_RELAY_AUTH_ACCOUNT)
    return gh_relay_publish_set_url_auth(publish, url, mode, error);
  GhAccountAuth *auth = account_auth(self, gh_relay_publish_get_generation(publish), error);
  return auth &&
         gh_relay_publish_set_account_signer(publish, gh_account_auth_get_signer(auth), error) &&
         gh_relay_publish_set_url_auth(publish, url, GH_RELAY_AUTH_ACCOUNT, error);
}

GhAuthAccountState
gh_auth_policy_get_account_state(GhAuthPolicy *self, const gchar *url)
{
  g_return_val_if_fail(GH_IS_AUTH_POLICY(self), GH_AUTH_ACCOUNT_STATE_NONE);
  if (!self->auth || !self->accounts ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return GH_AUTH_ACCOUNT_STATE_NONE;
  switch (gh_account_auth_get_relay_state(self->auth, url)) {
  case GH_ACCOUNT_AUTH_RELAY_WAITING:
    return GH_AUTH_ACCOUNT_STATE_WAITING;
  case GH_ACCOUNT_AUTH_RELAY_APPROVED:
    return GH_AUTH_ACCOUNT_STATE_APPROVED;
  case GH_ACCOUNT_AUTH_RELAY_DECLINED:
    return GH_AUTH_ACCOUNT_STATE_DECLINED;
  case GH_ACCOUNT_AUTH_RELAY_NONE:
  default:
    return GH_AUTH_ACCOUNT_STATE_NONE;
  }
}

guint64
gh_auth_policy_get_generation(GhAuthPolicy *self)
{
  g_return_val_if_fail(GH_IS_AUTH_POLICY(self), 0);
  return self->auth ? self->generation : 0;
}

static GQuark
policy_quark(void)
{
  return g_quark_from_static_string("gh-auth-policy");
}

GhAuthPolicy *
gh_auth_policy_get_for_accounts(GhAccountController *accounts)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  GhAuthPolicy *self = g_object_get_qdata(G_OBJECT(accounts), policy_quark());
  if (self)
    return self;
  self = g_object_new(GH_TYPE_AUTH_POLICY, NULL);
  /* Weak both ways round: the controller owns the policy, and the policy's
   * GhAccountAuth holds the controller weakly too, so nothing keeps the
   * controller alive. */
  self->accounts = accounts;
  g_object_add_weak_pointer(G_OBJECT(accounts), (gpointer *)&self->accounts);
  self->accounts_handler = g_signal_connect_swapped(accounts, "changed",
                                                    G_CALLBACK(on_accounts_changed), self);
  g_object_set_qdata_full(G_OBJECT(accounts), policy_quark(), self, g_object_unref);
  return self;
}

static void
gh_auth_policy_dispose(GObject *object)
{
  GhAuthPolicy *self = GH_AUTH_POLICY(object);
  drop_auth(self);
  if (self->accounts) {
    g_clear_signal_handler(&self->accounts_handler, self->accounts);
    g_object_remove_weak_pointer(G_OBJECT(self->accounts), (gpointer *)&self->accounts);
    self->accounts = NULL;
  }
  G_OBJECT_CLASS(gh_auth_policy_parent_class)->dispose(object);
}

static void
gh_auth_policy_class_init(GhAuthPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_auth_policy_dispose;
  signals[SIGNAL_ACCOUNT_STATE_CHANGED] =
    g_signal_new("account-state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                 NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_auth_policy_init(GhAuthPolicy *self)
{
  (void)self;
}
