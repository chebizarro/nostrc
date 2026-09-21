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
  free(payload_json);
  if (rc != 0) return -1;

  unsigned char *packet = NULL;
  size_t packet_len = 0;
  if (nh_auth_recv_packet(fd, &packet, &packet_len) != 0) return -1;
  nh_auth_message response;
  rc = nh_auth_message_parse(packet, packet_len, &response);
  free(packet);
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

int nh_auth_client_begin_login(int fd, const char *username, const char *service,
                               nh_auth_provider_list *providers_out,
                               nh_auth_result *result_out) {
  if (!username || !service || !result_out) return -1;
  if (providers_out) memset(providers_out, 0, sizeof *providers_out);

  json_t *begin = json_object();
  if (!begin || json_object_set_new(begin, "username", json_string(username)) ||
      json_object_set_new(begin, "service", json_string(service))) {
    if (begin) json_decref(begin);
    return -1;
  }
  char *response_json = NULL;
  if (client_op_raw(fd, NH_AUTH_OP_BEGIN_LOGIN, begin, &response_json) != 0)
    return -1;
  int rc = extract_result(response_json, result_out);
  if (rc == 0 && *result_out == NH_AUTH_RESULT_OK && providers_out) {
    json_error_t e;
    json_t *root = json_loads(response_json, 0, &e);
    if (root) {
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
    }
  }
  free(response_json);
  return rc;
}

int nh_auth_client_submit_selection(int fd, const char *provider,
                                    const char *passphrase,
                                    nh_auth_result *result_out) {
  if (!provider || !result_out) return -1;
  int is_nip46 = !strcmp(provider, NH_AUTH_PROVIDER_NAME_NIP46);
  if (!is_nip46 && strcmp(provider, NH_AUTH_PROVIDER_NAME_LOCAL) != 0)
    return -1;
  if (!is_nip46 && !passphrase) return -1;

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
