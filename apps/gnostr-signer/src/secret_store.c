/* secret_store.c - Secure key storage implementation
 *
 * Wraps the nip55l signer_ops for UI-friendly operations with additional
 * features like listing all identities and generating new keys.
 *
 * Uses secure memory for handling private keys to prevent:
 * - Keys being swapped to disk
 * - Keys remaining in memory after use
 * - Timing attacks via constant-time comparison
 */
#include "secret_store.h"
#include "secure-memory.h"
#include "secure-mem.h"
#include "secure-delete.h"
#include <nostr/nip55l/signer_ops.h>
#include <nostr/nip55l/error.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_keys.h>
/* Core APIs still needed: nsec decode (GObject NIP-19 doesn't expose secret key hex),
 * key generation (GNostrKeys doesn't expose private key hex) */
#include <nostr/nip19/nip19.h>
#include <keys.h>
#include <gio/gio.h>  /* For GTask async API */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#define GNOSTR_HAVE_KEYCHAIN 1
#endif

/* Internal helpers */
static gboolean is_hex_64(const gchar *s) {
  if (!s) return FALSE;
  gsize n = strlen(s);
  if (n != 64) return FALSE;
  for (gsize i = 0; i < n; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

/* Convert binary to hex string in secure memory */
static gchar *bin_to_hex_secure(const guint8 *buf, gsize len) {
  static const gchar hexd[16] = "0123456789abcdef";
  gchar *out = (gchar*)gn_secure_alloc(len * 2 + 1);
  if (!out) return NULL;
  for (gsize i = 0; i < len; i++) {
    out[2*i] = hexd[(buf[i] >> 4) & 0xF];
    out[2*i+1] = hexd[buf[i] & 0xF];
  }
  out[len*2] = '\0';
  return out;
}

/* Legacy bin_to_hex for non-sensitive data */
static gchar *bin_to_hex(const guint8 *buf, gsize len) {
  static const gchar hexd[16] = "0123456789abcdef";
  gchar *out = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    out[2*i] = hexd[(buf[i] >> 4) & 0xF];
    out[2*i+1] = hexd[buf[i] & 0xF];
  }
  out[len*2] = '\0';
  return out;
}

/* Internal: hex string to raw bytes (for Keychain backend) */
static gboolean hex_to_bytes_ss(const gchar *hex, guint8 *out, gsize out_len) {
  gsize hex_len = strlen(hex);
  if (hex_len != out_len * 2) return FALSE;
  for (gsize i = 0; i < out_len; i++) {
    unsigned int byte;
    if (sscanf(hex + 2*i, "%2x", &byte) != 1) return FALSE;
    out[i] = (guint8)byte;
  }
  return TRUE;
}

/* Get fingerprint (first 8 hex chars of pubkey) from npub
 * Available for both libsecret and Keychain backends.
 */
static gchar *npub_to_fingerprint(const gchar *npub) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) {
    return NULL;
  }

  g_autoptr(GNostrNip19) nip19 = gnostr_nip19_decode(npub, NULL);
  if (!nip19) return NULL;

  const gchar *pubkey_hex = gnostr_nip19_get_pubkey(nip19);
  if (!pubkey_hex || strlen(pubkey_hex) < 8) {
    return NULL;
  }

  /* Use first 8 hex chars (4 bytes) */
  gchar *fp = g_strndup(pubkey_hex, 8);
  return fp;
}

#ifdef GNOSTR_HAVE_LIBSECRET
/* Schema for storing Nostr identity keys in the secret service.
 *
 * Attributes:
 * - key_id: Primary identifier (typically the npub)
 * - npub: Bech32-encoded public key (npub1...)
 * - fingerprint: First 8 characters of hex pubkey for quick lookup
 * - label: User-friendly display name
 * - hardware: "true" if this is a hardware key reference
 * - owner_uid: Unix UID of the key owner (empty if unset)
 * - owner_username: Unix username of the key owner (empty if unset)
 * - created_at: ISO 8601 timestamp of when the key was stored
 */
