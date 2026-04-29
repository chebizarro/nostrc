#include "nostr/nip47/nwc_client.h"
#include "nostr/nip44/nip44.h"
#include "nostr/nip04.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-keys.h"
#include "nostr-simple-pool.h"
#include "nostr-tag.h"
#include "secure_buf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

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

static const char *nwc_enc_label(NostrNwcEncryption enc) {
  switch (enc) {
    case NOSTR_NWC_ENC_NIP44_V2: return "nip44-v2";
    case NOSTR_NWC_ENC_NIP04: return "nip04";
    default: return "nip44-v2";
  }
}

static char *nwc_json_quote_token(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *out = (char *)malloc(n + 3);
  if (!out) return NULL;
  out[0] = '"'; memcpy(out + 1, s, n); out[1 + n] = '"'; out[2 + n] = '\0';
  return out;
}

static void nwc_response_middleware(NostrIncomingEvent *incoming, void *user_data) {
  NostrNwcClientSession *s = (NostrNwcClientSession *)user_data;
  if (!s || !incoming || !incoming->event || !s->response_cb) return;
  if (nostr_event_get_kind(incoming->event) != NOSTR_EVENT_KIND_NWC_RESPONSE) return;

  char *event_json = nostr_event_serialize_compact(incoming->event);
  if (!event_json) return;

  char *client_pub = NULL;
  char *req_id = NULL;
  NostrNwcResponseBody body = {0};
  if (nostr_nwc_response_parse(event_json, &client_pub, &req_id, NULL, &body) == 0) {
    if (!s->client_pub_hex || !client_pub || strcmp(client_pub, s->client_pub_hex) == 0) {
      s->response_cb(event_json, req_id, &body, s->response_cb_data);
    }
  }
  free(client_pub);
  free(req_id);
  nostr_nwc_response_body_clear(&body);
  free(event_json);
}

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
  nostr_nwc_client_stop_response_subscription(s);
  free(s->wallet_pub_hex);
  memset(s, 0, sizeof(*s));
}

int nostr_nwc_client_build_request(const NostrNwcClientSession *s,
                                   const NostrNwcRequestBody *body,
                                   char **out_event_json) {
  if (!s || !s->wallet_pub_hex) return -1;
  return nostr_nwc_request_build(s->wallet_pub_hex, s->enc, body, out_event_json);
}

int nostr_nwc_client_build_signed_request(const NostrNwcClientSession *s,
                                          const char *client_sk_hex,
                                          const NostrNwcRequestBody *body,
                                          char **out_event_json) {
  if (!s || !s->wallet_pub_hex || !client_sk_hex || !body || !body->method || !out_event_json) return -1;
  *out_event_json = NULL;

  int rc = -1;
  char *mqt = NULL;
  char *plaintext = NULL;
  char *ciphertext = NULL;
  NostrEvent *ev = NULL;

  mqt = nwc_json_quote_token(body->method);
  if (!mqt) goto out;
  const char *params = body->params_json && *body->params_json ? body->params_json : "{}";
  size_t plen = strlen(mqt) + strlen(params) + 32;
  plaintext = (char *)malloc(plen);
  if (!plaintext) goto out;
  snprintf(plaintext, plen, "{\"method\":%s,\"params\":%s}", mqt, params);

  if (nostr_nwc_client_encrypt(s, client_sk_hex, s->wallet_pub_hex, plaintext, &ciphertext) != 0 || !ciphertext) goto out;

  ev = nostr_event_new();
  if (!ev) goto out;
  nostr_event_set_kind(ev, NOSTR_EVENT_KIND_NWC_REQUEST);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, ciphertext);

  NostrTags *tags = nostr_tags_new(0);
  if (!tags) goto out;
  NostrTag *p = nostr_tag_new("p", s->wallet_pub_hex, NULL);
  NostrTag *enc = nostr_tag_new("encryption", nwc_enc_label(s->enc), NULL);
  if (!p || !enc) {
    if (p) nostr_tag_free(p);
    if (enc) nostr_tag_free(enc);
    nostr_tags_free(tags);
    goto out;
  }
  nostr_tags_append(tags, p);
  nostr_tags_append(tags, enc);
  nostr_event_set_tags(ev, tags);

  if (nostr_event_sign(ev, client_sk_hex) != 0) goto out;
  *out_event_json = nostr_event_serialize_compact(ev);
  rc = *out_event_json ? 0 : -1;

out:
  if (ev) nostr_event_free(ev);
  free(mqt);
  free(plaintext);
  free(ciphertext);
  return rc;
}

