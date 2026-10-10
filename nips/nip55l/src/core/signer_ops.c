/* Core helpers for NIP-55L signer operations.
 * Dependencies: libnostr JSON/event APIs, NIP-04, NIP-44 v2.
 */

#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nostr-json.h>
#include <nostr-event.h>
#include <nostr-utils.h>      /* nostr_hex2bin */
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip19/nip19.h>
#include <keys.h>

#include <secure_buf.h>

#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <openssl/rand.h>

#ifdef NIP55L_HAVE_LIBSECRET
#include <libsecret/secret.h>
#include <sys/types.h>
#include <unistd.h>
/* Identity-backed storage: one item per identity; attributes allow selection
 * by key_id or npub. The schema (org.gnostr.Signer/identity) is owned by
 * gnome/seahorse (gnostr-secret) — nostrc-bml6. */
#include "seahorse/secret_store.h"
#endif

#ifdef NIP55L_HAVE_KEYCHAIN
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
/* kSecAttrService value shared with the GUI (secret_store.c
 * GNOSTR_KC_SERVICE) so both processes see the same items. */
#define KC_SIGNER_SERVICE        "Gnostr Identity Key"
#endif

static int is_hex_64(const char *s) {
  if (!s) return 0; size_t n = strlen(s); if (n != 64) return 0; for (size_t i=0;i<n;i++){ char c=s[i]; if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0; } return 1;
}

/* --- In-process active-account cache (nostrc-7g9d) --------------------------
 *
 * Between a successful StoreKey and the next GetPublicKey / SignEvent the
 * daemon used to make a fresh libsecret round-trip through resolve_seckey_hex().
 * On the aarch64 lab that round-trip returned NOT_FOUND for a key libsecret
 * had just accepted — the smoke transcript in
 * `docs/reviews/nostr-dav-publish-live-2026-09-26.md` reproduces it. The
 * proximate cause is timing / schema quirks between store and search on the
 * running gnome-keyring; the durable fix is to keep the just-stored key alive
 * in-process so read APIs never depend on a re-fetch. StoreKey populates the
 * cache from the value it verified; ClearKey wipes it; GetPublicKey and the
 * resolver both consult the cache before falling back to env / libsecret /
 * Keychain. The sk_hex string is zeroed with secure_wipe() on eviction.
 *
 * Security: the daemon already holds the secret key in memory for the
 * duration of every signing call it services; the cache extends that lifetime
 * to “between StoreKey and the matching ClearKey”. An attacker with the same
 * uid could always have called SignEvent directly, so this does not enlarge
 * the attack surface — it removes a functional bug that made publish
 * end-to-end unusable. */
static char *g_cached_active_sk_hex = NULL;
static char *g_cached_active_npub  = NULL;

static void signer_cache_clear(void){
  if (g_cached_active_sk_hex) {
    secure_wipe(g_cached_active_sk_hex, strlen(g_cached_active_sk_hex));
    free(g_cached_active_sk_hex);
    g_cached_active_sk_hex = NULL;
  }
  if (g_cached_active_npub) {
    free(g_cached_active_npub);
    g_cached_active_npub = NULL;
  }
}

static void signer_cache_set(const char *sk_hex, const char *npub){
  signer_cache_clear();
  if (sk_hex && is_hex_64(sk_hex)) g_cached_active_sk_hex = strdup(sk_hex);
  if (npub && *npub)               g_cached_active_npub  = strdup(npub);
}

static char *bin_to_hex(const uint8_t *buf, size_t len){
  static const char hexd[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
  char *out = (char*)malloc(len*2+1); if(!out) return NULL;
  for(size_t i=0;i<len;i++){ out[2*i]=hexd[(buf[i]>>4)&0xF]; out[2*i+1]=hexd[buf[i]&0xF]; }
  out[len*2]='\0'; return out;
}

/* The key in the daemon's environment (NOSTR_SIGNER_SECKEY_HEX, else
 * NOSTR_SIGNER_NSEC) as 64-hex: 0, NOT_FOUND when there is none, INVALID_KEY
 * for an nsec that does not decode. */
static int env_seckey_hex(char **out_sk_hex){
  *out_sk_hex = NULL;
  const char *hex = getenv("NOSTR_SIGNER_SECKEY_HEX");
  if (hex && is_hex_64(hex)) { *out_sk_hex = strdup(hex); return *out_sk_hex ? 0 : NOSTR_SIGNER_ERROR_BACKEND; }
  const char *nsec = getenv("NOSTR_SIGNER_NSEC");
  if (!nsec || strncmp(nsec, "nsec1", 5) != 0) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  uint8_t sk[32];
  if (nostr_nip19_decode_nsec(nsec, sk) != 0) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  *out_sk_hex = bin_to_hex(sk, 32);
  secure_wipe(sk, sizeof sk);
  return *out_sk_hex ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
}

#ifdef NIP55L_HAVE_LIBSECRET
#include <gio/gio.h>
/* Grotto's chosen identity (GSettings org.nostr.Grotto default-identity),
 * or NULL when unset or the schema is not installed. */
static gchar *grotto_default_identity(void){
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.nostr.Grotto", TRUE) : NULL;
  if (!schema) return NULL;
  gboolean has = g_settings_schema_has_key(schema, "default-identity");
  g_settings_schema_unref(schema);
  if (!has) return NULL;
  GSettings *settings = g_settings_new("org.nostr.Grotto");
  gchar *npub = g_settings_get_string(settings, "default-identity");
  g_object_unref(settings);
  if (npub && !*npub) g_clear_pointer(&npub, g_free);
  return npub;
}

/* The Secret Service with a session open, for a search that loads secrets;
 * NULL (and *error set) when there is none or it opens no session
 * (nostrc-poc10). secret_service_search_sync(..., SECRET_SEARCH_LOAD_SECRETS,
 * ...) in libsecret 0.21.8 opens the session itself with a NULL GError, and
 * its fallback from the dh-ietf1024-sha256-aes128-cbc-pkcs7 algorithm to
 * "plain" then reads that NULL: a Secret Service offering only plain
 * sessions, or refusing OpenSession, crashed the daemon on the first lookup
 * that loaded a secret, which any caller could reach with an unknown
 * selector. Opened here with an error to report into, the fallback works and
 * the searches reuse the session. */
static SecretService *signer_secret_service(GError **error){
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, error);
  if (!service) return NULL;
  if (!secret_service_ensure_session_sync(service, NULL, error)) {
    g_object_unref(service);
    return NULL;
  }
  return service;
}

/* --- Key store reads (nostrc-sjyl3, nostrc-f6l29) ---------------------------
 *
 * Each returns 0; NOSTR_SIGNER_ERROR_NOT_FOUND when the store holds no such
 * identity, or there is no Secret Service on the bus at all (a headless
 * deployment with only an environment key); NOSTR_SIGNER_ERROR_BACKEND when
 * a Secret Service is there but cannot be read. A store that fails is not a
 * store without the key: the daemon answers Error.Internal for it, not
 * Error.NoKeyConfigured, as ListIdentities does. */

/* No Secret Service on the bus at all (nothing owns or activates
 * org.freedesktop.secrets), as opposed to one that fails. */
static gboolean secret_service_absent(const GError *e){
  return e && (g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
               g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER));
}

static int store_error_rc(const char *what, const GError *e){
  if (secret_service_absent(e)) {
    g_debug("nip55l: no Secret Service: %s", e->message);
    return NOSTR_SIGNER_ERROR_NOT_FOUND;
  }
  g_message("nip55l: key store %s: %s", what, e ? e->message : "unknown error");
  return NOSTR_SIGNER_ERROR_BACKEND;
}

static int store_service(SecretService **out_service){
  GError *e = NULL;
  *out_service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &e);
  if (*out_service) return 0;
  int rc = store_error_rc("unavailable", e);
  g_clear_error(&e);
  return rc;
}

/* The identity items matching @attrs (empty: all), in *out_items (NULL when
 * none). The search loads no secrets and so needs no session: a store that
 * refuses sessions still says which identities it holds. */
static int store_search(SecretService *service, GHashTable *attrs, GList **out_items){
  GError *e = NULL;
  *out_items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                          SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK, NULL, &e);
  if (!e) return 0;
  g_list_free_full(*out_items, g_object_unref);
  *out_items = NULL;
  int rc = store_error_rc("search failed", e);
  g_clear_error(&e);
  return rc;
}

/* Secret text (64-hex or nsec1) → lowercase 64-hex; NULL if neither. */
static char *secret_text_to_sk_hex(const char *sec){
  if (!sec) return NULL;
  if (is_hex_64(sec)) {
    char *hex = strdup(sec);
    for (char *p = hex; p && *p; p++) *p = (char)g_ascii_tolower(*p);
    return hex;
  }
  if (strncmp(sec, "nsec1", 5) == 0) {
    uint8_t sk[32];
    if (nostr_nip19_decode_nsec(sec, sk) != 0) return NULL;
    char *hex = bin_to_hex(sk, 32);
    secure_wipe(sk, sizeof sk);
    return hex;
  }
  return NULL;
}

/* The secret key of @item, as 64-hex. The session is opened first, with an
 * error to report into (signer_secret_service(), nostrc-poc10). INVALID_KEY
 * when the secret is not a key. */
static int store_item_sk_hex(SecretService *service, SecretItem *item, char **out_sk_hex){
  GError *e = NULL;
  if (!secret_service_ensure_session_sync(service, NULL, &e) ||
      !secret_item_load_secret_sync(item, NULL, &e)) {
    int rc = store_error_rc("secret unreadable", e);
    g_clear_error(&e);
    return rc == NOSTR_SIGNER_ERROR_NOT_FOUND ? NOSTR_SIGNER_ERROR_BACKEND : rc;
  }
  SecretValue *sv = secret_item_get_secret(item);
  if (!sv) return NOSTR_SIGNER_ERROR_BACKEND;
  *out_sk_hex = secret_text_to_sk_hex(secret_value_get_text(sv));
  secret_value_unref(sv);
  return *out_sk_hex ? 0 : NOSTR_SIGNER_ERROR_INVALID_KEY;
}

/* The secret key of the first identity item matching @attrs. */
static int store_first_sk_hex(GHashTable *attrs, char **out_sk_hex){
  SecretService *service = NULL;
  int rc = store_service(&service);
  if (rc != 0) return rc;
  GList *items = NULL;
  rc = store_search(service, attrs, &items);
  if (rc == 0)
    rc = items ? store_item_sk_hex(service, SECRET_ITEM(items->data), out_sk_hex)
               : NOSTR_SIGNER_ERROR_NOT_FOUND;
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  return rc;
}

/* The secret key of the first identity item whose @attr is @value. */
static int store_lookup_sk_hex(const char *attr, const char *value, char **out_sk_hex){
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)attr, (gpointer)value);
  int rc = store_first_sk_hex(attrs, out_sk_hex);
  g_hash_table_unref(attrs);
  return rc;
}

/* The secret key of the first identity item linked to this login user. */
static int store_owned_sk_hex(char **out_sk_hex){
  gchar uid_buf[32];
  g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
  return store_lookup_sk_hex("owner_uid", uid_buf, out_sk_hex);
}
#endif

