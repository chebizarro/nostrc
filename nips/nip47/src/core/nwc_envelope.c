/* NIP-47 envelope build/parse helpers */
#include "nostr/nip47/nwc_envelope.h"
#include "nostr/nip47/nwc.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <jansson.h>

static const char *enc_label(NostrNwcEncryption enc) {
  switch (enc) {
    case NOSTR_NWC_ENC_NIP44_V2: return "nip44-v2";
    case NOSTR_NWC_ENC_NIP04: return "nip04";
    default: return "nip44-v2";
  }
}

/* naive string escaper for identifiers (method/result_type), assumed to be simple tokens */
static char *json_quote_token(const char *s) {
  size_t n = strlen(s);
  char *out = (char *)malloc(n + 3);
  if (!out) return NULL;
  out[0] = '"'; memcpy(out + 1, s, n); out[1 + n] = '"'; out[2 + n] = '\0';
  return out;
}

int nostr_nwc_request_build(const char *wallet_pub_hex, NostrNwcEncryption enc,
                            const NostrNwcRequestBody *body,
                            char **out_event_json) {
  if (!out_event_json || !body || !body->method) return -1;
  *out_event_json = NULL;

  int rc = -1;
  NostrEvent *ev = nostr_event_new();
  if (!ev) return -1;
  nostr_event_set_kind(ev, NOSTR_EVENT_KIND_NWC_REQUEST);
  /* ensure created_at is set for serializer */
  {
    time_t now = time(NULL);
    if (now > 0) nostr_event_set_created_at(ev, (int64_t)now);
  }

  /* content: {"method":"...","params": <params_json or {}>} */
  char *mqt = json_quote_token(body->method);
  if (!mqt) goto out;
  const char *params = body->params_json && *body->params_json ? body->params_json : "{}";
  size_t cap = strlen(mqt) + strlen(params) + 32;
  char *content = (char *)malloc(cap);
  if (!content) { free(mqt); goto out; }
  snprintf(content, cap, "{\"method\":%s,\"params\":%s}", mqt, params);
  nostr_event_set_content(ev, content);
  free(mqt); free(content);

  /* tags: ["p", wallet_pub_hex] (route) and ["encryption", enc] */
  size_t tcount = 0;
  tcount += wallet_pub_hex && *wallet_pub_hex ? 1 : 0;
  tcount += 1; /* encryption */
  NostrTags *tags = nostr_tags_new(tcount ? tcount : 1);
  if (!tags) goto out;
  size_t idx = 0;
  if (wallet_pub_hex && *wallet_pub_hex) {
    NostrTag *p = nostr_tag_new("p", wallet_pub_hex, NULL);
    if (!p) { nostr_tags_free(tags); goto out; }
    nostr_tags_set(tags, idx++, p);
  }
  NostrTag *e = nostr_tag_new("encryption", enc_label(enc), NULL);
  if (!e) { nostr_tags_free(tags); goto out; }
  nostr_tags_set(tags, idx++, e);
  nostr_event_set_tags(ev, tags);

  char *json = nostr_event_serialize(ev);
  if (!json) goto out;
  *out_event_json = json;
  rc = 0;

out:
  nostr_event_free(ev);
  return rc;
}

int nostr_nwc_response_build(const char *client_pub_hex, const char *req_event_id,
                             NostrNwcEncryption enc, const NostrNwcResponseBody *body,
                             char **out_event_json) {
  if (!out_event_json || !body) return -1;
  *out_event_json = NULL;
  int rc = -1;
  NostrEvent *ev = nostr_event_new();
  if (!ev) return -1;
  nostr_event_set_kind(ev, NOSTR_EVENT_KIND_NWC_RESPONSE);
  /* ensure created_at is set for serializer */
  {
    time_t now = time(NULL);
    if (now > 0) nostr_event_set_created_at(ev, (int64_t)now);
  }

  /* content: either {"error":{"code":"...","message":"..."}} or {"result_type":"...","result": <json|null>} */
  char *content = NULL;
  if (body->error_code || body->error_message) {
    const char *code = body->error_code ? body->error_code : "";
    const char *msg = body->error_message ? body->error_message : "";
    size_t cap = strlen(code) + strlen(msg) + 64;
    content = (char *)malloc(cap);
    if (!content) goto out;
    snprintf(content, cap, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, msg);
  } else {
    const char *rtype = body->result_type ? body->result_type : "";
    char *qt = json_quote_token(rtype);
    if (!qt) goto out;
    const char *rjson = body->result_json && *body->result_json ? body->result_json : "null";
    size_t cap = strlen(qt) + strlen(rjson) + 32;
    content = (char *)malloc(cap);
    if (!content) { free(qt); goto out; }
    snprintf(content, cap, "{\"result_type\":%s,\"result\":%s}", qt, rjson);
    free(qt);
  }
  nostr_event_set_content(ev, content);
  free(content);

  /* tags: reference request id and routing/encryption */
  size_t tcount = 1; /* encryption */
  tcount += (req_event_id && *req_event_id) ? 1 : 0; /* e */
  tcount += (client_pub_hex && *client_pub_hex) ? 1 : 0; /* p */
  NostrTags *tags = nostr_tags_new(tcount);
  if (!tags) goto out;
  size_t idx = 0;
  if (req_event_id && *req_event_id) {
    NostrTag *et = nostr_tag_new("e", req_event_id, NULL);
    if (!et) { nostr_tags_free(tags); goto out; }
    nostr_tags_set(tags, idx++, et);
  }
  if (client_pub_hex && *client_pub_hex) {
    NostrTag *pt = nostr_tag_new("p", client_pub_hex, NULL);
    if (!pt) { nostr_tags_free(tags); goto out; }
    nostr_tags_set(tags, idx++, pt);
  }
  NostrTag *enc_t = nostr_tag_new("encryption", enc_label(enc), NULL);
  if (!enc_t) { nostr_tags_free(tags); goto out; }
  nostr_tags_set(tags, idx++, enc_t);
  nostr_event_set_tags(ev, tags);

  char *json = nostr_event_serialize(ev);
  if (!json) goto out;
  *out_event_json = json;
  rc = 0;

out:
  nostr_event_free(ev);
  return rc;
}

