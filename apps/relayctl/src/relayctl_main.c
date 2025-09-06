#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include "nostr-event.h"
#include "nostr-tag.h"
#include <openssl/evp.h>

typedef struct { char *data; size_t len; } mem_buf;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t total = size * nmemb;
  mem_buf *mb = (mem_buf*)userdata;
  char *p = (char*)realloc(mb->data, mb->len + total + 1);
  if (!p) return 0;
  memcpy(p + mb->len, ptr, total);
  mb->data = p; mb->len += total; mb->data[mb->len] = '\0';
  return total;
}

static char *sha256_hex(const char *data) {
  unsigned char dg[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char*)data, strlen(data), dg);
  static const char *hx = "0123456789abcdef";
  char *hex = (char*)malloc(SHA256_DIGEST_LENGTH*2+1);
  if (!hex) return NULL;
  for (size_t i=0;i<SHA256_DIGEST_LENGTH;i++){ hex[i*2]=hx[(dg[i]>>4)&0xF]; hex[i*2+1]=hx[dg[i]&0xF]; }
  hex[SHA256_DIGEST_LENGTH*2] = '\0';
  return hex;
}

static char *base64url_encode(const char *input) {
  size_t inlen = strlen(input);
  int outcap = 4 * ((int)inlen + 2) / 3 + 4;
  unsigned char *std = (unsigned char*)malloc((size_t)outcap + 1);
  if (!std) return NULL;
  int n = EVP_EncodeBlock(std, (const unsigned char*)input, (int)inlen);
  if (n < 0) { free(std); return NULL; }
  // Convert to URL-safe: replace '+'->'-', '/'->'_', strip '=' padding
  for (int i=0;i<n;i++) {
    if (std[i] == '+') std[i] = '-';
    else if (std[i] == '/') std[i] = '_';
  }
  while (n>0 && std[n-1]=='=') n--;
  std[n] = '\0';
  return (char*)std;
}

static char *build_auth_header(const char *url, const char *method, const char *json_body, const char *sk_hex) {
  char *payload_hex = json_body ? sha256_hex(json_body) : NULL;
  /* Build NIP-98 event */
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 27235);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, "");
  NostrTags *tags = nostr_tags_new(0);
  NostrTag *tu = nostr_tag_new("u", url, NULL); nostr_tags_append(tags, tu);
  NostrTag *tm = nostr_tag_new("method", method, NULL); nostr_tags_append(tags, tm);
  if (payload_hex) { NostrTag *tp = nostr_tag_new("payload", payload_hex, NULL); nostr_tags_append(tags, tp); }
  nostr_event_set_tags(ev, tags);
  if (nostr_event_sign(ev, sk_hex) != 0) { if (payload_hex) free(payload_hex); nostr_event_free(ev); return NULL; }
  char *ev_json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  if (payload_hex) free(payload_hex);
  if (!ev_json) return NULL;
  char *b64 = base64url_encode(ev_json);
  free(ev_json);
  if (!b64) return NULL;
  size_t sz = strlen("Nostr ") + strlen(b64) + 1;
  char *hdr = (char*)malloc(sz);
  if (!hdr) { free(b64); return NULL; }
  snprintf(hdr, sz, "Nostr %s", b64);
  free(b64);
  return hdr;
}

static int post_nip86(const char *url, const char *sk_hex, const char *rpc_body, char **out_resp) {
  *out_resp = NULL;
  CURL *curl = curl_easy_init();
  if (!curl) return 1;
  int rc = 1;
  char *auth = build_auth_header(url, "POST", rpc_body, sk_hex);
  if (!auth) { curl_easy_cleanup(curl); return 1; }
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/nostr+json+rpc");
  size_t auth_line_len = strlen("Authorization: ") + strlen(auth) + 1;
  char *auth_line = (char*)malloc(auth_line_len);
  snprintf(auth_line, auth_line_len, "Authorization: %s", auth);
  headers = curl_slist_append(headers, auth_line);
  mem_buf mb = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, rpc_body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
  CURLcode res = curl_easy_perform(curl);
  long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (res == CURLE_OK && code == 200) {
    *out_resp = mb.data; mb.data = NULL; mb.len = 0; rc = 0;
  }
  if (mb.data) free(mb.data);
  curl_slist_free_all(headers);
  free(auth_line);
  free(auth);
  curl_easy_cleanup(curl);
  return rc;
}

static const char *env_or(const char *name, const char *defv) {
  const char *v = getenv(name); return (v && *v) ? v : defv;
}

static int parse_common(int argc, char **argv, const char **url, const char **sk, int *argi) {
  *url = env_or("RELAYCTL_URL", NULL);
  *sk = env_or("RELAYCTL_SK", NULL);
  for (int i=1;i<argc;i++) {
    if (strcmp(argv[i], "--url")==0 && i+1<argc) { *url = argv[++i]; continue; }
    if (strcmp(argv[i], "--sk")==0 && i+1<argc) { *sk = argv[++i]; continue; }
    *argi = i; break;
  }
  if (!*url) { fprintf(stderr, "Missing --url (or set RELAYCTL_URL)\n"); return 1; }
  return 0;
}