/* Forward declarations */
static int resolve_seckey_hex(const char *current_user, char **out_sk_hex);
static char *env_identity_npub(void);

/* Secure resolver: yields a 32-byte private key in a nostr_secure_buf.
 * Internally leverages resolve_seckey_hex for selection, then converts to binary
 * and wipes the transient hex string.
 */
static int resolve_seckey_secure(const char *current_user, nostr_secure_buf *out_sk){
  if (!out_sk) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_sk = (nostr_secure_buf){0};
  char *sk_hex = NULL;
  int rc = resolve_seckey_hex(current_user, &sk_hex);
  if (rc != 0 || !sk_hex) return rc ? rc : NOSTR_SIGNER_ERROR_NOT_FOUND;
  if (!is_hex_64(sk_hex)) { memset(sk_hex, 0, strlen(sk_hex)); free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  nostr_secure_buf sb = secure_alloc(32);
  if (!sb.ptr) { memset(sk_hex, 0, strlen(sk_hex)); free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  if (!nostr_hex2bin((uint8_t*)sb.ptr, sk_hex, 32)) {
    secure_free(&sb);
    memset(sk_hex, 0, strlen(sk_hex));
    free(sk_hex);
    return NOSTR_SIGNER_ERROR_INVALID_KEY;
  }
  memset(sk_hex, 0, strlen(sk_hex));
  free(sk_hex);
  *out_sk = sb;
  return 0;
}

/* Resolve a secret key for the current user. Accepts:
 * - 64-hex seckey
 * - nsec1... bech32
 * - env NOSTR_SIGNER_SECKEY_HEX or NOSTR_SIGNER_NSEC (fallbacks)
 * On success, returns newly allocated 64-hex in *out_sk_hex. NOT_FOUND /
 * INVALID_KEY when no key matches; BACKEND when the key store cannot be read
 * (see "Key store reads").
 */
static int resolve_seckey_hex(const char *current_user, char **out_sk_hex){
  if (!out_sk_hex) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_sk_hex=NULL;
  /* nostrc-7g9d: honour the in-process cache before the env/libsecret path
   * so a key freshly stored via StoreKey is visible immediately. When the
   * caller supplies a specific selector (identity) we only match if the
   * cached npub equals it — otherwise fall through to the selector-based
   * lookup below. */
  if (g_cached_active_sk_hex) {
    if (!current_user || !*current_user ||
        (g_cached_active_npub &&
         strcmp(current_user, g_cached_active_npub) == 0)) {
      *out_sk_hex = strdup(g_cached_active_sk_hex);
      return *out_sk_hex ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
    }
  }
  const char *cand = current_user;
  if (!cand || !*cand) {
    int rc_env = env_seckey_hex(out_sk_hex);
    if (rc_env != NOSTR_SIGNER_ERROR_NOT_FOUND) return rc_env;
    /* The keyring: the identity linked to this login user, else the one
     * Grotto has chosen (its default-identity setting), else the only one
     * stored. A key created in Grotto's setup is not linked to the user,
     * and after a restart (logout, upgrade) the daemon answered "no key
     * configured" for it (nostrc-wic1). More than one unlinked identity and
     * no choice stays not found: the daemon never guesses between keys.
     * Searches load the secrets: without SECRET_SEARCH_LOAD_SECRETS,
     * secret_item_get_secret() is NULL and even the linked key was "not
     * found" once the in-memory copy from StoreKey was gone. */
#ifdef NIP55L_HAVE_LIBSECRET
    {
      int rc_s = store_owned_sk_hex(out_sk_hex);
      if (rc_s == NOSTR_SIGNER_ERROR_NOT_FOUND) {
        SecretService *service = NULL;
        rc_s = store_service(&service);
        if (rc_s == 0) {
          g_autofree gchar *chosen = grotto_default_identity();
          GHashTable *all = g_hash_table_new(g_str_hash, g_str_equal);
          if (chosen && *chosen)
            g_hash_table_insert(all, (gpointer)"npub", chosen);
          GList *items = NULL;
          rc_s = store_search(service, all, &items);
          g_hash_table_unref(all);
          if (rc_s == 0)
            rc_s = items && (chosen && *chosen ? TRUE : g_list_length(items) == 1)
                     ? store_item_sk_hex(service, SECRET_ITEM(items->data), out_sk_hex)
                     : NOSTR_SIGNER_ERROR_NOT_FOUND;
          g_list_free_full(items, g_object_unref);
          g_object_unref(service);
        }
      }
      return rc_s;
    }
#elif defined(NIP55L_HAVE_KEYCHAIN)
    /* macOS Keychain: find any identity item in this user's keychain */
    {
      CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
      CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
      CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
      CFStringRef service = CFStringCreateWithCString(NULL, KC_SIGNER_SERVICE, kCFStringEncodingUTF8);
      if (service) { CFDictionarySetValue(q, kSecAttrService, service); }
      CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
      CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
      CFTypeRef result = NULL; OSStatus st = SecItemCopyMatching(q, &result);
      if (service) CFRelease(service);
      if (q) CFRelease(q);
      if (st == errSecSuccess && result) {
        CFDataRef d = (CFDataRef)result;
        const UInt8 *bytes = CFDataGetBytePtr(d);
        CFIndex blen = CFDataGetLength(d);
        int rc_kc = NOSTR_SIGNER_ERROR_NOT_FOUND;
        if (blen == 32 && bytes) {
          char *hex = bin_to_hex((const uint8_t*)bytes, 32);
          if (hex) { *out_sk_hex = hex; rc_kc = 0; }
          else rc_kc = NOSTR_SIGNER_ERROR_BACKEND;
        }
        /* Wipe key material from the CF buffer before release. */
        if (bytes && blen > 0) memset((void *)bytes, 0, (size_t)blen);
        CFRelease(d);
        return rc_kc;
      }
    }
#endif
    return NOSTR_SIGNER_ERROR_NOT_FOUND;
  }
  if (is_hex_64(cand)) { *out_sk_hex = strdup(cand); return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND; }
  if (strncmp(cand, "nsec1", 5)==0) {
    uint8_t sk[32]; if (nostr_nip19_decode_nsec(cand, sk)!=0) return NOSTR_SIGNER_ERROR_INVALID_KEY;
    *out_sk_hex = bin_to_hex(sk, 32);
    secure_wipe(sk, sizeof sk);
    return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND;
  }
  /* An npub selects exactly that identity, never another key: the
   * environment key's, else a stored one (nostrc-a4w5). */
  const int sel_is_npub = strncmp(cand, "npub1", 5) == 0;
  if (sel_is_npub) {
    char *env_npub = env_identity_npub();
    const int is_env = env_npub && strcmp(env_npub, cand) == 0;
    free(env_npub);
    if (is_env) return env_seckey_hex(out_sk_hex);
  }
#ifdef NIP55L_HAVE_LIBSECRET
  /* Treat current_user as identity selector: key_id or npub */
  {
    int rc_l = store_lookup_sk_hex("key_id", cand, out_sk_hex);
    if (rc_l == NOSTR_SIGNER_ERROR_NOT_FOUND)
      rc_l = store_lookup_sk_hex("npub", cand, out_sk_hex);
    if (rc_l != NOSTR_SIGNER_ERROR_NOT_FOUND || sel_is_npub) return rc_l;
    /* A key_id or label naming no item: this user's identity, else the
     * environment key. */
    rc_l = store_owned_sk_hex(out_sk_hex);
    if (rc_l != NOSTR_SIGNER_ERROR_NOT_FOUND) return rc_l;
    rc_l = env_seckey_hex(out_sk_hex);
    if (rc_l != NOSTR_SIGNER_ERROR_NOT_FOUND) return rc_l;
  }
#elif defined(NIP55L_HAVE_KEYCHAIN)
  /* Treat current_user as identity selector when provided */
  {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
    CFStringRef service = CFStringCreateWithCString(NULL, KC_SIGNER_SERVICE, kCFStringEncodingUTF8);
    if (service) CFDictionarySetValue(q, kSecAttrService, service);
    /* Try account == selector */
    CFStringRef account = CFStringCreateWithCString(NULL, cand, kCFStringEncodingUTF8);
    if (account) CFDictionarySetValue(q, kSecAttrAccount, account);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = NULL; OSStatus st = SecItemCopyMatching(q, &result);
    if (st != errSecSuccess) {
      /* Try comment == selector */
      if (account) { CFDictionaryRemoveValue(q, kSecAttrAccount); }
      CFStringRef comment = CFStringCreateWithCString(NULL, cand, kCFStringEncodingUTF8);
      if (comment) {
        CFDictionarySetValue(q, kSecAttrComment, comment);
        st = SecItemCopyMatching(q, &result);
        CFRelease(comment);
      }
    }
    if (service) CFRelease(service);
    if (account) CFRelease(account);
    if (st == errSecSuccess && result) {
      CFDataRef d = (CFDataRef)result;
      const UInt8 *bytes = CFDataGetBytePtr(d);
      CFIndex blen = CFDataGetLength(d);
      int rc_kc = NOSTR_SIGNER_ERROR_NOT_FOUND;
      if (blen == 32 && bytes) {
        char *hex = bin_to_hex((const uint8_t*)bytes, 32);
        if (hex) { *out_sk_hex = hex; rc_kc = 0; }
        else rc_kc = NOSTR_SIGNER_ERROR_BACKEND;
      }
      CFRelease(d);
      CFRelease(q);
      return rc_kc;
    }
    if (q) CFRelease(q);
  }
#endif
  return NOSTR_SIGNER_ERROR_INVALID_KEY;
}

static int sk_hex_to_npub(const char *sk_hex, char **out_npub){
  *out_npub = NULL;
  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) return NOSTR_SIGNER_ERROR_BACKEND;
  uint8_t pk[32]; if (!nostr_hex2bin(pk, pk_hex, sizeof pk)) { free(pk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  free(pk_hex);
  char *npub=NULL; if (nostr_nip19_encode_npub(pk, &npub)!=0 || !npub) return NOSTR_SIGNER_ERROR_BACKEND;
  *out_npub = npub; return 0;
}

int nostr_nip55l_resolve_npub(const char *current_user, char **out_npub){
  if (!out_npub) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_npub = NULL;
  char *sk_hex = NULL; int rc = resolve_seckey_hex(current_user, &sk_hex);
  if (rc != 0 || !sk_hex) return rc ? rc : NOSTR_SIGNER_ERROR_NOT_FOUND;
  rc = is_hex_64(sk_hex) ? sk_hex_to_npub(sk_hex, out_npub) : NOSTR_SIGNER_ERROR_INVALID_KEY;
  secure_wipe(sk_hex, strlen(sk_hex));
  free(sk_hex);
  return rc;
}

int nostr_nip55l_normalize_selector(const char *selector, char **out_selector, char **out_npub){
  if (!out_selector || !out_npub) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_selector = NULL; *out_npub = NULL;
  if (!selector) selector = "";
  if (strncmp(selector, "nsec1", 5) == 0) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  /* The npub this selector names, when it names one (canonical encoding). */
  char *want = NULL;
  uint8_t pk[32];
  if (is_hex_64(selector)) {
    if (!nostr_hex2bin(pk, selector, sizeof pk)) return NOSTR_SIGNER_ERROR_INVALID_ARG;
    if (nostr_nip19_encode_npub(pk, &want) != 0 || !want) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  } else if (strncmp(selector, "npub1", 5) == 0 || strncmp(selector, "NPUB1", 5) == 0) {
    if (nostr_nip19_decode_npub(selector, pk) != 0) return NOSTR_SIGNER_ERROR_INVALID_ARG;
    if (nostr_nip19_encode_npub(pk, &want) != 0 || !want) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  }
  if (!want) {
    /* "" or a key_id / label: resolved as before. */
    char *np = NULL;
    int rc = nostr_nip55l_resolve_npub(selector, &np);
    if (rc != 0 || !np) { free(np); return rc ? rc : NOSTR_SIGNER_ERROR_NOT_FOUND; }
    *out_selector = strdup(selector);
    if (!*out_selector) { free(np); return NOSTR_SIGNER_ERROR_BACKEND; }
    *out_npub = np;
    return 0;
  }
  /* The active identity (whatever backend holds it: cache, env, keyring). */
  char *active = NULL;
  if (nostr_nip55l_resolve_npub("", &active) == 0 && active && strcmp(active, want) == 0) {
    free(active);
    *out_selector = strdup("");
    if (!*out_selector) { free(want); return NOSTR_SIGNER_ERROR_BACKEND; }
    *out_npub = want;
    return 0;
  }
  free(active);
  /* A stored identity whose key is exactly this npub (the lookup's
   * fallbacks may return another key, which must not be used). */
  char *np = NULL;
  int rc = nostr_nip55l_resolve_npub(want, &np);
  if (rc == 0 && np && strcmp(np, want) == 0) {
    free(np);
    *out_selector = strdup(want);
    if (!*out_selector) { free(want); return NOSTR_SIGNER_ERROR_BACKEND; }
    *out_npub = want;
    return 0;
  }
  free(np);
  free(want);
  /* No such identity (or the lookup's fallback found another key), or the
   * key store could not say: BACKEND, not NOT_FOUND (nostrc-f6l29). */
  return rc == 0 ? NOSTR_SIGNER_ERROR_NOT_FOUND : rc;
}

int nostr_nip55l_get_public_key(char **out_npub){
  if(!out_npub) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_npub=NULL;
  /* Fast path: return the cached active npub if StoreKey populated it.
   * Bypasses libsecret entirely and closes the smoke-test race that
   * saw GetPublicKey report NoKeyConfigured after a successful
   * StoreKey (nostrc-7g9d). */
  if (g_cached_active_npub && *g_cached_active_npub) {
    *out_npub = strdup(g_cached_active_npub);
    return *out_npub ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
  }
  return nostr_nip55l_resolve_npub(NULL, out_npub);
}

int nostr_nip55l_sign_event_full(const char *event_json,
                                 const char *current_user,
                                 const char *app_id,
                                 char **out_event_id,
                                 char **out_pubkey_hex,
                                 char **out_signature){
  (void)app_id;
  if(!event_json) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  if(out_event_id) *out_event_id = NULL;
  if(out_pubkey_hex) *out_pubkey_hex = NULL;
  if(out_signature) *out_signature = NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  NostrEvent *ev = nostr_event_new(); if(!ev){ secure_free(&sb); return NOSTR_SIGNER_ERROR_BACKEND; }
  if (nostr_event_deserialize(ev, event_json)!=0) { nostr_event_free(ev); secure_free(&sb); return NOSTR_SIGNER_ERROR_INVALID_JSON; }
  if (ev->created_at == 0) { ev->created_at = (int64_t)time(NULL); }
  if (nostr_event_sign_secure(ev, &sb)!=0) { nostr_event_free(ev); secure_free(&sb); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  secure_free(&sb);
  if (!ev->sig) { nostr_event_free(ev); return NOSTR_SIGNER_ERROR_BACKEND; }
  if(out_event_id && ev->id) *out_event_id = strdup(ev->id);
  if(out_pubkey_hex && ev->pubkey) *out_pubkey_hex = strdup(ev->pubkey);
  if(out_signature) *out_signature = strdup(ev->sig);
  nostr_event_free(ev);
  if(out_signature && !*out_signature) return NOSTR_SIGNER_ERROR_BACKEND;
  return 0;
}

int nostr_nip55l_sign_event(const char *event_json,
                            const char *current_user,
                            const char *app_id,
                            char **out_signature){
  return nostr_nip55l_sign_event_full(event_json, current_user, app_id,
                                       NULL, NULL, out_signature);
}

int nostr_nip55l_sign_event_json(const char *event_json,
                                 const char *current_user,
                                 const char *app_id,
                                 char **out_signed_event_json){
  (void)app_id;
  if(!event_json || !out_signed_event_json) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_signed_event_json = NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  NostrEvent *ev = nostr_event_new(); if(!ev){ secure_free(&sb); return NOSTR_SIGNER_ERROR_BACKEND; }
  if (nostr_event_deserialize(ev, event_json)!=0) { nostr_event_free(ev); secure_free(&sb); return NOSTR_SIGNER_ERROR_INVALID_JSON; }
  if (ev->created_at == 0) { ev->created_at = (int64_t)time(NULL); }
  if (nostr_event_sign_secure(ev, &sb)!=0) { nostr_event_free(ev); secure_free(&sb); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  secure_free(&sb);
  if (!ev->sig || !ev->id) { nostr_event_free(ev); return NOSTR_SIGNER_ERROR_BACKEND; }
  *out_signed_event_json = nostr_event_serialize(ev);
  nostr_event_free(ev);
  return *out_signed_event_json ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
}

int nostr_nip55l_sign_hash(const char *hash_hex,
                          const char *current_user,
                          char **out_signature){
  if(!hash_hex || !out_signature) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_signature = NULL;
  if (!is_hex_64(hash_hex)) return NOSTR_SIGNER_ERROR_INVALID_ARG;

  int rc; nostr_secure_buf sb = {0};
  rc = resolve_seckey_secure(current_user, &sb);
  if(rc!=0) return rc;

  /* Decode hash from hex */
  uint8_t hash[32];
  if (!nostr_hex2bin(hash, hash_hex, 32)) {
    secure_free(&sb);
    return NOSTR_SIGNER_ERROR_INVALID_ARG;
  }

  /* Set up secp256k1 context and keypair */
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  if (!ctx) { secure_free(&sb); return NOSTR_SIGNER_ERROR_BACKEND; }

  unsigned char seckey[32];
  memcpy(seckey, sb.ptr, 32);
  secure_free(&sb);

  int ret = NOSTR_SIGNER_ERROR_CRYPTO_FAILED;
  if (!secp256k1_ec_seckey_verify(ctx, seckey)) goto sign_hash_cleanup;

  secp256k1_keypair keypair;
  if (secp256k1_keypair_create(ctx, &keypair, seckey) != 1) goto sign_hash_cleanup;

  unsigned char auxiliary_rand[32];
  if (RAND_bytes(auxiliary_rand, sizeof(auxiliary_rand)) != 1) goto sign_hash_cleanup;

  unsigned char sig_bin[64];
  if (secp256k1_schnorrsig_sign32(ctx, sig_bin, hash, &keypair, auxiliary_rand) != 1) goto sign_hash_cleanup;

  *out_signature = bin_to_hex(sig_bin, 64);
  ret = *out_signature ? 0 : NOSTR_SIGNER_ERROR_BACKEND;

sign_hash_cleanup:
  { volatile unsigned char *p = seckey; for(size_t i=0;i<32;i++) p[i]=0; }
  secp256k1_context_destroy(ctx);
  return ret;
}

int nostr_nip55l_nip04_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64){
  if(!plaintext || !peer_pub_hex || !out_cipher_b64) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_cipher_b64=NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  char *err=NULL; char *ct=NULL;
  int enc_rc = nostr_nip04_encrypt_secure(plaintext, peer_pub_hex, &sb, &ct, &err);
  secure_free(&sb);
  if (enc_rc!=0) { if(err) free(err); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  *out_cipher_b64 = ct; return 0;
}

int nostr_nip55l_nip04_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext){
  if(!cipher_b64 || !peer_pub_hex || !out_plaintext) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_plaintext=NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  char *err=NULL; char *pt=NULL;
  int dec_rc = nostr_nip04_decrypt_secure(cipher_b64, peer_pub_hex, &sb, &pt, &err);
  secure_free(&sb);
  if (dec_rc!=0) { if(err) free(err); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  *out_plaintext = pt; return 0;
}

int nostr_nip55l_nip44_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64){
  if(!plaintext || !peer_pub_hex || !out_cipher_b64) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_cipher_b64=NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  uint8_t *sk = (uint8_t*)sb.ptr;
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  char *b64=NULL;
  if (nostr_nip44_encrypt_v2(sk, pkx, (const uint8_t*)plaintext, strlen(plaintext), &b64)!=0) { secure_free(&sb); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  secure_free(&sb);
  *out_cipher_b64 = b64; return 0;
}

int nostr_nip55l_nip44_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext){
  if(!cipher_b64 || !peer_pub_hex || !out_plaintext) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_plaintext=NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  uint8_t *sk = (uint8_t*)sb.ptr;
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t *pt=NULL; size_t ptlen=0;
  if (nostr_nip44_decrypt_v2(sk, pkx, cipher_b64, &pt, &ptlen)!=0) { secure_free(&sb); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  secure_free(&sb);
  char *out = (char*)malloc(ptlen+1); if(!out){ free(pt); return NOSTR_SIGNER_ERROR_BACKEND; }
  memcpy(out, pt, ptlen); out[ptlen]='\0'; free(pt);
  *out_plaintext = out; return 0;
}

/* Binary-safe NIP-44 (see signer_ops.h). Provided by nostr_nip44_core; the
 * decoder is strict and canonical, so a mangled parameter fails instead of
 * yielding a shorter plaintext. */
extern int nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);
extern int nip44_base64_decode(const char *b64, uint8_t **out_buf, size_t *out_len);

int nostr_nip55l_nip44_encrypt_b64(const char *plaintext_b64, const char *peer_pub_hex,
                                   const char *current_user, char **out_cipher_b64){
  if(!plaintext_b64 || !peer_pub_hex || !out_cipher_b64) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_cipher_b64=NULL;
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t *pt=NULL; size_t pt_len=0;
  if (nip44_base64_decode(plaintext_b64, &pt, &pt_len)!=0 || !pt) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  nostr_secure_buf sb = {0};
  int rc = resolve_seckey_secure(current_user, &sb);
  if (rc!=0) { memset(pt,0,pt_len); free(pt); return rc; }
  char *b64=NULL;
  int enc = nostr_nip44_encrypt_v2((uint8_t*)sb.ptr, pkx, pt, pt_len, &b64);
  secure_free(&sb);
  memset(pt,0,pt_len); free(pt);
  if (enc!=0 || !b64) { free(b64); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  *out_cipher_b64 = b64; return 0;
}

int nostr_nip55l_nip44_decrypt_b64(const char *cipher_b64, const char *peer_pub_hex,
                                   const char *current_user, char **out_plaintext_b64){
  if(!cipher_b64 || !peer_pub_hex || !out_plaintext_b64) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_plaintext_b64=NULL;
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  nostr_secure_buf sb = {0};
  int rc = resolve_seckey_secure(current_user, &sb);
  if (rc!=0) return rc;
  uint8_t *pt=NULL; size_t pt_len=0;
  int dec = nostr_nip44_decrypt_v2((uint8_t*)sb.ptr, pkx, cipher_b64, &pt, &pt_len);
  secure_free(&sb);
  if (dec!=0 || !pt) { free(pt); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  char *b64=NULL;
  int enc = nip44_base64_encode(pt, pt_len, &b64);
  memset(pt,0,pt_len); free(pt);
  if (enc!=0 || !b64) { free(b64); return NOSTR_SIGNER_ERROR_BACKEND; }
  *out_plaintext_b64 = b64; return 0;
}

/* NIP-44 v2 conversation key (nostr_nip44_convkey: HKDF-extract(SHA256,
 * ECDH shared-x, salt "nip44-v2")) between the selected identity and a peer,
 * as 64 lowercase hex. The secret key never leaves this process; the caller
 * receives only the per-peer conversation key. Anyone holding it can open
 * every NIP-44 payload exchanged with that peer, which is why the D-Bus
 * method that exposes this is approval-gated (signer_service_g.c). */
int nostr_nip55l_nip44_conversation_key(const char *peer_pub_hex,
                                        const char *current_user,
                                        char **out_convkey_hex){
  if (!peer_pub_hex || !out_convkey_hex) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_convkey_hex = NULL;
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32];
  if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  nostr_secure_buf sb = {0};
  int rc = resolve_seckey_secure(current_user, &sb);
  if (rc != 0) return rc;
  uint8_t ck[32];
  int drc = nostr_nip44_convkey((const uint8_t *)sb.ptr, pkx, ck);
  secure_free(&sb);
  /* convkey fails for a peer that is not a point on the curve. */
  if (drc != 0) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  char *hex = (char *)malloc(65);
  if (!hex) { secure_wipe(ck, sizeof ck); return NOSTR_SIGNER_ERROR_BACKEND; }
  static const char HX[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { hex[2*i] = HX[ck[i] >> 4]; hex[2*i+1] = HX[ck[i] & 0xf]; }
  hex[64] = '\0';
  secure_wipe(ck, sizeof ck);
  *out_convkey_hex = hex;
  return 0;
}

int nostr_nip55l_decrypt_zap_event(const char *event_json,
                                    const char *current_user, char **out_json){
  if(!out_json || !event_json) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_json=NULL;
  /* Strategy: parse event; find first 'p' tag as peer; attempt NIP-44 v2 decrypt of content,
   * then fallback to NIP-04. If decrypt ok, replace content and return serialized event. */
  int rc; char *sk_hex=NULL; rc = resolve_seckey_hex(current_user, &sk_hex); if (rc!=0) return rc;
  NostrEvent *ev = nostr_event_new(); if(!ev){ free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  if (nostr_event_deserialize(ev, event_json)!=0) { nostr_event_free(ev); free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_JSON; }
  const char *peer_pub_hex = NULL;
  NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
  if (tags){
    size_t n = nostr_tags_size(tags);
    for (size_t i=0; i<n; i++){
      NostrTag *t = nostr_tags_get(tags, i);
      if (!t) continue;
      const char *key = nostr_tag_get_key(t);
      if (key && strcmp(key, "p")==0) { peer_pub_hex = nostr_tag_get(t, 1); break; }
    }
  }
  if (!peer_pub_hex) { nostr_event_free(ev); free(sk_hex); return NOSTR_SIGNER_ERROR_NOT_FOUND; }
  const char *content = nostr_event_get_content(ev);
  if (!content) { nostr_event_free(ev); free(sk_hex); return NOSTR_SIGNER_ERROR_NOT_FOUND; }
  /* Try NIP-44 first */
  int dec_ok = 0; char *pt = NULL;
  do {
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr) break;
    if (!nostr_hex2bin((uint8_t*)sb.ptr, sk_hex, 32)) { secure_free(&sb); break; }
    if (!is_hex_64(peer_pub_hex)) break;
    uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) break;
    uint8_t *ptbuf=NULL; size_t ptlen=0;
    if (nostr_nip44_decrypt_v2((uint8_t*)sb.ptr, pkx, content, &ptbuf, &ptlen)==0) {
      pt = (char*)malloc(ptlen+1); if(pt){ memcpy(pt, ptbuf, ptlen); pt[ptlen]='\0'; dec_ok=1; }
      free(ptbuf);
    }
    secure_free(&sb);
  } while(0);
  if (!dec_ok) {
    /* Fallback NIP-04 */
    char *err=NULL; char *out=NULL;
    if (nostr_nip04_decrypt(content, peer_pub_hex, sk_hex, &out, &err)==0 && out) {
      pt = out; dec_ok = 1; 
    }
    if (err) free(err);
  }
  free(sk_hex);
  if (!dec_ok || !pt) { nostr_event_free(ev); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  /* Replace content and serialize */
  nostr_event_set_content(ev, pt);
  free(pt);
  char *js = nostr_event_serialize(ev);
  if (!js) { nostr_event_free(ev); return NOSTR_SIGNER_ERROR_BACKEND; }
  nostr_event_free(ev);
  *out_json = js; return 0;
}

/* ---- GetRelays: explicit per-user relay configuration ----
 *
 * The signer answers GetRelays only from configuration the user (or an
 * enrollment tool) wrote down; it never fetches NIP-65 lists from the
 * network. The document is a JSON array of ws:// or wss:// URL strings.
 * Relay URLs never need JSON escapes, so the parser refuses them rather than
 * decoding them: anything that is not a plain printable URL is a
 * configuration error, reported as such instead of being silently dropped. */

#define NIP55L_RELAYS_MAX_DOC   (64u * 1024u)
#define NIP55L_RELAYS_MAX_COUNT 64u
#define NIP55L_RELAY_URL_MAX    512u

typedef struct {
  char *v[NIP55L_RELAYS_MAX_COUNT];
  size_t n;
} relay_set;

static void relay_set_clear(relay_set *rs){
  for (size_t i = 0; i < rs->n; i++) free(rs->v[i]);
  rs->n = 0;
}

static int ascii_prefix_ci(const char *s, size_t len, const char *prefix){
  size_t pl = strlen(prefix);
  if (len < pl) return 0;
  for (size_t i = 0; i < pl; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != prefix[i]) return 0;
  }
  return 1;
}

/* Lowercases scheme and host, drops a bare trailing "/" path. NULL if the
 * URL is not an acceptable relay URL. */
static char *relay_url_normalize(const char *s, size_t len){
  if (!s || len == 0 || len > NIP55L_RELAY_URL_MAX) return NULL;
  size_t scheme_len;
  if (ascii_prefix_ci(s, len, "wss://")) scheme_len = 6;
  else if (ascii_prefix_ci(s, len, "ws://")) scheme_len = 5;
  else return NULL;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '<' || c == '>') return NULL;
  }
  size_t host_end = scheme_len;
  while (host_end < len && s[host_end] != '/' && s[host_end] != '?' && s[host_end] != '#') host_end++;
  if (host_end == scheme_len) return NULL;
  char *out = (char*)malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, s, len);
  out[len] = '\0';
  for (size_t i = 0; i < host_end; i++) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
  }
  if (host_end == len - 1 && out[host_end] == '/') out[host_end] = '\0';
  return out;
}

/* Takes ownership of url. 0 on success (duplicates are dropped), -1 when the
 * set is full. */
static int relay_set_add(relay_set *rs, char *url){
  for (size_t i = 0; i < rs->n; i++) {
    if (strcmp(rs->v[i], url) == 0) { free(url); return 0; }
  }
  if (rs->n >= NIP55L_RELAYS_MAX_COUNT) { free(url); return -1; }
  rs->v[rs->n++] = url;
  return 0;
}

static int relay_set_to_json(const relay_set *rs, char **out_json){
  size_t cap = 3;
  for (size_t i = 0; i < rs->n; i++) cap += strlen(rs->v[i]) + 3;
  char *js = (char*)malloc(cap);
  if (!js) return NOSTR_SIGNER_ERROR_BACKEND;
  size_t off = 0;
  js[off++] = '[';
  for (size_t i = 0; i < rs->n; i++) {
    if (i) js[off++] = ',';
    js[off++] = '"';
    size_t l = strlen(rs->v[i]);
    memcpy(js + off, rs->v[i], l);
    off += l;
    js[off++] = '"';
  }
  js[off++] = ']';
  js[off] = '\0';
  *out_json = js;
  return 0;
}

static size_t json_skip_ws(const char *s, size_t len, size_t i){
  while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  return i;
}

static int relays_parse_doc(const char *doc, size_t len, relay_set *rs){
  if (memchr(doc, '\0', len)) return -1;
  size_t i = json_skip_ws(doc, len, 0);
  if (i >= len || doc[i] != '[') return -1;
  i = json_skip_ws(doc, len, i + 1);
  if (i < len && doc[i] == ']') {
    i = json_skip_ws(doc, len, i + 1);
    return i == len ? 0 : -1;
  }
  for (;;) {
    if (i >= len || doc[i] != '"') return -1;
    size_t start = ++i;
    while (i < len && doc[i] != '"') {
      unsigned char c = (unsigned char)doc[i];
      if (c == '\\' || c < 0x20) return -1;
      i++;
    }
    if (i >= len) return -1;
    char *url = relay_url_normalize(doc + start, i - start);
    if (!url) return -1;
    if (relay_set_add(rs, url) != 0) return -1;
    i = json_skip_ws(doc, len, i + 1);
    if (i < len && doc[i] == ',') { i = json_skip_ws(doc, len, i + 1); continue; }
    if (i < len && doc[i] == ']') { i = json_skip_ws(doc, len, i + 1); break; }
    return -1;
  }
  return i == len ? 0 : -1;
}

int nostr_nip55l_relays_normalize_json(const char *doc, size_t len, char **out_relays_json){
  if (!doc || !out_relays_json) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_relays_json = NULL;
  if (len > NIP55L_RELAYS_MAX_DOC) return NOSTR_SIGNER_ERROR_INVALID_JSON;
  relay_set rs = {0};
  if (relays_parse_doc(doc, len, &rs) != 0) { relay_set_clear(&rs); return NOSTR_SIGNER_ERROR_INVALID_JSON; }
  if (rs.n == 0) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  int rc = relay_set_to_json(&rs, out_relays_json);
  relay_set_clear(&rs);
  return rc;
}

int nostr_nip55l_relays_from_list(const char *const *urls, size_t n, char **out_relays_json){
  if (!out_relays_json || (n && !urls)) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_relays_json = NULL;
  relay_set rs = {0};
  for (size_t i = 0; i < n; i++) {
    char *url = urls[i] ? relay_url_normalize(urls[i], strlen(urls[i])) : NULL;
    if (!url || relay_set_add(&rs, url) != 0) { relay_set_clear(&rs); return NOSTR_SIGNER_ERROR_INVALID_ARG; }
  }
  if (rs.n == 0) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  int rc = relay_set_to_json(&rs, out_relays_json);
  relay_set_clear(&rs);
  return rc;
}

/* $XDG_CONFIG_HOME/nostr/relays.conf, falling back to ~/.config per the XDG
 * base-directory spec (a relative XDG_CONFIG_HOME is ignored). */
static char *relays_conf_path(void){
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  const char *base = NULL, *mid = "";
  if (xdg && xdg[0] == '/') base = xdg;
  else if (home && *home) { base = home; mid = "/.config"; }
  else return NULL;
  const char *tail = "/nostr/relays.conf";
  size_t n = strlen(base) + strlen(mid) + strlen(tail) + 1;
  char *p = (char*)malloc(n);
  if (!p) return NULL;
  snprintf(p, n, "%s%s%s", base, mid, tail);
  return p;
}

int nostr_nip55l_get_relays(char **out_relays_json){
  if(!out_relays_json) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_relays_json=NULL;
  char *path = relays_conf_path();
  if (!path) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  FILE *fp = fopen(path, "rb");
  free(path);
  if (!fp) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  char *doc = (char*)malloc(NIP55L_RELAYS_MAX_DOC + 1);
  if (!doc) { fclose(fp); return NOSTR_SIGNER_ERROR_BACKEND; }
  size_t len = fread(doc, 1, NIP55L_RELAYS_MAX_DOC + 1, fp);
  int read_err = ferror(fp);
  fclose(fp);
  int rc;
  if (read_err) rc = NOSTR_SIGNER_ERROR_BACKEND;
  else if (len > NIP55L_RELAYS_MAX_DOC) rc = NOSTR_SIGNER_ERROR_INVALID_JSON;
  else rc = nostr_nip55l_relays_normalize_json(doc, len, out_relays_json);
  free(doc);
  return rc;
}

int nostr_nip55l_store_key(const char *key, const char *identity){
  if (!key) return NOSTR_SIGNER_ERROR_INVALID_ARG;
#ifdef NIP55L_HAVE_LIBSECRET
  /* Interpret 'identity' as selector. Write both key_id and npub attributes to the same value. */
  const char *sel_in = (identity && *identity) ? identity : NULL;
  /* Normalize key to hex and derive npub for attributes */
  char *sk_hex = NULL;
  if (is_hex_64(key)) sk_hex = strdup(key);
  else if (strncmp(key, "nsec1", 5)==0) {
    uint8_t sk[32]; if (nostr_nip19_decode_nsec(key, sk)!=0) return NOSTR_SIGNER_ERROR_INVALID_KEY; sk_hex = bin_to_hex(sk, 32);
    secure_wipe(sk, sizeof sk);
  } else {
    return NOSTR_SIGNER_ERROR_INVALID_KEY;
  }
  if (!sk_hex) return NOSTR_SIGNER_ERROR_BACKEND;
  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) { if (sk_hex) { memset(sk_hex,0,strlen(sk_hex)); } free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  uint8_t pk[32]; if (!nostr_hex2bin(pk, pk_hex, sizeof pk)) { free(pk_hex); free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  char *npub = NULL; if (nostr_nip19_encode_npub(pk, &npub)!=0 || !npub) { free(pk_hex); if (sk_hex) { memset(sk_hex,0,strlen(sk_hex)); } free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  free(pk_hex);
  /* Choose key_id: prefer provided identity, else derived npub */
  const char *key_id_attr = sel_in ? sel_in : npub;
  GError *gerr = NULL;
  gchar uid_buf[32]; g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
  const GnostrSecretIdentity ident = {
    .key_id = key_id_attr,
    .npub = npub,
    /* A caller-chosen selector that is not the npub doubles as the label. */
    .label = (sel_in && strcmp(sel_in, npub) != 0) ? sel_in : NULL,
    .owner_uid = uid_buf,
    .origin = GNOSTR_SECRET_ORIGIN_SOFTWARE,
  };
  gboolean ok = gnostr_secret_store_save(&ident, sk_hex, &gerr);
  if (gerr) {
    g_warning("nip55l: StoreKey: secret service store failed: %s", gerr->message);
    g_error_free(gerr);
  }
  /* Publish the just-stored key to the in-process cache so the next
   * GetPublicKey / SignEvent never has to round-trip libsecret. This is the
   * key half of nostrc-7g9d — without it, resolve_seckey_hex(NULL, ...)
   * would fall back to a libsecret search that intermittently returned
   * NOT_FOUND on the aarch64 lab. */
  if (ok) signer_cache_set(sk_hex, npub);
  if (sk_hex) { memset(sk_hex, 0, strlen(sk_hex)); }
  free(sk_hex);
  free(npub);
  return ok ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#elif defined(NIP55L_HAVE_KEYCHAIN)
  /* macOS Keychain fallback */
  const char *sel_in = (identity && *identity) ? identity : NULL;
  /* Normalize key to hex and derive npub for attributes */
  char *sk_hex = NULL;
  if (is_hex_64(key)) sk_hex = strdup(key);
  else if (strncmp(key, "nsec1", 5)==0) {
    uint8_t skb[32]; if (nostr_nip19_decode_nsec(key, skb)!=0) return NOSTR_SIGNER_ERROR_INVALID_KEY; sk_hex = bin_to_hex(skb, 32);
    secure_wipe(skb, sizeof skb);
  } else {
    return NOSTR_SIGNER_ERROR_INVALID_KEY;
  }
  if (!sk_hex) return NOSTR_SIGNER_ERROR_BACKEND;
  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) { free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  uint8_t pk[32]; if (!nostr_hex2bin(pk, pk_hex, sizeof pk)) { free(pk_hex); free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  char *npub = NULL; if (nostr_nip19_encode_npub(pk, &npub)!=0 || !npub) { free(pk_hex); free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  free(pk_hex);
  const char *key_id_attr = sel_in ? sel_in : npub;

  /* Prepare secret bytes */
  uint8_t skb[32];
  if (!nostr_hex2bin(skb, sk_hex, sizeof skb)) { free(sk_hex); free(npub); return NOSTR_SIGNER_ERROR_INVALID_KEY; }

  /* Keychain write using SecItem APIs */
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!query) { free(sk_hex); free(npub); return NOSTR_SIGNER_ERROR_BACKEND; }
  CFStringRef service = CFStringCreateWithCString(NULL, KC_SIGNER_SERVICE, kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, key_id_attr, kCFStringEncodingUTF8);
  CFStringRef label = CFStringCreateWithCString(NULL, "Gnostr Identity", kCFStringEncodingUTF8);
  CFStringRef comment = CFStringCreateWithCString(NULL, npub, kCFStringEncodingUTF8);
  CFDataRef secretData = CFDataCreate(NULL, skb, (CFIndex)sizeof(skb));
  secure_wipe(skb, sizeof skb);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecAttrLabel, label);
  if (comment) CFDictionarySetValue(query, kSecAttrComment, comment);
  /* Replace existing if present */
  SecItemDelete(query);
  CFDictionarySetValue(query, kSecValueData, secretData);
  CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
  OSStatus st = SecItemAdd(query, NULL);
  if (service) CFRelease(service);
  if (account) CFRelease(account);
  if (label) CFRelease(label);
  if (comment) CFRelease(comment);
  /* Wipe key material from the CF buffer before release. */
  if (secretData) {
    const UInt8 *sd_bytes = CFDataGetBytePtr(secretData);
    CFIndex sd_len = CFDataGetLength(secretData);
    if (sd_bytes && sd_len > 0) memset((void *)sd_bytes, 0, (size_t)sd_len);
    CFRelease(secretData);
  }
  if (query) CFRelease(query);
  /* Populate in-process cache; see the libsecret branch for rationale. */
  if (st == errSecSuccess) signer_cache_set(sk_hex, npub);
  free(sk_hex);
  free(npub);
  return (st == errSecSuccess) ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)identity; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

int nostr_nip55l_clear_key(const char *identity){
  /* Best-effort: even if libsecret has nothing to remove, drop the
   * in-process cache so the daemon stops answering with the just-cleared
   * key. Cache eviction is coupled to the caller's intent (“no more of
   * this account”) rather than to the storage backend's success. */
  if (!identity || !*identity ||
      (g_cached_active_npub && strcmp(identity, g_cached_active_npub) == 0))
    signer_cache_clear();
#ifdef NIP55L_HAVE_LIBSECRET
  const char *sel = (identity && *identity) ? identity : "";
  GError *gerr = NULL;
  gboolean ok1 = secret_password_clear_sync(&gnostr_secret_schema, NULL, &gerr,
                                            "key_id", sel, NULL);
  if (gerr) { g_error_free(gerr); gerr = NULL; }
  gboolean ok2 = secret_password_clear_sync(&gnostr_secret_schema, NULL, &gerr,
                                            "npub", sel, NULL);
  if (gerr) { g_error_free(gerr); }
  return (ok1 || ok2) ? 0 : NOSTR_SIGNER_ERROR_NOT_FOUND;
#elif defined(NIP55L_HAVE_KEYCHAIN)
  /* Delete by selector if provided; otherwise delete all for service (best-effort single delete) */
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  CFStringRef service = CFStringCreateWithCString(NULL, KC_SIGNER_SERVICE, kCFStringEncodingUTF8);
  if (service) CFDictionarySetValue(q, kSecAttrService, service);
  if (identity && *identity) {
    CFStringRef account = CFStringCreateWithCString(NULL, identity, kCFStringEncodingUTF8);
    if (account) CFDictionarySetValue(q, kSecAttrAccount, account);
    OSStatus st = SecItemDelete(q);
    if (service) CFRelease(service);
    if (account) CFRelease(account);
    CFRelease(q);
    return (st == errSecSuccess) ? 0 : NOSTR_SIGNER_ERROR_NOT_FOUND;
  } else {
    /* Without selector: attempt one delete by service */
    OSStatus st = SecItemDelete(q);
    if (service) CFRelease(service);
    CFRelease(q);
    return (st == errSecSuccess) ? 0 : NOSTR_SIGNER_ERROR_NOT_FOUND;
  }
#else
  (void)identity; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

/* ---------------------------------------------------------------------------
 * List identities.
 * ------------------------------------------------------------------------- */

/* npub of the key in the daemon's environment (NOSTR_SIGNER_SECKEY_HEX, else
 * NOSTR_SIGNER_NSEC), or NULL. resolve_seckey_hex() makes it the active
 * identity and its npub/hex selects it, so it is an identity the signer
 * holds (nostrc-sic82). */
static char *env_identity_npub(void){
  char *sk_hex = NULL;
  if (env_seckey_hex(&sk_hex) != 0 || !sk_hex) return NULL;
  char *npub = NULL;
  if (sk_hex_to_npub(sk_hex, &npub) != 0) npub = NULL;
  secure_wipe(sk_hex, strlen(sk_hex));
  free(sk_hex);
  return npub;
}

/* The npubs in the key store. 0 on success (possibly none);
 * NOSTR_SIGNER_ERROR_NOT_FOUND when there is no key store (none compiled in,
 * or no Secret Service on the bus); NOSTR_SIGNER_ERROR_BACKEND when the store
 * could not be read. */
static int list_stored_identities(char ***out_npubs, int *out_count) {
#ifdef NIP55L_HAVE_LIBSECRET
  GError *gerr = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &gerr);
  if (!service) {
    int rc = store_error_rc("unavailable", gerr);
    g_clear_error(&gerr);
    return rc;
  }

  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                            SECRET_SEARCH_ALL, NULL, &gerr);
  g_hash_table_unref(attrs);
  if (gerr) {
    int rc = store_error_rc("search failed", gerr);
    g_clear_error(&gerr);
    g_list_free_full(items, g_object_unref);
    g_object_unref(service);
    return rc;
  }

  int n = g_list_length(items);
  char **npubs = calloc(n + 1, sizeof(char*));
  if (!npubs) { g_list_free_full(items, g_object_unref); g_object_unref(service); return NOSTR_SIGNER_ERROR_BACKEND; }

  int idx = 0;
  for (GList *it = items; it; it = it->next) {
    SecretItem *item = SECRET_ITEM(it->data);
    GHashTable *ia = secret_item_get_attributes(item);
    const char *np = g_hash_table_lookup(ia, "npub");
    if (np && *np) npubs[idx++] = strdup(np);
    g_hash_table_unref(ia);
  }
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  *out_npubs = npubs;
  *out_count = idx;
  return 0;

#elif defined(NIP55L_HAVE_KEYCHAIN)
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
  CFStringRef service = CFStringCreateWithCString(NULL, KC_SIGNER_SERVICE, kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  if (service) CFDictionarySetValue(q, kSecAttrService, service);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(q, &result);
  if (service) CFRelease(service);
  CFRelease(q);
  if (st != errSecSuccess || !result) {
    if (result) CFRelease(result);
    /* No items is not an error; any other status (locked, denied) is:
     * an empty list would claim the signer holds nothing (nostrc-sjyl3). */
    if (st != errSecItemNotFound && st != errSecSuccess) return NOSTR_SIGNER_ERROR_BACKEND;
    *out_npubs = calloc(1, sizeof(char*));
    if (!*out_npubs) return NOSTR_SIGNER_ERROR_BACKEND;
    *out_count = 0;
    return 0;
  }
  CFArrayRef items = (CFArrayRef)result;
  CFIndex count = CFArrayGetCount(items);
  char **npubs = calloc((size_t)count + 1, sizeof(char*));
  if (!npubs) { CFRelease(result); return NOSTR_SIGNER_ERROR_BACKEND; }
  int idx = 0;
  for (CFIndex i = 0; i < count; i++) {
    CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);
    /* The comment field stores the npub; account field stores key_id/npub */
    CFStringRef comment = CFDictionaryGetValue(item, kSecAttrComment);
    CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);
    CFStringRef src = comment ? comment : account;
    if (!src) continue;
    CFIndex len = CFStringGetLength(src);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    char *buf = malloc((size_t)maxSize);
    if (buf && CFStringGetCString(src, buf, maxSize, kCFStringEncodingUTF8)) {
      /* Only include bech32 npub strings */
      if (strncmp(buf, "npub1", 5) == 0) {
        npubs[idx++] = buf;
      } else {
        free(buf);
      }
    } else {
      free(buf);
    }
  }
  CFRelease(result);
  *out_npubs = npubs;
  *out_count = idx;
  return 0;

#else
  (void)out_npubs; (void)out_count;
  return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

int nostr_nip55l_list_identities(char ***out_npubs, int *out_count) {
  if (!out_npubs || !out_count) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  *out_npubs = NULL;
  *out_count = 0;

  char *env_npub = env_identity_npub();
  char **stored = NULL;
  int n_stored = 0;
  int rc = list_stored_identities(&stored, &n_stored);
  if (rc != 0) {
    /* A store that fails is reported even beside an environment key: a
     * short list would deny stored identities that do exist. With no store
     * at all, the environment key is the signer's only identity. */
    if (rc != NOSTR_SIGNER_ERROR_NOT_FOUND || !env_npub) { free(env_npub); return rc; }
    stored = NULL;
    n_stored = 0;
  }

  char **npubs = calloc((size_t)n_stored + 2, sizeof(char*));
  if (!npubs) {
    for (int i = 0; i < n_stored; i++) free(stored[i]);
    free(stored);
    free(env_npub);
    return NOSTR_SIGNER_ERROR_BACKEND;
  }
  /* Each npub once: the environment key may also be stored, and one key
   * may have several items (a software copy beside a hardware enrolment). */
  int n = 0;
  if (env_npub) npubs[n++] = env_npub;
  for (int i = 0; i < n_stored; i++) {
    int dup = 0;
    for (int j = 0; j < n && !dup; j++) dup = strcmp(stored[i], npubs[j]) == 0;
    if (dup) { free(stored[i]); continue; }
    npubs[n++] = stored[i];
  }
  free(stored);
  *out_npubs = npubs;
  *out_count = n;
  return 0;
}

#ifdef NIP55L_HAVE_LIBSECRET
static SecretItem *find_identity_item(const char *selector){
  if (!selector || !*selector) return NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
  if (!service) return NULL;
  GError *gerr = NULL;
  /* Try key_id */
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"key_id", (gpointer)selector);
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK, NULL, &gerr);
  g_hash_table_unref(attrs);
  if (gerr) { g_error_free(gerr); gerr = NULL; }
  SecretItem *ret = NULL;
  if (items) {
    ret = SECRET_ITEM(g_object_ref(items->data));
    g_list_free_full(items, g_object_unref);
    g_object_unref(service);
    return ret;
  }
  /* Try npub */
  attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"npub", (gpointer)selector);
  items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                     SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK, NULL, &gerr);
  g_hash_table_unref(attrs);
  if (gerr) { g_error_free(gerr); gerr = NULL; }
  if (items) {
    ret = SECRET_ITEM(g_object_ref(items->data));
    g_list_free_full(items, g_object_unref);
  }
  g_object_unref(service);
  return ret;
}
#endif