int nostr_nwc_request_parse(const char *event_json,
                            char **out_wallet_pub_hex,
                            NostrNwcEncryption *out_enc,
                            NostrNwcRequestBody *out_body) {
  if (!event_json || !out_body) return -1;
  int rc = -1;
  char *content = NULL;
  char *method = NULL;
  char *params_json = NULL;
  char *wallet_pub = NULL;
  NostrNwcEncryption enc = NOSTR_NWC_ENC_NIP44_V2;

  /* Deserialize and validate kind first */
  NostrEvent *ev = nostr_event_new();
  if (!ev) goto out;
  if (nostr_event_deserialize(ev, event_json) != 0) { nostr_event_free(ev); goto out; }
  if (nostr_event_get_kind(ev) != NOSTR_EVENT_KIND_NWC_REQUEST) { nostr_event_free(ev); goto out; }

  if (nostr_json_get_string(event_json, "content", &content) != 0 || !content) { nostr_event_free(ev); goto out; }
  if (nostr_json_get_string(content, "method", &method) != 0 || !method) { nostr_event_free(ev); goto out; }
  /* params may be missing; default to {}. If present and not a string, dump JSON compact */
  {
    json_error_t jerr; json_t *root = json_loads(content, 0, &jerr);
    if (root && json_is_object(root)) {
      json_t *pv = json_object_get(root, "params");
      if (pv) {
        if (json_is_string(pv)) {
          const char *s = json_string_value(pv);
          if (s) params_json = strdup(s);
        } else {
          params_json = json_dumps(pv, JSON_COMPACT);
        }
      }
      json_decref(root);
    }
    if (!params_json) params_json = strdup("{}");
  }

  /* parse tags for p and encryption */
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
  if (tags) {
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      const char *k = nostr_tag_get_key(t);
      if (!k) continue;
      if (!wallet_pub && strcmp(k, "p") == 0 && nostr_tag_size(t) >= 2) {
        wallet_pub = strdup(nostr_tag_get_value(t));
      } else if (strcmp(k, "encryption") == 0 && nostr_tag_size(t) >= 2) {
        const char *v = nostr_tag_get_value(t);
        enc = (v && strcmp(v, "nip04") == 0) ? NOSTR_NWC_ENC_NIP04 : NOSTR_NWC_ENC_NIP44_V2;
      }
    }
  }
  nostr_event_free(ev);

  if (out_wallet_pub_hex) *out_wallet_pub_hex = wallet_pub; else { if (wallet_pub) free(wallet_pub); }
  if (out_enc) *out_enc = enc;
  out_body->method = method; method = NULL;
  out_body->params_json = params_json; params_json = NULL;
  rc = 0;

out:
  if (content) free(content);
  if (method) free(method);
  if (params_json) free(params_json);
  return rc;
}

