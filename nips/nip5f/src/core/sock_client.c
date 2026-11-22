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

/* Validate a response frame with the JSON parser: its "id" must echo @expect_id
 * and its "error" member must be present and null. Scanning the frame text for
 * "error"/"result" is not safe: a string result containing the escaped text
 * \"error\" would be mistaken for the member. */
static int check_response(const char *resp, const char *expect_id) {
  char *rid = NULL;
  if (nostr_json_get_string(resp, "id", &rid) != 0 || !rid) return -1;
  int same = strcmp(rid, expect_id) == 0;
  free(rid);
  return same && nostr_json_get_type(resp, "error") == NOSTR_JSON_NULL ? 0 : -1;
}

/* Send one request and return its result, which must have JSON type @want:
 * a string result is returned fully unescaped, an object/array result as
 * compact JSON. */
static int rpc_call(struct Nip5fConn *c, const char *method, const char *params,
                    NostrJsonType want, char **out) {
  *out = NULL;
  char *req = NULL; char idbuf[32];
  if (jsonrpc_build_req(c, method, params, &req, idbuf) != 0) return -1;
  char *resp = NULL; size_t rlen = 0;
  int rc = rpc_write_and_read(c, req, &resp, &rlen);
  free(req);
  if (rc != 0) return -1;
  rc = check_response(resp, idbuf);
  if (rc == 0 && want == NOSTR_JSON_STRING)
    rc = nostr_json_get_string(resp, "result", out);
  else if (rc == 0)
    rc = nostr_json_get_type(resp, "result") == want ? nostr_json_get_raw(resp, "result", out) : -1;
  free(resp);
  if (rc == 0 && *out) return 0;
  free(*out); *out = NULL;
  return -1;
}

/* params {"peer_pub":<peer>,"<key>":<value>} with both values JSON-escaped. */
static char *peer_params(const char *peer_pub_hex, const char *key, const char *value) {
  char *peer = json_str(peer_pub_hex), *val = json_str(value);
  char *params = NULL;
  if (peer && val) {
    int n = snprintf(NULL, 0, "{\"peer_pub\":%s,\"%s\":%s}", peer, key, val);
    if (n >= 0 && (params = (char*)malloc((size_t)n + 1)) != NULL)
      snprintf(params, (size_t)n + 1, "{\"peer_pub\":%s,\"%s\":%s}", peer, key, val);
  }
  free(peer); free(val);
  return params;
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
#ifdef SO_NOSIGPIPE
  /* macOS/BSD: avoid SIGPIPE if the server rejects auth and closes the fd */
  { int on = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
  struct sockaddr_un addr; memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX;
  size_t maxlen = sizeof(addr.sun_path)-1;
  if (strlen(resolved) > maxlen) {
    close(fd); free(resolved); free(c); errno = ENAMETOOLONG; return -1;
  }
  memcpy(addr.sun_path, resolved, strlen(resolved) + 1);
  free(resolved);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd); free(c); return -1;
  }
  // Expect banner, then send hello
  char *banner = NULL; size_t blen = 0;
  if (nip5f_read_frame(fd, &banner, &blen) != 0) { close(fd); free(c); return -1; }
  if (banner) free(banner);
  /* Present a bearer token in the hello when one is configured on the CLIENT side.
   * Prefer NOSTR_SIGNER_CLIENT_AUTH_TOKEN so a client can hold a token distinct from
   * (or absent when) the server's NOSTR_SIGNER_AUTH_TOKEN; fall back to
   * NOSTR_SIGNER_AUTH_TOKEN for the common single-env case. */
  const char *ctok = getenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
  if (!ctok || !*ctok) ctok = getenv("NOSTR_SIGNER_AUTH_TOKEN");
  char *hello_buf = NULL;
  if (ctok && *ctok) {
    char *tok_json = json_str(ctok); /* returns a quoted+escaped JSON string */
    if (!tok_json) { close(fd); free(c); return -1; }
    int need = snprintf(NULL, 0, "{\"name\":\"nostr-client\",\"version\":1,\"auth_token\":%s}", tok_json);
    if (need < 0) { free(tok_json); close(fd); free(c); return -1; }
    hello_buf = (char*)malloc((size_t)need + 1);
    if (!hello_buf) { free(tok_json); close(fd); free(c); return -1; }
    snprintf(hello_buf, (size_t)need + 1, "{\"name\":\"nostr-client\",\"version\":1,\"auth_token\":%s}", tok_json);
    free(tok_json);
  } else {
    hello_buf = strdup("{\"name\":\"nostr-client\",\"version\":1}");
    if (!hello_buf) { close(fd); free(c); return -1; }
  }
  if (nip5f_write_frame(fd, hello_buf, strlen(hello_buf)) != 0) { free(hello_buf); close(fd); free(c); return -1; }
  free(hello_buf);
  c->fd = fd;
  c->next_id = 1;
  *out_conn = c;
  return 0;
}