static const SecretSchema IDENTITY_SCHEMA = {
  "org.gnostr.Signer/identity",
  SECRET_SCHEMA_NONE,
  {
    { "key_id",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",           SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "fingerprint",    SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "label",          SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "hardware",       SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_uid",      SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_username", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "created_at",     SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

/* Get current ISO 8601 timestamp */
static gchar *get_iso8601_timestamp(void) {
  GDateTime *now = g_date_time_new_now_utc();
  gchar *timestamp = g_date_time_format_iso8601(now);
  g_date_time_unref(now);
  return timestamp;
}
#endif

gboolean secret_store_is_available(void) {
#ifdef GNOSTR_HAVE_LIBSECRET
  /* Check if secret service is reachable */
  GError *err = NULL;
  SecretService *svc = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
  if (err) {
    g_clear_error(&err);
    return FALSE;
  }
  if (svc) {
    g_object_unref(svc);
    return TRUE;
  }
  return FALSE;
#elif defined(GNOSTR_HAVE_KEYCHAIN)
  return TRUE;
#else
  return FALSE;
#endif
}

const gchar *secret_store_backend_name(void) {
#ifdef GNOSTR_HAVE_LIBSECRET
  return "libsecret";
#elif defined(GNOSTR_HAVE_KEYCHAIN)
  return "Keychain";
#else
  return "none";
#endif
}

/* nostrc-prqu.2: the signer daemon (org.nostr.Signer) is the only writer of
 * key material. Adding and removing keys go through its StoreKey / ClearKey
 * methods, which store under the unified schema the daemon owns; this client
 * never writes the Secret Service or the Keychain itself. */
#define SIGNER_BUS_NAME  "org.nostr.Signer"
#define SIGNER_OBJ_PATH  "/org/nostr/signer"

static SecretStoreResult result_from_dbus_error(const gchar *what, GError *err) {
  g_autofree gchar *remote = err ? g_dbus_error_get_remote_error(err) : NULL;
  g_warning("secret_store: %s via signer failed: %s", what, err ? err->message : "?");
  if (remote && (g_str_equal(remote, "org.nostr.Signer.InvalidKey") ||
                 g_str_equal(remote, "org.nostr.Signer.InvalidArgument")))
    return SECRET_STORE_ERR_INVALID_KEY;
  if (remote && g_str_equal(remote, "org.nostr.Signer.NotFound"))
    return SECRET_STORE_ERR_NOT_FOUND;
  return SECRET_STORE_ERR_BACKEND;
}

/* Blocking StoreKey; safe from the GTask worker threads below. */
static SecretStoreResult daemon_store_key(const gchar *key, const gchar *label, gchar **out_npub) {
  if (out_npub) *out_npub = NULL;
  GError *err = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) return result_from_dbus_error("StoreKey (no session bus)", err);
  g_autoptr(GVariant) ret = g_dbus_connection_call_sync(bus, SIGNER_BUS_NAME, SIGNER_OBJ_PATH,
      SIGNER_BUS_NAME, "StoreKey", g_variant_new("(ss)", key, label ? label : ""),
      G_VARIANT_TYPE("(bs)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
  if (!ret) {
    SecretStoreResult r = result_from_dbus_error("StoreKey", err);
    g_clear_error(&err);
    return r;
  }
  gboolean ok = FALSE;
  const gchar *npub = NULL;
  g_variant_get(ret, "(b&s)", &ok, &npub);
  if (!ok) return SECRET_STORE_ERR_BACKEND;
  if (out_npub && npub && *npub) *out_npub = g_strdup(npub);
  return SECRET_STORE_OK;
}

SecretStoreResult secret_store_add(const gchar *key,
                                   const gchar *label,
                                   gboolean link_to_user) {
  /* The daemon always records the owning uid. */
  (void)link_to_user;
  if (!key || !*key) return SECRET_STORE_ERR_INVALID_KEY;
  if (!is_hex_64(key) && !g_str_has_prefix(key, "nsec1")) return SECRET_STORE_ERR_INVALID_KEY;
  return daemon_store_key(key, label, NULL);
}

SecretStoreResult secret_store_remove(const gchar *selector) {
  if (!selector || !*selector) return SECRET_STORE_ERR_INVALID_KEY;
  GError *err = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    SecretStoreResult r = result_from_dbus_error("ClearKey (no session bus)", err);
    g_clear_error(&err);
    return r;
  }
  g_autoptr(GVariant) ret = g_dbus_connection_call_sync(bus, SIGNER_BUS_NAME, SIGNER_OBJ_PATH,
      SIGNER_BUS_NAME, "ClearKey", g_variant_new("(s)", selector),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
  if (!ret) {
    SecretStoreResult r = result_from_dbus_error("ClearKey", err);
    g_clear_error(&err);
    return r;
  }
  gboolean ok = FALSE;
  g_variant_get(ret, "(b)", &ok);
  return ok ? SECRET_STORE_OK : SECRET_STORE_ERR_NOT_FOUND;
}

void secret_store_entry_free(SecretStoreEntry *entry) {
  if (!entry) return;
  g_free(entry->npub);
  g_free(entry->key_id);
  g_free(entry->label);
  g_free(entry->owner_username);
  g_free(entry);
}

GPtrArray *secret_store_list(void) {
  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)secret_store_entry_free);

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
  if (err || !service) {
    if (err) g_clear_error(&err);
    return arr;
  }

  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  GList *items = secret_service_search_sync(service, &IDENTITY_SCHEMA, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, &err);
  g_hash_table_unref(attrs);

  if (err) {
    g_warning("secret_store_list: %s", err->message);
    g_clear_error(&err);
  }

  for (GList *it = items; it; it = it->next) {
    SecretItem *item = SECRET_ITEM(it->data);
    GHashTable *item_attrs = secret_item_get_attributes(item);

    SecretStoreEntry *entry = g_new0(SecretStoreEntry, 1);

    const gchar *npub = g_hash_table_lookup(item_attrs, "npub");
    const gchar *key_id = g_hash_table_lookup(item_attrs, "key_id");
    const gchar *label = g_hash_table_lookup(item_attrs, "label");
    const gchar *owner_uid = g_hash_table_lookup(item_attrs, "owner_uid");
    const gchar *owner_username = g_hash_table_lookup(item_attrs, "owner_username");

    entry->npub = npub ? g_strdup(npub) : NULL;
    entry->key_id = key_id ? g_strdup(key_id) : NULL;
    entry->label = label ? g_strdup(label) : NULL;

    if (owner_uid && *owner_uid) {
      entry->has_owner = TRUE;
      entry->owner_uid = (uid_t)g_ascii_strtoull(owner_uid, NULL, 10);
      entry->owner_username = owner_username ? g_strdup(owner_username) : NULL;
    }

    g_hash_table_unref(item_attrs);
    g_ptr_array_add(arr, entry);
  }

  g_list_free_full(items, g_object_unref);
  g_object_unref(service);

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS: Query all items with our service name */
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(query, &result);

  if (st == errSecSuccess && result) {
    CFArrayRef items = (CFArrayRef)result;
    CFIndex count = CFArrayGetCount(items);

    for (CFIndex i = 0; i < count; i++) {
      CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);

      SecretStoreEntry *entry = g_new0(SecretStoreEntry, 1);

      CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);
      CFStringRef label = CFDictionaryGetValue(item, kSecAttrLabel);

      if (account) {
        CFIndex len = CFStringGetLength(account);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        gchar *buf = g_malloc(maxSize);
        if (CFStringGetCString(account, buf, maxSize, kCFStringEncodingUTF8)) {
          entry->npub = buf;
          entry->key_id = g_strdup(buf);
        } else {
          g_free(buf);
        }
      }

      if (label) {
        CFIndex len = CFStringGetLength(label);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        gchar *buf = g_malloc(maxSize);
        if (CFStringGetCString(label, buf, maxSize, kCFStringEncodingUTF8)) {
          entry->label = buf;
        } else {
          g_free(buf);
        }
      }

      g_ptr_array_add(arr, entry);
    }
    CFRelease(result);
  }

  if (service) CFRelease(service);
  CFRelease(query);
#endif

  return arr;
}

