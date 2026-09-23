#define _GNU_SOURCE
#include "auth_client.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* SUBMIT_UNLOCK placeholder for the external NIP-46 signer. The broker
 * requires a non-empty secret; the provider ignores its value and gates
 * approval at the bunker. */
#define NH_AUTH_CLIENT_APPROVE_TOKEN "approve"

static int gen_request_id(char out[NH_AUTH_REQUEST_ID_HEX_LEN + 1]) {
  uint8_t raw[NH_AUTH_REQUEST_ID_HEX_LEN / 2];
  size_t off = 0;
  while (off < sizeof raw) {
    ssize_t n = getrandom(raw + off, sizeof raw - off, 0);
    if (n < 0) return -1;
    off += (size_t)n;
  }
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof raw; i++) {
    out[i * 2] = hex[raw[i] >> 4];
    out[i * 2 + 1] = hex[raw[i] & 0xf];
  }
  out[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  return 0;
}

int nh_auth_client_connect(const char *socket_path, int *fd_out) {
  if (!socket_path || !fd_out) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen(socket_path) >= sizeof addr.sun_path) return -1;
  strcpy(addr.sun_path, socket_path);
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  *fd_out = fd;
  return 0;
}

void nh_auth_client_close(int fd) {
  if (fd >= 0) close(fd);
}

static int result_from_name(const char *name, nh_auth_result *out) {
  for (int r = 0; r <= NH_AUTH_RESULT_INTERNAL_ERROR; r++) {
    const char *n = nh_auth_result_name((nh_auth_result)r);
    if (n && name && !strcmp(n, name)) { *out = (nh_auth_result)r; return 0; }
  }
  return -1;
}

int nh_auth_provider_list_has(const nh_auth_provider_list *list,
                              const char *canonical_name) {
  if (!list || !canonical_name) return 0;
  for (size_t i = 0; i < list->count; i++)
    if (!strcmp(list->names[i], canonical_name)) return 1;
  return 0;
}

const char *nh_auth_provider_choice_parse(const char *raw) {
  if (!raw) return NULL;
  /* Reject any non-ASCII or control-except-whitespace byte. */
  for (const unsigned char *p = (const unsigned char *)raw; *p; p++)
    if (*p >= 0x80 || (*p < 0x20 && *p != ' ' && *p != '\t' && *p != '\r' &&
                       *p != '\n'))
      return NULL;
  /* Trim leading/trailing whitespace. */
  const char *start = raw;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
    start++;
  size_t len = strlen(start);
  while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                     start[len - 1] == '\r' || start[len - 1] == '\n'))
    len--;
  if (len == 0 || len > 16) return NULL;
  char buf[17];
  memcpy(buf, start, len);
  buf[len] = '\0';
  /* Match strict lowercase ASCII per the B0 contract. */
  if (!strcmp(buf, "local")) return NH_AUTH_PROVIDER_NAME_LOCAL;
  if (!strcmp(buf, "remote") || !strcmp(buf, "nip46"))
    return NH_AUTH_PROVIDER_NAME_NIP46;
  if (!strcmp(buf, "qr") || !strcmp(buf, "nip46qr"))
    return NH_AUTH_PROVIDER_NAME_NIP46_QR;
  return NULL;
}

/* Sends one request with the given payload object and captures the response
 * payload_json in *response_json_out (heap-allocated, caller frees). Returns 0
 * on transport success. */
static int client_op_raw(int fd, nh_auth_operation op, json_t *payload /*stolen*/,
                         char **response_json_out) {
  *response_json_out = NULL;
  if (!payload) return -1;
  char *payload_json = json_dumps(payload, JSON_COMPACT);
  json_decref(payload);
  if (!payload_json) return -1;
  nh_auth_message request;
  memset(&request, 0, sizeof request);
  request.operation = op;
  request.transaction_id[0] = '\0';
  request.payload_json = payload_json;
  if (gen_request_id(request.request_id) != 0) { free(payload_json); return -1; }
  int rc = nh_auth_send_message(fd, &request);
  /* Wipe the sent payload buffer: it may hold a passphrase. */
  volatile char *w = (volatile char *)payload_json;
  for (size_t i = 0; payload_json[i]; i++) w[i] = 0;
  free(payload_json);
  if (rc != 0) return -1;

  unsigned char *packet = NULL;
  size_t packet_len = 0;
  if (nh_auth_recv_packet(fd, &packet, &packet_len) != 0) return -1;
  nh_auth_message response;
  rc = nh_auth_message_parse(packet, packet_len, &response);
  /* Wipe the received packet: it may hold an SMB password. */
  if (packet) {
    volatile unsigned char *pw = (volatile unsigned char *)packet;
    for (size_t i = 0; i < packet_len; i++) pw[i] = 0;
    free(packet);
  }
  if (rc != 0) return -1;
  *response_json_out = response.payload_json ? strdup(response.payload_json)
                                             : strdup("{}");
  nh_auth_message_clear(&response);
  return *response_json_out ? 0 : -1;
}