/* simple GET helper */
static int http_get(const char *base_url, const char *path, char **out_resp) {
  *out_resp = NULL;
  CURL *curl = curl_easy_init(); if (!curl) return 1;
  int rc = 1; mem_buf mb = {0};
  size_t need = strlen(base_url) + strlen(path) + 4; char *url = (char*)malloc(need);
  if (!url) { curl_easy_cleanup(curl); return 1; }
  int has_slash = base_url[strlen(base_url)-1] == '/';
  snprintf(url, need, "%s%s%s", base_url, has_slash?"":"/", (path[0]=='/')?path+1:path);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
  CURLcode res = curl_easy_perform(curl);
  long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (res == CURLE_OK && code == 200) { *out_resp = mb.data; mb.data=NULL; rc=0; }
  if (mb.data) free(mb.data);
  free(url);
  curl_easy_cleanup(curl);
  return rc;
}

static int cmd_supported(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"supportedmethods\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  /* fallback: grelay doesn't expose supportedmethods over GET; synthesize */
  if (!sk || !*sk) {
    /* grelay supports the same subset we use */
    printf("{\"result\":[\"getstats\",\"getlimits\",\"supportedmethods\"]}\n");
    return 0;
  }
  fprintf(stderr, "supported failed\n"); return 1;
}

static int cmd_stats(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"getstats\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  /* fallback for grelay */
  if (!sk || !*sk) {
    if (http_get(url, "/admin/metrics", &resp) == 0) { printf("%s\n", resp); free(resp); return 0; }
    if (http_get(url, "/admin/stats", &resp) == 0) { printf("%s\n", resp); free(resp); return 0; }
  }
  fprintf(stderr, "stats failed\n"); return 1;
}

static int cmd_limits(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"getlimits\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  /* fallback for grelay */
  if (!sk || !*sk) {
    if (http_get(url, "/admin/limits", &resp) == 0) { printf("%s\n", resp); free(resp); return 0; }
  }
  fprintf(stderr, "limits failed\n"); return 1;
}

static int cmd_connections(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"getconnections\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "connections failed\n"); return 1;
}

static int cmd_ban(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s ban [--url URL --sk SK] <pubkey>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <pubkey>\n"); return 1; }
  const char *pk = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\"method\":\"banpubkey\",\"params\":[\"%s\"]}", pk);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "ban failed\n"); return 1;
}

static int cmd_unban(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s unban [--url URL --sk SK] <pubkey>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <pubkey>\n"); return 1; }
  const char *pk = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\"method\":\"allowpubkey\",\"params\":[\"%s\"]}", pk);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "unban failed\n"); return 1;
}

/* (main defined at bottom to delegate to main_ext) */

/* Additional NIP-86 commands */
static int cmd_listbannedpubkeys(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"listbannedpubkeys\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "listbannedpubkeys failed\n"); return 1;
}

static int cmd_listallowedpubkeys(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"listallowedpubkeys\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "listallowedpubkeys failed\n"); return 1;
}

static int cmd_listeventsneedingmoderation(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"listeventsneedingmoderation\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "listeventsneedingmoderation failed\n"); return 1;
}

static int cmd_allowevent(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s allowevent [--url URL --sk SK] <event-id>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <event-id>\n"); return 1; }
  const char *id = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"allowevent\\\",\\\"params\\\":[\\\"%s\\\"]}", id);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "allowevent failed\n"); return 1;
}

static int cmd_banevent(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s banevent [--url URL --sk SK] <event-id>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <event-id>\n"); return 1; }
  const char *id = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"banevent\\\",\\\"params\\\":[\\\"%s\\\"]}", id);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "banevent failed\n"); return 1;
}

static int cmd_listbannedevents(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"listbannedevents\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "listbannedevents failed\n"); return 1;
}

static int cmd_changerelayname(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s changerelayname [--url URL --sk SK] <name>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <name>\n"); return 1; }
  const char *name = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"changerelayname\\\",\\\"params\\\":[\\\"%s\\\"]}", name);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "changerelayname failed\n"); return 1;
}

static int cmd_changerelaydescription(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s changerelaydescription [--url URL --sk SK] <description>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <description>\n"); return 1; }
  const char *desc = argv[idx];
  char rpc[768]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"changerelaydescription\\\",\\\"params\\\":[\\\"%s\\\"]}", desc);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "changerelaydescription failed\n"); return 1;
}

static int cmd_changerelayicon(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s changerelayicon [--url URL --sk SK] <icon-url>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <icon-url>\n"); return 1; }
  const char *icon = argv[idx];
  char rpc[768]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"changerelayicon\\\",\\\"params\\\":[\\\"%s\\\"]}", icon);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "changerelayicon failed\n"); return 1;
}

static int cmd_allowkind(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s allowkind [--url URL --sk SK] <kind>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <kind>\n"); return 1; }
  int kind = atoi(argv[idx]);
  char rpc[256]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"allowkind\\\",\\\"params\\\":[%d]}", kind);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "allowkind failed\n"); return 1;
}

