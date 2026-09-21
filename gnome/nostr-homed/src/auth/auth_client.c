#define _GNU_SOURCE
#include "auth_client.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

int nh_auth_client_check_account(int fd, const char *username,
                                 nh_auth_result *result_out) {
  if (!username || !result_out) return -1;
  json_t *payload = json_object();
  if (!payload) return -1;
  if (json_object_set_new(payload, "username", json_string(username)) != 0) {
    json_decref(payload);
    return -1;
  }
  char *payload_json = json_dumps(payload, JSON_COMPACT);
  json_decref(payload);
  if (!payload_json) return -1;

  nh_auth_message request;
  memset(&request, 0, sizeof request);
  request.operation = NH_AUTH_OP_CHECK_ACCOUNT;
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

  rc = -1;
  json_error_t e;
  json_t *root = response.payload_json
                     ? json_loads(response.payload_json, 0, &e)
                     : NULL;
  json_t *result = root ? json_object_get(root, "result") : NULL;
  if (result && json_is_string(result) &&
      result_from_name(json_string_value(result), result_out) == 0)
    rc = 0;
  if (root) json_decref(root);
  nh_auth_message_clear(&response);
  return rc;
}