int nostr_nip55l_get_owner(const char *selector, int *has_owner, uid_t *uid_out, char **username_out){
#ifdef NIP55L_HAVE_LIBSECRET
  if (has_owner) *has_owner = 0; if (uid_out) *uid_out = 0; if (username_out) *username_out = NULL;
  SecretItem *item = find_identity_item(selector);
  if (!item) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  GHashTable *a = secret_item_get_attributes(item);
  const char *uid_s = a ? g_hash_table_lookup(a, "owner_uid") : NULL;
  const char *user_s = a ? g_hash_table_lookup(a, "owner_username") : NULL;
  if (uid_s && *uid_s) {
    if (has_owner) *has_owner = 1;
    if (uid_out) {
      unsigned long v = strtoul(uid_s, NULL, 10);
      *uid_out = (uid_t)v;
    }
    if (username_out && user_s) *username_out = g_strdup(user_s);
  }
  if (a) g_hash_table_unref(a);
  g_object_unref(item);
  return 0;
#else
  (void)selector; (void)has_owner; (void)uid_out; (void)username_out; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

int nostr_nip55l_set_owner(const char *selector, uid_t uid, const char *username){
#ifdef NIP55L_HAVE_LIBSECRET
  SecretItem *item = find_identity_item(selector);
  if (!item) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  GHashTable *a = secret_item_get_attributes(item);
  if (!a) a = g_hash_table_new(g_str_hash, g_str_equal);
  gchar uid_buf[32]; g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)uid);
  g_hash_table_replace(a, g_strdup("owner_uid"), g_strdup(uid_buf));
  if (username && *username)
    g_hash_table_replace(a, g_strdup("owner_username"), g_strdup(username));
  else
    g_hash_table_remove(a, "owner_username");
  GError *gerr = NULL;
  gboolean ok = secret_item_set_attributes_sync(item, &gnostr_secret_schema, a, NULL, &gerr);
  if (a) g_hash_table_unref(a);
  if (gerr) { g_error_free(gerr); }
  g_object_unref(item);
  return ok ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)selector; (void)uid; (void)username; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