SecretStoreResult secret_store_get_public_key(const gchar *selector,
                                              gchar **out_npub) {
  if (!out_npub) return SECRET_STORE_ERR_INVALID_KEY;
  *out_npub = NULL;

  gchar *npub = NULL;
  int rc = nostr_nip55l_get_public_key(&npub);

  if (rc == 0 && npub) {
    *out_npub = g_strdup(npub);
    free(npub);
    return SECRET_STORE_OK;
  }

  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  return SECRET_STORE_ERR_BACKEND;
}

SecretStoreResult secret_store_sign_event(const gchar *event_json,
                                          const gchar *selector,
                                          gchar **out_signature) {
  if (!event_json || !out_signature) return SECRET_STORE_ERR_INVALID_KEY;
  *out_signature = NULL;

  gchar *sig = NULL;
  int rc = nostr_nip55l_sign_event(event_json, selector, NULL, &sig);

  if (rc == 0 && sig) {
    *out_signature = g_strdup(sig);
    free(sig);
    return SECRET_STORE_OK;
  }

  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  if (rc == NOSTR_SIGNER_ERROR_INVALID_KEY) return SECRET_STORE_ERR_INVALID_KEY;
  return SECRET_STORE_ERR_BACKEND;
}

SecretStoreResult secret_store_sign_event_json(const gchar *event_json,
                                               const gchar *selector,
                                               gchar **out_signed_event_json) {
  if (!event_json || !out_signed_event_json) return SECRET_STORE_ERR_INVALID_KEY;
  *out_signed_event_json = NULL;

  char *json = NULL;
  int rc = nostr_nip55l_sign_event_json(event_json, selector, NULL, &json);

  if (rc == 0 && json) {
    *out_signed_event_json = g_strdup(json);
    free(json);
    return SECRET_STORE_OK;
  }

  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  if (rc == NOSTR_SIGNER_ERROR_INVALID_KEY) return SECRET_STORE_ERR_INVALID_KEY;
  return SECRET_STORE_ERR_BACKEND;
}

