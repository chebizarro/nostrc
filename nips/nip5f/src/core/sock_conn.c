#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "sock_internal.h"
#include <jansson.h>
#include "nostr/nip5f/nip5f.h"
#include "sock_conn.h"
#include "json.h"

/* Minimal raw extractor for nested value: returns newly-allocated raw JSON value for
 * object_key.entry_key. Handles string, object, array, numbers, true/false/null. */
static char *json_get_raw_at(const char *json, const char *object_key, const char *entry_key) {
  if (!json || !object_key || !entry_key) return NULL;
  // Find object_key occurrence
  char keybuf[128]; snprintf(keybuf, sizeof(keybuf), "\"%s\"", object_key);
  const char *p = strstr(json, keybuf);
  if (!p) return NULL;
  p = strchr(p, ':'); if (!p) return NULL; p++;
  while (*p==' '||*p=='\n'||*p=='\t'||*p=='\r') p++;
  if (*p!='{') return NULL;
  // Now in object; find entry_key at this level only (simple scan)
  const char *obj_start = p; int depth=0; int in_str=0; int esc=0;
  const char *found = NULL;
  char entbuf[128]; snprintf(entbuf, sizeof(entbuf), "\"%s\"", entry_key);
  while (*p) {
    char c = *p;
    if (in_str) {
      if (!esc && c=='\\') { esc=1; p++; continue; }
      if (!esc && c=='\"') { in_str=0; }
      esc=0; p++; continue;
    }
    if (c=='\"') { in_str=1; p++; continue; }
    if (c=='{') { depth++; p++; continue; }
    if (c=='}') { depth--; if (depth==0) break; p++; continue; }
    if (depth==1) {
      // Potential key start
      if (*p=='\"') {
        if (strncmp(p, entbuf, strlen(entbuf))==0) { found = p; break; }
      }
    }
    p++;
  }
  if (!found) return NULL;
  // Move to ':' then value start
  const char *q = strchr(found, ':'); if (!q) return NULL; q++;
  while (*q==' '||*q=='\n'||*q=='\t'||*q=='\r') q++;
  if (!*q) return NULL;
  const char *start = q;
  if (*q=='\"') {
    // string
    q++; int esc2=0; while (*q) { if (!esc2 && *q=='\"') { q++; break; } if (!esc2 && *q=='\\') { esc2=1; q++; continue; } esc2=0; q++; }
  } else if (*q=='{' || *q=='[') {
    char open=*q, close=(open=='{'?'}':']'); int d=0; int ins=0; int es=0; while (*q) {
      char ch=*q++;
      if (ins) { if (!es && ch=='\\') { es=1; continue; } if (!es && ch=='\"') { ins=0; } es=0; continue; }
      if (ch=='\"') { ins=1; continue; }
      if (ch==open) d++; else if (ch==close) { d--; if (d==0) break; }
    }
  } else {
    // literal until ',' or '}' at depth of params object
    while (*q && *q!=',' && *q!='}') q++;
  }
  size_t len = (size_t)(q - start);
  char *out = (char*)malloc(len+1); if (!out) return NULL;
  memcpy(out, start, len); out[len]='\0';
  return out;
}

/* Optional logging: enable by setting NOSTR_SIGNER_LOG=1 */
static int signer_log_enabled(void) {
  static int inited = 0; static int enabled = 0;
  if (!inited) {
    const char *e = getenv("NOSTR_SIGNER_LOG");
    enabled = (e && *e && strcmp(e, "0")!=0) ? 1 : 0;
    inited = 1;
  }
  return enabled;
}

/* Build a minimal error response: {"id":"<id>","result":null,"error":{"code":X,"message":"..."}} */
static char *build_error_json(const char *id, int code, const char *msg) {
  const char *id_field = id ? id : "";
  /* generous overhead to avoid truncation */
  size_t need = strlen(id_field) + strlen(msg) + 96;
  char *buf = (char*)malloc(need);
  if (!buf) return NULL;
  int n = snprintf(buf, need, "{\"id\":\"%s\",\"result\":null,\"error\":{\"code\":%d,\"message\":\"%s\"}}", id_field, code, msg);
  if (n < 0 || (size_t)n >= need) { free(buf); return NULL; }
  return buf;
}

/* Build a minimal success response with string result: {"id":"<id>","result":...,"error":null} */
static char *build_ok_json_raw(const char *id, const char *raw_json) {
  const char *id_field = id ? id : "";
  /* generous overhead to avoid truncation */
  size_t need = strlen(id_field) + strlen(raw_json) + 64;
  char *buf = (char*)malloc(need);
  if (!buf) return NULL;
  int n = snprintf(buf, need, "{\"id\":\"%s\",\"result\":%s,\"error\":null}", id_field, raw_json);
  if (n < 0 || (size_t)n >= need) { free(buf); return NULL; }
  return buf;
}