int nostr_nip55l_clear_owner(const char *selector){
#ifdef NIP55L_HAVE_LIBSECRET
  SecretItem *item = find_identity_item(selector);
  if (!item) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  GHashTable *a = secret_item_get_attributes(item);
  if (!a) a = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_remove(a, "owner_uid");
  g_hash_table_remove(a, "owner_username");
  GError *gerr = NULL;
  gboolean ok = secret_item_set_attributes_sync(item, &gnostr_secret_schema, a, NULL, &gerr);
  if (a) g_hash_table_unref(a);
  if (gerr) { g_error_free(gerr); }
  g_object_unref(item);
  return ok ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)selector; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

/* ---------------------------------------------------------------------------
 * One-shot keyring migration (nostrc-bml6)
 *
 * Re-stores key material found under the legacy schemas
 * (org.gnostr.Signer/key from grotto's removed secret-storage.c, the
 * Seahorse helper's former org.gnostr.Key, and — nostrc-e5nz — the gnostr
 * client's retired org.gnostr.NostrKey keystore) under the unified
 * org.gnostr.Signer/identity schema, then deletes each original.
 *
 * Idempotent: re-storing an identical item replaces it in place, and an
 * original is only deleted after its replacement was stored. A marker item
 * (org.gnostr.Signer/migration name=GNOSTR_SECRET_MIGRATION_MARKER) is
 * written once a pass leaves nothing retryable behind, and short-circuits
 * every later start. The marker name is versioned: adding a legacy schema
 * bumps it, so keyrings that finished an earlier pass are scanned again.
 * When no legacy item exists the marker is not written: that would force an
 * unlock prompt at daemon start just to record that there was nothing to do,
 * and the attribute-only searches it would save cost no prompt.
 * ------------------------------------------------------------------------- */