/* Extracts the "result" string from a response payload into *out. */
static int extract_result(const char *payload_json, nh_auth_result *out) {
  if (!payload_json) return -1;
  json_error_t e;
  json_t *root = json_loads(payload_json, 0, &e);
  if (!root) return -1;
  json_t *result = json_object_get(root, "result");
  int rc = -1;
  if (result && json_is_string(result))
    rc = result_from_name(json_string_value(result), out);
  json_decref(root);
  return rc;
}

/* Legacy helper preserved for check_account and internal call sites that do
 * not need the raw payload back. */
static int client_op(int fd, nh_auth_operation op, json_t *payload /*stolen*/,
                     nh_auth_result *result_out) {
  char *response_json = NULL;
  int rc = client_op_raw(fd, op, payload, &response_json);
  if (rc != 0) { free(response_json); return -1; }
  rc = extract_result(response_json, result_out);
  free(response_json);
  return rc;
}

int nh_auth_client_check_account(int fd, const char *username,
                                 nh_auth_result *result_out) {
  if (!username || !result_out) return -1;
  json_t *payload = json_object();
  if (!payload || json_object_set_new(payload, "username",
                                      json_string(username)) != 0) {
    if (payload) json_decref(payload);
    return -1;
  }
  return client_op(fd, NH_AUTH_OP_CHECK_ACCOUNT, payload, result_out);
}

static int parse_providers(const char *response_json,
                           nh_auth_provider_list *providers_out) {
  if (!providers_out) return 0;
  json_error_t e;
  json_t *root = json_loads(response_json, 0, &e);
  if (!root) return -1;
  json_t *arr = json_object_get(root, "providers");
  if (arr && json_is_array(arr)) {
    size_t idx;
    json_t *v;
    json_array_foreach(arr, idx, v) {
      if (!json_is_string(v)) continue;
      if (providers_out->count >= NH_AUTH_PROVIDER_LIST_CAP) break;
      const char *s = json_string_value(v);
      if (!s) continue;
      size_t sl = strlen(s);
      if (sl > NH_AUTH_PROVIDER_NAME_MAX) continue;
      memcpy(providers_out->names[providers_out->count], s, sl + 1);
      providers_out->count++;
    }
  }
  json_decref(root);
  return 0;
}

/* Parse account_username / identifier out of the BEGIN_LOGIN reply.
 * These are additive fields the broker attaches when the caller
 * supplied a NIP-05 identifier as `username`; absence is normal for
 * a canonical-username login. Bounded copies; malformed values are
 * silently dropped (the PAM module falls back to whatever it typed). */
static void parse_canonical(const char *response_json,
                            nh_auth_login_canonical *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!response_json) return;
  json_error_t je;
  json_t *root = json_loads(response_json, 0, &je);
  if (!root) return;
  json_t *cu = json_object_get(root, "account_username");
  if (cu && json_is_string(cu)) {
    const char *v = json_string_value(cu);
    if (v && *v) {
      size_t n = strlen(v);
      if (n < sizeof out->canonical) memcpy(out->canonical, v, n + 1);
    }
  }
  json_t *id = json_object_get(root, "identifier");
  if (id && json_is_string(id)) {
    const char *v = json_string_value(id);
    if (v && *v) {
      size_t n = strlen(v);
      if (n < sizeof out->identifier) memcpy(out->identifier, v, n + 1);
    }
  }
  json_decref(root);
}

int nh_auth_client_begin_login_with_identifier(
    int fd, const char *username, const char *service,
    const char *identifier,
    nh_auth_provider_list *providers_out,
    nh_auth_login_canonical *canonical_out,
    nh_auth_result *result_out) {
  if (!username || !service || !result_out) return -1;
  if (providers_out) memset(providers_out, 0, sizeof *providers_out);
  if (canonical_out) memset(canonical_out, 0, sizeof *canonical_out);

  json_t *begin = json_object();
  if (!begin || json_object_set_new(begin, "username", json_string(username)) ||
      json_object_set_new(begin, "service", json_string(service))) {
    if (begin) json_decref(begin);
    return -1;
  }
  /* Additive-optional payload field: the caller asserts the NIP-05
   * identifier that canonicalised to `username` on an earlier
   * connection so the broker can carry it into the greeter artifact.
   * Old brokers ignore unknown payload keys per §5.3. */
  if (identifier && identifier[0]) {
    if (json_object_set_new(begin, "identifier", json_string(identifier))) {
      json_decref(begin);
      return -1;
    }
  }
  char *response_json = NULL;
  if (client_op_raw(fd, NH_AUTH_OP_BEGIN_LOGIN, begin, &response_json) != 0)
    return -1;
  int rc = extract_result(response_json, result_out);
  if (rc == 0 && *result_out == NH_AUTH_RESULT_OK) {
    if (providers_out) (void)parse_providers(response_json, providers_out);
    if (canonical_out) parse_canonical(response_json, canonical_out);
  }
  free(response_json);
  return rc;
}