SecretStoreResult secret_store_sign_hash(const gchar *hash_hex,
                                         const gchar *selector,
                                         gchar **out_signature) {
  if (!hash_hex || !out_signature) return SECRET_STORE_ERR_INVALID_KEY;
  *out_signature = NULL;

  char *sig = NULL;
  int rc = nostr_nip55l_sign_hash(hash_hex, selector, &sig);

  if (rc == 0 && sig) {
    *out_signature = g_strdup(sig);
    free(sig);
    return SECRET_STORE_OK;
  }

  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  if (rc == NOSTR_SIGNER_ERROR_INVALID_KEY) return SECRET_STORE_ERR_INVALID_KEY;
  return SECRET_STORE_ERR_BACKEND;
}

SecretStoreResult secret_store_generate(const gchar *label,
                                        gboolean link_to_user,
                                        gchar **out_npub) {
  if (!out_npub) return SECRET_STORE_ERR_INVALID_KEY;
  *out_npub = NULL;

  /* Generate new keypair using libnostr */
  gchar *sk_hex_raw = nostr_key_generate_private();
  if (!sk_hex_raw) {
    return SECRET_STORE_ERR_BACKEND;
  }

  /* Copy to secure memory immediately and clear original */
  gchar *sk_hex = gn_secure_strdup(sk_hex_raw);
  gn_secure_clear_string(sk_hex_raw);  /* Zero and free the original */

  if (!sk_hex) {
    return SECRET_STORE_ERR_BACKEND;
  }

  /* Store it through the daemon; its reply names the npub it stored. */
  (void)link_to_user;
  gchar *npub = NULL;
  SecretStoreResult rc = daemon_store_key(sk_hex, label, &npub);
  if (rc == SECRET_STORE_OK && !npub) {
    g_autoptr(GNostrKeys) keys = gnostr_keys_new_from_hex(sk_hex, NULL);
    if (keys) npub = gnostr_keys_get_npub(keys);
  }
  gn_secure_strfree(sk_hex);
  if (rc != SECRET_STORE_OK) {
    g_free(npub);
    return rc;
  }
  if (!npub) {
    return SECRET_STORE_ERR_BACKEND;
  }

  *out_npub = npub;
  return SECRET_STORE_OK;
}

SecretStoreResult secret_store_set_label(const gchar *selector,
                                         const gchar *new_label) {
  /* nostrc-prqu.2: labels are kept by AccountsStore. The keyring item (and
   * its label) belongs to the signer daemon, which sets it at StoreKey time;
   * this client no longer edits it. */
  (void)new_label;
  if (!selector || !*selector) return SECRET_STORE_ERR_INVALID_KEY;
  return SECRET_STORE_OK;
}