int nostr_nwc_response_parse(const char *event_json,
                             char **out_client_pub_hex,
                             char **out_req_event_id,
                             NostrNwcEncryption *out_enc,
                             NostrNwcResponseBody *out_body) {
  if (!event_json || !out_body) return -1;
  int rc = -1;
  char *content = NULL;
  char *rtype = NULL;
  char *rjson = NULL;
  char *ecode = NULL;
  char *emsg = NULL;
  char *client_pub = NULL;
  char *req_id = NULL;
  NostrNwcEncryption enc = NOSTR_NWC_ENC_NIP44_V2;

  /* Deserialize and validate kind first */
  NostrEvent *ev = nostr_event_new();
  if (!ev) goto out;
  if (nostr_event_deserialize(ev, event_json) != 0) { nostr_event_free(ev); goto out; }
  if (nostr_event_get_kind(ev) != NOSTR_EVENT_KIND_NWC_RESPONSE) { nostr_event_free(ev); goto out; }

  if (nostr_json_get_string(event_json, "content", &content) != 0 || !content) { nostr_event_free(ev); goto out; }
  /* Either error object or result pair. Use JSON parsing to extract. */
  {
    json_error_t jerr; json_t *root = json_loads(content, 0, &jerr);
    if (!root) { nostr_event_free(ev); goto out; }
    json_t *err = json_object_get(root, "error");
    if (err && json_is_object(err)) {
      json_t *code = json_object_get(err, "code");
      json_t *msg = json_object_get(err, "message");
      if (json_is_string(code)) { const char *s = json_string_value(code); if (s) ecode = strdup(s); }
      if (json_is_string(msg)) { const char *s = json_string_value(msg); if (s) emsg = strdup(s); }
    } else {
      json_t *rt = json_object_get(root, "result_type");
      if (json_is_string(rt)) { const char *s = json_string_value(rt); if (s) rtype = strdup(s); }
      json_t *rv = json_object_get(root, "result");
      if (rv) rjson = json_dumps(rv, JSON_COMPACT);
    }
    json_decref(root);
  }

  /* parse tags: e (request id), p (client pub), encryption */
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
  if (tags) {
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      const char *k = nostr_tag_get_key(t);
      if (!k) continue;
      if (!req_id && strcmp(k, "e") == 0 && nostr_tag_size(t) >= 2) req_id = strdup(nostr_tag_get_value(t));
      if (!client_pub && strcmp(k, "p") == 0 && nostr_tag_size(t) >= 2) client_pub = strdup(nostr_tag_get_value(t));
      if (strcmp(k, "encryption") == 0 && nostr_tag_size(t) >= 2) {
        const char *v = nostr_tag_get_value(t);
        enc = (v && strcmp(v, "nip04") == 0) ? NOSTR_NWC_ENC_NIP04 : NOSTR_NWC_ENC_NIP44_V2;
      }
    }
  }
  nostr_event_free(ev);

  if (out_client_pub_hex) *out_client_pub_hex = client_pub; else { if (client_pub) free(client_pub); }
  if (out_req_event_id) *out_req_event_id = req_id; else { if (req_id) free(req_id); }
  if (out_enc) *out_enc = enc;
  out_body->result_type = rtype; rtype = NULL;
  out_body->result_json = rjson; rjson = NULL;
  out_body->error_code = ecode; ecode = NULL;
  out_body->error_message = emsg; emsg = NULL;
  rc = 0;

out:
  if (content) free(content);
  if (rtype) free(rtype);
  if (rjson) free(rjson);
  if (ecode) free(ecode);
  if (emsg) free(emsg);
  return rc;
}

int nostr_nwc_select_encryption(const char **client_supported, size_t client_n,
                                const char **wallet_supported, size_t wallet_n,
                                NostrNwcEncryption *out_enc) {
  if (!out_enc) return -1;
  int client_has_v2 = 0, client_has_04 = 0;
  int wallet_has_v2 = 0, wallet_has_04 = 0;
  for (size_t i = 0; i < client_n; i++) {
    const char *s = client_supported ? client_supported[i] : NULL;
    if (!s) continue;
    if (strcmp(s, "nip44-v2") == 0) client_has_v2 = 1;
    else if (strcmp(s, "nip04") == 0) client_has_04 = 1;
  }
  for (size_t i = 0; i < wallet_n; i++) {
    const char *s = wallet_supported ? wallet_supported[i] : NULL;
    if (!s) continue;
    if (strcmp(s, "nip44-v2") == 0) wallet_has_v2 = 1;
    else if (strcmp(s, "nip04") == 0) wallet_has_04 = 1;
  }
  if (client_has_v2 && wallet_has_v2) { *out_enc = NOSTR_NWC_ENC_NIP44_V2; return 0; }
  if (client_has_04 && wallet_has_04) { *out_enc = NOSTR_NWC_ENC_NIP04; return 0; }
  return -1;
}

void nostr_nwc_request_body_clear(NostrNwcRequestBody *b) {
  if (!b) return;
  if (b->method) { free(b->method); b->method = NULL; }
  if (b->params_json) { free(b->params_json); b->params_json = NULL; }
}

void nostr_nwc_response_body_clear(NostrNwcResponseBody *b) {
  if (!b) return;
  if (b->result_type) { free(b->result_type); b->result_type = NULL; }
  if (b->result_json) { free(b->result_json); b->result_json = NULL; }
  if (b->error_code) { free(b->error_code); b->error_code = NULL; }
  if (b->error_message) { free(b->error_message); b->error_message = NULL; }
}
