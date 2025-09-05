/* Core helpers for NIP-55L signer operations.
 * Dependencies: libnostr JSON/event APIs, NIP-04, NIP-44 v2.
 */

#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"

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

#ifdef NIP55L_HAVE_LIBSECRET
#include <libsecret/secret.h>
#include <sys/types.h>
#include <unistd.h>
/* Identity-backed storage: one item per identity; attributes allow selection by key_id or npub. */
static const SecretSchema SIGNER_IDENTITY_SCHEMA = {
  "org.gnostr.Signer/identity",
  SECRET_SCHEMA_NONE,
  {
    { "key_id",   SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",     SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "label",    SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "hardware", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_uid",      SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_username", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};
#endif

#ifdef NIP55L_HAVE_KEYCHAIN
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

static int is_hex_64(const char *s) {
  if (!s) return 0; size_t n = strlen(s); if (n != 64) return 0; for (size_t i=0;i<n;i++){ char c=s[i]; if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0; } return 1;
}

static char *bin_to_hex(const uint8_t *buf, size_t len){
  static const char hexd[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
  char *out = (char*)malloc(len*2+1); if(!out) return NULL;
  for(size_t i=0;i<len;i++){ out[2*i]=hexd[(buf[i]>>4)&0xF]; out[2*i+1]=hexd[buf[i]&0xF]; }
  out[len*2]='\0'; return out;
}

/* Forward declaration */
static int resolve_seckey_hex(const char *current_user, char **out_sk_hex);

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
 * On success, returns newly allocated 64-hex in *out_sk_hex.
 */
static int resolve_seckey_hex(const char *current_user, char **out_sk_hex){
  if (!out_sk_hex) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_sk_hex=NULL;
  const char *cand = current_user;
  if (!cand || !*cand) {
    cand = getenv("NOSTR_SIGNER_SECKEY_HEX");
    if (cand && is_hex_64(cand)) { *out_sk_hex = strdup(cand); return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND; }
    const char *nsec = getenv("NOSTR_SIGNER_NSEC");
    if (nsec && strncmp(nsec, "nsec1", 5)==0) {
      uint8_t sk[32]; if (nostr_nip19_decode_nsec(nsec, sk)!=0) return NOSTR_SIGNER_ERROR_INVALID_KEY;
      *out_sk_hex = bin_to_hex(sk, 32);
      secure_wipe(sk, sizeof sk);
      return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND;
    }
    /* Try libsecret fallback by linked owner (current uid) */
#ifdef NIP55L_HAVE_LIBSECRET
    {
      SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
      if (service) {
        GError *gerr = NULL;
        GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
        gchar uid_buf[32];
        g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
        g_hash_table_insert(attrs, (gpointer)"owner_uid", (gpointer)uid_buf);
        GList *items = secret_service_search_sync(service, &SIGNER_IDENTITY_SCHEMA, attrs,
                                                  SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK, NULL, &gerr);
        g_hash_table_unref(attrs);
        if (gerr) { g_error_free(gerr); gerr = NULL; }
        SecretItem *item = NULL;
        if (items) {
          /* Accept the first match for this uid */
          item = SECRET_ITEM(g_object_ref(items->data));
        }
        if (items) g_list_free_full(items, g_object_unref);
        if (item) {
          SecretValue *sv = secret_item_get_secret(item);
          const gchar *sec = sv ? secret_value_get_text(sv) : NULL;
          int rc_l = NOSTR_SIGNER_ERROR_NOT_FOUND;
          if (sec) {
            if (is_hex_64(sec)) { *out_sk_hex = strdup(sec); rc_l = *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND; }
            else if (strncmp(sec, "nsec1", 5)==0) {
              uint8_t sk[32]; rc_l = (nostr_nip19_decode_nsec(sec, sk)==0) ? 0 : NOSTR_SIGNER_ERROR_INVALID_KEY;
              if (rc_l==0) { *out_sk_hex = bin_to_hex(sk, 32); if (!*out_sk_hex) rc_l = NOSTR_SIGNER_ERROR_BACKEND; }
              secure_wipe(sk, sizeof sk);
            } else {
              rc_l = NOSTR_SIGNER_ERROR_INVALID_KEY;
            }
          }
          if (sv) secret_value_unref(sv);
          g_object_unref(item);
          g_object_unref(service);
          return rc_l;
        }
        if (service) g_object_unref(service);
      }
    }
#elif defined(NIP55L_HAVE_KEYCHAIN)
    /* macOS Keychain: find any identity item in this user's keychain */
    {
      CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
      CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
      CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
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
#ifdef NIP55L_HAVE_LIBSECRET
  /* Treat current_user as identity selector: key_id or npub */
  {
    int rc_l = NOSTR_SIGNER_ERROR_NOT_FOUND;
    GError *gerr = NULL;
    /* Try lookup by key_id */
    gchar *secret = secret_password_lookup_sync(&SIGNER_IDENTITY_SCHEMA, NULL, &gerr,
                                                "key_id", cand,
                                                NULL);
    if (gerr) { g_error_free(gerr); gerr = NULL; }
    if (!secret) {
      /* Try lookup by npub */
      secret = secret_password_lookup_sync(&SIGNER_IDENTITY_SCHEMA, NULL, &gerr,
                                           "npub", cand,
                                           NULL);
      if (gerr) { g_error_free(gerr); gerr = NULL; }
    }
    if (secret) {
      if (is_hex_64(secret)) {
        *out_sk_hex = strdup(secret);
        rc_l = *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND;
      } else if (strncmp(secret, "nsec1", 5)==0) {
        uint8_t sk[32]; rc_l = (nostr_nip19_decode_nsec(secret, sk)==0) ? 0 : NOSTR_SIGNER_ERROR_INVALID_KEY;
        if (rc_l==0) { *out_sk_hex = bin_to_hex(sk, 32); if (!*out_sk_hex) rc_l = NOSTR_SIGNER_ERROR_BACKEND; }
      } else {
        rc_l = NOSTR_SIGNER_ERROR_INVALID_KEY;
      }
      secret_password_free(secret);
      return rc_l;
    }
    /* Fallback: search by current owner_uid if selector lookup failed */
    {
      SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
      if (service) {
        GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
        gchar uid_buf[32];
        g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
        g_hash_table_insert(attrs, (gpointer)"owner_uid", (gpointer)uid_buf);
        GError *gerr2 = NULL;
        GList *items = secret_service_search_sync(service, &SIGNER_IDENTITY_SCHEMA, attrs,
                                                  SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK, NULL, &gerr2);
        g_hash_table_unref(attrs);
        if (gerr2) { g_error_free(gerr2); gerr2 = NULL; }
        SecretItem *item = NULL;
        if (items) item = SECRET_ITEM(g_object_ref(items->data));
        if (items) g_list_free_full(items, g_object_unref);
        if (item) {
          SecretValue *sv = secret_item_get_secret(item);
          const gchar *sec = sv ? secret_value_get_text(sv) : NULL;
          if (sec) {
            if (is_hex_64(sec)) { *out_sk_hex = strdup(sec); rc_l = *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND; }
            else if (strncmp(sec, "nsec1", 5)==0) {
              uint8_t sk[32]; rc_l = (nostr_nip19_decode_nsec(sec, sk)==0) ? 0 : NOSTR_SIGNER_ERROR_INVALID_KEY;
              if (rc_l==0) { *out_sk_hex = bin_to_hex(sk, 32); if (!*out_sk_hex) rc_l = NOSTR_SIGNER_ERROR_BACKEND; }
            } else {
              rc_l = NOSTR_SIGNER_ERROR_INVALID_KEY;
            }
          }
          if (sv) secret_value_unref(sv);
          g_object_unref(item);
          g_object_unref(service);
          return rc_l;
        }
        g_object_unref(service);
      }
    }
    /* Final fallback: environment variables */
    {
      const char *ehex = getenv("NOSTR_SIGNER_SECKEY_HEX");
      if (ehex && is_hex_64(ehex)) { *out_sk_hex = strdup(ehex); return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND; }
      const char *nsec = getenv("NOSTR_SIGNER_NSEC");
      if (nsec && strncmp(nsec, "nsec1", 5)==0) {
        uint8_t sk[32]; if (nostr_nip19_decode_nsec(nsec, sk)!=0) return NOSTR_SIGNER_ERROR_INVALID_KEY;
        *out_sk_hex = bin_to_hex(sk, 32);
        secure_wipe(sk, sizeof sk);
        return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND;
      }
    }
  }
#elif defined(NIP55L_HAVE_KEYCHAIN)
  /* Treat current_user as identity selector when provided */
  {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
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

int nostr_nip55l_get_public_key(char **out_npub){
  if(!out_npub) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_npub=NULL;
  int rc;
  char *sk_hex=NULL; rc = resolve_seckey_hex(NULL, &sk_hex); if(rc!=0) return rc;
  char *pk_hex = nostr_key_get_public(sk_hex);
  if (sk_hex) { memset(sk_hex, 0, strlen(sk_hex)); }
  free(sk_hex);
  if (!pk_hex) return NOSTR_SIGNER_ERROR_BACKEND;
  uint8_t pk[32]; if (!nostr_hex2bin(pk, pk_hex, sizeof pk)) { free(pk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  free(pk_hex);
  char *npub=NULL; if (nostr_nip19_encode_npub(pk, &npub)!=0 || !npub) return NOSTR_SIGNER_ERROR_BACKEND;
  *out_npub = npub; return 0;
}

int nostr_nip55l_sign_event(const char *event_json,
                            const char *current_user,
                            const char *app_id,
                            char **out_signature){
  (void)app_id;
  if(!out_signature || !event_json) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_signature=NULL;
  int rc; nostr_secure_buf sb = {0}; rc = resolve_seckey_secure(current_user, &sb); if(rc!=0) return rc;
  NostrEvent *ev = nostr_event_new(); if(!ev){ secure_free(&sb); return NOSTR_SIGNER_ERROR_BACKEND; }
  if (nostr_event_deserialize(ev, event_json)!=0) { nostr_event_free(ev); secure_free(&sb); return NOSTR_SIGNER_ERROR_INVALID_JSON; }
  if (ev->created_at == 0) { ev->created_at = (int64_t)time(NULL); }
  if (nostr_event_sign_secure(ev, &sb)!=0) { nostr_event_free(ev); secure_free(&sb); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  secure_free(&sb);
  if (!ev->sig) { nostr_event_free(ev); return NOSTR_SIGNER_ERROR_BACKEND; }
  *out_signature = strdup(ev->sig);
  nostr_event_free(ev);
  return *out_signature?0:NOSTR_SIGNER_ERROR_BACKEND;
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

int nostr_nip55l_get_relays(char **out_relays_json){
  if(!out_relays_json) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_relays_json=NULL;
  /* Return an empty list for now to indicate no configured relays instead of NOT_FOUND. */
  const char *empty = "[]";
  char *dup = strdup(empty);
  if (!dup) return NOSTR_SIGNER_ERROR_BACKEND;
  *out_relays_json = dup;
  return 0;
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
  gboolean ok = secret_password_store_sync(&SIGNER_IDENTITY_SCHEMA,
                                           SECRET_COLLECTION_DEFAULT,
                                           "Gnostr Identity Key",
                                             sk_hex,
                                            NULL,
                                            &gerr,
                                            "key_id", key_id_attr,
                                            "npub", npub,
                                            "owner_uid", uid_buf,
                                            "hardware", "false",
                                            NULL);
  if (gerr) { g_error_free(gerr); }
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
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, key_id_attr, kCFStringEncodingUTF8);
  CFStringRef label = CFStringCreateWithCString(NULL, "Gnostr Identity", kCFStringEncodingUTF8);
  CFStringRef comment = CFStringCreateWithCString(NULL, npub, kCFStringEncodingUTF8);
  CFDataRef secretData = CFDataCreate(NULL, skb, (CFIndex)sizeof(skb));
  secure_wipe(skb, sizeof skb);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
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
  if (secretData) CFRelease(secretData);
  if (query) CFRelease(query);
  free(sk_hex);
  free(npub);
  return (st == errSecSuccess) ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)identity; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

int nostr_nip55l_clear_key(const char *identity){
#ifdef NIP55L_HAVE_LIBSECRET
  const char *sel = (identity && *identity) ? identity : "";
  GError *gerr = NULL;
  gboolean ok1 = secret_password_clear_sync(&SIGNER_IDENTITY_SCHEMA, NULL, &gerr,
                                            "key_id", sel, NULL);
  if (gerr) { g_error_free(gerr); gerr = NULL; }
  gboolean ok2 = secret_password_clear_sync(&SIGNER_IDENTITY_SCHEMA, NULL, &gerr,
                                            "npub", sel, NULL);
  if (gerr) { g_error_free(gerr); }
  return (ok1 || ok2) ? 0 : NOSTR_SIGNER_ERROR_NOT_FOUND;
#elif defined(NIP55L_HAVE_KEYCHAIN)
  /* Delete by selector if provided; otherwise delete all for service (best-effort single delete) */
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!q) return NOSTR_SIGNER_ERROR_BACKEND;
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
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

#ifdef NIP55L_HAVE_LIBSECRET
static SecretItem *find_identity_item(const char *selector){
  if (!selector || !*selector) return NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
  if (!service) return NULL;
  GError *gerr = NULL;
  /* Try key_id */
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"key_id", (gpointer)selector);
  GList *items = secret_service_search_sync(service, &SIGNER_IDENTITY_SCHEMA, attrs,
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
  items = secret_service_search_sync(service, &SIGNER_IDENTITY_SCHEMA, attrs,
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
  gboolean ok = secret_item_set_attributes_sync(item, &SIGNER_IDENTITY_SCHEMA, a, NULL, &gerr);
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
  gboolean ok = secret_item_set_attributes_sync(item, &SIGNER_IDENTITY_SCHEMA, a, NULL, &gerr);
  if (a) g_hash_table_unref(a);
  if (gerr) { g_error_free(gerr); }
  g_object_unref(item);
  return ok ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)selector; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}
