/**
 * Identity metadata shim - macOS Keychain (nostrc-e5nz). See keystore.h.
 *
 * Queries return attributes only (kSecReturnAttributes, never
 * kSecReturnData), so no secret enters this process.
 *
 * - Signer identities: generic-password items the nip55l daemon writes
 *   (nips/nip55l/src/core/signer_ops.c, Keychain branch): service
 *   "Gnostr Identity Key", account = key_id selector, comment = npub.
 * - Legacy client keys: items GNostr itself stored before nostrc-e5nz:
 *   service "org.gnostr.Client", account = npub. The nip55l daemon imports
 *   these into its own items once, when it starts (nostrc-de9h:
 *   nostr_nip55l_migrate_legacy_keys), so the UI says to start GNostr
 *   Signer rather than to import them by hand (nostrc-jppi).
 */

#ifdef HAVE_MACOS_KEYCHAIN

#include "keystore.h"
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

#define SIGNER_SERVICE_NAME "Gnostr Identity Key"
#define LEGACY_CLIENT_SERVICE_NAME "org.gnostr.Client"

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
  return TRUE;
}

gboolean gnostr_keystore_legacy_migrates_automatically(void) {
  return TRUE;
}

static char *cfstring_dup(CFTypeRef value) {
  if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return NULL;
  CFStringRef s = (CFStringRef)value;
  CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s),
                                                  kCFStringEncodingUTF8) + 1;
  char *buf = g_malloc(max);
  if (!CFStringGetCString(s, buf, max, kCFStringEncodingUTF8)) {
    g_free(buf);
    return NULL;
  }
  return buf;
}

static gint compare_npub(gconstpointer a, gconstpointer b) {
  return g_strcmp0(((const GnostrKeyInfo *)a)->npub, ((const GnostrKeyInfo *)b)->npub);
}

/* Generic-password items for service_name, one GnostrKeyInfo per npub. The
 * npub is the comment (signer items) or the account (legacy client items). */
static GList *list_service(const char *service_name, gboolean npub_in_comment,
                           GError **error) {
  CFStringRef service = CFStringCreateWithCString(NULL, service_name,
                                                  kCFStringEncodingUTF8);
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);

  CFArrayRef items = NULL;
  OSStatus status = SecItemCopyMatching(query, (CFTypeRef *)&items);
  CFRelease(query);
  CFRelease(service);

  if (status == errSecItemNotFound || (status == errSecSuccess && !items))
    return NULL;
  if (status != errSecSuccess) {
    g_set_error(error, GNOSTR_KEYSTORE_ERROR, GNOSTR_KEYSTORE_ERROR_FAILED,
                "Keychain query failed (OSStatus %d)", (int)status);
    if (items) CFRelease(items);
    return NULL;
  }

  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  GList *result = NULL;
  CFIndex count = CFArrayGetCount(items);
  for (CFIndex i = 0; i < count; i++) {
    CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);
    char *account = cfstring_dup(CFDictionaryGetValue(item, kSecAttrAccount));
    char *npub = npub_in_comment
        ? cfstring_dup(CFDictionaryGetValue(item, kSecAttrComment))
        : g_strdup(account);
    if (npub && g_str_has_prefix(npub, "npub1") && !g_hash_table_contains(seen, npub)) {
      GnostrKeyInfo *info = g_new0(GnostrKeyInfo, 1);
      info->npub = g_steal_pointer(&npub);
      /* A signer selector other than the npub is the identity's name. */
      if (npub_in_comment && account && g_strcmp0(account, info->npub) != 0)
        info->label = g_strdup(account);
      CFDateRef created = CFDictionaryGetValue(item, kSecAttrCreationDate);
      if (created && CFGetTypeID(created) == CFDateGetTypeID())
        info->created_at = (gint64)(CFDateGetAbsoluteTime(created) +
                                    kCFAbsoluteTimeIntervalSince1970);
      g_hash_table_add(seen, info->npub);
      result = g_list_prepend(result, info);
    }
    g_free(npub);
    g_free(account);
  }
  g_hash_table_unref(seen);
  CFRelease(items);
  return g_list_sort(result, compare_npub);
}

GList *gnostr_keystore_list_keys(GError **error) {
  return list_service(SIGNER_SERVICE_NAME, TRUE, error);
}

gboolean gnostr_keystore_has_key(const char *npub) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) return FALSE;
  GList *keys = gnostr_keystore_list_keys(NULL);
  gboolean found = FALSE;
  for (GList *l = keys; l && !found; l = l->next)
    found = g_strcmp0(((GnostrKeyInfo *)l->data)->npub, npub) == 0;
  g_list_free_full(keys, (GDestroyNotify)gnostr_key_info_free);
  return found;
}

GList *gnostr_keystore_list_legacy_keys(GError **error) {
  return list_service(LEGACY_CLIENT_SERVICE_NAME, FALSE, error);
}

#endif /* HAVE_MACOS_KEYCHAIN */
