/* SPDX-License-Identifier: MIT */
#pragma once
#include <glib/gi18n-lib.h>
static const struct { guint kind; const char *caption; } kinds[] = {
#include "kind-catalog.inc"
};
static inline const char *kind_name(gint kind) {
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) if ((gint)kinds[i].kind == kind) return _(kinds[i].caption);
  return _("Custom Event Type");
}
static inline const char *operation_name(const char *operation) {
  static const char *ops[] = {"event", "sign_event", "get_public_key", "get_relays", "ping", "connect", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt", "zap_decrypt", "self_test"};
  static const char *names[] = {N_("Sign Event"), N_("Sign Event"), N_("Share Public Key"), N_("Read Relay List"), N_("Check Connection"), N_("Connect App"), N_("Encrypt Message (Legacy)"), N_("Decrypt Message (Legacy)"), N_("Encrypt Message"), N_("Decrypt Message"), N_("Decrypt Payment"), N_("Test Identity")};
  for (guint i = 0; i < G_N_ELEMENTS(ops); i++) if (!g_strcmp0(ops[i], operation)) return _(names[i]);
  return _("Operation");
}