#ifdef NIP55L_HAVE_LIBSECRET
#define NIP55L_MIGRATION_MARKER GNOSTR_SECRET_MIGRATION_MARKER

typedef enum { MIG_OK, MIG_SKIP, MIG_RETRY } mig_result;

static gboolean migration_marker_present(SecretService *service){
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"name", (gpointer)NIP55L_MIGRATION_MARKER);
  GList *items = secret_service_search_sync(service, &gnostr_secret_migration_schema,
                                            attrs, SECRET_SEARCH_NONE, NULL, NULL);
  g_hash_table_unref(attrs);
  gboolean present = items != NULL;
  g_list_free_full(items, g_object_unref);
  return present;
}

static GList *search_legacy(SecretService *service, const SecretSchema *schema,
                            SecretSearchFlags flags, GError **error){
  /* Empty attribute set: libsecret matches on xdg:schema = schema->name. */
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  GList *items = secret_service_search_sync(service, schema, attrs,
                                            flags | SECRET_SEARCH_ALL, NULL, error);
  g_hash_table_unref(attrs);
  return items;
}

static char *npub_from_sk_hex(const char *sk_hex){
  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) return NULL;
  uint8_t pk[32];
  char *npub = NULL;
  if (nostr_hex2bin(pk, pk_hex, sizeof pk) && nostr_nip19_encode_npub(pk, &npub) != 0) npub = NULL;
  free(pk_hex);
  return npub;
}

