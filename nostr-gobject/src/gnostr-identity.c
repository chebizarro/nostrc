/**
 * GNostr Identity Management Implementation
 *
 * GSettings schema ID is injected via gnostr_identity_init().
 * The library has no opinion about schema names.
 *
 * nostrc-e5nz: identity metadata only. There is deliberately no key
 * import/export/delete here: private keys live in the signer.
 */

#include "gnostr-identity.h"
#include <nostr-gobject-1.0/gnostr-app-bridge.h>

/* Constructor-injected GSettings schema ID and key */
static char *s_identity_schema_id = NULL;
#define SETTINGS_KEY_CURRENT_NPUB "current-npub"

/* Helper: create GSettings for the injected schema, or NULL if unavailable.
 * Mirrors the safe pattern from gnostr-relays.c to avoid g_settings_new()
 * aborting when the schema is not installed. */
static GSettings *identity_settings_new(void) {
  if (!s_identity_schema_id) return NULL;
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup(src, s_identity_schema_id, TRUE);
  if (!schema) {
    g_warning("gnostr-identity: schema '%s' not found", s_identity_schema_id);
    return NULL;
  }
  GSettings *settings = g_settings_new(s_identity_schema_id);
  g_settings_schema_unref(schema);
  return settings;
}

void gnostr_identity_init(const char *schema_id) {
  g_free(s_identity_schema_id);
  s_identity_schema_id = g_strdup(schema_id);
}

void gnostr_identity_free(GNostrIdentity *identity) {
  if (!identity) return;
  g_free(identity->npub);
  g_free(identity->label);
  g_free(identity->signer_type);
  g_free(identity);
}

GNostrIdentity *gnostr_identity_copy(const GNostrIdentity *identity) {
  if (!identity) return NULL;
  GNostrIdentity *copy = g_new0(GNostrIdentity, 1);
  copy->npub = g_strdup(identity->npub);
  copy->label = g_strdup(identity->label);
  copy->signer_holds_key = identity->signer_holds_key;
  copy->signer_type = g_strdup(identity->signer_type);
  return copy;
}

GNostrIdentity *gnostr_identity_get_current(void) {
  if (!s_identity_schema_id) {
    g_warning("gnostr_identity_get_current: schema not set, call gnostr_identity_init() first");
    return NULL;
  }

  GSettings *settings = identity_settings_new();
  if (!settings) return NULL;

  char *npub = g_settings_get_string(settings, SETTINGS_KEY_CURRENT_NPUB);
  g_object_unref(settings);

  if (!npub || !*npub || !g_str_has_prefix(npub, "npub1")) {
    g_free(npub);
    return NULL;
  }

  GNostrIdentity *identity = g_new0(GNostrIdentity, 1);
  identity->npub = npub;
  identity->signer_holds_key = gnostr_app_bridge_keystore_has_key(npub);
  /* A hint, not the signing route: see GNostrIdentity. */
  identity->signer_type = g_strdup(identity->signer_holds_key ? "nip55l" : "external");

  return identity;
}

void gnostr_identity_set_current(const char *npub) {
  if (!s_identity_schema_id) {
    g_warning("gnostr_identity_set_current: schema not set, call gnostr_identity_init() first");
    return;
  }

  GSettings *settings = identity_settings_new();
  if (!settings) return;

  g_settings_set_string(settings, SETTINGS_KEY_CURRENT_NPUB, npub ? npub : "");
  g_object_unref(settings);
}

GList *gnostr_identity_list_stored(GError **error) {
  GList *keys = gnostr_app_bridge_keystore_list_keys(error);
  if (!keys) return NULL;

  GList *identities = NULL;
  for (GList *l = keys; l != NULL; l = l->next) {
    GnostrKeyInfo *key_info = (GnostrKeyInfo *)l->data;

    GNostrIdentity *identity = g_new0(GNostrIdentity, 1);
    identity->npub = g_strdup(key_info->npub);
    identity->label = g_strdup(key_info->label);
    identity->signer_holds_key = TRUE;
    identity->signer_type = g_strdup("nip55l");

    identities = g_list_prepend(identities, identity);
  }

  g_list_free_full(keys, (GDestroyNotify)gnostr_app_bridge_key_info_free);

  return g_list_reverse(identities);
}

gboolean gnostr_identity_signer_holds_key(const char *npub) {
  return gnostr_app_bridge_keystore_has_key(npub);
}

gboolean gnostr_identity_secure_storage_available(void) {
  return gnostr_app_bridge_keystore_available();
}