void *nip5f_conn_thread(void *arg) {
  struct Nip5fConnArg *carg = (struct Nip5fConnArg*)arg;
  int fd = carg->fd;
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] client connected fd=%d\n", fd);
  for (;;) {
    char *req = NULL; size_t rlen = 0;
    if (nip5f_read_frame(fd, &req, &rlen) != 0) break;
    // Extract id and method via nested lookups if needed
    char *id = NULL; char *method = NULL;
    (void)nostr_json_get_string(req, "id", &id);
    (void)nostr_json_get_string(req, "method", &method);
    if (signer_log_enabled()) fprintf(stderr, "[nip5f] request id=%s method=%s\n", id?id:"", method?method:"<none>");
    if (!method) {
      if (signer_log_enabled()) fprintf(stderr, "[nip5f] invalid request: raw=%.*s\n", (int)(rlen > 512 ? 512 : rlen), req ? req : "");
      char *err = build_error_json(id, 1, "invalid request");
      if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
      free(id); free(req); continue;
    }
    // Dispatch methods
    if (strcmp(method, "get_public_key") == 0) {
      // Call handler if present
      char *pub = NULL;
      int rc = -2;
      if (carg->get_pub) rc = carg->get_pub(carg->ud, &pub);
      else rc = nostr_nip5f_builtin_get_public_key(&pub);
      if (rc == 0 && pub) {
        // Wrap as JSON string result: "<hex>"
        size_t L = strlen(pub) + 3;
        char *jres = (char*)malloc(L);
        if (jres) {
          snprintf(jres, L, "\"%s\"", pub);
          char *ok = build_ok_json_raw(id, jres);
          if (ok) {
            if (signer_log_enabled()) fprintf(stderr, "[nip5f] -> %s\n", ok);
            nip5f_write_frame(fd, ok, strlen(ok)); free(ok);
          }
          free(jres);
        }
        free(pub);
      } else {
        if (signer_log_enabled()) fprintf(stderr, "[nip5f] get_public_key failed\n");
        char *err = build_error_json(id, 10, "get_public_key failed");
        if (err) { if (signer_log_enabled()) fprintf(stderr, "[nip5f] -> %s\n", err); nip5f_write_frame(fd, err, strlen(err)); free(err); }
      }
    } else if (strcmp(method, "sign_event") == 0) {
      char *ev = NULL; char *pub = NULL; int ok1=-1, ok2=-1;
      ok1 = nostr_json_get_string_at(req, "params", "event", &ev);
      if (signer_log_enabled()) {
        int elen = ev ? (int) (strlen(ev) > 80 ? 80 : strlen(ev)) : 0;
        fprintf(stderr, "[nip5f] sign_event params: ok1=%d ev_is_null=%d ev_snip=%.*s\n", ok1, ev?0:1, elen, ev?ev:"");
        if (ev) fprintf(stderr, "[nip5f] sign_event params: ev_first_char=%c\n", ev[0]);
      }
      if (ok1 != 0 || !ev) {
        // Fallback A: extract raw via simple scanner
        char *raw = json_get_raw_at(req, "params", "event");
        if (raw) { ev = raw; ok1 = 0; if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event params: recovered raw event (scan, first=%c)\n", ev[0]); }
      }
      if (ok1 != 0 || !ev) {
        // Fallback B: robust Jansson parse to get params.event and dump raw
        json_error_t jerr; json_t *root = json_loads(req, 0, &jerr);
        if (root) {
          json_t *params = json_object_get(root, "params");
          json_t *evnode = params ? json_object_get(params, "event") : NULL;
          if (evnode) {
            char *dump = json_dumps(evnode, JSON_COMPACT);
            if (dump) { ev = strdup(dump); free(dump); ok1 = 0; if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event params: recovered raw event (jansson)\n"); }
          }
          json_decref(root);
        }
      }
      (void)nostr_json_get_string_at(req, "params", "pubkey", &pub); // optional
      if (ok1 != 0 || !ev) {
        char *err = build_error_json(id, 1, "invalid params");
        if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
      } else {
        char *signed_json = NULL; int rc = -2;
        if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event dispatch: using %s, pub=%s, ev_snip=%.*s\n",
                                          carg->sign_event?"custom":"builtin",
                                          pub?pub:"(none)",
                                          ev? (int) (strlen(ev) > 120 ? 120 : strlen(ev)) : 0,
                                          ev? ev : "");
        if (carg->sign_event) rc = carg->sign_event(carg->ud, ev, pub, &signed_json);
        else rc = nostr_nip5f_builtin_sign_event(ev, pub, &signed_json);
        if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event dispatch: rc=%d, signed_json_null=%d\n", rc, signed_json?0:1);
        if (rc == 0 && signed_json) {
          // Return raw JSON as result
          char *ok = build_ok_json_raw(id, signed_json);
          if (ok) { nip5f_write_frame(fd, ok, strlen(ok)); free(ok); }
          free(signed_json);
        } else {
          if (signer_log_enabled()) fprintf(stderr, "[nip5f] sign_event failed\n");
          char *err = build_error_json(id, 10, "sign_event failed");
          if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
        }
      }
      if (ev) free(ev); if (pub) free(pub); (void)ok2;
    } else if (strcmp(method, "nip44_encrypt") == 0) {
      char *peer = NULL; char *pt = NULL;
      int okp = nostr_json_get_string_at(req, "params", "peer_pub", &peer);
      int okc = nostr_json_get_string_at(req, "params", "plaintext", &pt);
      if (okp != 0 || okc != 0 || !peer || !pt) {
        char *err = build_error_json(id, 1, "invalid params");
        if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
      } else {
        char *b64 = NULL; int rc = -2;
        if (carg->enc44) rc = carg->enc44(carg->ud, peer, pt, &b64);
        else rc = nostr_nip5f_builtin_nip44_encrypt(peer, pt, &b64);
        if (rc == 0 && b64) {
          size_t L = strlen(b64) + 3; char *jres = (char*)malloc(L);
          if (jres) {
            snprintf(jres, L, "\"%s\"", b64);
            char *ok = build_ok_json_raw(id, jres);
            if (ok) { nip5f_write_frame(fd, ok, strlen(ok)); free(ok); }
            free(jres);
          }
          free(b64);
        } else {
          if (signer_log_enabled()) fprintf(stderr, "[nip5f] nip44_encrypt failed\n");
          char *err = build_error_json(id, 10, "not implemented");
          if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
        }
      }
      if (peer) free(peer); if (pt) free(pt);
    } else if (strcmp(method, "nip44_decrypt") == 0) {
      char *peer = NULL; char *ct = NULL;
      int okp = nostr_json_get_string_at(req, "params", "peer_pub", &peer);
      int okc = nostr_json_get_string_at(req, "params", "cipher_b64", &ct);
      if (okp != 0 || okc != 0 || !peer || !ct) {
        char *err = build_error_json(id, 1, "invalid params");
        if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
      } else {
        char *pt = NULL; int rc = -2;
        if (carg->dec44) rc = carg->dec44(carg->ud, peer, ct, &pt);
        else rc = nostr_nip5f_builtin_nip44_decrypt(peer, ct, &pt);
        if (rc == 0 && pt) {
          size_t L = strlen(pt) + 3; char *jres = (char*)malloc(L);
          if (jres) {
            snprintf(jres, L, "\"%s\"", pt);
            char *ok = build_ok_json_raw(id, jres);
            if (ok) { nip5f_write_frame(fd, ok, strlen(ok)); free(ok); }
            free(jres);
          }
          free(pt);
        } else {
          if (signer_log_enabled()) fprintf(stderr, "[nip5f] nip44_decrypt failed\n");
          char *err = build_error_json(id, 10, "not implemented");
          if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
        }
      }
      if (peer) free(peer); if (ct) free(ct);
    } else if (strcmp(method, "list_public_keys") == 0) {
      char *arr = NULL; int rc = -2;
      if (carg->list_keys) rc = carg->list_keys(carg->ud, &arr);
      else rc = nostr_nip5f_builtin_list_public_keys(&arr);
      if (rc == 0 && arr) {
        char *ok = build_ok_json_raw(id, arr); // raw JSON array
        if (ok) { nip5f_write_frame(fd, ok, strlen(ok)); free(ok); }
        free(arr);
      } else {
        if (signer_log_enabled()) fprintf(stderr, "[nip5f] list_public_keys failed\n");
        char *err = build_error_json(id, 10, "not implemented");
        if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
      }
    } else {
      if (signer_log_enabled()) fprintf(stderr, "[nip5f] unknown method: %s\n", method);
      char *err = build_error_json(id, 2, "method not supported");
      if (err) { nip5f_write_frame(fd, err, strlen(err)); free(err); }
    }
    free(id); free(method); free(req);
  }
  if (signer_log_enabled()) fprintf(stderr, "[nip5f] client disconnected fd=%d\n", fd);
  close(fd);
  free(carg);
  return NULL;
}
