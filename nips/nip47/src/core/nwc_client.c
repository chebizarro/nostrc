#include "nostr/nip47/nwc_client.h"
#include "nostr/nip44/nip44.h"
#include "nostr/nip04.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Local hex helpers (mirrors NIP-46 impl) */
static int hex_nibble(char c){
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return 10+(c-'a');
  if (c>='A'&&c<='F') return 10+(c-'A');
  return -1;
}
static int hex_to_bytes_exact(const char *hex, unsigned char *out, size_t outlen){
  if (!hex||!out) return -1; size_t n=strlen(hex); if (n!=outlen*2) return -1;
  for (size_t i=0;i<outlen;i++){ int h=hex_nibble(hex[2*i]); int l=hex_nibble(hex[2*i+1]); if (h<0||l<0) return -1; out[i]=(unsigned char)((h<<4)|l); }
  return 0;
}

/* Build SEC1-compressed hex (66 chars) from x-only 64-hex. prefix must be 0x02 or 0x03. */
static int build_sec1_from_xonly(const char *x64, char out66[67], char prefix){
  if (!x64) return -1; if (strlen(x64)!=64) return -1; if (!(prefix==0x02||prefix==0x03)) return -1;
  out66[0] = '0'; out66[1] = (prefix==0x02? '2':'3'); memcpy(out66+2, x64, 64); out66[66] = '\0'; return 0;
}
/* Accept 64 (x-only), 66 (33B SEC1), 130 (65B SEC1) and output x-only 32B */
static int parse_peer_xonly32(const char *hex, unsigned char out32[32]){
  if (!hex||!out32) return -1; size_t n=strlen(hex);
  if (n==64) return hex_to_bytes_exact(hex,out32,32);
  if (n==66){ unsigned char comp[33]; if (hex_to_bytes_exact(hex,comp,33)!=0) return -1; if (!(comp[0]==0x02||comp[0]==0x03)) return -1; memcpy(out32, comp+1, 32); return 0; }
  if (n==130){ unsigned char uncmp[65]; if (hex_to_bytes_exact(hex,uncmp,65)!=0) return -1; if (uncmp[0]!=0x04) return -1; memcpy(out32, uncmp+1, 32); return 0; }
  return -1;
}
static int parse_sk32(const char *hex, unsigned char out32[32]){ if (!hex||!out32) return -1; return hex_to_bytes_exact(hex,out32,32); }

int nostr_nwc_client_session_init(NostrNwcClientSession *s,
                                  const char *wallet_pub_hex,
                                  const char **client_supported, size_t client_n,
                                  const char **wallet_supported, size_t wallet_n) {
  if (!s || !wallet_pub_hex) return -1;
  memset(s, 0, sizeof(*s));
  NostrNwcEncryption enc = 0;
  if (nostr_nwc_select_encryption(client_supported, client_n, wallet_supported, wallet_n, &enc) != 0) {
    return -1;
  }
  s->wallet_pub_hex = strdup(wallet_pub_hex);
  if (!s->wallet_pub_hex) { memset(s, 0, sizeof(*s)); return -1; }
  s->enc = enc;
  return 0;
}

void nostr_nwc_client_session_clear(NostrNwcClientSession *s) {
  if (!s) return;
  free(s->wallet_pub_hex);
  memset(s, 0, sizeof(*s));
}

int nostr_nwc_client_build_request(const NostrNwcClientSession *s,
                                   const NostrNwcRequestBody *body,
                                   char **out_event_json) {
  if (!s || !s->wallet_pub_hex) return -1;
  return nostr_nwc_request_build(s->wallet_pub_hex, s->enc, body, out_event_json);
}

int nostr_nwc_client_encrypt(const NostrNwcClientSession *s,
                             const char *client_sk_hex,
                             const char *wallet_pub_hex,
                             const char *plaintext,
                             char **out_ciphertext) {
  if (!s || !client_sk_hex || !wallet_pub_hex || !plaintext || !out_ciphertext) return -1;
  *out_ciphertext = NULL;
  if (s->enc == NOSTR_NWC_ENC_NIP44_V2) {
    unsigned char sk[32]; unsigned char pkx[32];
    if (parse_sk32(client_sk_hex, sk) != 0) { return -1; }
    if (parse_peer_xonly32(wallet_pub_hex, pkx) != 0) { return -1; }
    char *b64 = NULL;
    if (nostr_nip44_encrypt_v2(sk, pkx, (const uint8_t*)plaintext, strlen(plaintext), &b64) != 0 || !b64) { return -1; }
    *out_ciphertext = b64; return 0;
  } else { /* NIP-04 */
    char sec1[67]; const char *peer = wallet_pub_hex; char *cipher = NULL; char *err = NULL;
    if (strlen(wallet_pub_hex)==64) { if (build_sec1_from_xonly(wallet_pub_hex, sec1, 0x02)==0) peer = sec1; }
    if (nostr_nip04_encrypt(plaintext, peer, client_sk_hex, &cipher, &err) != 0 || !cipher) {
      if (err) { free(err); err=NULL; }
      /* Fallback try 0x03 if we converted */
      if (peer==sec1 && build_sec1_from_xonly(wallet_pub_hex, sec1, 0x03)==0) {
        if (nostr_nip04_encrypt(plaintext, sec1, client_sk_hex, &cipher, &err) != 0 || !cipher) { if (err) free(err); return -1; }
      } else {
        return -1;
      }
    }
    *out_ciphertext = cipher; return 0;
  }
}

int nostr_nwc_client_decrypt(const NostrNwcClientSession *s,
                             const char *client_sk_hex,
                             const char *wallet_pub_hex,
                             const char *ciphertext,
                             char **out_plaintext) {
  if (!s || !client_sk_hex || !wallet_pub_hex || !ciphertext || !out_plaintext) return -1;
  *out_plaintext = NULL;
  if (s->enc == NOSTR_NWC_ENC_NIP44_V2) {
    unsigned char sk[32]; unsigned char pkx[32];
    if (parse_sk32(client_sk_hex, sk) != 0) return -1;
    if (parse_peer_xonly32(wallet_pub_hex, pkx) != 0) return -1;
    uint8_t *plain = NULL; size_t plen = 0;
    if (nostr_nip44_decrypt_v2(sk, pkx, ciphertext, &plain, &plen) != 0 || !plain) return -1;
    char *out = (char*)malloc(plen+1); if (!out){ free(plain); return -1; }
    memcpy(out, plain, plen); out[plen] = '\0'; free(plain);
    *out_plaintext = out; return 0;
  } else { /* NIP-04 */
    char sec1[67]; const char *peer = wallet_pub_hex; char *plain = NULL; char *err = NULL;
    if (strlen(wallet_pub_hex)==64) { if (build_sec1_from_xonly(wallet_pub_hex, sec1, 0x02)==0) peer = sec1; }
    if (nostr_nip04_decrypt(ciphertext, peer, client_sk_hex, &plain, &err) != 0 || !plain) {
      if (err) { free(err); err=NULL; }
      if (peer==sec1 && build_sec1_from_xonly(wallet_pub_hex, sec1, 0x03)==0) {
        if (nostr_nip04_decrypt(ciphertext, sec1, client_sk_hex, &plain, &err) != 0 || !plain) { if (err) free(err); return -1; }
      } else {
        return -1;
      }
    }
    *out_plaintext = plain; return 0;
  }
}
