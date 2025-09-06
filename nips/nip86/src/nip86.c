/* Minimal NIP-86 implementation with NIP-98 Authorization verification. */

#include <time.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdlib.h>
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-json.h"
#include <openssl/evp.h>

/* Forward decls for internal helpers */
static void load_policy(void);

/* === Minimal base64url decode helper (OpenSSL-based) ===================== */
static unsigned char *b64url_decode(const char *b64url, size_t *out_len) {
  if (!b64url) return NULL;
  size_t len = strlen(b64url);
  /* Convert to standard base64 by replacing -_/ removing padding handling */
  char *tmp = (char*)malloc(len + 4);
  if (!tmp) return NULL;
  for (size_t i=0;i<len;i++) {
    char c = b64url[i];
    if (c=='-') tmp[i] = '+'; else if (c=='_') tmp[i] = '/'; else tmp[i] = c;
  }
  size_t std_len = len;
  size_t pad = (4 - (std_len % 4)) % 4;
  for (size_t i=0;i<pad;i++) tmp[std_len++] = '=';
  tmp[std_len] = '\0';
  int outcap = (int)((std_len/4)*3 + 3);
  unsigned char *out = (unsigned char*)malloc((size_t)outcap);
  if (!out) { free(tmp); return NULL; }
  int n = EVP_DecodeBlock(out, (const unsigned char*)tmp, (int)std_len);
  free(tmp);
  if (n < 0) { free(out); return NULL; }
  /* Adjust for padding '=' that EVP_DecodeBlock doesn't subtract */
  if (pad) n -= (int)pad;
  if (out_len) *out_len = (size_t)n;
  return out;
}

/* In-memory simple lists for management (stub storage) */
typedef struct { char pubkey[65]; char reason[128]; } pk_entry;
typedef struct { char id[65]; char reason[128]; } id_entry;
typedef struct { int kinds[64]; int n; } kinds_list;
static pk_entry banned_pubkeys[128]; static int banned_count = 0;
static pk_entry allowed_pubkeys[128]; static int allowed_count = 0;
static id_entry banned_events[128]; static int banned_events_count = 0;
static kinds_list allowed_kinds = { .n = 0 };
static char blocked_ips[128][64]; static int blocked_ip_count = 0;
static char relay_name[128] = "";
static char relay_description[256] = "";
static char relay_icon[256] = "";
static int policy_loaded = 0;

static const char *policy_path(void) {
  const char *p = getenv("NOSTR_RELAY_POLICY");
  return (p && *p) ? p : "relay_policy.json";
}

int nostr_nip86_load_policy(void) {
  load_policy();
  return 0;
}

static void save_policy(void) {
  FILE *f = fopen(policy_path(), "w"); if (!f) return;
  fprintf(f, "{");
  fprintf(f, "\"banned_pubkeys\":[");
  for (int i=0;i<banned_count;i++) fprintf(f, "%s\"%s\"", i?",":"", banned_pubkeys[i].pubkey);
  fprintf(f, "],\"allowed_pubkeys\":[");
  for (int i=0;i<allowed_count;i++) fprintf(f, "%s\"%s\"", i?",":"", allowed_pubkeys[i].pubkey);
  fprintf(f, "],\"banned_events\":[");
  for (int i=0;i<banned_events_count;i++) fprintf(f, "%s\"%s\"", i?",":"", banned_events[i].id);
  fprintf(f, "],\"allowed_kinds\":[");
  for (int i=0;i<allowed_kinds.n;i++) fprintf(f, "%s%d", i?",":"", allowed_kinds.kinds[i]);
  fprintf(f, "],\"blocked_ips\":[");
  for (int i=0;i<blocked_ip_count;i++) fprintf(f, "%s\"%s\"", i?",":"", blocked_ips[i]);
  fprintf(f, "],\"relay_name\":\"%s\",\"relay_description\":\"%s\",\"relay_icon\":\"%s\"}", relay_name, relay_description, relay_icon);
  fclose(f);
}

static char *slurp_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "r"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); if (sz < 0) { fclose(f); return NULL; }
  fseek(f, 0, SEEK_SET);
  char *buf = (char*)malloc((size_t)sz + 1); if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)sz, f); fclose(f); buf[n] = '\0'; if (len_out) *len_out = n; return buf;
}

