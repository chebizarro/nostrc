#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "nostr/nip5f/nip5f.h"
#include "sock_internal.h"
#include "json.h"
#include "nostr-utils.h"

struct Nip5fConn { int fd; unsigned long next_id; };

/* Local JSON helpers (kept internal to NIP-5F client) */
static char *json_str(const char *s) {
  /* Escape the string content and wrap in quotes for valid JSON string value */
  char *escaped = nostr_escape_string(s);
  if (!escaped) return NULL;
  size_t elen = strlen(escaped);
  char *quoted = (char*)malloc(elen + 3); /* 2 quotes + NUL */
  if (!quoted) { free(escaped); return NULL; }
  quoted[0] = '"';
  memcpy(quoted + 1, escaped, elen);
  quoted[elen + 1] = '"';
  quoted[elen + 2] = '\0';
  free(escaped);
  return quoted;
}

static int jsonrpc_build_req(struct Nip5fConn *c, const char *method, const char *params_raw, char **out_req, char idbuf[32]) {
  if (!c || !method || !out_req) return -1; *out_req = NULL; if (idbuf) idbuf[0] = '\0';
  snprintf(idbuf, 32, "%lu", ++c->next_id);
  const char *params = (params_raw && *params_raw) ? params_raw : "null";
  int need = snprintf(NULL, 0, "{\"id\":\"%s\",\"method\":\"%s\",\"params\":%s}", idbuf, method, params);
  if (need < 0) return -1;
  size_t L = (size_t)need + 1; char *req = (char*)malloc(L); if (!req) return -1;
  snprintf(req, L, "{\"id\":\"%s\",\"method\":\"%s\",\"params\":%s}", idbuf, method, params);
  *out_req = req; return 0;
}

static int rpc_write_and_read(struct Nip5fConn *c, const char *req, char **out_resp, size_t *out_len) {
  if (!c || !req || !out_resp) return -1; *out_resp = NULL; if (out_len) *out_len = 0;
  if (nip5f_write_frame(c->fd, req, strlen(req)) != 0) return -1;
  size_t rlen = 0; char *resp = NULL; if (nip5f_read_frame(c->fd, &resp, &rlen) != 0) return -1;
  if (out_len) *out_len = rlen; *out_resp = resp; return 0;
}

/* Minimal extractor: returns newly-allocated copy of the JSON value at top-level key "result".
 * Handles string, object, array, numbers, true/false/null. Returns NULL on parse failure. */
static char *extract_result_raw(const char *json) {
  if (!json) return NULL;
  const char *p = strstr(json, "\"result\"");
  if (!p) return NULL;
  p = strchr(p, ':'); if (!p) return NULL; p++;
  while (*p==' '||*p=='\n'||*p=='\t'||*p=='\r') p++;
  if (*p=='\0') return NULL;
  const char *start = p;
  if (*p=='\"') {
    // string: find ending quote handling escapes
    p++; int esc=0; while (*p) { if (!esc && *p=='\"') { p++; break; } esc = (!esc && *p=='\\'); if (esc) { p++; esc=0; } else p++; }
    size_t len = (size_t)(p - start);
    char *out = (char*)malloc(len+1); if (!out) return NULL;
    memcpy(out, start, len); out[len]='\0'; return out;
  } else if (*p=='{' || *p=='[') {
    char open = *p; char close = (open=='{'?'}':']'); int depth=0; int in_str=0; int esc=0;
    while (*p) {
      char c = *p++;
      if (in_str) {
        if (!esc && c=='\\') { esc=1; continue; }
        if (!esc && c=='\"') { in_str=0; }
        esc=0; continue;
      }
      if (c=='\"') { in_str=1; continue; }
      if (c==open) depth++;
      else if (c==close) { depth--; if (depth==0) break; }
    }
    size_t len = (size_t)(p - start);
    char *out = (char*)malloc(len+1); if (!out) return NULL;
    memcpy(out, start, len); out[len]='\0'; return out;
  } else {
    // literal (null,true,false,number) until ',' or '}'
    while (*p && *p!=',' && *p!='}') p++;
    size_t len = (size_t)(p - start);
    char *out = (char*)malloc(len+1); if (!out) return NULL;
    memcpy(out, start, len); out[len]='\0'; return out;
  }
}

/* Return 0 and fill out_result_raw with newly-allocated JSON node for result when error==null.
 * Return non-zero on protocol/parse error or if server returned error. */