gboolean secret_store_has_secret(const gchar *selector) {
  if (!selector || !*selector) return FALSE;
#ifdef GNOSTR_HAVE_LIBSECRET
  const char *keys[] = { "npub", "key_id" };
  for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
    GError *err = NULL;
    GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attrs, (gpointer)keys[k], (gpointer)selector);
    GList *items = secret_password_searchv_sync(&IDENTITY_SCHEMA, attrs,
                                                SECRET_SEARCH_NONE, NULL, &err);
    g_hash_table_unref(attrs);
    g_clear_error(&err);
    gboolean found = items != NULL;
    g_list_free_full(items, g_object_unref);
    if (found) return TRUE;
  }
  return FALSE;
#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, selector, kCFStringEncodingUTF8);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(query, &result);
  if (result) CFRelease(result);
  CFRelease(service); CFRelease(account); CFRelease(query);
  return st == errSecSuccess;
#else
  return FALSE;
#endif
}

SecretStoreResult secret_store_get_secret(const gchar *selector,
                                          gchar **out_nsec) {
  if (!out_nsec) return SECRET_STORE_ERR_INVALID_KEY;
  *out_nsec = NULL;

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  gchar *secret = NULL;

  /* Try by npub */
  secret = secret_password_lookup_sync(&IDENTITY_SCHEMA, NULL, &err,
                                       "npub", selector ? selector : "",
                                       NULL);
  if (err) {
    g_clear_error(&err);
  }

  if (!secret && selector) {
    /* Try by key_id */
    secret = secret_password_lookup_sync(&IDENTITY_SCHEMA, NULL, &err,
                                         "key_id", selector,
                                         NULL);
    if (err) g_clear_error(&err);
  }

  if (!secret) {
    return SECRET_STORE_ERR_NOT_FOUND;
  }

  /* Convert hex to nsec via GNostrNip19 */
  if (is_hex_64(secret)) {
    g_autoptr(GNostrNip19) nip19 = gnostr_nip19_encode_nsec(secret, NULL);
    if (nip19) {
      const gchar *nsec_str = gnostr_nip19_get_bech32(nip19);
      if (nsec_str) {
        *out_nsec = gn_secure_strdup(nsec_str);
      }
    }
  } else if (g_str_has_prefix(secret, "nsec1")) {
    /* Return nsec in secure memory */
    *out_nsec = gn_secure_strdup(secret);
  }

  /* Securely clear the secret before freeing */
  gn_secure_zero(secret, strlen(secret));
  secret_password_free(secret);
  return *out_nsec ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
  CFStringRef account = selector ? CFStringCreateWithCString(NULL, selector, kCFStringEncodingUTF8) : NULL;

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  if (account) CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(query, &result);

  if (service) CFRelease(service);
  if (account) CFRelease(account);
  CFRelease(query);

  if (st == errSecSuccess && result) {
    CFDataRef data = (CFDataRef)result;
    CFIndex len = CFDataGetLength(data);
    const UInt8 *bytes = CFDataGetBytePtr(data);

    if (len == 32 && bytes) {
      /* Convert raw bytes to hex, then encode via GNostrNip19 */
      gchar *sk_hex = bin_to_hex(bytes, 32);
      g_autoptr(GNostrNip19) nip19 = gnostr_nip19_encode_nsec(sk_hex, NULL);
      if (nip19) {
        const gchar *nsec_str = gnostr_nip19_get_bech32(nip19);
        if (nsec_str) {
          /* Secure memory as on the libsecret path: callers free with
           * gn_secure_strfree() (secure-memory.c), never g_free() or
           * gnostr_secure_strfree() (macOS device smoke 2026-10-01). */
          *out_nsec = gn_secure_strdup(nsec_str);
        }
      }
      memset(sk_hex, 0, 64);
      g_free(sk_hex);
    }
    CFRelease(result);
    return *out_nsec ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;
  }

  return SECRET_STORE_ERR_NOT_FOUND;
#else
  (void)selector;
  return SECRET_STORE_ERR_BACKEND;
#endif
}