static void load_policy(void) {
  if (policy_loaded) return;
  policy_loaded = 1;
  size_t n=0; char *json = slurp_file(policy_path(), &n); if (!json) return;
  /* banned_pubkeys */
  char **arr = NULL; size_t ac = 0;
  if (nostr_json_get_string_array(json, "banned_pubkeys", &arr, &ac) == 0) {
    for (size_t i=0;i<ac && banned_count < (int)(sizeof(banned_pubkeys)/sizeof(banned_pubkeys[0])); i++) {
      snprintf(banned_pubkeys[banned_count++].pubkey, sizeof(banned_pubkeys[0].pubkey), "%s", arr[i]);
      free(arr[i]);
    }
  /* blocked_ips */
  char **ips=NULL; size_t ipc=0; if (nostr_json_get_string_array(json, "blocked_ips", &ips, &ipc) == 0) {
    for (size_t i=0;i<ipc && blocked_ip_count < (int)(sizeof(blocked_ips)/sizeof(blocked_ips[0])); i++) {
      snprintf(blocked_ips[blocked_ip_count++], sizeof(blocked_ips[0]), "%s", ips[i]);
      free(ips[i]);
    }
    free(ips);
  }
    free(arr);
  }
  arr = NULL; ac = 0;
  if (nostr_json_get_string_array(json, "allowed_pubkeys", &arr, &ac) == 0) {
    for (size_t i=0;i<ac && allowed_count < (int)(sizeof(allowed_pubkeys)/sizeof(allowed_pubkeys[0])); i++) {
      snprintf(allowed_pubkeys[allowed_count++].pubkey, sizeof(allowed_pubkeys[0].pubkey), "%s", arr[i]);
      free(arr[i]);
    }
    free(arr);
  }
  arr = NULL; ac = 0;
  if (nostr_json_get_string_array(json, "banned_events", &arr, &ac) == 0) {
    for (size_t i=0;i<ac && banned_events_count < (int)(sizeof(banned_events)/sizeof(banned_events[0])); i++) {
      snprintf(banned_events[banned_events_count++].id, sizeof(banned_events[0].id), "%s", arr[i]);
      free(arr[i]);
    }
    free(arr);
  }
  int *kinds=NULL; size_t kc=0;
  if (nostr_json_get_int_array(json, "allowed_kinds", &kinds, &kc) == 0) {
    for (size_t i=0;i<kc && allowed_kinds.n < (int)(sizeof(allowed_kinds.kinds)/sizeof(allowed_kinds.kinds[0])); i++) {
      allowed_kinds.kinds[allowed_kinds.n++] = kinds[i];
    }
    free(kinds);
  }
  char *s=NULL; if (nostr_json_get_string(json, "relay_name", &s) == 0) { snprintf(relay_name, sizeof(relay_name), "%s", s); free(s);} 
  s=NULL; if (nostr_json_get_string(json, "relay_description", &s) == 0) { snprintf(relay_description, sizeof(relay_description), "%s", s); free(s);} 
  s=NULL; if (nostr_json_get_string(json, "relay_icon", &s) == 0) { snprintf(relay_icon, sizeof(relay_icon), "%s", s); free(s);} 
  free(json);
}

static char *make_response(const char *result_json, const char *error_str) {
  const char *res = result_json ? result_json : "{}";
  if (!error_str) {
    size_t sz = strlen(res) + 32; char *buf = (char*)malloc(sz);
    if (!buf) return NULL; snprintf(buf, sz, "{\"result\":%s,\"error\":null}", res); return buf;
  } else {
    size_t sz = strlen(res) + strlen(error_str) + 64; char *buf = (char*)malloc(sz);
    if (!buf) return NULL; snprintf(buf, sz, "{\"result\":%s,\"error\":\"%s\"}", res, error_str); return buf;
  }
}

static int hex_eq(const char *a, const char *b) { return a && b && strcmp(a,b)==0; }

