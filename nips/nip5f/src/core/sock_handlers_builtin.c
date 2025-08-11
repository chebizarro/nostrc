#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "nostr/nip5f/nip5f.h"

#include <nostr-event.h>
#include <nostr-json.h>
#include <nostr-utils.h>      /* nostr_hex2bin */
#include <keys.h>              /* nostr_key_get_public */
#include <nip19.h>             /* nsec decode */
#include <nostr/nip44/nip44.h> /* nip44 v2 */

static int signer_log_enabled(void) {
  const char *e = getenv("NOSTR_SIGNER_LOG");
  return (e && *e && strcmp(e, "0")!=0) ? 1 : 0;
}

/* best-effort zeroization */
static void secure_bzero(void *ptr, size_t len) {
  volatile unsigned char *p = (volatile unsigned char*)ptr;
  while (len--) *p++ = 0;
}

/* local helpers */
static int is_hex_64(const char *s) {
  if (!s) return 0; size_t n = strlen(s); if (n != 64) return 0;
  for (size_t i=0;i<n;i++){ char c=s[i];
    if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0; }
  return 1;
}

static char *bin_to_hex(const uint8_t *buf, size_t len){
  static const char hexd[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
  char *out = (char*)malloc(len*2+1); if(!out) return NULL;
  for(size_t i=0;i<len;i++){ out[2*i]=hexd[(buf[i]>>4)&0xF]; out[2*i+1]=hexd[buf[i]&0xF]; }
  out[len*2]='\0'; return out;
}

/* Resolve seckey from environment with precedence:
 * 1) NOSTR_SIGNER_KEY (hex or nsec)
 * 2) NOSTR_SIGNER_SECKEY_HEX (legacy)
 * 3) NOSTR_SIGNER_NSEC (legacy)
 */
static int resolve_seckey_hex_env(char **out_sk_hex){
  if (!out_sk_hex) return -1; *out_sk_hex = NULL;
  /* Preferred: NOSTR_SIGNER_KEY */
  const char *key = getenv("NOSTR_SIGNER_KEY");
  if (key && *key) {
    if (is_hex_64(key)) {
      *out_sk_hex = strdup(key);
      if (*out_sk_hex && signer_log_enabled()) fprintf(stderr, "[nip5f] using seckey from KEY env (%.4s...)\n", key);
      return *out_sk_hex?0:-1;
    }
    if (strncmp(key, "nsec1", 5)==0) {
      uint8_t sk[32]; if (nostr_nip19_decode_nsec(key, sk)!=0) return -1;
      *out_sk_hex = bin_to_hex(sk, 32); secure_bzero(sk, sizeof sk);
      if (*out_sk_hex && signer_log_enabled()) fprintf(stderr, "[nip5f] using seckey from KEY env (nsec) (%.4s...)\n", *out_sk_hex);
      return *out_sk_hex?0:-1;
    }
    if (signer_log_enabled()) fprintf(stderr, "[nip5f] invalid NOSTR_SIGNER_KEY format; expecting 64-hex or nsec1...\n");
  }
  /* Legacy HEX */
  const char *cand = getenv("NOSTR_SIGNER_SECKEY_HEX");
  if (cand) {
    if (is_hex_64(cand)) { *out_sk_hex = strdup(cand); if (signer_log_enabled()) fprintf(stderr, "[nip5f] using seckey from HEX env (%.4s...)\n", cand); return *out_sk_hex?0:-1; }
    if (signer_log_enabled()) fprintf(stderr, "[nip5f] invalid NOSTR_SIGNER_SECKEY_HEX length/format\n");
  }
  /* Legacy NSEC */
  const char *nsec = getenv("NOSTR_SIGNER_NSEC");
  if (nsec && strncmp(nsec, "nsec1", 5)==0) {
    uint8_t sk[32]; if (nostr_nip19_decode_nsec(nsec, sk)!=0) return -1;
    *out_sk_hex = bin_to_hex(sk, 32);
    secure_bzero(sk, sizeof sk);
    if (*out_sk_hex && signer_log_enabled()) fprintf(stderr, "[nip5f] using seckey from NSEC env (%.4s...)\n", *out_sk_hex);
    return *out_sk_hex?0:-1;
  }
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] no signing key env found\n");
  return -1;
}

int nostr_nip5f_builtin_get_public_key(char **out_pub_hex) {
  if (!out_pub_hex) return -1; *out_pub_hex = NULL;
  char *sk_hex = NULL; if (resolve_seckey_hex_env(&sk_hex)!=0) return -1;
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] derive pub from sk (%.4s...)\n", sk_hex);
  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex && signer_log_enabled()) fprintf(stderr, "[nip5f] nostr_key_get_public failed\n");
  if (pk_hex && signer_log_enabled()) fprintf(stderr, "[nip5f] derived pub (%.4s...)\n", pk_hex);
  free(sk_hex);
  if (!pk_hex) return -1;
  *out_pub_hex = pk_hex; /* already heap-allocated */
  return 0;
}