static int parse_ok_and_get_result(const char *resp, const char *expect_id, char **out_result_raw) {
  if (!resp || !expect_id || !out_result_raw) return -1;
  *out_result_raw = NULL;
  // Validate id
  char *rid = NULL; (void)nostr_json_get_string(resp, "id", &rid);
  if (!rid || strcmp(rid, expect_id)!=0) { if (rid) free(rid); return -1; }
  free(rid);
  // Validate error is null
  const char *e = strstr(resp, "\"error\"");
  if (!e) return -1;
  const char *col = strchr(e, ':'); if (!col) return -1; col++;
  while (*col==' '||*col=='\n'||*col=='\t'||*col=='\r') col++;
  if (strncmp(col, "null", 4)!=0) {
    return -2; // server error present
  }
  // Extract "result" raw JSON value
  char *raw = extract_result_raw(resp);
  if (!raw) return -1;
  *out_result_raw = raw;
  return 0;
}

int nostr_nip5f_client_connect(const char *socket_path, void **out_conn) {
  if (!out_conn) return -1;
  struct Nip5fConn *c = (struct Nip5fConn*)calloc(1, sizeof(*c));
  if (!c) return -1;
  char *resolved = NULL;
  if (socket_path && *socket_path) resolved = strdup(socket_path);
  else resolved = nip5f_resolve_socket_path();
  if (!resolved) { free(c); return -1; }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { free(resolved); free(c); return -1; }
  struct sockaddr_un addr; memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX;
  size_t maxlen = sizeof(addr.sun_path)-1;
  strncpy(addr.sun_path, resolved, maxlen); addr.sun_path[maxlen] = '\0';
  free(resolved);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd); free(c); return -1;
  }
  // Expect banner, then send hello
  char *banner = NULL; size_t blen = 0;
  if (nip5f_read_frame(fd, &banner, &blen) != 0) { close(fd); free(c); return -1; }
  if (banner) free(banner);
  const char *hello = "{\"name\":\"nostr-client\",\"version\":1}";
  if (nip5f_write_frame(fd, hello, strlen(hello)) != 0) { close(fd); free(c); return -1; }
  c->fd = fd;
  c->next_id = 1;
  *out_conn = c;
  return 0;
}

int nostr_nip5f_client_close(void *conn) {
  if (!conn) return 0;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  if (c->fd > 0) close(c->fd);
  free(c);
  return 0;
}

int nostr_nip5f_client_get_public_key(void *conn, char **out_pub_hex) {
  if (!conn || !out_pub_hex) return -1;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  char *req = NULL; char idbuf[32]; if (jsonrpc_build_req(c, "get_public_key", "null", &req, idbuf)!=0) return -1;
  char *resp = NULL; size_t rlen = 0; if (rpc_write_and_read(c, req, &resp, &rlen)!=0) { free(req); return -1; }
  free(req);
  // Validate response id and error == null, then extract result string
  int rc = 0; char *raw = NULL;
  rc = parse_ok_and_get_result(resp, idbuf, &raw);
  if (rc!=0) { free(resp); return -1; }
  // raw may be a JSON string like "hex..."; strip quotes if present and unescape minimal
  if (raw[0]=='\"') {
    // remove surrounding quotes
    size_t L = strlen(raw);
    if (L<2 || raw[L-1] != '\"') { free(raw); free(resp); return -1; }
    char *val = (char*)malloc(L-1);
    if (!val) { free(raw); free(resp); return -1; }
    memcpy(val, raw+1, L-2); val[L-2]='\0';
    free(raw);
    *out_pub_hex = val;
  } else {
    // Unexpected non-string
    free(raw); free(resp); return -1;
  }
  free(resp);
  return 0;
}

int nostr_nip5f_client_sign_event(void *conn, const char *event_json, const char *pubkey_hex, char **out_signed_event_json) {
  if (!conn || !event_json || !out_signed_event_json) return -1; *out_signed_event_json=NULL;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  int plen = pubkey_hex ? snprintf(NULL,0, "{\"event\":%s,\"pubkey\":\"%s\"}", event_json, pubkey_hex)
                        : snprintf(NULL,0, "{\"event\":%s}", event_json);
  if (plen < 0) return -1; size_t pL = (size_t)plen + 1; char *params = (char*)malloc(pL); if (!params) return -1;
  if (pubkey_hex) snprintf(params, pL, "{\"event\":%s,\"pubkey\":\"%s\"}", event_json, pubkey_hex);
  else snprintf(params, pL, "{\"event\":%s}", event_json);
  char *req=NULL; char idbuf[32]; if (jsonrpc_build_req(c, "sign_event", params, &req, idbuf)!=0) { free(params); return -1; }
  free(params);
  char *resp=NULL; size_t rlen=0; if (rpc_write_and_read(c, req, &resp, &rlen)!=0) { free(req); return -1; }
  free(req);
  char *raw=NULL; int prc = parse_ok_and_get_result(resp, idbuf, &raw);
  free(resp);
  if (prc!=0 || !raw) return -1;
  *out_signed_event_json = raw; return 0;
}

