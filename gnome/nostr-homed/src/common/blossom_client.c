#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "nostr_blossom.h"
#include "nostr_dbus.h"
#include <openssl/evp.h>
#include <gio/gio.h>

/* Base64url encode (no padding). Caller must free result. */
static char *base64url_encode(const unsigned char *data, size_t len){
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t olen = ((len + 2) / 3) * 4 + 1;
  char *out = (char*)malloc(olen); if (!out) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < len; ){
    unsigned int a = i < len ? data[i++] : 0;
    unsigned int b = i < len ? data[i++] : 0;
    unsigned int c = i < len ? data[i++] : 0;
    unsigned int triple = (a << 16) | (b << 8) | c;
    size_t rem = len - (i - 3);
    out[j++] = b64[(triple >> 18) & 0x3F];
    out[j++] = b64[(triple >> 12) & 0x3F];
    out[j++] = (rem > 1) ? b64[(triple >> 6) & 0x3F] : '\0';
    out[j++] = (rem > 2) ? b64[(triple >> 0) & 0x3F] : '\0';
  }
  /* Trim trailing nulls from no-padding */
  while (j > 0 && out[j-1] == '\0') j--;
  out[j] = '\0';
  /* Replace + with -, / with _ for base64url */
  for (size_t i = 0; i < j; i++){
    if (out[i] == '+') out[i] = '-';
    else if (out[i] == '/') out[i] = '_';
  }
  return out;
}

static size_t sink(void *ptr, size_t size, size_t nmemb, void *userdata){
  (void)ptr; (void)userdata; return size*nmemb;
}

static int sha256_file_hex(const char *path, char out_hex[65]){
  int ret = -1;
  FILE *fp = fopen(path, "rb"); if (!fp) return -1;
  unsigned char buf[8192]; unsigned char dig[32];
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new(); if (!mdctx){ fclose(fp); return -1; }
  const EVP_MD *md = EVP_sha256();
  if (EVP_DigestInit_ex(mdctx, md, NULL) != 1) goto done;
  for(;;){
    size_t n = fread(buf, 1, sizeof buf, fp);
    if (n > 0){ if (EVP_DigestUpdate(mdctx, buf, n) != 1) goto done; }
    if (n < sizeof buf){ if (ferror(fp)) goto done; break; }
  }
  unsigned int dl = 0;
  if (EVP_DigestFinal_ex(mdctx, dig, &dl) != 1 || dl != 32) goto done;
  {
    static const char *hex = "0123456789abcdef";
    for (unsigned int i = 0; i < dl; i++){
      out_hex[i*2] = hex[(dig[i] >> 4) & 0xF];
      out_hex[i*2+1] = hex[dig[i] & 0xF];
    }
    out_hex[64] = '\0';
  }
  ret = 0;
done:
  EVP_MD_CTX_free(mdctx);
  fclose(fp);
  return ret;
}

typedef struct { FILE *fp; } upload_src_t;
static size_t file_read(void *ptr, size_t size, size_t nmemb, void *userdata){
  upload_src_t *s = (upload_src_t*)userdata;
  if (!s || !s->fp) return 0;
  return fread(ptr, size, nmemb, s->fp);
}

/**
 * Build a NIP-98 Authorization header for Blossom upload.
 *
 * Creates a kind-27235 event with:
 *   tags: [["u",url],["method","PUT"],["payload",sha256hex]]
 *   content: ""
 *
 * Signs via org.nostr.Signer.SignEvent, then base64url-encodes the
 * signed event JSON. Returns "Nostr <b64url>" or NULL on failure.
 * Caller must free the result.
 */
static char *build_nip98_auth_header(const char *url, const char *sha256hex){
  if (!url || !sha256hex) return NULL;

  int64_t now = (int64_t)time(NULL);
  char event_json[2048];
  snprintf(event_json, sizeof event_json,
    "{\"kind\":27235,\"created_at\":%lld,"
    "\"tags\":[[\"u\",\"%s\"],[\"method\",\"PUT\"],[\"payload\",\"%s\"]],"
    "\"content\":\"\"}",
    (long long)now, url, sha256hex);

  const char *busname = nh_signer_bus_name();
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    if (err) g_error_free(err);
    return NULL;
  }

  GVariant *ret = g_dbus_connection_call_sync(bus, busname,
    "/org/nostr/signer", "org.nostr.Signer", "SignEvent",
    g_variant_new("(sss)", event_json, "", "nostr-homed"),
    G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

  if (!ret){
    if (err) g_error_free(err);
    g_object_unref(bus);
    return NULL;
  }

  const char *sig = NULL;
  g_variant_get(ret, "(&s)", &sig);

  /* Build the signed event JSON with the signature included.
   * Re-serialize with sig field appended. */
  char signed_json[4096];
  snprintf(signed_json, sizeof signed_json,
    "{\"kind\":27235,\"created_at\":%lld,"
    "\"tags\":[[\"u\",\"%s\"],[\"method\",\"PUT\"],[\"payload\",\"%s\"]],"
    "\"content\":\"\",\"sig\":\"%s\"}",
    (long long)now, url, sha256hex, sig ? sig : "");

  g_variant_unref(ret);
  g_object_unref(bus);

  /* Base64url encode the signed event JSON */
  char *b64 = base64url_encode((const unsigned char*)signed_json,
                                strlen(signed_json));
  if (!b64) return NULL;

  /* Build "Nostr <b64url>" header value */
  size_t hlen = 7 + strlen(b64) + 1;
  char *header = (char*)malloc(hlen);
  if (!header){ free(b64); return NULL; }
  snprintf(header, hlen, "Nostr %s", b64);
  free(b64);
  return header;
}