int nh_auth_client_begin_login_ex(int fd, const char *username,
                                  const char *service,
                                  nh_auth_provider_list *providers_out,
                                  nh_auth_login_canonical *canonical_out,
                                  nh_auth_result *result_out) {
  return nh_auth_client_begin_login_with_identifier(
      fd, username, service, NULL, providers_out, canonical_out, result_out);
}

int nh_auth_client_begin_login(int fd, const char *username, const char *service,
                               nh_auth_provider_list *providers_out,
                               nh_auth_result *result_out) {
  return nh_auth_client_begin_login_ex(fd, username, service, providers_out,
                                       NULL, result_out);
}

int nh_auth_client_begin_smb_proof(int fd, const char *service,
                                   nh_auth_provider_list *providers_out,
                                   nh_auth_result *result_out) {
  if (!result_out) return -1;
  if (providers_out) memset(providers_out, 0, sizeof *providers_out);

  json_t *begin = json_object();
  if (!begin) return -1;
  if (service && service[0]) {
    if (json_object_set_new(begin, "service", json_string(service))) {
      json_decref(begin);
      return -1;
    }
  }
  char *response_json = NULL;
  if (client_op_raw(fd, NH_AUTH_OP_BEGIN_SMB_PROOF, begin, &response_json) != 0)
    return -1;
  int rc = extract_result(response_json, result_out);
  if (rc == 0 && *result_out == NH_AUTH_RESULT_OK && providers_out)
    (void)parse_providers(response_json, providers_out);
  free(response_json);
  return rc;
}

int nh_auth_client_submit_selection(int fd, const char *provider,
                                    const char *passphrase,
                                    nh_auth_result *result_out) {
  return nh_auth_client_submit_selection_display(fd, provider, passphrase,
                                                 NULL, NULL, NULL, result_out);
}

/* Parses the optional display block from a SELECT_PROVIDER response
 * payload. Never fatal — a malformed display leaves *out zeroed. */
static void parse_display(const char *response_json, nh_auth_display *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!response_json) return;
  json_error_t je;
  json_t *root = json_loads(response_json, 0, &je);
  if (!root) return;
  json_t *disp = json_object_get(root, "display");
  if (!disp || !json_is_object(disp)) { json_decref(root); return; }
  json_t *k = json_object_get(disp, "kind");
  json_t *u = json_object_get(disp, "uri");
  json_t *h = json_object_get(disp, "hint");
  json_t *pc = json_object_get(disp, "pairing_code");
  json_t *ex = json_object_get(disp, "expires_in_ms");
  if (k && json_is_string(k)) {
    const char *v = json_string_value(k);
    if (v) { strncpy(out->kind, v, sizeof out->kind - 1); }
  }
  if (u && json_is_string(u)) {
    const char *v = json_string_value(u);
    if (v) { strncpy(out->uri, v, sizeof out->uri - 1); }
  }
  if (h && json_is_string(h)) {
    const char *v = json_string_value(h);
    if (v) { strncpy(out->hint, v, sizeof out->hint - 1); }
  }
  if (pc && json_is_string(pc)) {
    const char *v = json_string_value(pc);
    if (v) { strncpy(out->pairing_code, v, sizeof out->pairing_code - 1); }
  }
  if (ex && json_is_integer(ex)) {
    json_int_t v = json_integer_value(ex);
    if (v > 0 && v <= 0xffffffffLL) out->expires_in_ms = (uint32_t)v;
  }
  /* expires_at is the absolute unix-seconds deadline emitted by the
   * broker (design §5.3 + greeter-extension consumer contract). PAM does
   * not currently render it — it uses expires_in_ms for a local
   * countdown — but the field is parsed so future callers (and tests)
   * can assert the wire truth. */
  json_t *eat = json_object_get(disp, "expires_at");
  if (eat && json_is_integer(eat)) {
    json_int_t v = json_integer_value(eat);
    if (v > 0) out->expires_at = (int64_t)v;
  }
  json_decref(root);
}