int nostr_nwc_client_start_response_subscription(NostrNwcClientSession *s,
                                                 const char **relays,
                                                 size_t n_relays,
                                                 const char *client_pub_hex,
                                                 NostrNwcResponseCallback cb,
                                                 void *user_data) {
  if (!s || !relays || n_relays == 0 || !client_pub_hex || !cb) return -1;

  nostr_nwc_client_stop_response_subscription(s);

  s->client_pub_hex = strdup(client_pub_hex);
  if (!s->client_pub_hex) return -1;
  s->response_cb = cb;
  s->response_cb_data = user_data;
  s->response_pool = nostr_simple_pool_new();
  if (!s->response_pool) {
    nostr_nwc_client_stop_response_subscription(s);
    return -1;
  }

  nostr_simple_pool_set_event_middleware_ex(s->response_pool, nwc_response_middleware, s);
  nostr_simple_pool_set_auto_unsub_on_eose(s->response_pool, false);
  nostr_simple_pool_start(s->response_pool);

  NostrFilter *filter = nostr_filter_new();
  if (!filter) {
    nostr_nwc_client_stop_response_subscription(s);
    return -1;
  }
  nostr_filter_add_kind(filter, NOSTR_EVENT_KIND_NWC_RESPONSE);
  nostr_filter_tags_append(filter, "p", client_pub_hex, NULL);
  nostr_filter_set_since_i64(filter, (int64_t)time(NULL));

  NostrFilters filters = {0};
  filters.filters = filter;
  filters.count = 1;
  filters.capacity = 1;
  nostr_simple_pool_subscribe(s->response_pool, relays, n_relays, filters, true);
  nostr_filter_free(filter);
  return 0;
}

void nostr_nwc_client_stop_response_subscription(NostrNwcClientSession *s) {
  if (!s) return;
  if (s->response_pool) {
    nostr_simple_pool_stop(s->response_pool);
    nostr_simple_pool_free(s->response_pool);
    s->response_pool = NULL;
  }
  free(s->client_pub_hex);
  s->client_pub_hex = NULL;
  s->response_cb = NULL;
  s->response_cb_data = NULL;
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
    if (parse_peer_xonly32(wallet_pub_hex, pkx) != 0) { secure_wipe(sk, sizeof sk); return -1; }
    char *b64 = NULL;
    if (nostr_nip44_encrypt_v2(sk, pkx, (const uint8_t*)plaintext, strlen(plaintext), &b64) != 0 || !b64) { secure_wipe(sk, sizeof sk); return -1; }
    secure_wipe(sk, sizeof sk);
    *out_ciphertext = b64; return 0;
  } else { /* NIP-04 */
    char sec1[67]; const char *peer = wallet_pub_hex; char *cipher = NULL; char *err = NULL;
    if (strlen(wallet_pub_hex)==64) { if (build_sec1_from_xonly(wallet_pub_hex, sec1, 0x02)==0) peer = sec1; }
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr || parse_sk32(client_sk_hex, (unsigned char*)sb.ptr) != 0) { if (sb.ptr) secure_free(&sb); return -1; }
    if (nostr_nip04_encrypt_secure(plaintext, peer, &sb, &cipher, &err) != 0 || !cipher) {
      secure_free(&sb);
      if (err) { free(err); err=NULL; }
      /* Fallback try 0x03 if we converted */
      if (peer==sec1 && build_sec1_from_xonly(wallet_pub_hex, sec1, 0x03)==0) {
        if (nostr_nip04_encrypt_secure(plaintext, sec1, &sb, &cipher, &err) != 0 || !cipher) { if (err) free(err); secure_free(&sb); return -1; }
      } else {
        secure_free(&sb); return -1;
      }
    }
    secure_free(&sb);
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
    if (parse_peer_xonly32(wallet_pub_hex, pkx) != 0) { secure_wipe(sk, sizeof sk); return -1; }
    uint8_t *plain = NULL; size_t plen = 0;
    if (nostr_nip44_decrypt_v2(sk, pkx, ciphertext, &plain, &plen) != 0 || !plain) { secure_wipe(sk, sizeof sk); return -1; }
    secure_wipe(sk, sizeof sk);
    char *out = (char*)malloc(plen+1); if (!out){ free(plain); return -1; }
    memcpy(out, plain, plen); out[plen] = '\0'; free(plain);
    *out_plaintext = out; return 0;
  } else { /* NIP-04 */
    char sec1[67]; const char *peer = wallet_pub_hex; char *plain = NULL; char *err = NULL;
    if (strlen(wallet_pub_hex)==64) { if (build_sec1_from_xonly(wallet_pub_hex, sec1, 0x02)==0) peer = sec1; }
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr || parse_sk32(client_sk_hex, (unsigned char*)sb.ptr) != 0) { if (sb.ptr) secure_free(&sb); return -1; }
    if (nostr_nip04_decrypt_secure(ciphertext, peer, &sb, &plain, &err) != 0 || !plain) {
      secure_free(&sb);
      if (err) { free(err); err=NULL; }
      if (peer==sec1 && build_sec1_from_xonly(wallet_pub_hex, sec1, 0x03)==0) {
        if (nostr_nip04_decrypt_secure(ciphertext, sec1, &sb, &plain, &err) != 0 || !plain) { if (err) free(err); secure_free(&sb); return -1; }
      } else {
        secure_free(&sb); return -1;
      }
    }
    secure_free(&sb);
    *out_plaintext = plain; return 0;
  }
}
