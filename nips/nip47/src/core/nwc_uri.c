#include "nostr/nip47/nwc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static void secure_free(char *p) {
  if (!p) return;
  size_t n = strlen(p);
  memset(p, 0, n);
  free(p);
}

void nostr_nwc_connection_clear(NostrNwcConnection *c) {
  if (!c) return;
  secure_free(c->wallet_pubkey_hex); c->wallet_pubkey_hex = NULL;
  if (c->relays) {
    for (size_t i=0; c->relays[i]; ++i) secure_free(c->relays[i]);
    free(c->relays);
    c->relays = NULL;
  }
  secure_free(c->secret_hex); c->secret_hex = NULL;
  secure_free(c->lud16); c->lud16 = NULL;
}

static int is_hexstr(const char *s, size_t want_len) {
  if (!s) return 0;
  size_t n = strlen(s);
  if (want_len && n != want_len) return 0;
  for (size_t i=0;i<n;i++) {
    if (!isxdigit((unsigned char)s[i])) return 0;
  }
  return 1;
}

static int from_hex(char c) {
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return 10 + (c-'a');
  if (c>='A'&&c<='F') return 10 + (c-'A');
  return -1;
}

static char *pct_decode(const char *in) {
  size_t n = strlen(in);
  char *out = (char*)malloc(n+1);
  if (!out) return NULL;
  size_t oi=0;
  for (size_t i=0;i<n;i++) {
    if (in[i]=='%' && i+2<n) {
      int h = from_hex(in[i+1]);
      int l = from_hex(in[i+2]);
      if (h>=0 && l>=0) { out[oi++] = (char)((h<<4)|l); i+=2; continue; }
    }
    out[oi++] = in[i];
  }
  out[oi]=0;
  return out;
}

static int is_unreserved(char c){
  return (isalnum((unsigned char)c) || c=='-'||c=='_'||c=='.'||c=='~');
}

static char *pct_encode(const char *in) {
  size_t n = strlen(in);
  /* Worst case: every char encoded as %XX */
  char *out = (char*)malloc(n*3 + 1);
  if (!out) return NULL;
  size_t oi=0;
  for (size_t i=0;i<n;i++) {
    unsigned char c = (unsigned char)in[i];
    if (is_unreserved((char)c)) {
      out[oi++] = (char)c;
    } else {
      static const char *hex = "0123456789ABCDEF";
      out[oi++]='%'; out[oi++]=hex[(c>>4)&0xF]; out[oi++]=hex[c&0xF];
    }
  }
  out[oi]=0;
  return out;
}

