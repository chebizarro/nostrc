/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include <string.h>

static gboolean invalid(GError **error) {
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid public panel data");
  return FALSE;
}
gboolean grotto_panels_public_text_validate(const char *value, GError **error) {
  if (!value) return TRUE;
  if (!g_utf8_validate(value, -1, NULL) || strlen(value) > 512) return invalid(error);
  g_autofree char *lower = g_ascii_strdown(value, -1);
  static const char *const markers[] = {"nsec1", "ncryptsec1", "nostrconnect:", "bunker:",
    "mnemonic=", "mnemonic:", "token=", "token:", "secret=", "secret:", "access_token", "-----begin private key-----"};
  for (guint i = 0; i < G_N_ELEMENTS(markers); i++)
    if (strstr(lower, markers[i])) return invalid(error);
  /* Conservative lexical recovery-phrase admission check, NOT key derivation
   * or a claim to recognize every possible secret hidden in human text. */
  guint words = 0;
  gboolean in_word = FALSE, phrase = TRUE;
  for (const char *p = lower; *p; p++) {
    if (g_ascii_islower(*p)) { if (!in_word) words++; in_word = TRUE; }
    else if (*p == ' ') in_word = FALSE;
    else phrase = FALSE;
    if (g_ascii_iscntrl(*p)) return invalid(error);
  }
  if (phrase && (words == 12 || words == 15 || words == 18 || words == 21 || words == 24))
    return invalid(error);
  return TRUE;
}
static gboolean identifier(const char *value, gboolean required, GError **error) {
  if (!grotto_panels_public_text_validate(value, error)) return FALSE;
  if ((!value || !*value) && required) return invalid(error);
  return TRUE;
}
static gboolean relay(const char *value, GError **error) {
  if (!identifier(value, TRUE, error)) return FALSE;
  g_autoptr(GUri) uri = g_uri_parse(value, G_URI_FLAGS_NONE, NULL);
  if (!uri || !g_uri_get_host(uri) || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
      (g_strcmp0(g_uri_get_scheme(uri), "wss") && g_strcmp0(g_uri_get_scheme(uri), "ws")))
    return invalid(error);
  return TRUE;
}
static gboolean vector(const char *const *values, gboolean urls, GError **error) {
  if (!values) return TRUE;
  for (guint i = 0; values[i]; i++) {
    if (i >= 64 || !(urls ? relay(values[i], error) : identifier(values[i], TRUE, error)))
      return error && *error ? FALSE : invalid(error);
  }
  return TRUE;
}
gboolean grotto_panels_metadata_validate(const GrottoPanelsMetadata *m, GError **error) {
  if (!m) return invalid(error);
  const char *fields[] = {m->id, m->label, m->public_key, m->state, m->reason, m->access_policy,
    m->principal, m->verified_as, m->transport, m->operation, m->outcome, m->trust, m->decision, m->relay};
  for (guint i = 0; i < G_N_ELEMENTS(fields); i++)
    if (!grotto_panels_public_text_validate(fields[i], error)) return FALSE;
  return vector(m->relays, TRUE, error) && vector(m->methods, FALSE, error) &&
    vector(m->effective_permissions, FALSE, error);
}
gboolean grotto_panel_command_args_validate(GrottoPanelCommand command,
  const GrottoPanelCommandArgs *a, GError **error) {
  if (command < 0 || command >= GROTTO_PANEL_N_COMMANDS) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported panel command");
    return FALSE;
  }
  const GrottoPanelCommandArgs empty = {0};
  if (!a) a = &empty;
  /* Reject unused fields too: no side channel through a permissive dictionary
   * or a cosmetic label smuggled into a tier/default/pause command. */
  gboolean principal = command == GROTTO_PANEL_SET_CLIENT_LABEL || command == GROTTO_PANEL_SET_CLIENT_TRUST ||
    command == GROTTO_PANEL_SET_PERMISSIONS || command == GROTTO_PANEL_REVOKE_CLIENT;
  gboolean identity = command == GROTTO_PANEL_SET_DEFAULT_IDENTITY || command == GROTTO_PANEL_SET_PERMISSIONS || command == GROTTO_PANEL_SET_IDENTITY_LABEL;
  gboolean permissions = command == GROTTO_PANEL_SET_PERMISSIONS;
  gboolean remote = command == GROTTO_PANEL_CONFIGURE_REMOTE;
  if ((!principal && a->principal) || (!identity && a->public_key) ||
      (command != GROTTO_PANEL_SET_CLIENT_LABEL && command != GROTTO_PANEL_SET_IDENTITY_LABEL && a->label) ||
      (command != GROTTO_PANEL_SET_CLIENT_TRUST && a->trust) ||
      (!permissions && (a->permissions || a->n_permissions || a->expected_revision)) ||
      (!remote && (a->remote_enabled || a->relays || a->methods))) return invalid(error);
  if (!identifier(a->principal, principal, error) || !identifier(a->public_key, identity, error) ||
      !identifier(a->label, FALSE, error) || !identifier(a->trust, command == GROTTO_PANEL_SET_CLIENT_TRUST, error)) return FALSE;
  if ((command == GROTTO_PANEL_SET_CLIENT_LABEL || command == GROTTO_PANEL_SET_IDENTITY_LABEL) && !a->label) return invalid(error);
  if (a->n_permissions > 256 || (a->n_permissions && !a->permissions)) return invalid(error);
  for (gsize i = 0; i < a->n_permissions; i++) {
    const GrottoPanelPermission *p = &a->permissions[i];
    if (!identifier(p->operation, TRUE, error) || !identifier(p->decision, TRUE, error) ||
        (p->relay && !relay(p->relay, error))) return FALSE;
  }
  return vector(a->relays, TRUE, error) && vector(a->methods, FALSE, error);
}