SecretStoreResult secret_store_lookup_by_fingerprint(const gchar *fingerprint,
                                                      SecretStoreEntry **out_entry) {
  if (!fingerprint || !out_entry) return SECRET_STORE_ERR_INVALID_KEY;
  *out_entry = NULL;

  /* Validate and normalize fingerprint */
  gsize len = strlen(fingerprint);
  if (len < 4 || len > 64) return SECRET_STORE_ERR_INVALID_KEY;

  gchar *fp_lower = g_ascii_strdown(fingerprint, len);

  /* Validate hex */
  for (gsize i = 0; i < len; i++) {
    gchar c = fp_lower[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      g_free(fp_lower);
      return SECRET_STORE_ERR_INVALID_KEY;
    }
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
  if (err || !service) {
    if (err) g_clear_error(&err);
    g_free(fp_lower);
    return SECRET_STORE_ERR_BACKEND;
  }

  /* Search by fingerprint attribute (for exact 8-char match) */
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);

  /* If fingerprint is exactly 8 chars, search by fingerprint attribute */
  if (len == 8) {
    g_hash_table_insert(attrs, (gpointer)"fingerprint", (gpointer)fp_lower);
  }

  GList *items = secret_service_search_sync(service, &IDENTITY_SCHEMA, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, &err);
  g_hash_table_unref(attrs);

  if (err) {
    g_debug("secret_store_lookup_by_fingerprint: search failed: %s", err->message);
    g_clear_error(&err);
  }

  /* If no results with fingerprint attribute, search all and filter */
  if (!items && len != 8) {
    attrs = g_hash_table_new(g_str_hash, g_str_equal);
    items = secret_service_search_sync(service, &IDENTITY_SCHEMA, attrs,
                                       SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                       NULL, &err);
    g_hash_table_unref(attrs);
    if (err) g_clear_error(&err);
  }

  SecretStoreEntry *found = NULL;
  for (GList *it = items; it && !found; it = it->next) {
    SecretItem *item = SECRET_ITEM(it->data);
    GHashTable *item_attrs = secret_item_get_attributes(item);

    const gchar *npub = g_hash_table_lookup(item_attrs, "npub");
    const gchar *fp_attr = g_hash_table_lookup(item_attrs, "fingerprint");

    gboolean matches = FALSE;

    /* Check fingerprint attribute */
    if (fp_attr && *fp_attr) {
      matches = g_str_has_prefix(fp_attr, fp_lower);
    }

    /* If no fingerprint attribute, derive from npub */
    if (!matches && npub) {
      gchar *derived_fp = npub_to_fingerprint(npub);
      if (derived_fp) {
        matches = g_str_has_prefix(derived_fp, fp_lower);
        g_free(derived_fp);
      }
    }

    if (matches) {
      found = g_new0(SecretStoreEntry, 1);
      found->npub = npub ? g_strdup(npub) : NULL;
      found->key_id = g_strdup(g_hash_table_lookup(item_attrs, "key_id"));
      found->label = g_strdup(g_hash_table_lookup(item_attrs, "label"));

      const gchar *owner_uid = g_hash_table_lookup(item_attrs, "owner_uid");
      const gchar *owner_username = g_hash_table_lookup(item_attrs, "owner_username");

      if (owner_uid && *owner_uid) {
        found->has_owner = TRUE;
        found->owner_uid = (uid_t)g_ascii_strtoull(owner_uid, NULL, 10);
        found->owner_username = owner_username ? g_strdup(owner_username) : NULL;
      }
    }

    g_hash_table_unref(item_attrs);
  }

  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  g_free(fp_lower);

  if (found) {
    *out_entry = found;
    return SECRET_STORE_OK;
  }
  return SECRET_STORE_ERR_NOT_FOUND;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS: Query all items and filter by fingerprint */
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef svc = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, svc);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(query, &result);

  SecretStoreEntry *found = NULL;

  if (st == errSecSuccess && result) {
    CFArrayRef items = (CFArrayRef)result;
    CFIndex count = CFArrayGetCount(items);

    for (CFIndex i = 0; i < count && !found; i++) {
      CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);
      CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);

      if (account) {
        CFIndex acc_len = CFStringGetLength(account);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(acc_len, kCFStringEncodingUTF8) + 1;
        gchar *acc_buf = g_malloc(maxSize);

        if (CFStringGetCString(account, acc_buf, maxSize, kCFStringEncodingUTF8)) {
          /* Account is npub, derive fingerprint */
          gchar *derived_fp = npub_to_fingerprint(acc_buf);
          if (derived_fp && g_str_has_prefix(derived_fp, fp_lower)) {
            found = g_new0(SecretStoreEntry, 1);
            found->npub = g_strdup(acc_buf);
            found->key_id = g_strdup(acc_buf);

            CFStringRef label = CFDictionaryGetValue(item, kSecAttrLabel);
            if (label) {
              CFIndex lbl_len = CFStringGetLength(label);
              CFIndex lbl_max = CFStringGetMaximumSizeForEncoding(lbl_len, kCFStringEncodingUTF8) + 1;
              gchar *lbl_buf = g_malloc(lbl_max);
              if (CFStringGetCString(label, lbl_buf, lbl_max, kCFStringEncodingUTF8)) {
                found->label = lbl_buf;
              } else {
                g_free(lbl_buf);
              }
            }
          }
          g_free(derived_fp);
        }
        g_free(acc_buf);
      }
    }
    CFRelease(result);
  }

  if (svc) CFRelease(svc);
  CFRelease(query);
  g_free(fp_lower);

  if (found) {
    *out_entry = found;
    return SECRET_STORE_OK;
  }
  return SECRET_STORE_ERR_NOT_FOUND;