int nostr_nip5f_client_close(void *conn) {
  if (!conn) return 0;
  struct Nip5fConn *c = (struct Nip5fConn*)conn;
  if (c->fd >= 0) close(c->fd);
  free(c);
  return 0;
}

int nostr_nip5f_client_get_public_key(void *conn, char **out_pub_hex) {
  if (!conn || !out_pub_hex) return -1; *out_pub_hex = NULL;
  return rpc_call((struct Nip5fConn*)conn, "get_public_key", "null", NOSTR_JSON_STRING, out_pub_hex);
}

int nostr_nip5f_client_sign_event(void *conn, const char *event_json, const char *pubkey_hex, char **out_signed_event_json) {
  if (!conn || !event_json || !out_signed_event_json) return -1; *out_signed_event_json=NULL;
  char *pk = pubkey_hex ? json_str(pubkey_hex) : NULL;
  if (pubkey_hex && !pk) return -1;
  int plen = pk ? snprintf(NULL,0, "{\"event\":%s,\"pubkey\":%s}", event_json, pk)
                : snprintf(NULL,0, "{\"event\":%s}", event_json);
  char *params = plen < 0 ? NULL : (char*)malloc((size_t)plen + 1);
  if (!params) { free(pk); return -1; }
  if (pk) snprintf(params, (size_t)plen + 1, "{\"event\":%s,\"pubkey\":%s}", event_json, pk);
  else snprintf(params, (size_t)plen + 1, "{\"event\":%s}", event_json);
  free(pk);
  int rc = rpc_call((struct Nip5fConn*)conn, "sign_event", params, NOSTR_JSON_OBJECT, out_signed_event_json);
  free(params);
  return rc;
}

int nostr_nip5f_client_nip44_encrypt(void *conn, const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64) {
  if (!conn || !peer_pub_hex || !plaintext || !out_cipher_b64) return -1; *out_cipher_b64=NULL;
  char *params = peer_params(peer_pub_hex, "plaintext", plaintext);
  if (!params) return -1;
  int rc = rpc_call((struct Nip5fConn*)conn, "nip44_encrypt", params, NOSTR_JSON_STRING, out_cipher_b64);
  free(params);
  return rc;
}

int nostr_nip5f_client_nip44_decrypt(void *conn, const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext) {
  if (!conn || !peer_pub_hex || !cipher_b64 || !out_plaintext) return -1; *out_plaintext=NULL;
  char *params = peer_params(peer_pub_hex, "cipher_b64", cipher_b64);
  if (!params) return -1;
  int rc = rpc_call((struct Nip5fConn*)conn, "nip44_decrypt", params, NOSTR_JSON_STRING, out_plaintext);
  free(params);
  return rc;
}

int nostr_nip5f_client_list_public_keys(void *conn, char **out_keys_json) {
  if (!conn || !out_keys_json) return -1; *out_keys_json=NULL;
  return rpc_call((struct Nip5fConn*)conn, "list_public_keys", "null", NOSTR_JSON_ARRAY, out_keys_json);
}