/* TRUE when a unified item for npub holds a working copy of that key: its
 * secret decodes to a private key whose npub is npub. Attributes alone are
 * not evidence — a half-written item must not justify deleting the only
 * good copy. Hardware references carry no private key and never match.
 * @service already has its session open (signer_secret_service()). */
static gboolean unified_holds_same_key(SecretService *service, const char *npub){
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"npub", (gpointer)npub);
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK |
                                            SECRET_SEARCH_LOAD_SECRETS, NULL, NULL);
  g_hash_table_unref(attrs);
  gboolean same = FALSE;
  for (GList *l = items; l && !same; l = l->next) {
    SecretValue *sv = secret_item_get_secret(l->data);
    char *hex = sv ? secret_text_to_sk_hex(secret_value_get_text(sv)) : NULL;
    char *derived = hex ? npub_from_sk_hex(hex) : NULL;
    same = derived && strcmp(derived, npub) == 0;
    free(derived);
    if (hex) { secure_wipe(hex, strlen(hex)); free(hex); }
    if (sv) secret_value_unref(sv);
  }
  g_list_free_full(items, g_object_unref);
  return same;
}

static mig_result migrate_one(SecretService *service, SecretItem *item,
                              GnostrSecretLegacyKind kind,
                              const char *schema_name, const char *uid_buf){
  mig_result res = MIG_RETRY;
  GHashTable *legacy = secret_item_get_attributes(item);
  GnostrSecretIdentity id;
  const gchar *why = NULL;
  SecretValue *sv = NULL;
  char *sk_hex = NULL, *npub = NULL;
  GError *err = NULL;

  if (!gnostr_secret_legacy_to_identity(kind, legacy, &id, &why)) {
    g_message("nip55l: keyring-migration: leaving %s item in place: %s", schema_name, why);
    res = MIG_SKIP;
    goto out;
  }
  sv = secret_item_get_secret(item);
  if (!sv) {
    /* Still locked (unlock prompt dismissed) or secret failed to load. */
    g_message("nip55l: keyring-migration: %s item secret unavailable (locked?); will retry",
              schema_name);
    goto out;
  }
  sk_hex = secret_text_to_sk_hex(secret_value_get_text(sv));
  npub = sk_hex ? npub_from_sk_hex(sk_hex) : NULL;
  if (!npub) {
    g_message("nip55l: keyring-migration: leaving %s item in place: secret is not a secp256k1 private key",
              schema_name);
    res = MIG_SKIP;
    goto out;
  }
  if (id.npub && *id.npub && strcmp(id.npub, npub) != 0)
    g_message("nip55l: keyring-migration: %s item npub attribute %s does not match its key; using %s",
              schema_name, id.npub, npub);
  /* Same selector convention as StoreKey: a friendly name that is not the
   * npub becomes key_id, so the legacy label keeps selecting this key; the
   * npub still resolves through the daemon's npub fallback lookup. The
   * client keystore's fixed import label is not a selector. */
  id.key_id = (gnostr_secret_legacy_label_is_selector(kind) &&
               id.label && *id.label && strcmp(id.label, npub) != 0) ? id.label : npub;
  id.npub = npub;
  id.owner_uid = uid_buf;

  /* A key the client also held may already have been imported into the
   * signer by hand. Re-storing it under the import label would replace that
   * item (same {key_id, npub}, different attribute set) and lose the user's
   * label; when the signer's item provably holds the same key, only the
   * client copy goes. Otherwise store normally (an origin=hardware item for
   * the npub is never pruned by the save). */
  if (kind == GNOSTR_SECRET_LEGACY_CLIENT_KEY && unified_holds_same_key(service, npub)) {
    g_message("nip55l: keyring-migration: %s already held by the signer; retiring the %s copy",
              npub, schema_name);
  } else if (!gnostr_secret_store_save(&id, sk_hex, &err)) {
    g_message("nip55l: keyring-migration: re-store of %s failed: %s; will retry",
              npub, err ? err->message : "unknown");
    g_clear_error(&err);
    goto out;
  }
  if (!secret_item_delete_sync(item, NULL, &err)) {
    /* The copy exists; the next pass re-stores it in place and retries. */
    g_message("nip55l: keyring-migration: %s migrated but legacy item not deleted: %s; will retry",
              npub, err ? err->message : "unknown");
    g_clear_error(&err);
    goto out;
  }
  res = MIG_OK;

out:
  if (sk_hex) { secure_wipe(sk_hex, strlen(sk_hex)); free(sk_hex); }
  free(npub);
  if (sv) secret_value_unref(sv);
  if (legacy) g_hash_table_unref(legacy);
  return res;
}
#endif /* NIP55L_HAVE_LIBSECRET */