static int nip98_verify(const char *auth_header, const char *method, const char *url, const char *body) {
  if (!auth_header) return 0;
  /* Expect: "Nostr <base64>" */
  const char *sp = strchr(auth_header, ' ');
  if (!sp) return 0;
  if (strncasecmp(auth_header, "Nostr", (size_t)(sp - auth_header)) != 0) return 0;
  const char *b64 = sp + 1; while (*b64 == ' ') b64++;
  size_t dst_len = 0;
  unsigned char *out = b64url_decode(b64, &dst_len);
  if (!out) return 0;
  out[dst_len] = '\0';
  /* Parse JSON into event */
  NostrEvent *ev = nostr_event_new(); int ok = 0;
  if (nostr_event_deserialize_compact(ev, (const char*)out)) ok = 1; else ok = (nostr_event_deserialize(ev, (const char*)out) == 0);
  free(out);
  if (!ok) { nostr_event_free(ev); return 0; }
  if (nostr_event_get_kind(ev) != 27235) { nostr_event_free(ev); return 0; }
  time_t now = time(NULL);
  long dt = (long)(now - (time_t)nostr_event_get_created_at(ev));
  if (dt < -60 || dt > 60) { nostr_event_free(ev); return 0; }
  /* Collect tags u, method, payload */
  NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
  const char *tag_u = NULL; const char *tag_method = NULL; const char *tag_payload = NULL;
  if (tags) {
    size_t tcount = nostr_tags_size(tags);
    for (size_t i=0;i<tcount;i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      const char *k = t ? nostr_tag_get_key(t) : NULL;
      if (!k) continue;
      if (!tag_u && strcmp(k, "u") == 0) tag_u = nostr_tag_get_value(t);
      else if (!tag_method && strcmp(k, "method") == 0) tag_method = nostr_tag_get_value(t);
      else if (!tag_payload && strcmp(k, "payload") == 0) tag_payload = nostr_tag_get_value(t);
    }
  }
  if (!tag_u || !tag_method) { nostr_event_free(ev); return 0; }
  if (!hex_eq(tag_method, method)) { nostr_event_free(ev); return 0; }
  /* Compare absolute URL exactly */
  if (strcmp(tag_u, url) != 0) { nostr_event_free(ev); return 0; }
  /* Optional payload verification */
  if (tag_payload && body) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)body, strlen(body), digest);
    char hex[SHA256_DIGEST_LENGTH*2+1];
    static const char *hx = "0123456789abcdef";
    for (size_t i=0;i<SHA256_DIGEST_LENGTH;i++){ hex[i*2]=hx[(digest[i]>>4)&0xF]; hex[i*2+1]=hx[digest[i]&0xF]; }
    hex[SHA256_DIGEST_LENGTH*2]='\0';
    if (strcmp(tag_payload, hex) != 0) { nostr_event_free(ev); return 0; }
  }
  /* Signature verification */
  if (!nostr_event_check_signature(ev)) { nostr_event_free(ev); return 0; }
  nostr_event_free(ev);
  return 1;
}

/* libjson-based parsing helpers */
static int extract_method(const char *body, char *out, size_t outsz) {
  char *m=NULL; if (nostr_json_get_string(body, "method", &m) != 0 || !m) return 0; snprintf(out, outsz, "%s", m); free(m); return 1;
}
static int get_first_param_string(const char *body, char *out, size_t outsz) {
  char **arr=NULL; size_t n=0; if (nostr_json_get_string_array(body, "params", &arr, &n) != 0 || n==0) return 0; snprintf(out, outsz, "%s", arr[0]); for (size_t i=0;i<n;i++) free(arr[i]); free(arr); return 1;
}
static int get_first_param_int(const char *body, int *outv) {
  int *arr=NULL; size_t n=0; if (nostr_json_get_int_array(body, "params", &arr, &n) != 0 || n==0) return 0; *outv = arr[0]; free(arr); return 1;
}

