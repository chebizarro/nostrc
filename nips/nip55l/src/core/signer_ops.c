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
#include <nip19.h>
#include <keys.h>

#ifdef NIP55L_HAVE_LIBSECRET
#include <libsecret/secret.h>
static const SecretSchema SIGNER_SECRET_SCHEMA = {
  "org.nostr.Signer",
  SECRET_SCHEMA_NONE,
  {
    { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};
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
      *out_sk_hex = bin_to_hex(sk, 32); return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND;
    }
#ifdef NIP55L_HAVE_LIBSECRET
    {
      /* Try Secret Service (Libsecret). We use a single account key for now. */
      GError *gerr = NULL; const char *acct = "default";
      gchar *secret = secret_password_lookup_sync(&SIGNER_SECRET_SCHEMA, NULL, &gerr, "account", acct, NULL);
      if (gerr) { g_error_free(gerr); }
      if (secret) {
        int rc_l = 0;
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
    }
#endif
    return NOSTR_SIGNER_ERROR_NOT_FOUND;
  }
  if (is_hex_64(cand)) { *out_sk_hex = strdup(cand); return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND; }
  if (strncmp(cand, "nsec1", 5)==0) {
    uint8_t sk[32]; if (nostr_nip19_decode_nsec(cand, sk)!=0) return NOSTR_SIGNER_ERROR_INVALID_KEY;
    *out_sk_hex = bin_to_hex(sk, 32); return *out_sk_hex?0:NOSTR_SIGNER_ERROR_BACKEND;
  }
  return NOSTR_SIGNER_ERROR_INVALID_KEY;
}

int nostr_nip55l_get_public_key(char **out_npub){
  if(!out_npub) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_npub=NULL;
  int rc;
  char *sk_hex=NULL; rc = resolve_seckey_hex(NULL, &sk_hex); if(rc!=0) return rc;
  char *pk_hex = nostr_key_get_public(sk_hex);
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
  int rc; char *sk_hex=NULL; rc = resolve_seckey_hex(current_user, &sk_hex); if(rc!=0) return rc;
  NostrEvent *ev = nostr_event_new(); if(!ev){ free(sk_hex); return NOSTR_SIGNER_ERROR_BACKEND; }
  if (nostr_event_deserialize(ev, event_json)!=0) { nostr_event_free(ev); free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_JSON; }
  if (ev->created_at == 0) { ev->created_at = (int64_t)time(NULL); }
  if (nostr_event_sign(ev, sk_hex)!=0) { nostr_event_free(ev); free(sk_hex); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  free(sk_hex);
  if (!ev->sig) { nostr_event_free(ev); return NOSTR_SIGNER_ERROR_BACKEND; }
  *out_signature = strdup(ev->sig);
  nostr_event_free(ev);
  return *out_signature?0:NOSTR_SIGNER_ERROR_BACKEND;
}

int nostr_nip55l_nip04_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64){
  if(!plaintext || !peer_pub_hex || !out_cipher_b64) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_cipher_b64=NULL;
  int rc; char *sk_hex=NULL; rc = resolve_seckey_hex(current_user, &sk_hex); if(rc!=0) return rc;
  char *err=NULL; char *ct=NULL;
  int enc_rc = nostr_nip04_encrypt(plaintext, peer_pub_hex, sk_hex, &ct, &err);
  free(sk_hex);
  if (enc_rc!=0) { if(err) free(err); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  *out_cipher_b64 = ct; return 0;
}

int nostr_nip55l_nip04_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext){
  if(!cipher_b64 || !peer_pub_hex || !out_plaintext) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_plaintext=NULL;
  int rc; char *sk_hex=NULL; rc = resolve_seckey_hex(current_user, &sk_hex); if(rc!=0) return rc;
  char *err=NULL; char *pt=NULL;
  int dec_rc = nostr_nip04_decrypt(cipher_b64, peer_pub_hex, sk_hex, &pt, &err);
  free(sk_hex);
  if (dec_rc!=0) { if(err) free(err); return NOSTR_SIGNER_ERROR_CRYPTO_FAILED; }
  *out_plaintext = pt; return 0;
}

