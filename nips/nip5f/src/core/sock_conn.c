/**
 * NIP-5F Socket Connection Handler
 *
 * One thread per connection: handshake, then request frames until the
 * client disconnects. With server hooks installed (gnostr-signer-daemon)
 * every method goes through hooks.request, which decides per caller; the
 * built-in handlers (keys from the server's environment) serve only servers
 * started without hooks, such as nostr-signer-sockd.
 *
 * Migrated from jansson to NostrJsonInterface (nostrc-3nj)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "sock_internal.h"
#include "nostr/nip5f/nip5f.h"
#include "sock_conn.h"
#include "json.h"

/* Optional logging: enable by setting NOSTR_SIGNER_LOG=1. Method names and
 * ids only: request and result bodies may carry plaintext. */
static int signer_log_enabled(void) {
  static int inited = 0; static int enabled = 0;
  if (!inited) {
    const char *e = getenv("NOSTR_SIGNER_LOG");
    enabled = (e && *e && strcmp(e, "0")!=0) ? 1 : 0;
    inited = 1;
  }
  return enabled;
}

static void wipe_free(char *p) {
  if (!p) return;
  volatile char *v = p;
  while (*v) *v++ = 0;
  free(p);
}

/* @s as a JSON string literal (malloc). */
static char *json_quote(const char *s) {
  if (!s) s = "";
  size_t n = strlen(s);
  char *o = (char*)malloc(n * 6 + 3);
  if (!o) return NULL;
  char *w = o;
  *w++ = '"';
  for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
    switch (*p) {
      case '"':  *w++ = '\\'; *w++ = '"'; break;
      case '\\': *w++ = '\\'; *w++ = '\\'; break;
      case '\n': *w++ = '\\'; *w++ = 'n'; break;
      case '\r': *w++ = '\\'; *w++ = 'r'; break;
      case '\t': *w++ = '\\'; *w++ = 't'; break;
      default:
        if (*p < 0x20) { snprintf(w, 7, "\\u%04x", *p); w += 6; }
        else *w++ = (char)*p;
    }
  }
  *w++ = '"';
  *w = '\0';
  return o;
}

/* {"id":"<id>","result":null,"error":{"code":X,"message":"..."}} */
static char *build_error_json(const char *id, int code, const char *msg) {
  char *qid = json_quote(id), *qmsg = json_quote(msg ? msg : "error");
  char *buf = NULL;
  if (qid && qmsg) {
    size_t need = strlen(qid) + strlen(qmsg) + 64;
    buf = (char*)malloc(need);
    if (buf) snprintf(buf, need, "{\"id\":%s,\"result\":null,\"error\":{\"code\":%d,\"message\":%s}}", qid, code, qmsg);
  }
  free(qid); free(qmsg);
  return buf;
}

/* {"id":"<id>","result":<raw_json>,"error":null} */
static char *build_ok_json_raw(const char *id, const char *raw_json) {
  char *qid = json_quote(id);
  if (!qid) return NULL;
  size_t need = strlen(qid) + strlen(raw_json) + 40;
  char *buf = (char*)malloc(need);
  if (buf) snprintf(buf, need, "{\"id\":%s,\"result\":%s,\"error\":null}", qid, raw_json);
  free(qid);
  return buf;
}

static void send_frame(int fd, char *json, int wipe) {
  if (!json) return;
  nip5f_write_frame(fd, json, strlen(json));
  if (wipe) wipe_free(json); else free(json);
}

static void send_error(int fd, const char *id, int code, const char *msg) {
  send_frame(fd, build_error_json(id, code, msg), 0);
}

/* Banner, then the client hello. If the server has NOSTR_SIGNER_AUTH_TOKEN
 * set, the hello must carry the same "auth_token". Returns 0 to proceed. */
static int handshake(int fd) {
  const char *banner = "{\"name\":\"nostr-signer\",\"supported_methods\":[\"get_public_key\",\"sign_event\",\"nip44_encrypt\",\"nip44_decrypt\",\"list_public_keys\"]}";
  if (nip5f_write_frame(fd, banner, strlen(banner)) != 0) return -1;
  char *hello = NULL; size_t hlen = 0;
  if (nip5f_read_frame(fd, &hello, &hlen) != 0) { free(hello); return -1; }
  int ok = 1;
  const char *srv_tok = getenv("NOSTR_SIGNER_AUTH_TOKEN");
  if (srv_tok && *srv_tok) {
    char *client_tok = NULL;
    if (hello) (void)nostr_json_get_string(hello, "auth_token", &client_tok);
    ok = client_tok && strcmp(client_tok, srv_tok) == 0;
    free(client_tok);
  }
  free(hello);
  return ok ? 0 : -1;
}

/* params.event as a JSON object, or as a string holding one. */
static char *sign_event_param(const char *req) {
  char *ev = NULL;
  if (nostr_json_get_string_at(req, "params", "event", &ev) == 0 && ev) return ev;
  free(ev);
  ev = NULL;
  char *params_raw = NULL;
  if (nostr_json_get_raw(req, "params", &params_raw) == 0 && params_raw) {
    (void)nostr_json_get_raw(params_raw, "event", &ev);
    free(params_raw);
  }
  return ev;
}