#ifdef NIP55L_HAVE_KEYCHAIN
/* ---- macOS Keychain: import pre-e5nz gnostr client keys (nostrc-de9h) ----
 *
 * Before nostrc-e5nz GNostr stored each nsec itself as a generic-password
 * item: service "org.gnostr.Client", account = npub, data = the nsec text.
 * Mirrors the libsecret migrate_one(): the key is re-stored in the daemon's
 * own format (service "Gnostr Identity Key", account = key_id = npub,
 * comment = npub, data = 32 raw bytes) and the legacy item is deleted only
 * once the daemon's item provably holds the same key - never on attribute
 * evidence alone. A marker item ends the one-shot pass. The attribute-only
 * probe never prompts; reading a legacy secret may raise the Keychain's
 * access prompt (the item belongs to GNostr), and a dismissed prompt only
 * postpones that item to the next start. */
#define KC_LEGACY_CLIENT_SERVICE "org.gnostr.Client"
#define KC_MARKER_SERVICE        "Gnostr Signer Migration"
#define KC_MARKER_ACCOUNT        "legacy-client-keys-v1"

#ifdef NIP55L_KEYCHAIN_TEST_HOOKS
/* Test executables only: pin every query and write to one keychain file so a
 * test never reads or writes the user's login keychain. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static SecKeychainRef kc_test_keychain = NULL;
void nostr_nip55l_test_use_keychain(SecKeychainRef kc);
void nostr_nip55l_test_use_keychain(SecKeychainRef kc){ kc_test_keychain = kc; }
static void kc_scope(CFMutableDictionaryRef q, int for_add){
  if (!kc_test_keychain) abort(); /* refuse to touch the login keychain */
  if (for_add) {
    CFDictionarySetValue(q, kSecUseKeychain, kc_test_keychain);
  } else {
    const void *kcs[1] = { kc_test_keychain };
    CFArrayRef list = CFArrayCreate(NULL, kcs, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(q, kSecMatchSearchList, list);
    CFRelease(list);
  }
}
#pragma clang diagnostic pop
#else
static void kc_scope(CFMutableDictionaryRef q, int for_add){ (void)q; (void)for_add; }
#endif

static CFMutableDictionaryRef kc_query(const char *service, CFStringRef attr, const char *value){
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!q) return NULL;
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  CFStringRef svc = CFStringCreateWithCString(NULL, service, kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecAttrService, svc);
  CFRelease(svc);
  if (attr && value) {
    CFStringRef v = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, attr, v);
    CFRelease(v);
  }
  return q;
}

static char *kc_cfstring_dup(CFTypeRef v){
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return NULL;
  CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength((CFStringRef)v), kCFStringEncodingUTF8) + 1;
  char *buf = malloc((size_t)max);
  if (buf && !CFStringGetCString((CFStringRef)v, buf, max, kCFStringEncodingUTF8)) { free(buf); buf = NULL; }
  return buf;
}

/* nsec1... or 64-hex text (not NUL-terminated) -> lowercase 64-hex, or NULL. */
static char *kc_text_to_sk_hex(const UInt8 *bytes, CFIndex len){
  if (!bytes || len <= 0 || len > 200) return NULL;
  char text[201];
  memcpy(text, bytes, (size_t)len);
  text[len] = '\0';
  char *hex = NULL;
  if (is_hex_64(text)) {
    hex = strdup(text);
    for (char *p = hex; p && *p; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
  } else if (strncmp(text, "nsec1", 5) == 0) {
    uint8_t sk[32];
    if (nostr_nip19_decode_nsec(text, sk) == 0) hex = bin_to_hex(sk, 32);
    secure_wipe(sk, sizeof sk);
  }
  secure_wipe(text, sizeof text);
  return hex;
}

static char *kc_npub_from_sk_hex(const char *sk_hex){
  char *npub = NULL;
  return sk_hex_to_npub(sk_hex, &npub) == 0 ? npub : NULL;
}

/* TRUE when a daemon item for npub (by comment or account) holds a 32-byte
 * key whose npub is npub. */
static int kc_signer_holds(const char *npub){
  CFStringRef attrs[2] = { kSecAttrComment, kSecAttrAccount };
  int same = 0;
  for (int i = 0; i < 2 && !same; i++) {
    CFMutableDictionaryRef q = kc_query(KC_SIGNER_SERVICE, attrs[i], npub);
    if (!q) return 0;
    kc_scope(q, 0);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef res = NULL;
    if (SecItemCopyMatching(q, &res) == errSecSuccess && res &&
        CFGetTypeID(res) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)res) == 32) {
      char *hex = bin_to_hex(CFDataGetBytePtr((CFDataRef)res), 32);
      char *derived = hex ? kc_npub_from_sk_hex(hex) : NULL;
      same = derived && strcmp(derived, npub) == 0;
      free(derived);
      if (hex) { secure_wipe(hex, strlen(hex)); free(hex); }
    }
    if (res) CFRelease(res);
    CFRelease(q);
  }
  return same;
}