int nostr_nip5f_client_nip44_encrypt(void *conn, const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64) {
  if (!conn || !peer_pub_hex || !plaintext || !out_cipher_b64) return -1; *out_cipher_b64=NULL;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  char *pt_esc = json_str(plaintext); if (!pt_esc) return -1;
  int plen = snprintf(NULL,0, "{\"peer_pub\":\"%s\",\"plaintext\":%s}", peer_pub_hex, pt_esc);
  if (plen < 0) { free(pt_esc); return -1; }
  size_t pL = (size_t)plen + 1; char *params = (char*)malloc(pL); if (!params) { free(pt_esc); return -1; }
  snprintf(params, pL, "{\"peer_pub\":\"%s\",\"plaintext\":%s}", peer_pub_hex, pt_esc);
  free(pt_esc);
  char *req=NULL; char idbuf[32]; if (jsonrpc_build_req(c, "nip44_encrypt", params, &req, idbuf)!=0) { free(params); return -1; }
  free(params);
  char *resp=NULL; size_t rlen=0; if (rpc_write_and_read(c, req, &resp, &rlen)!=0) { free(req); return -1; }
  free(req);
  char *raw=NULL; int prc = parse_ok_and_get_result(resp, idbuf, &raw);
  free(resp);
  if (prc!=0 || !raw) return -1;
  // Expect string
  if (raw[0]=='\"') {
    size_t L = strlen(raw); if (L<2 || raw[L-1] != '\"') { free(raw); return -1; }
    char *val = (char*)malloc(L-1); if (!val) { free(raw); return -1; }
    memcpy(val, raw+1, L-2); val[L-2]='\0'; free(raw); *out_cipher_b64 = val; return 0;
  }
  free(raw); return -1;
}

int nostr_nip5f_client_nip44_decrypt(void *conn, const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext) {
  if (!conn || !peer_pub_hex || !cipher_b64 || !out_plaintext) return -1; *out_plaintext=NULL;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  char *ct_esc = json_str(cipher_b64); if (!ct_esc) return -1;
  int plen = snprintf(NULL,0, "{\"peer_pub\":\"%s\",\"cipher_b64\":%s}", peer_pub_hex, ct_esc);
  if (plen < 0) { free(ct_esc); return -1; }
  size_t pL = (size_t)plen + 1; char *params = (char*)malloc(pL); if (!params) { free(ct_esc); return -1; }
  snprintf(params, pL, "{\"peer_pub\":\"%s\",\"cipher_b64\":%s}", peer_pub_hex, ct_esc);
  free(ct_esc);
  char *req=NULL; char idbuf[32]; if (jsonrpc_build_req(c, "nip44_decrypt", params, &req, idbuf)!=0) { free(params); return -1; }
  free(params);
  char *resp=NULL; size_t rlen=0; if (rpc_write_and_read(c, req, &resp, &rlen)!=0) { free(req); return -1; }
  free(req);
  char *raw=NULL; int prc = parse_ok_and_get_result(resp, idbuf, &raw);
  free(resp);
  if (prc!=0 || !raw) return -1;
  if (raw[0]=='\"') {
    size_t L = strlen(raw); if (L<2 || raw[L-1] != '\"') { free(raw); return -1; }
    char *val = (char*)malloc(L-1); if (!val) { free(raw); return -1; }
    memcpy(val, raw+1, L-2); val[L-2]='\0'; free(raw); *out_plaintext = val; return 0;
  }
  free(raw); return -1;
}

int nostr_nip5f_client_list_public_keys(void *conn, char **out_keys_json) {
  if (!conn || !out_keys_json) return -1; *out_keys_json=NULL;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  char *req=NULL; char idbuf[32]; if (jsonrpc_build_req(c, "list_public_keys", "null", &req, idbuf)!=0) return -1;
  char *resp=NULL; size_t rlen=0; if (rpc_write_and_read(c, req, &resp, &rlen)!=0) { free(req); return -1; }
  free(req);
  char *raw=NULL; int prc = parse_ok_and_get_result(resp, idbuf, &raw); free(resp);
  if (!raw) return -1; *out_keys_json = raw; return 0;
}
