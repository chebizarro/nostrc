/**
 * Identity metadata shim - stub (nostrc-e5nz). See keystore.h.
 *
 * Built when neither libsecret nor the macOS Keychain is available: no
 * key store can be queried, so there is no identity metadata and no legacy
 * client keys to report. Signing still works through the signer.
 */

#if !defined(HAVE_LIBSECRET) && !defined(HAVE_MACOS_KEYCHAIN)

#include "keystore.h"

G_DEFINE_QUARK(gnostr-keystore-error-quark, gnostr_keystore_error)

void gnostr_key_info_free(GnostrKeyInfo *info) {
  if (!info) return;
  g_free(info->npub);
  g_free(info->label);
  g_free(info);
}

GnostrKeyInfo *gnostr_key_info_copy(const GnostrKeyInfo *info) {
  if (!info) return NULL;
  GnostrKeyInfo *copy = g_new0(GnostrKeyInfo, 1);
  copy->npub = g_strdup(info->npub);
  copy->label = g_strdup(info->label);
  copy->created_at = info->created_at;
  return copy;
}

gboolean gnostr_keystore_available(void) {
  return FALSE;
}

gboolean gnostr_keystore_legacy_migrates_automatically(void) {
  return FALSE;
}

GList *gnostr_keystore_list_keys(GError **error) {
  (void)error;
  return NULL;
}

gboolean gnostr_keystore_has_key(const char *npub) {
  (void)npub;
  return FALSE;
}

GList *gnostr_keystore_list_legacy_keys(GError **error) {
  (void)error;
  return NULL;
}

#endif /* !HAVE_LIBSECRET && !HAVE_MACOS_KEYCHAIN */
