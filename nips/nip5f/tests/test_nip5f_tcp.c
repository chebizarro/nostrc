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

int main(void) {
  const char *ep = getenv("NOSTR_SIGNER_ENDPOINT");
  const char *tok = getenv("NOSTR_SIGNER_TOKEN");
  if (!ep || strncmp(ep, "tcp:", 4) != 0 || !tok || !*tok) {
    /* Signal SKIP to CTest (set in CMake) */
    fprintf(stderr, "Skipping (no tcp endpoint/token)\n");
    return 77;
  }
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
