#include "gh-relay-deny-policy.h"
#include "gh-relay-scope.h"
#include <string.h>

struct _GhRelayDenyPolicy {
  GObject parent_instance;
  GMutex lock;
  GHashTable *urls; /* replaced, never mutated after installation */
  guint64 generation;
  gboolean enforcing;
  gboolean active;
};
G_DEFINE_TYPE(GhRelayDenyPolicy, gh_relay_deny_policy, G_TYPE_OBJECT)
static guint changed_signal;

GQuark gh_relay_deny_error_quark(void) { return g_quark_from_static_string("gh-relay-deny-error"); }

gchar *
gh_relay_deny_policy_normalize(const gchar *url)
{
  if (!gh_relay_url_validate(url, NULL))
    return NULL;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
  if (!uri)
    return NULL;
  g_autofree gchar *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
  g_autofree gchar *host = g_ascii_strdown(g_uri_get_host(uri), -1);
  gsize len = strlen(host);
  if (len && host[len - 1] == '.') host[len - 1] = '\0';
  gint port = g_uri_get_port(uri);
  if ((port == 443 && !strcmp(scheme, "wss")) || (port == 80 && !strcmp(scheme, "ws"))) port = -1;
  const gchar *path = g_uri_get_path(uri);
  return g_uri_join(G_URI_FLAGS_ENCODED, scheme, NULL, host, port,
                    path && *path ? path : "/", g_uri_get_query(uri), NULL);
}

static void
replace(GhRelayDenyPolicy *self, guint64 generation, const gchar *const *urls, gboolean active)
{
  GHashTable *snapshot = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; urls && urls[i]; i++) {
    gchar *key = gh_relay_deny_policy_normalize(urls[i]);
    if (key) g_hash_table_add(snapshot, key);
  }
  g_mutex_lock(&self->lock);
  GHashTable *old = self->urls;
  self->urls = snapshot;
  self->generation = generation;
  self->enforcing = TRUE;
  self->active = active;
  g_mutex_unlock(&self->lock);
  if (old) g_hash_table_unref(old);
  g_signal_emit(self, changed_signal, 0);
}

void
gh_relay_deny_policy_bind(GhRelayDenyPolicy *self, guint64 generation, const gchar *const *urls)
{
  replace(self, generation, urls, TRUE);
}

void gh_relay_deny_policy_revoke(GhRelayDenyPolicy *self) { replace(self, 0, NULL, FALSE); }

gboolean
gh_relay_deny_policy_check(guint64 generation, const gchar *url, gboolean signer_control,
                            GError **error)
{
  if (signer_control) return TRUE; /* credential transport, not application traffic */
  GhRelayDenyPolicy *self = gh_relay_deny_policy_get_default();
  g_autofree gchar *key = gh_relay_deny_policy_normalize(url);
  g_mutex_lock(&self->lock);
  gboolean allowed = !self->enforcing || (self->active && self->generation == generation &&
    key && !g_hash_table_contains(self->urls, key));
  g_mutex_unlock(&self->lock);
  if (!allowed) g_set_error_literal(error, GH_RELAY_DENY_ERROR, 1, GH_RELAY_DENY_MESSAGE);
  return allowed;
}

static void
gh_relay_deny_policy_finalize(GObject *object)
{
  GhRelayDenyPolicy *self = GH_RELAY_DENY_POLICY(object);
  g_clear_pointer(&self->urls, g_hash_table_unref);
  g_mutex_clear(&self->lock);
  G_OBJECT_CLASS(gh_relay_deny_policy_parent_class)->finalize(object);
}
static void gh_relay_deny_policy_init(GhRelayDenyPolicy *self) { g_mutex_init(&self->lock); }
static void
gh_relay_deny_policy_class_init(GhRelayDenyPolicyClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_relay_deny_policy_finalize;
  changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
GhRelayDenyPolicy *
gh_relay_deny_policy_get_default(void)
{
  static gsize instance;
  if (g_once_init_enter(&instance)) {
    GhRelayDenyPolicy *policy = g_object_new(GH_TYPE_RELAY_DENY_POLICY, NULL);
    g_once_init_leave(&instance, (gsize)policy);
  }
  return (GhRelayDenyPolicy *)instance;
}