int nostr_nip5f_builtin_sign_event(const char *event_json, const char *pubkey_hex, char **out_signed_event_json) {
  if (!event_json || !out_signed_event_json) return -1; *out_signed_event_json = NULL;
  int rc = 0; char *sk_hex = NULL; if (resolve_seckey_hex_env(&sk_hex)!=0) { if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: no secret key available in env\n"); return -1; }
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: input json=%.*s\n", (int)strnlen(event_json, 512), event_json);
  /* Derive pubkey from secret once */
  char *derived = nostr_key_get_public(sk_hex);
  if (!derived) { if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: derive pub from sk failed\n"); free(sk_hex); return -1; }
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: derived pub (%.4s...)\n", derived);
  /* If caller provided pubkey, ensure it matches derived pubkey to prevent signing under wrong identity */
  if (pubkey_hex && *pubkey_hex) {
    if (strcmp(derived, pubkey_hex)!=0) { if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: provided pubkey mismatch\n"); free(derived); free(sk_hex); return -1; }
  }
  NostrEvent *ev = nostr_event_new(); if (!ev){ free(sk_hex); return -1; }
  int prc = nostr_event_deserialize(ev, event_json);
  if (prc!=0) { if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: event deserialize failed rc=%d\n", prc); nostr_event_free(ev); free(sk_hex); return -1; }
  if (signer_log_enabled()) {
    const char *pk = ev->pubkey ? ev->pubkey : "";
    const char *id = ev->id ? ev->id : "";
    const char *sig = ev->sig ? ev->sig : "";
    const char *ct = ev->content ? ev->content : "";
    fprintf(stderr, "[nip5f] sign_event: parsed kind=%d created_at=%lld pubkey=%.8s content_len=%zu id_set=%d sig_set=%d\n",
            ev->kind, (long long)ev->created_at, pk, strlen(ct), id[0] != '\0', sig[0] != '\0');
  }
  /* If event.pubkey missing or empty, set it to derived */
  if (!ev->pubkey || ev->pubkey[0]=='\0') {
    nostr_event_set_pubkey(ev, derived);
    if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: populated missing pubkey\n");
  }
  if (ev->created_at == 0) { ev->created_at = (int64_t)time(NULL); }
  int src = nostr_event_sign(ev, sk_hex);
  if (src!=0) { if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: nostr_event_sign failed rc=%d\n", src); nostr_event_free(ev); free(sk_hex); return -1; }
  if (signer_log_enabled()) {
    bool ok = nostr_event_check_signature(ev);
    fprintf(stderr, "[nip5f] sign_event: signature computed; verify=%s\n", ok?"ok":"FAIL");
  }
  /* Zeroize secret as best-effort */
  memset(sk_hex, 0, strlen(sk_hex));
  free(sk_hex);
  char *sjson = nostr_event_serialize(ev);
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event: serialized signed event%s\n", sjson?"":" (NULL)");
  nostr_event_free(ev);
  if (!sjson) return -1;
  *out_signed_event_json = sjson;
  free(derived);
  return rc;
}

int nostr_nip5f_builtin_nip44_encrypt(const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64) {
  if (!peer_pub_hex || !plaintext || !out_cipher_b64) return -1; *out_cipher_b64=NULL;
  char *sk_hex=NULL; if (resolve_seckey_hex_env(&sk_hex)!=0) return -1;
  uint8_t sk[32]; if (!nostr_hex2bin(sk, sk_hex, sizeof sk)) { free(sk_hex); return -1; }
  /* wipe hex after conversion */
  memset(sk_hex, 0, strlen(sk_hex));
  free(sk_hex);
  if (!is_hex_64(peer_pub_hex)) return -1;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return -1;
  char *b64=NULL;
  if (nostr_nip44_encrypt_v2(sk, pkx, (const uint8_t*)plaintext, strlen(plaintext), &b64)!=0) { secure_bzero(sk, sizeof sk); return -1; }
  secure_bzero(sk, sizeof sk);
  *out_cipher_b64 = b64; return 0;
}

int nostr_nip5f_builtin_nip44_decrypt(const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext) {
  if (!peer_pub_hex || !cipher_b64 || !out_plaintext) return -1; *out_plaintext=NULL;
  char *sk_hex=NULL; if (resolve_seckey_hex_env(&sk_hex)!=0) return -1;
  uint8_t sk[32]; if (!nostr_hex2bin(sk, sk_hex, sizeof sk)) { free(sk_hex); return -1; }
  memset(sk_hex, 0, strlen(sk_hex));
  free(sk_hex);
  if (!is_hex_64(peer_pub_hex)) return -1;
  uint8_t pkx[32]; if (!nostr_hex2bin(pkx, peer_pub_hex, sizeof pkx)) return -1;
  uint8_t *pt=NULL; size_t ptlen=0;
  if (nostr_nip44_decrypt_v2(sk, pkx, cipher_b64, &pt, &ptlen)!=0) { secure_bzero(sk, sizeof sk); return -1; }
  secure_bzero(sk, sizeof sk);
  char *out = (char*)malloc(ptlen+1); if(!out){ free(pt); return -1; }
  memcpy(out, pt, ptlen); out[ptlen]='\0'; free(pt);
  *out_plaintext = out; return 0;
}

int nostr_nip5f_builtin_list_public_keys(char **out_keys_json) {
  if (!out_keys_json) return -1; *out_keys_json=NULL;
  char *pk_hex=NULL; if (nostr_nip5f_builtin_get_public_key(&pk_hex)!=0 || !pk_hex) return -1;
  /* Return a minimal JSON array with a single entry */
  size_t L = strlen(pk_hex) + 5; // ["%s"] + NUL
  char *j = (char*)malloc(L);
  if (!j){ free(pk_hex); return -1; }
  snprintf(j, L, "[\"%s\"]", pk_hex);
  free(pk_hex);
  *out_keys_json = j; return 0;
}