char *nostr_nip86_process_request(void *app_ctx, const char *auth, const char *body, const char *method, const char *url, int *http_status) {
  (void)app_ctx;
  if (http_status) *http_status = 200;
  if (!auth || !*auth) { if (http_status) *http_status = 401; return make_response("{}", "unauthorized"); }
  if (!nip98_verify(auth, method ? method : "POST", url ? url : "", body ? body : "")) { if (http_status) *http_status = 401; return make_response("{}", "unauthorized"); }
  load_policy();

  /* Dispatch NIP-86 methods */
  char mth[64]; if (!extract_method(body, mth, sizeof(mth))) return make_response("{}", "invalid: missing method");

  if (strcmp(mth, "supportedmethods") == 0) {
    return make_response("[\"supportedmethods\",\"banpubkey\",\"listbannedpubkeys\",\"allowpubkey\",\"listallowedpubkeys\",\"listeventsneedingmoderation\",\"allowevent\",\"banevent\",\"listbannedevents\",\"changerelayname\",\"changerelaydescription\",\"changerelayicon\",\"allowkind\",\"disallowkind\",\"listallowedkinds\",\"blockip\",\"unblockip\",\"listblockedips\"]", NULL);
  }
  else if (strcmp(mth, "banpubkey") == 0) {
    if (banned_count < (int)(sizeof(banned_pubkeys)/sizeof(banned_pubkeys[0]))) {
      char pk[65]; if (get_first_param_string(body, pk, sizeof(pk))) { snprintf(banned_pubkeys[banned_count++].pubkey, sizeof(banned_pubkeys[0].pubkey), "%s", pk); }
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "listbannedpubkeys") == 0) {
    /* return array of objects */
    char *buf = (char*)malloc(4096); if (!buf) return make_response("[]", NULL);
    size_t off=0; off+=snprintf(buf+off, 4096-off, "[");
    for (int i=0;i<banned_count;i++) { off+=snprintf(buf+off, 4096-off, "%s{\"pubkey\":\"%s\"}", i?",":"", banned_pubkeys[i].pubkey); if (off>=4090) break; }
    off+=snprintf(buf+off, 4096-off, "]"); char *resp = make_response(buf, NULL); free(buf); return resp;
  }
  else if (strcmp(mth, "allowpubkey") == 0) {
    if (allowed_count < (int)(sizeof(allowed_pubkeys)/sizeof(allowed_pubkeys[0]))) {
      char pk[65]; if (get_first_param_string(body, pk, sizeof(pk))) { snprintf(allowed_pubkeys[allowed_count++].pubkey, sizeof(allowed_pubkeys[0].pubkey), "%s", pk); }
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "listallowedpubkeys") == 0) {
    char *buf = (char*)malloc(4096); if (!buf) return make_response("[]", NULL);
    size_t off=0; off+=snprintf(buf+off, 4096-off, "[");
    for (int i=0;i<allowed_count;i++) { off+=snprintf(buf+off, 4096-off, "%s{\"pubkey\":\"%s\"}", i?",":"", allowed_pubkeys[i].pubkey); if (off>=4090) break; }
    off+=snprintf(buf+off, 4096-off, "]"); char *resp = make_response(buf, NULL); free(buf); return resp;
  }
  else if (strcmp(mth, "listeventsneedingmoderation") == 0) {
    return make_response("[]", NULL);
  }
  else if (strcmp(mth, "allowevent") == 0) {
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "banevent") == 0) {
    if (banned_events_count < (int)(sizeof(banned_events)/sizeof(banned_events[0]))) {
      char id[65]; if (get_first_param_string(body, id, sizeof(id))) { snprintf(banned_events[banned_events_count++].id, sizeof(banned_events[0].id), "%s", id); }
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "listbannedevents") == 0) {
    char *buf = (char*)malloc(4096); if (!buf) return make_response("[]", NULL);
    size_t off=0; off+=snprintf(buf+off, 4096-off, "[");
    for (int i=0;i<banned_events_count;i++) { off+=snprintf(buf+off, 4096-off, "%s{\"id\":\"%s\"}", i?",":"", banned_events[i].id); if (off>=4090) break; }
    off+=snprintf(buf+off, 4096-off, "]"); char *resp = make_response(buf, NULL); free(buf); return resp;
  }
  else if (strcmp(mth, "changerelayname") == 0) {
    char name[128]; if (get_first_param_string(body, name, sizeof(name))) snprintf(relay_name, sizeof(relay_name), "%s", name);
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "changerelaydescription") == 0) {
    char desc[256]; if (get_first_param_string(body, desc, sizeof(desc))) snprintf(relay_description, sizeof(relay_description), "%s", desc);
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "changerelayicon") == 0) {
    char icon[256]; if (get_first_param_string(body, icon, sizeof(icon))) snprintf(relay_icon, sizeof(relay_icon), "%s", icon);
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "allowkind") == 0) {
    if (allowed_kinds.n < (int)(sizeof(allowed_kinds.kinds)/sizeof(allowed_kinds.kinds[0]))) {
      int k=0; if (get_first_param_int(body, &k)) allowed_kinds.kinds[allowed_kinds.n++] = k;
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "disallowkind") == 0) {
    int k=-1; (void)get_first_param_int(body, &k);
    if (k >= 0) {
      for (int i=0;i<allowed_kinds.n;i++) if (allowed_kinds.kinds[i]==k) { memmove(&allowed_kinds.kinds[i], &allowed_kinds.kinds[i+1], (allowed_kinds.n-i-1)*sizeof(int)); allowed_kinds.n--; break; }
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "listallowedkinds") == 0) {
    char *buf = (char*)malloc(2048); if (!buf) return make_response("[]", NULL);
    size_t off=0; off+=snprintf(buf+off, 2048-off, "[");
    for (int i=0;i<allowed_kinds.n;i++) { off+=snprintf(buf+off, 2048-off, "%s%d", i?",":"", allowed_kinds.kinds[i]); if (off>=2040) break; }
    off+=snprintf(buf+off, 2048-off, "]"); char *resp = make_response(buf, NULL); free(buf); return resp;
  }
  else if (strcmp(mth, "blockip") == 0) {
    if (blocked_ip_count < (int)(sizeof(blocked_ips)/sizeof(blocked_ips[0]))) {
      char ip[64]; if (get_first_param_string(body, ip, sizeof(ip))) snprintf(blocked_ips[blocked_ip_count++], sizeof(blocked_ips[0]), "%s", ip);
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "unblockip") == 0) {
    char ip[64]; if (get_first_param_string(body, ip, sizeof(ip))) {
      for (int i=0;i<blocked_ip_count;i++) if (strcmp(blocked_ips[i], ip)==0) { memmove(&blocked_ips[i], &blocked_ips[i+1], (blocked_ip_count-i-1)*sizeof(blocked_ips[0])); blocked_ip_count--; break; }
    }
    save_policy();
    return make_response("true", NULL);
  }
  else if (strcmp(mth, "listblockedips") == 0) {
    char *buf = (char*)malloc(4096); if (!buf) return make_response("[]", NULL);
    size_t off=0; off+=snprintf(buf+off, 4096-off, "[");
    for (int i=0;i<blocked_ip_count;i++) { off+=snprintf(buf+off, 4096-off, "%s{\\\"ip\\\":\\\"%s\\\"}", i?",":"", blocked_ips[i]); if (off>=4090) break; }
    off+=snprintf(buf+off, 4096-off, "]"); char *resp = make_response(buf, NULL); free(buf); return resp;
  }

  return make_response("{}", "unsupported: method");
}

/* Policy getters */
int nostr_nip86_is_pubkey_banned(const char *hex32) {
  if (!hex32 || !*hex32) return 0;
  for (int i=0;i<banned_count;i++) if (strcmp(banned_pubkeys[i].pubkey, hex32)==0) return 1;
  return 0;
}
int nostr_nip86_has_allowlist(void) { return allowed_count > 0; }
int nostr_nip86_is_pubkey_allowed(const char *hex32) {
  if (!hex32 || !*hex32) return 0;
  for (int i=0;i<allowed_count;i++) if (strcmp(allowed_pubkeys[i].pubkey, hex32)==0) return 1;
  return 0;
}
int nostr_nip86_has_allowed_kinds(void) { return allowed_kinds.n > 0; }
int nostr_nip86_is_kind_allowed(int kind) {
  for (int i=0;i<allowed_kinds.n;i++) if (allowed_kinds.kinds[i]==kind) return 1;
  return 0;
}
int nostr_nip86_is_ip_blocked(const char *ip) {
  if (!ip || !*ip) return 0;
  for (int i=0;i<blocked_ip_count;i++) if (strcmp(blocked_ips[i], ip)==0) return 1;
  return 0;
}