/* The daemon's item format (see nostr_nip55l_store_key), key_id = npub. */
static int kc_store_signer(const char *sk_hex, const char *npub){
  uint8_t skb[32];
  if (!nostr_hex2bin(skb, sk_hex, sizeof skb)) return 0;
  CFMutableDictionaryRef q = kc_query(KC_SIGNER_SERVICE, kSecAttrAccount, npub);
  if (!q) { secure_wipe(skb, sizeof skb); return 0; }
  kc_scope(q, 0);
  SecItemDelete(q);            /* replace a stale item for the same key_id */
  CFRelease(q);
  q = kc_query(KC_SIGNER_SERVICE, kSecAttrAccount, npub);
  if (!q) { secure_wipe(skb, sizeof skb); return 0; }
  kc_scope(q, 1);
  CFStringRef label = CFStringCreateWithCString(NULL, "Gnostr Identity", kCFStringEncodingUTF8);
  CFStringRef comment = CFStringCreateWithCString(NULL, npub, kCFStringEncodingUTF8);
  CFDataRef data = CFDataCreate(NULL, skb, (CFIndex)sizeof skb);
  secure_wipe(skb, sizeof skb);
  CFDictionarySetValue(q, kSecAttrLabel, label);
  CFDictionarySetValue(q, kSecAttrComment, comment);
  CFDictionarySetValue(q, kSecValueData, data);
  CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
  OSStatus st = SecItemAdd(q, NULL);
  /* Wipe key material from the CF buffer before release. */
  { const UInt8 *db = CFDataGetBytePtr(data); CFIndex dl = CFDataGetLength(data);
    if (db && dl > 0) memset((void *)db, 0, (size_t)dl); }
  CFRelease(label); CFRelease(comment); CFRelease(data); CFRelease(q);
  return st == errSecSuccess;
}

static int kc_marker_present(void){
  CFMutableDictionaryRef q = kc_query(KC_MARKER_SERVICE, kSecAttrAccount, KC_MARKER_ACCOUNT);
  if (!q) return 0;
  kc_scope(q, 0);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  OSStatus st = SecItemCopyMatching(q, NULL);
  CFRelease(q);
  return st == errSecSuccess;
}

static int kc_marker_write(void){
  CFMutableDictionaryRef q = kc_query(KC_MARKER_SERVICE, kSecAttrAccount, KC_MARKER_ACCOUNT);
  if (!q) return 0;
  kc_scope(q, 1);
  CFStringRef label = CFStringCreateWithCString(NULL, "Nostr signer migration marker (not a key)", kCFStringEncodingUTF8);
  CFDataRef data = CFDataCreate(NULL, (const UInt8 *)KC_MARKER_ACCOUNT, (CFIndex)strlen(KC_MARKER_ACCOUNT));
  CFDictionarySetValue(q, kSecAttrLabel, label);
  CFDictionarySetValue(q, kSecValueData, data);
  OSStatus st = SecItemAdd(q, NULL);
  CFRelease(label); CFRelease(data); CFRelease(q);
  return st == errSecSuccess || st == errSecDuplicateItem;
}

typedef enum { KC_MIG_OK, KC_MIG_SKIP, KC_MIG_RETRY } kc_mig_result;

static kc_mig_result kc_migrate_one(const char *account){
  CFMutableDictionaryRef q = kc_query(KC_LEGACY_CLIENT_SERVICE, kSecAttrAccount, account);
  if (!q) return KC_MIG_RETRY;
  kc_scope(q, 0);
  CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef res = NULL;
  OSStatus st = SecItemCopyMatching(q, &res);
  CFRelease(q);
  if (st != errSecSuccess || !res || CFGetTypeID(res) != CFDataGetTypeID()) {
    if (res) CFRelease(res);
    fprintf(stderr, "nip55l: keyring-migration: %s client item secret unavailable (OSStatus %d); will retry\n",
            account, (int)st);
    return KC_MIG_RETRY;
  }
  const UInt8 *mig_bytes = CFDataGetBytePtr((CFDataRef)res);
  CFIndex mig_len = CFDataGetLength((CFDataRef)res);
  char *sk_hex = kc_text_to_sk_hex(mig_bytes, mig_len);
  /* Wipe secret material from the CF buffer before release. */
  if (mig_bytes && mig_len > 0) memset((void *)mig_bytes, 0, (size_t)mig_len);
  CFRelease(res);
  char *npub = sk_hex ? kc_npub_from_sk_hex(sk_hex) : NULL;
  kc_mig_result r = KC_MIG_RETRY;
  if (!npub) {
    fprintf(stderr, "nip55l: keyring-migration: leaving client item %s in place: not a secp256k1 private key\n",
            account);
    r = KC_MIG_SKIP;
    goto out;
  }
  if (strcmp(account, npub) != 0)
    fprintf(stderr, "nip55l: keyring-migration: client item account %s does not match its key; using %s\n",
            account, npub);
  if (kc_signer_holds(npub)) {
    fprintf(stderr, "nip55l: keyring-migration: %s already held by the signer; retiring the client copy\n", npub);
  } else if (!kc_store_signer(sk_hex, npub) || !kc_signer_holds(npub)) {
    fprintf(stderr, "nip55l: keyring-migration: re-store of %s failed; will retry\n", npub);
    goto out;
  }
  q = kc_query(KC_LEGACY_CLIENT_SERVICE, kSecAttrAccount, account);
  if (!q) goto out;
  kc_scope(q, 0);
  st = SecItemDelete(q);
  CFRelease(q);
  if (st != errSecSuccess && st != errSecItemNotFound) {
    fprintf(stderr, "nip55l: keyring-migration: %s migrated but client item not deleted (OSStatus %d); will retry\n",
            npub, (int)st);
    goto out;
  }
  r = KC_MIG_OK;
out:
  if (sk_hex) { secure_wipe(sk_hex, strlen(sk_hex)); free(sk_hex); }
  free(npub);
  return r;
}

static int kc_migrate_legacy_keys(nostr_nip55l_keyring_migration *r){
  if (kc_marker_present()) { r->already_done = 1; return 0; }
  /* Attribute-only probe: never prompts. */
  CFMutableDictionaryRef q = kc_query(KC_LEGACY_CLIENT_SERVICE, NULL, NULL);
  if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
  kc_scope(q, 0);
  CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitAll);
  CFTypeRef res = NULL;
  OSStatus st = SecItemCopyMatching(q, &res);
  CFRelease(q);
  if (st == errSecItemNotFound) {
    return kc_marker_write() ? (r->marker_written = 1, 0) : NOSTR_SIGNER_ERROR_BACKEND;
  }
  if (st != errSecSuccess || !res || CFGetTypeID(res) != CFArrayGetTypeID()) {
    if (res) CFRelease(res);
    return NOSTR_SIGNER_ERROR_BACKEND;
  }
  CFArrayRef items = (CFArrayRef)res;
  CFIndex n = CFArrayGetCount(items);
  r->found = (unsigned)n;
  for (CFIndex i = 0; i < n; i++) {
    CFDictionaryRef a = CFArrayGetValueAtIndex(items, i);
    char *account = (a && CFGetTypeID(a) == CFDictionaryGetTypeID())
                      ? kc_cfstring_dup(CFDictionaryGetValue(a, kSecAttrAccount)) : NULL;
    if (!account) { r->skipped++; continue; }
    switch (kc_migrate_one(account)) {
      case KC_MIG_OK:    r->migrated++; break;
      case KC_MIG_SKIP:  r->skipped++;  break;
      case KC_MIG_RETRY: r->failed++;   break;
    }
    free(account);
  }
  CFRelease(items);
  if (r->failed) return NOSTR_SIGNER_ERROR_BACKEND;
  r->marker_written = kc_marker_write();
  return r->marker_written ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
}
#endif /* NIP55L_HAVE_KEYCHAIN */

int nostr_nip55l_migrate_legacy_keys(nostr_nip55l_keyring_migration *out){
  nostr_nip55l_keyring_migration r;
  memset(&r, 0, sizeof r);
#ifdef NIP55L_HAVE_LIBSECRET
  static const struct {
    const SecretSchema *schema;
    GnostrSecretLegacyKind kind;
  } sources[] = {
    { &gnostr_secret_legacy_signer_key_schema, GNOSTR_SECRET_LEGACY_SIGNER_KEY },
    { &gnostr_secret_legacy_helper_schema,     GNOSTR_SECRET_LEGACY_HELPER_KEY },
    { &gnostr_secret_legacy_client_schema,     GNOSTR_SECRET_LEGACY_CLIENT_KEY },
  };
  int rc = 0;
  GError *err = NULL;
  gboolean retry_later = FALSE;
  gchar uid_buf[32];
  g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
  if (!service) {
    g_clear_error(&err);
    rc = NOSTR_SIGNER_ERROR_BACKEND;
    goto done;
  }
  if (migration_marker_present(service)) {
    r.already_done = 1;
    goto done;
  }

  /* Attribute-only probe: works on locked collections without prompting. */
  for (size_t i = 0; i < G_N_ELEMENTS(sources); i++) {
    GList *items = search_legacy(service, sources[i].schema, SECRET_SEARCH_NONE, &err);
    if (err) { g_clear_error(&err); rc = NOSTR_SIGNER_ERROR_BACKEND; }
    r.found += g_list_length(items);
    g_list_free_full(items, g_object_unref);
  }
  if (rc != 0 || r.found == 0) goto done;

  /* The searches below load secrets: open the session first (nostrc-poc10). */
  if (!secret_service_ensure_session_sync(service, NULL, &err)) {
    g_message("nip55l: keyring-migration: no Secret Service session: %s; will retry",
              err ? err->message : "unknown error");
    g_clear_error(&err);
    rc = NOSTR_SIGNER_ERROR_BACKEND;
    goto done;
  }

  unsigned processed = 0;
  for (size_t i = 0; i < G_N_ELEMENTS(sources); i++) {
    GList *items = search_legacy(service, sources[i].schema,
                                 SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS, &err);
    if (err) {
      g_message("nip55l: keyring-migration: %s search failed: %s; will retry",
                sources[i].schema->name, err->message);
      g_clear_error(&err);
      retry_later = TRUE;
    }
    for (GList *l = items; l; l = l->next) {
      processed++;
      switch (migrate_one(service, l->data, sources[i].kind, sources[i].schema->name, uid_buf)) {
        case MIG_OK:    r.migrated++; break;
        case MIG_SKIP:  r.skipped++;  break;
        case MIG_RETRY: r.failed++;   break;
      }
    }
    g_list_free_full(items, g_object_unref);
  }

  /* The unlocking search must see everything the probe saw: a shortfall
   * (e.g. a backend that hides items when the unlock prompt is dismissed)
   * must not be mistaken for completion, or those items would be stranded
   * behind the marker. */
  if (processed < r.found) {
    g_message("nip55l: keyring-migration: probe saw %u legacy item(s), unlocked search %u; will retry",
              r.found, processed);
    retry_later = TRUE;
  }

  if (!retry_later && r.failed == 0) {
    r.marker_written = secret_password_store_sync(&gnostr_secret_migration_schema,
                                                  SECRET_COLLECTION_DEFAULT,
                                                  "Nostr signer migration marker (not a key)",
                                                  NIP55L_MIGRATION_MARKER, NULL, &err,
                                                  "name", NIP55L_MIGRATION_MARKER,
                                                  NULL) ? 1 : 0;
    if (err) {
      g_message("nip55l: keyring-migration: marker not written: %s", err->message);
      g_clear_error(&err);
    }
  } else {
    rc = NOSTR_SIGNER_ERROR_BACKEND;
  }

done:
  if (service) g_object_unref(service);
  if (out) *out = r;
  return rc;
#elif defined(NIP55L_HAVE_KEYCHAIN)
  int rc = kc_migrate_legacy_keys(&r);
  if (out) *out = r;
  return rc;
#else
  if (out) *out = r;
  return 0;
#endif
}