int nostr_nip55l_nip44_encrypt(const char *plaintext, const char *peer_pub_hex,
                               const char *current_user, char **out_cipher_b64){
  if(!plaintext || !peer_pub_hex || !out_cipher_b64) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_cipher_b64=NULL;
  int rc; char *sk_hex=NULL; rc = resolve_seckey_hex(current_user, &sk_hex); if(rc!=0) return rc;
  uint8_t sk[32]; if (!nostr_hex2bin(sk, sk_hex, sizeof sk)) { free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  free(sk_hex);
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  char *b64=NULL;
  if (nostr_nip44_encrypt_v2(sk, pkx, (const uint8_t*)plaintext, strlen(plaintext), &b64)!=0) return NOSTR_SIGNER_ERROR_CRYPTO_FAILED;
  *out_cipher_b64 = b64; return 0;
}

int nostr_nip55l_nip44_decrypt(const char *cipher_b64, const char *peer_pub_hex,
                               const char *current_user, char **out_plaintext){
  if(!cipher_b64 || !peer_pub_hex || !out_plaintext) return NOSTR_SIGNER_ERROR_INVALID_ARG; *out_plaintext=NULL;
  int rc; char *sk_hex=NULL; rc = resolve_seckey_hex(current_user, &sk_hex); if(rc!=0) return rc;
  uint8_t sk[32]; if (!nostr_hex2bin(sk, sk_hex, sizeof sk)) { free(sk_hex); return NOSTR_SIGNER_ERROR_INVALID_KEY; }
  free(sk_hex);
  if (!is_hex_64(peer_pub_hex)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return NOSTR_SIGNER_ERROR_INVALID_KEY;
  uint8_t *pt=NULL; size_t ptlen=0;
  if (nostr_nip44_decrypt_v2(sk, pkx, cipher_b64, &pt, &ptlen)!=0) return NOSTR_SIGNER_ERROR_CRYPTO_FAILED;
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
    uint8_t sk[32]; if (!nostr_hex2bin(sk, sk_hex, sizeof sk)) break;
    if (!is_hex_64(peer_pub_hex)) break;
    uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) break;
    uint8_t *ptbuf=NULL; size_t ptlen=0;
    if (nostr_nip44_decrypt_v2(sk, pkx, content, &ptbuf, &ptlen)==0) {
      pt = (char*)malloc(ptlen+1); if(pt){ memcpy(pt, ptbuf, ptlen); pt[ptlen]='\0'; dec_ok=1; }
      free(ptbuf);
    }
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

int nostr_nip55l_store_secret(const char *secret, const char *account){
  if (!secret) return NOSTR_SIGNER_ERROR_INVALID_ARG;
  const char *acct = (account && *account) ? account : "default";
#ifdef NIP55L_HAVE_LIBSECRET
  GError *gerr = NULL;
  gboolean ok = secret_password_store_sync(&SIGNER_SECRET_SCHEMA,
                                           SECRET_COLLECTION_DEFAULT,
                                           "Nostr Signer Secret",
                                           secret,
                                           NULL,
                                           &gerr,
                                           "account", acct,
                                           NULL);
  if (gerr) { g_error_free(gerr); }
  return ok ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)acct; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}

int nostr_nip55l_clear_secret(const char *account){
  const char *acct = (account && *account) ? account : "default";
#ifdef NIP55L_HAVE_LIBSECRET
  GError *gerr = NULL;
  gboolean ok = secret_password_clear_sync(&SIGNER_SECRET_SCHEMA, NULL, &gerr,
                                           "account", acct, NULL);
  if (gerr) { g_error_free(gerr); }
  return ok ? 0 : NOSTR_SIGNER_ERROR_BACKEND;
#else
  (void)acct; return NOSTR_SIGNER_ERROR_NOT_FOUND;
#endif
}
