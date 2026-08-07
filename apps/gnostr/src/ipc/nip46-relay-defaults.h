/*
 * NIP-46 Default Relay Configuration
 *
 * Provides a configurable list of fallback NIP-46 relays used when no
 * relay is specified in a bunker URI or stored in GSettings.
 *
 * Configuration priority:
 *   1. GNOSTR_NIP46_RELAY environment variable (single URL)
 *   2. GSettings org.gnostr.Client nip46-connect-relays (user-configurable)
 *   3. Built-in fallback list (multiple relays for redundancy)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NIP46_RELAY_DEFAULTS_H
#define NIP46_RELAY_DEFAULTS_H

#include <glib.h>
#include <gio/gio.h>
#include <stdlib.h>

/* Well-known NIP-46 relays. Multiple entries provide redundancy if one
 * goes offline. The first entry is preferred for nostrconnect:// URIs.
 * nostrc-qpow: entries must accept kind-24133 (ephemeral) events without
 * policy demands - nos.lol was dropped because it rejects them with
 * "pow: 28 bits needed", which is unmineable for interactive signing. */
static const char *const NIP46_FALLBACK_RELAYS[] = {
  "wss://relay.nsec.app",
  "wss://relay.primal.net",
  "wss://relay.damus.io",
  NULL  /* sentinel */
};

/* Derived from the array so the count can never drift out of sync with the
 * list (a hand-edited mismatch previously copied the NULL sentinel as a
 * relay entry). Excludes the sentinel. */
#define NIP46_FALLBACK_RELAY_COUNT (G_N_ELEMENTS(NIP46_FALLBACK_RELAYS) - 1)

/**
 * nip46_get_default_relay:
 *
 * Returns the preferred NIP-46 relay URL. Checks the GNOSTR_NIP46_RELAY
 * environment variable first, then falls back to the first entry in the
 * built-in relay list.
 *
 * The returned string must not be freed.
 *
 * Returns: (transfer none): A relay URL string
 */
static inline const char *
nip46_get_default_relay(void)
{
  const char *env = g_getenv("GNOSTR_NIP46_RELAY");
  if (env && *env)
    return env;
  return NIP46_FALLBACK_RELAYS[0];
}

/**
 * nip46_get_fallback_relays:
 * @out_count: (out): Number of relay URLs returned
 *
 * Returns the full list of fallback NIP-46 relay URLs. If the
 * GNOSTR_NIP46_RELAY environment variable is set, returns only that
 * single relay; otherwise returns the built-in list.
 *
 * The returned array and strings must not be freed.
 *
 * Returns: (transfer none): A NULL-terminated array of relay URL strings
 */
static inline const char *const *
nip46_get_fallback_relays(gsize *out_count)
{
  const char *env = g_getenv("GNOSTR_NIP46_RELAY");
  if (env && *env) {
    /* Return a static single-element array for the env override */
    static const char *env_relay[2] = { NULL, NULL };
    env_relay[0] = env;
    if (out_count) *out_count = 1;
    return env_relay;
  }
  if (out_count) *out_count = NIP46_FALLBACK_RELAY_COUNT;
  return NIP46_FALLBACK_RELAYS;
}

/**
 * nip46_get_connect_relays:
 * @out_count: (out) (nullable): Number of relay URLs returned
 *
 * nostrc-koso: Returns the relay list to use for NIP-46 pairing
 * (nostrconnect:// URI + response listener). Priority:
 *   1. GNOSTR_NIP46_RELAY environment variable (single relay)
 *   2. GSettings org.gnostr.Client "nip46-connect-relays" (if non-empty)
 *   3. Built-in fallback list
 *
 * Returns: (transfer full): A NULL-terminated strv; free with g_strfreev()
 */
static inline gchar **
nip46_get_connect_relays(gsize *out_count)
{
  const char *env = g_getenv("GNOSTR_NIP46_RELAY");
  if (env && *env) {
    gchar **v = g_new0(gchar *, 2);
    v[0] = g_strdup(env);
    if (out_count) *out_count = 1;
    return v;
  }

  /* User-configured relays. Guard with a schema lookup so processes without
   * the compiled schema (or with an older schema missing the key) fall back
   * gracefully instead of aborting in g_settings_new(). */
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  GSettingsSchema *schema = src
      ? g_settings_schema_source_lookup(src, "org.gnostr.Client", TRUE)
      : NULL;
  if (schema) {
    gboolean has_key = g_settings_schema_has_key(schema, "nip46-connect-relays");
    g_settings_schema_unref(schema);
    if (has_key) {
      GSettings *settings = g_settings_new("org.gnostr.Client");
      gchar **v = g_settings_get_strv(settings, "nip46-connect-relays");
      g_object_unref(settings);
      if (v && v[0]) {
        if (out_count) *out_count = g_strv_length(v);
        return v;
      }
      g_strfreev(v);
    }
  }

  /* Built-in fallback list */
  gchar **v = g_new0(gchar *, NIP46_FALLBACK_RELAY_COUNT + 1);
  for (gsize i = 0; i < NIP46_FALLBACK_RELAY_COUNT; i++)
    v[i] = g_strdup(NIP46_FALLBACK_RELAYS[i]);
  if (out_count) *out_count = NIP46_FALLBACK_RELAY_COUNT;
  return v;
}

#endif /* NIP46_RELAY_DEFAULTS_H */