int nh_auth_client_submit_selection_display(int fd, const char *provider,
                                            const char *secret,
                                            nh_auth_display *display_out,
                                            nh_auth_display_callback on_display,
                                            void *on_display_ctx,
                                            nh_auth_result *result_out) {
  if (!provider || !result_out) return -1;
  int is_nip46 = !strcmp(provider, NH_AUTH_PROVIDER_NAME_NIP46);
  int is_qr = !strcmp(provider, NH_AUTH_PROVIDER_NAME_NIP46_QR);
  int is_local = !strcmp(provider, NH_AUTH_PROVIDER_NAME_LOCAL);
  if (!is_nip46 && !is_qr && !is_local) return -1;
  if (is_local && !secret) return -1;

  if (display_out) memset(display_out, 0, sizeof *display_out);

  json_t *select = json_object();
  if (!select ||
      json_object_set_new(select, "provider", json_string(provider))) {
    if (select) json_decref(select);
    return -1;
  }
  char *sel_response = NULL;
  if (client_op_raw(fd, NH_AUTH_OP_SELECT_PROVIDER, select, &sel_response) != 0)
    return -1;
  if (extract_result(sel_response, result_out) != 0) {
    free(sel_response);
    return -1;
  }
  /* Best-effort display parse regardless of provider — old servers omit
   * it, new servers may include it for future flows too. */
  nh_auth_display local_disp;
  nh_auth_display *disp = display_out ? display_out : &local_disp;
  parse_display(sel_response, disp);
  free(sel_response);
  if (disp->kind[0] && on_display) on_display(on_display_ctx, disp);

  if (*result_out != NH_AUTH_RESULT_OK &&
      *result_out != NH_AUTH_RESULT_INTERACTION_REQUIRED)
    return 0;

  /* SUBMIT_UNLOCK secret payload: local sends the passphrase; pre-paired
   * bunker sends the placeholder "approve"; QR sends the placeholder
   * "qr". The provider ignores the value on the external-signer paths. */
  const char *submit_secret;
  if (is_local) submit_secret = secret;
  else if (is_qr) submit_secret = "qr";
  else submit_secret = NH_AUTH_CLIENT_APPROVE_TOKEN;

  json_t *unlock = json_object();
  if (!unlock ||
      json_object_set_new(unlock, "secret", json_string(submit_secret))) {
    if (unlock) json_decref(unlock);
    return -1;
  }
  return client_op(fd, NH_AUTH_OP_SUBMIT_UNLOCK, unlock, result_out);
}

int nh_auth_client_login_with(int fd, const char *username, const char *service,
                              const char *provider, const char *passphrase,
                              nh_auth_result *result_out) {
  if (!username || !service || !provider || !result_out) return -1;
  if (nh_auth_client_begin_login(fd, username, service, NULL, result_out) != 0)
    return -1;
  if (*result_out != NH_AUTH_RESULT_OK) return 0;
  return nh_auth_client_submit_selection(fd, provider, passphrase, result_out);
}

int nh_auth_client_login(int fd, const char *username, const char *service,
                         const char *passphrase, nh_auth_result *result_out) {
  if (!passphrase) return -1;
  return nh_auth_client_login_with(fd, username, service,
                                   NH_AUTH_PROVIDER_NAME_LOCAL, passphrase,
                                   result_out);
}

void nh_auth_smb_envelope_clear(nh_auth_smb_envelope *e) {
  if (!e) return;
  if (e->password.ptr) secure_free(&e->password);
  secure_wipe(e->credential_id, sizeof e->credential_id);
  secure_wipe(e->username, sizeof e->username);
  e->password_len = 0;
  e->issued_at_ms = 0;
  e->expires_at_ms = 0;
}

/* Populates *env from the SUBMIT_UNLOCK response payload. Returns 0 on
 * success, -1 if any required field is missing or ill-typed. Wipes any
 * partial state on failure. */