/* Built-in handlers (no hooks). Returns 0 with *out raw JSON, or an error code. */
static int builtin_call(const struct Nip5fConnArg *carg, const char *method,
                        const char *a, const char *b, char **out) {
  char *s = NULL;
  int rc;
  if (strcmp(method, "get_public_key") == 0) {
    rc = carg->get_pub ? carg->get_pub(carg->ud, &s) : nostr_nip5f_builtin_get_public_key(&s);
    if (rc == 0 && s) { *out = json_quote(s); free(s); return *out ? 0 : NIP5F_ERR_INTERNAL; }
  } else if (strcmp(method, "sign_event") == 0) {
    rc = carg->sign_event ? carg->sign_event(carg->ud, a, b, &s) : nostr_nip5f_builtin_sign_event(a, b, &s);
    if (rc == 0 && s) { *out = s; return 0; }
  } else if (strcmp(method, "nip44_encrypt") == 0) {
    rc = carg->enc44 ? carg->enc44(carg->ud, a, b, &s) : nostr_nip5f_builtin_nip44_encrypt(a, b, &s);
    if (rc == 0 && s) { *out = json_quote(s); free(s); return *out ? 0 : NIP5F_ERR_INTERNAL; }
  } else if (strcmp(method, "nip44_decrypt") == 0) {
    rc = carg->dec44 ? carg->dec44(carg->ud, a, b, &s) : nostr_nip5f_builtin_nip44_decrypt(a, b, &s);
    if (rc == 0 && s) { *out = json_quote(s); wipe_free(s); return *out ? 0 : NIP5F_ERR_INTERNAL; }
    wipe_free(s);
    return NIP5F_ERR_INTERNAL;
  } else if (strcmp(method, "list_public_keys") == 0) {
    rc = carg->list_keys ? carg->list_keys(carg->ud, &s) : nostr_nip5f_builtin_list_public_keys(&s);
    if (rc == 0 && s) { *out = s; return 0; }
  } else {
    return NIP5F_ERR_UNSUPPORTED;
  }
  free(s);
  return NIP5F_ERR_INTERNAL;
}

void *nip5f_conn_thread(void *arg) {
  struct Nip5fConnArg *carg = (struct Nip5fConnArg*)arg;
  int fd = carg->fd;
  void *conn = NULL;
  int opened = 0;
  /* Identify the peer before reading a byte from it: the client decides
   * when its hello arrives, and the process behind the connect-time
   * credentials must be read while it is still that process. */
  if (carg->have_hooks && carg->hooks.open) {
    conn = carg->hooks.open(carg->hooks_ud, &carg->peer);
    if (!conn) goto out;
    opened = 1;
  }
  if (carg->handshake && handshake(fd) != 0) goto out;
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] client connected fd=%d pid=%d\n", fd, carg->peer.pid);
  for (;;) {
    char *req = NULL; size_t rlen = 0;
    if (nip5f_read_frame(fd, &req, &rlen) != 0) break;
    char *id = NULL, *method = NULL, *a = NULL, *b = NULL, *result = NULL, *message = NULL;
    int code = 0;
    (void)nostr_json_get_string(req, "id", &id);
    (void)nostr_json_get_string(req, "method", &method);
    if (signer_log_enabled()) fprintf(stderr, "[nip5f] request id=%s method=%s\n", id ? id : "", method ? method : "<none>");
    if (!method) {
      code = NIP5F_ERR_INVALID_REQUEST;
    } else if (strcmp(method, "get_public_key") == 0 || strcmp(method, "list_public_keys") == 0) {
      /* no params */
    } else if (strcmp(method, "sign_event") == 0) {
      a = sign_event_param(req);
      (void)nostr_json_get_string_at(req, "params", "pubkey", &b); /* optional */
      if (!a) code = NIP5F_ERR_INVALID_PARAMS;
    } else if (strcmp(method, "nip44_encrypt") == 0 || strcmp(method, "nip44_decrypt") == 0) {
      int enc = method[6] == 'e';
      if (nostr_json_get_string_at(req, "params", "peer_pub", &a) != 0 || !a ||
          nostr_json_get_string_at(req, "params", enc ? "plaintext" : "cipher_b64", &b) != 0 || !b)
        code = NIP5F_ERR_INVALID_PARAMS;
    } else {
      code = NIP5F_ERR_UNSUPPORTED;
    }
    if (code == 0) {
      if (carg->have_hooks)
        code = carg->hooks.request(carg->hooks_ud, conn, method, a, b, &result, &message);
      else
        code = builtin_call(carg, method, a, b, &result);
      if (code == 0 && !result) code = NIP5F_ERR_INTERNAL;
    }
    if (code == 0) {
      send_frame(fd, build_ok_json_raw(id, result), 1);
    } else {
      const char *m = message;
      if (!m) {
        switch (code) {
          case NIP5F_ERR_INVALID_REQUEST: m = "invalid request"; break;
          case NIP5F_ERR_UNSUPPORTED:     m = "method not supported"; break;
          case NIP5F_ERR_INVALID_PARAMS:  m = "invalid params"; break;
          case NIP5F_ERR_NO_KEY:          m = "key not found"; break;
          case NIP5F_ERR_DECLINED:        m = "declined"; break;
          default:                        m = "internal error"; break;
        }
      }
      if (signer_log_enabled()) fprintf(stderr, "[nip5f] id=%s -> error %d\n", id ? id : "", code);
      send_error(fd, id, code, m);
    }
    wipe_free(result);
    free(message);
    wipe_free(a);
    wipe_free(b);
    free(id); free(method);
    if (req) { memset(req, 0, rlen); free(req); }
  }
out:
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] client disconnected fd=%d\n", fd);
  if (opened && carg->hooks.close) carg->hooks.close(carg->hooks_ud, conn);
  if (carg->peer.pidfd >= 0) close(carg->peer.pidfd);
  close(fd);
  free(carg);
  return NULL;
}
