#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "json.h"

int nip5f_read_frame(int fd, char **out_json, size_t *out_len);
int nip5f_write_frame(int fd, const char *json, size_t len);

static int tcp_connect(const char *host, unsigned short port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(fd); return -1; }
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  return fd;
}

/* A provisioned signer reaches only this registration: it alone declares
 * RESOURCE_GROUPS nip5f_signer:1, and the engine's resource spec carries the
 * loopback port and token as the resource id "p<port>_<64 hex>". CTest sets
 * CTEST_RESOURCE_GROUP_COUNT only when it schedules resources (it strips an
 * inherited value), so the count decides the route: under scheduling only an
 * exact allocation runs; anything else fails. Returns 1 with an exact
 * allocation, 0 without one and -1 when malformed. Never prints the token. */
static int ctest_allocation(char *ep, size_t ep_len, char *tok, size_t tok_len) {
  const char *count = getenv("CTEST_RESOURCE_GROUP_COUNT");
  if (!count || strcmp(count, "0") == 0) return 0;
  const char *group = getenv("CTEST_RESOURCE_GROUP_0");
  const char *value = getenv("CTEST_RESOURCE_GROUP_0_NIP5F_SIGNER");
  if (strcmp(count, "1") != 0 || !group || strcmp(group, "nip5f_signer") != 0 || !value
      || strncmp(value, "id:p", 4) != 0) return -1;
  const char *port = value + 4, *sep = strchr(port, '_');
  size_t digits = sep ? (size_t)(sep - port) : 0;
  if (digits < 1 || digits > 5 || strspn(port, "0123456789") != digits || port[0] == '0') return -1;
  const char *hex = sep + 1;
  if (strspn(hex, "0123456789abcdef") != 64 || strcmp(hex + 64, ",slots:1") != 0 || tok_len < 65) return -1;
  long number = strtol(port, NULL, 10);
  if (number > 65535 || snprintf(ep, ep_len, "tcp:127.0.0.1:%ld", number) >= (int)ep_len) return -1;
  memcpy(tok, hex, 64); tok[64] = '\0';
  return 1;
}

static int run_signer(const char *ep, const char *tok);

int main(void) {
  char allocated_ep[32], allocated_tok[65];
  int allocated = ctest_allocation(allocated_ep, sizeof(allocated_ep), allocated_tok, sizeof(allocated_tok));
  if (allocated < 0) { fprintf(stderr, "Malformed CTest nip5f_signer resource allocation\n"); return 1; }
  if (allocated) return run_signer(allocated_ep, allocated_tok);
  if (getenv("CTEST_RESOURCE_GROUP_COUNT")) {
    /* Resource-scheduled run without this allocation: a delivery failure, never
     * a skip and never inherited credentials. */
    fprintf(stderr, "No nip5f_signer resource allocated to this registration\n");
    return 1;
  }
  const char *ep = getenv("NOSTR_SIGNER_ENDPOINT");
  const char *tok = getenv("NOSTR_SIGNER_TOKEN");
  if (!ep || strncmp(ep, "tcp:", 4) != 0 || !tok || !*tok) {
    /* NOTE: For in-process loopback testing without env vars, see test_nip5f_loopback.c */
    fprintf(stderr, "Skipping TCP test (no tcp endpoint/token configured)\n");
    fprintf(stderr, "For in-process testing, run test_nip5f_loopback instead\n");
    return 77;  /* Signal SKIP to CTest */
  }
  return run_signer(ep, tok);
}

static int run_signer(const char *ep, const char *tok) {
  /* Parse tcp:HOST:PORT */
  const char *spec = ep + 4;
  const char *colon = strchr(spec, ':');
  if (!colon) { fprintf(stderr, "Bad endpoint: %s\n", ep); return 1; }
  char host[64]; memset(host, 0, sizeof(host));
  size_t hlen = (size_t)(colon - spec); if (hlen >= sizeof(host)) hlen = sizeof(host)-1;
  memcpy(host, spec, hlen); host[hlen] = '\0';
  unsigned short port = (unsigned short)atoi(colon+1);
  int fd = tcp_connect(host, port);
  if (fd < 0) { fprintf(stderr, "connect failed: %s\n", strerror(errno)); return 1; }
  dprintf(fd, "AUTH %s\n", tok);
  /* NIP-5F handshake and get_public_key */
  char *banner=NULL; size_t blen=0;
  if (nip5f_read_frame(fd, &banner, &blen)!=0) { close(fd); return 1; }
  free(banner);
  const char *hello = "{\"name\":\"test-nip5f-tcp\",\"version\":1}";
  if (nip5f_write_frame(fd, hello, strlen(hello))!=0) { close(fd); return 1; }
  const char *req = "{\"id\":\"42\",\"method\":\"get_public_key\",\"params\":null}";
  if (nip5f_write_frame(fd, req, strlen(req))!=0) { close(fd); return 1; }
  char *resp=NULL; size_t rlen=0;
  if (nip5f_read_frame(fd, &resp, &rlen)!=0) { close(fd); return 1; }
  char *rid=NULL; (void)nostr_json_get_string(resp, "id", &rid);
  if (!rid || strcmp(rid, "42")!=0) { free(rid); free(resp); close(fd); return 1; }
  free(rid);
  char *pub=NULL; if (nostr_json_get_string(resp, "result", &pub)!=0 || !pub) { free(resp); close(fd); return 1; }
  /* Basic sanity: 64 hex chars */
  int ok = (strlen(pub) == 64);
  free(pub); free(resp); close(fd);
  return ok ? 0 : 1;
}