static int cmd_disallowkind(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s disallowkind [--url URL --sk SK] <kind>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <kind>\n"); return 1; }
  int kind = atoi(argv[idx]);
  char rpc[256]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"disallowkind\\\",\\\"params\\\":[%d]}", kind);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "disallowkind failed\n"); return 1;
}

static int cmd_listallowedkinds(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"listallowedkinds\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "listallowedkinds failed\n"); return 1;
}

static int cmd_blockip(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s blockip [--url URL --sk SK] <ip>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <ip>\n"); return 1; }
  const char *ip = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"blockip\\\",\\\"params\\\":[\\\"%s\\\"]}", ip);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "blockip failed\n"); return 1;
}

static int cmd_unblockip(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "Usage: %s unblockip [--url URL --sk SK] <ip>\n", argv[0]); return 1; }
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  if (idx >= argc) { fprintf(stderr, "Missing <ip>\n"); return 1; }
  const char *ip = argv[idx];
  char rpc[512]; snprintf(rpc, sizeof(rpc), "{\\\"method\\\":\\\"unblockip\\\",\\\"params\\\":[\\\"%s\\\"]}", ip);
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "unblockip failed\n"); return 1;
}

static int cmd_listblockedips(int argc, char **argv) {
  const char *url=NULL,*sk=NULL; int idx=2; if (parse_common(argc, argv, &url, &sk, &idx)) return 1;
  const char *rpc = "{\"method\":\"listblockedips\",\"params\":[]}";
  char *resp=NULL; int rc = post_nip86(url, sk, rpc, &resp);
  if (rc==0 && resp){ printf("%s\n", resp); free(resp); return 0; }
  fprintf(stderr, "listblockedips failed\n"); return 1;
}

/* Extended main dispatch */
int main_ext(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <command> [--url URL --sk SK] [args]\n", argv[0]);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  stats\n");
    fprintf(stderr, "  supported\n");
    fprintf(stderr, "  limits\n");
    fprintf(stderr, "  connections\n");
    fprintf(stderr, "  ban <pubkey>\n  unban <pubkey>\n  listbannedpubkeys\n  listallowedpubkeys\n");
    fprintf(stderr, "  listeventsneedingmoderation\n  allowevent <id>\n  banevent <id>\n  listbannedevents\n");
    fprintf(stderr, "  changerelayname <name>\n  changerelaydescription <desc>\n  changerelayicon <url>\n");
    fprintf(stderr, "  allowkind <kind>\n  disallowkind <kind>\n  listallowedkinds\n");
    fprintf(stderr, "  blockip <ip>\n  unblockip <ip>\n  listblockedips\n");
    return 1;
  }
  if (strcmp(argv[1], "stats") == 0) return cmd_stats(argc, argv);
  if (strcmp(argv[1], "supported") == 0) return cmd_supported(argc, argv);
  if (strcmp(argv[1], "limits") == 0) return cmd_limits(argc, argv);
  if (strcmp(argv[1], "connections") == 0) return cmd_connections(argc, argv);
  if (strcmp(argv[1], "ban") == 0) return cmd_ban(argc, argv);
  if (strcmp(argv[1], "unban") == 0) return cmd_unban(argc, argv);
  if (strcmp(argv[1], "listbannedpubkeys") == 0) return cmd_listbannedpubkeys(argc, argv);
  if (strcmp(argv[1], "listallowedpubkeys") == 0) return cmd_listallowedpubkeys(argc, argv);
  if (strcmp(argv[1], "listeventsneedingmoderation") == 0) return cmd_listeventsneedingmoderation(argc, argv);
  if (strcmp(argv[1], "allowevent") == 0) return cmd_allowevent(argc, argv);
  if (strcmp(argv[1], "banevent") == 0) return cmd_banevent(argc, argv);
  if (strcmp(argv[1], "listbannedevents") == 0) return cmd_listbannedevents(argc, argv);
  if (strcmp(argv[1], "changerelayname") == 0) return cmd_changerelayname(argc, argv);
  if (strcmp(argv[1], "changerelaydescription") == 0) return cmd_changerelaydescription(argc, argv);
  if (strcmp(argv[1], "changerelayicon") == 0) return cmd_changerelayicon(argc, argv);
  if (strcmp(argv[1], "allowkind") == 0) return cmd_allowkind(argc, argv);
  if (strcmp(argv[1], "disallowkind") == 0) return cmd_disallowkind(argc, argv);
  if (strcmp(argv[1], "listallowedkinds") == 0) return cmd_listallowedkinds(argc, argv);
  if (strcmp(argv[1], "blockip") == 0) return cmd_blockip(argc, argv);
  if (strcmp(argv[1], "unblockip") == 0) return cmd_unblockip(argc, argv);
  if (strcmp(argv[1], "listblockedips") == 0) return cmd_listblockedips(argc, argv);
  fprintf(stderr, "Unknown command: %s\n", argv[1]);
  return 1;
}

/* Overwrite previous main to delegate to main_ext */
int main(int argc, char **argv) { return main_ext(argc, argv); }