static int parse_smb_envelope(const char *response_json,
                              nh_auth_smb_envelope *env) {
  json_error_t e;
  json_t *root = json_loads(response_json, 0, &e);
  if (!root) return -1;
  int rc = -1;
  json_t *cred = json_object_get(root, "credential");
  if (!cred || !json_is_object(cred)) goto done;
  json_t *jcid = json_object_get(cred, "credential_id");
  json_t *juser = json_object_get(cred, "username");
  json_t *jpw = json_object_get(cred, "password");
  json_t *jiat = json_object_get(cred, "issued_at_ms");
  json_t *jexp = json_object_get(cred, "expires_at_ms");
  if (!jcid || !json_is_string(jcid) || !juser || !json_is_string(juser) ||
      !jpw || !json_is_string(jpw) || !jiat || !json_is_integer(jiat) ||
      !jexp || !json_is_integer(jexp))
    goto done;
  const char *cid = json_string_value(jcid);
  const char *user = json_string_value(juser);
  const char *pw = json_string_value(jpw);
  size_t pw_len = jpw ? json_string_length(jpw) : 0;
  if (strlen(cid) >= sizeof env->credential_id ||
      strlen(user) >= sizeof env->username ||
      pw_len == 0 || pw_len > NH_AUTH_SMB_PASSWORD_MAX_LEN)
    goto done;
  nostr_secure_buf buf = secure_alloc(pw_len + 1);
  if (!buf.ptr) goto done;
  memcpy(buf.ptr, pw, pw_len);
  ((char *)buf.ptr)[pw_len] = '\0';
  strncpy(env->credential_id, cid, sizeof env->credential_id - 1);
  env->credential_id[sizeof env->credential_id - 1] = '\0';
  strncpy(env->username, user, sizeof env->username - 1);
  env->username[sizeof env->username - 1] = '\0';
  env->issued_at_ms = (uint64_t)json_integer_value(jiat);
  env->expires_at_ms = (uint64_t)json_integer_value(jexp);
  env->password_len = pw_len;
  env->password = buf;
  rc = 0;
done:
  /* Best-effort: wipe the parsed password string in the JSON tree. jansson
   * strings are immutable at the API level but the underlying bytes live in
   * a heap-allocated buffer we can overwrite via json_string_value(). */
  if (cred && json_is_object(cred)) {
    json_t *jpw2 = json_object_get(cred, "password");
    if (jpw2 && json_is_string(jpw2)) {
      const char *s = json_string_value(jpw2);
      if (s) {
        volatile char *v = (volatile char *)s;
        for (size_t i = 0; s[i]; i++) v[i] = 0;
      }
    }
  }
  json_decref(root);
  return rc;
}

int nh_auth_client_smb_proof_with(int fd, const char *service,
                                  const char *provider, const char *passphrase,
                                  nh_auth_smb_envelope *envelope_out,
                                  nh_auth_result *result_out) {
  if (!provider || !result_out || !envelope_out) return -1;
  memset(envelope_out, 0, sizeof *envelope_out);
  int is_nip46 = !strcmp(provider, NH_AUTH_PROVIDER_NAME_NIP46);
  if (!is_nip46 && strcmp(provider, NH_AUTH_PROVIDER_NAME_LOCAL) != 0)
    return -1;
  if (!is_nip46 && !passphrase) return -1;

  if (nh_auth_client_begin_smb_proof(fd, service, NULL, result_out) != 0)
    return -1;
  if (*result_out != NH_AUTH_RESULT_OK) return 0;

  json_t *select = json_object();
  if (!select ||
      json_object_set_new(select, "provider", json_string(provider))) {
    if (select) json_decref(select);
    return -1;
  }
  if (client_op(fd, NH_AUTH_OP_SELECT_PROVIDER, select, result_out) != 0)
    return -1;
  if (*result_out != NH_AUTH_RESULT_OK &&
      *result_out != NH_AUTH_RESULT_INTERACTION_REQUIRED)
    return 0;

  const char *secret = is_nip46 ? NH_AUTH_CLIENT_APPROVE_TOKEN : passphrase;
  json_t *unlock = json_object();
  if (!unlock || json_object_set_new(unlock, "secret", json_string(secret))) {
    if (unlock) json_decref(unlock);
    return -1;
  }
  char *response_json = NULL;
  if (client_op_raw(fd, NH_AUTH_OP_SUBMIT_UNLOCK, unlock, &response_json) != 0)
    return -1;
  int rc = extract_result(response_json, result_out);
  if (rc == 0 && *result_out == NH_AUTH_RESULT_OK) {
    if (parse_smb_envelope(response_json, envelope_out) != 0) {
      /* Malformed envelope: turn it into an internal error so the caller
       * doesn't act on partial state. */
      *result_out = NH_AUTH_RESULT_INTERNAL_ERROR;
      nh_auth_smb_envelope_clear(envelope_out);
    }
  }
  /* Wipe the response text before releasing it: it echoes the password. */
  if (response_json) {
    volatile char *v = (volatile char *)response_json;
    for (size_t i = 0; response_json[i]; i++) v[i] = 0;
  }
  free(response_json);
  return rc;
}