int nostr_nwc_uri_parse(const char *uri, NostrNwcConnection *out) {
  if (!uri || !out) return -1;
  memset(out, 0, sizeof(*out));
  const char *scheme = "nostr+walletconnect://";
  size_t slen = strlen(scheme);
  if (strncmp(uri, scheme, slen) != 0) return -1;

  const char *p = uri + slen;
  /* initialize relay accumulators early so fail path is safe */
  size_t rel_count = 0; size_t rel_cap = 0; char **rels = NULL;
  /* wallet pubkey until '?' or end */
  const char *q = strchr(p, '?');
  size_t wlen = q ? (size_t)(q - p) : strlen(p);
  if (wlen == 0) goto fail;
  char *wallet = (char*)malloc(wlen+1);
  if (!wallet) goto fail;
  memcpy(wallet, p, wlen); wallet[wlen]=0;
  if (!is_hexstr(wallet, 64)) { secure_free(wallet); goto fail; }
  out->wallet_pubkey_hex = wallet;

  if (!q) goto require_secret; /* no query: will fail later due to missing secret */
  p = q + 1;
  /* Parse query params: relay=..., secret=..., lud16=... */
  while (*p) {
    const char *amp = strchr(p, '&');
    size_t kvlen = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = memchr(p, '=', kvlen);
    const char *key = p; size_t klen = eq ? (size_t)(eq - p) : kvlen;
    const char *val = eq ? (eq + 1) : (p + klen);
    size_t vlen = kvlen - (eq ? (klen + 1) : klen);

    if (klen==5 && strncmp(key, "relay", 5)==0 && vlen>0) {
      char *enc = (char*)malloc(vlen+1); if(!enc) goto fail;
      memcpy(enc, val, vlen); enc[vlen]=0;
      char *dec = pct_decode(enc); secure_free(enc);
      if (!dec) goto fail;
      /* append to relays */
      if (rel_count+1 >= rel_cap) {
        size_t ncap = rel_cap? rel_cap*2 : 4;
        char **tmp = (char**)realloc(rels, sizeof(char*)*ncap);
        if (!tmp) { secure_free(dec); goto fail; }
        rels = tmp; rel_cap = ncap;
      }
      rels[rel_count++] = dec;
      rels[rel_count] = NULL;
    } else if (klen==6 && strncmp(key, "secret", 6)==0 && vlen>0) {
      char *s = (char*)malloc(vlen+1); if(!s) goto fail;
      memcpy(s, val, vlen); s[vlen]=0;
      if (!is_hexstr(s, 64)) { secure_free(s); goto fail; }
      out->secret_hex = s;
    } else if (klen==5 && strncmp(key, "lud16", 5)==0 && vlen>0) {
      char *enc = (char*)malloc(vlen+1); if(!enc) goto fail;
      memcpy(enc, val, vlen); enc[vlen]=0;
      char *dec = pct_decode(enc); secure_free(enc);
      if (!dec) goto fail;
      out->lud16 = dec;
    }

    if (!amp) break; else p = amp + 1;
  }

  if (rel_count>0) {
    out->relays = rels;
  } else if (rels) {
    free(rels);
  }

require_secret:
  if (!out->secret_hex) goto fail;
  return 0;

fail:
  if (rels) {
    for (size_t i=0;i<rel_count;i++) secure_free(rels[i]);
    free(rels);
  }
  nostr_nwc_connection_clear(out);
  memset(out, 0, sizeof(*out));
  return -1;
}

int nostr_nwc_uri_build(const NostrNwcConnection *in, char **out_uri) {
  if (!in || !out_uri || !in->wallet_pubkey_hex || !in->secret_hex) return -1;
  if (!is_hexstr(in->wallet_pubkey_hex, 64)) return -1;
  if (!is_hexstr(in->secret_hex, 64)) return -1;
  size_t sz = strlen("nostr+walletconnect://") + strlen(in->wallet_pubkey_hex) + 1 + strlen("secret=") + strlen(in->secret_hex) + 1;
  /* account for relays and lud16 percent-encoding */
  if (in->relays) {
    for (size_t i=0; in->relays[i]; ++i) {
      char *enc = pct_encode(in->relays[i]); if (!enc) return -1;
      sz += strlen("&relay=") + strlen(enc);
      free(enc);
    }
  }
  if (in->lud16) {
    char *enc = pct_encode(in->lud16); if (!enc) return -1;
    sz += strlen("&lud16=") + strlen(enc);
    free(enc);
  }
  char *buf = (char*)malloc(sz + 1);
  if (!buf) return -1;
  int n = snprintf(buf, sz + 1, "nostr+walletconnect://%s?secret=%s", in->wallet_pubkey_hex, in->secret_hex);
  if (n < 0) { free(buf); return -1; }
  if (in->relays) {
    for (size_t i=0; in->relays[i]; ++i) {
      char *enc = pct_encode(in->relays[i]); if (!enc) { free(buf); return -1; }
      size_t cur = strlen(buf);
      snprintf(buf + cur, sz + 1 - cur, "&relay=%s", enc);
      free(enc);
    }
  }
  if (in->lud16) {
    char *enc = pct_encode(in->lud16); if (!enc) { free(buf); return -1; }
    size_t cur = strlen(buf);
    snprintf(buf + cur, sz + 1 - cur, "&lud16=%s", enc);
    free(enc);
  }
  *out_uri = buf;
  return 0;
}