#else
  g_free(fp_lower);
  return SECRET_STORE_ERR_BACKEND;
#endif
}

/* ======== Async API implementation ======== */

typedef struct {
  SecretStoreListCallback callback;
  gpointer user_data;
} ListAsyncData;

static void list_async_thread_func(GTask *task, gpointer source_object,
                                   gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  /* Run the synchronous list in thread pool */
  GPtrArray *entries = secret_store_list();
  g_task_return_pointer(task, entries, (GDestroyNotify)g_ptr_array_unref);
}

static void list_async_ready_cb(GObject *source_object, GAsyncResult *res,
                                gpointer user_data) {
  (void)source_object;
  ListAsyncData *data = user_data;

  GError *error = NULL;
  GPtrArray *entries = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    g_warning("secret_store_list_async failed: %s", error->message);
    g_clear_error(&error);
    entries = NULL;
  }

  if (data->callback) {
    data->callback(entries, data->user_data);
  } else if (entries) {
    g_ptr_array_unref(entries);
  }

  g_free(data);
}

void secret_store_list_async(SecretStoreListCallback callback, gpointer user_data) {
  ListAsyncData *data = g_new0(ListAsyncData, 1);
  data->callback = callback;
  data->user_data = user_data;

  GTask *task = g_task_new(NULL, NULL, list_async_ready_cb, data);
  g_task_set_name(task, "secret_store_list_async");
  g_task_run_in_thread(task, list_async_thread_func);
  g_object_unref(task);
}

typedef struct {
  SecretStoreAvailableCallback callback;
  gpointer user_data;
} AvailableAsyncData;

static void available_async_thread_func(GTask *task, gpointer source_object,
                                        gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  gboolean available = secret_store_is_available();
  g_task_return_boolean(task, available);
}

static void available_async_ready_cb(GObject *source_object, GAsyncResult *res,
                                     gpointer user_data) {
  (void)source_object;
  AvailableAsyncData *data = user_data;

  GError *error = NULL;
  gboolean available = g_task_propagate_boolean(G_TASK(res), &error);

  if (error) {
    g_warning("secret_store_check_available_async failed: %s", error->message);
    g_clear_error(&error);
    available = FALSE;
  }

  if (data->callback) {
    data->callback(available, data->user_data);
  }

  g_free(data);
}

void secret_store_check_available_async(SecretStoreAvailableCallback callback,
                                        gpointer user_data) {
  AvailableAsyncData *data = g_new0(AvailableAsyncData, 1);
  data->callback = callback;
  data->user_data = user_data;

  GTask *task = g_task_new(NULL, NULL, available_async_ready_cb, data);
  g_task_set_name(task, "secret_store_check_available_async");
  g_task_run_in_thread(task, available_async_thread_func);
  g_object_unref(task);
}