int nh_blossom_upload(const char *base_url, const char *src_path, char **out_cid){
  if (out_cid) *out_cid = NULL;
  if (!base_url || !src_path || !out_cid) return -1;
  char cid[65]; if (sha256_file_hex(src_path, cid) != 0) return -1;
  CURL *c = curl_easy_init(); if (!c) return -1;
  char url[1024]; snprintf(url, sizeof url, "%s/%s", base_url, cid);
  upload_src_t s = {0};
  s.fp = fopen(src_path, "rb");
  if (!s.fp){ curl_easy_cleanup(c); return -1; }
  struct stat st;
  if (stat(src_path, &st) != 0){ fclose(s.fp); curl_easy_cleanup(c); return -1; }

  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_READFUNCTION, file_read);
  curl_easy_setopt(c, CURLOPT_READDATA, &s);
  curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);

  /* NIP-98 auth header — best effort; upload proceeds without if signer unavailable */
  struct curl_slist *headers = NULL;
  char *auth_val = build_nip98_auth_header(url, cid);
  if (auth_val){
    char auth_header[4096];
    snprintf(auth_header, sizeof auth_header, "Authorization: %s", auth_val);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    free(auth_val);
  }

  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  if (rc == CURLE_OK)
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  fclose(s.fp);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  if (rc == CURLE_OK && (code == 200 || code == 201 || code == 204)){
    *out_cid = strdup(cid);
    return *out_cid ? 0 : -1;
  }
  return -1;
}

int nh_blossom_head(const char *base_url, const char *cid){
  if (!base_url || !cid) return -1;
  CURL *c = curl_easy_init(); if (!c) return -1;
  char url[1024]; snprintf(url, sizeof url, "%s/%s", base_url, cid);
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  if (rc == CURLE_OK)
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  return (rc == CURLE_OK && code == 200) ? 0 : -1;
}

/* Streaming SHA-256 write callback for fetch verification */
typedef struct {
  FILE *fp;
  EVP_MD_CTX *mdctx;
} verified_sink_t;

static size_t verified_write(void *ptr, size_t size, size_t nmemb, void *userdata){
  verified_sink_t *vs = (verified_sink_t*)userdata;
  if (!vs || !vs->fp) return 0;
  size_t total = size * nmemb;
  size_t written = fwrite(ptr, size, nmemb, vs->fp);
  if (written == nmemb && vs->mdctx)
    EVP_DigestUpdate(vs->mdctx, ptr, total);
  return written;
}

static int ensure_parent_dirs(const char *path){
  char tmp[1024];
  size_t len = strnlen(path, sizeof tmp);
  if (len >= sizeof tmp) return -1;
  memcpy(tmp, path, len+1);
  char *p = strrchr(tmp, '/'); if (!p) return 0; *p = 0;
  if (tmp[0] == '\0') return 0;
  for (char *q = tmp+1; *q; q++){
    if (*q == '/'){ *q = 0; (void)mkdir(tmp, 0700); *q = '/'; }
  }
  if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
  return 0;
}

int nh_blossom_fetch(const char *base_url, const char *cid, const char *dest_path){
  if (!base_url || !cid || !dest_path) return -1;
  if (ensure_parent_dirs(dest_path) != 0) return -1;
  char url[1024]; snprintf(url, sizeof url, "%s/%s", base_url, cid);
  char tmppath[1060]; snprintf(tmppath, sizeof tmppath, "%s.tmp", dest_path);
  CURL *c = curl_easy_init(); if (!c) return -1;
  FILE *fp = fopen(tmppath, "wb");
  if (!fp){ curl_easy_cleanup(c); return -1; }

  /* Set up streaming SHA-256 verification */
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
  if (mdctx)
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);

  verified_sink_t vs = { .fp = fp, .mdctx = mdctx };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, verified_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &vs);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
  CURLcode rc = curl_easy_perform(c);
  int ret = -1;

  if (rc == CURLE_OK){
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp); fp = NULL;

    /* Verify SHA-256 of downloaded content matches requested cid */
    if (mdctx){
      unsigned char dig[32]; unsigned int dl = 0;
      if (EVP_DigestFinal_ex(mdctx, dig, &dl) == 1 && dl == 32){
        char hex[65];
        static const char *hx = "0123456789abcdef";
        for (unsigned int i = 0; i < 32; i++){
          hex[i*2] = hx[(dig[i] >> 4) & 0xF];
          hex[i*2+1] = hx[dig[i] & 0xF];
        }
        hex[64] = '\0';
        if (strcmp(hex, cid) == 0){
          /* Hash matches — safe to promote */
          if (rename(tmppath, dest_path) == 0)
            ret = 0;
        } else {
          fprintf(stderr, "blossom_fetch: SHA-256 mismatch for %s (got %s, expected %s)\n",
                  url, hex, cid);
        }
      }
    } else {
      /* No hash context — fall back to unverified (shouldn't happen) */
      if (rename(tmppath, dest_path) == 0)
        ret = 0;
    }
  }

  if (fp) fclose(fp);
  if (mdctx) EVP_MD_CTX_free(mdctx);
  curl_easy_cleanup(c);
  if (ret != 0) unlink(tmppath);
  return ret;
}