/* ======== Async Add Implementation ======== */

typedef struct {
  gchar *key;
  gchar *label;
  gboolean link_to_user;
} AddAsyncTaskData;

static void add_async_task_data_free(gpointer data) {
  AddAsyncTaskData *task_data = data;
  if (task_data->key) {
    gn_secure_zero(task_data->key, strlen(task_data->key));
    g_free(task_data->key);
  }
  g_free(task_data->label);
  g_free(task_data);
}

static void add_async_thread_func(GTask *task, gpointer source_object,
                                  gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  AddAsyncTaskData *data = task_data;
  SecretStoreResult result = secret_store_add(data->key, data->label, data->link_to_user);
  g_task_return_int(task, (gssize)result);
}

void secret_store_add_async(const gchar *key,
                            const gchar *label,
                            gboolean link_to_user,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data) {
  if (!key || !*key) {
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_return_int(task, (gssize)SECRET_STORE_ERR_INVALID_KEY);
    g_object_unref(task);
    return;
  }

  AddAsyncTaskData *task_data = g_new0(AddAsyncTaskData, 1);
  task_data->key = g_strdup(key);
  task_data->label = g_strdup(label);
  task_data->link_to_user = link_to_user;

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_name(task, "secret_store_add_async");
  g_task_set_task_data(task, task_data, add_async_task_data_free);
  g_task_run_in_thread(task, add_async_thread_func);
  g_object_unref(task);
}

SecretStoreResult secret_store_add_finish(GAsyncResult *result, GError **error) {
  GTask *task = G_TASK(result);
  gssize ret = g_task_propagate_int(task, error);
  return (SecretStoreResult)ret;
}

/* ======== Async Remove Implementation ======== */

typedef struct {
  gchar *selector;
} RemoveAsyncTaskData;

static void remove_async_task_data_free(gpointer data) {
  RemoveAsyncTaskData *task_data = data;
  g_free(task_data->selector);
  g_free(task_data);
}

static void remove_async_thread_func(GTask *task, gpointer source_object,
                                     gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  RemoveAsyncTaskData *data = task_data;
  SecretStoreResult result = secret_store_remove(data->selector);
  g_task_return_int(task, (gssize)result);
}

void secret_store_remove_async(const gchar *selector,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  if (!selector || !*selector) {
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_return_int(task, (gssize)SECRET_STORE_ERR_INVALID_KEY);
    g_object_unref(task);
    return;
  }

  RemoveAsyncTaskData *task_data = g_new0(RemoveAsyncTaskData, 1);
  task_data->selector = g_strdup(selector);

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_name(task, "secret_store_remove_async");
  g_task_set_task_data(task, task_data, remove_async_task_data_free);
  g_task_run_in_thread(task, remove_async_thread_func);
  g_object_unref(task);
}

SecretStoreResult secret_store_remove_finish(GAsyncResult *result, GError **error) {
  GTask *task = G_TASK(result);
  gssize ret = g_task_propagate_int(task, error);
  return (SecretStoreResult)ret;
}

/* ======== GError Domain and Utilities ======== */

G_DEFINE_QUARK(secret-store-error-quark, secret_store_error)

const gchar *secret_store_result_to_string(SecretStoreResult result) {
  switch (result) {
    case SECRET_STORE_OK:
      return "Success";
    case SECRET_STORE_ERR_INVALID_KEY:
      return "Invalid key format";
    case SECRET_STORE_ERR_NOT_FOUND:
      return "Key not found";
    case SECRET_STORE_ERR_BACKEND:
      return "Backend error";
    case SECRET_STORE_ERR_PERMISSION:
      return "Permission denied";
    case SECRET_STORE_ERR_DUPLICATE:
      return "Duplicate key";
    default:
      return "Unknown error";
  }
}

void secret_store_result_to_gerror(SecretStoreResult result, GError **error) {
  if (result == SECRET_STORE_OK || error == NULL) {
    return;
  }

  g_set_error(error,
              SECRET_STORE_ERROR,
              (gint)result,
              "%s",
              secret_store_result_to_string(result));
}
