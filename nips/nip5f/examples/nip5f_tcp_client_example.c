#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "json.h"

/* Reuse NIP-5F framing helpers (declared in sock_framing.c) */
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

static int do_jsonrpc_get_pubkey(int fd, char **out_pub) {
  *out_pub = NULL;
  const char *hello = "{\"name\":\"nip5f-tcp-client-example\",\"version\":1}";
  char *banner=NULL; size_t blen=0;
  if (nip5f_read_frame(fd, &banner, &blen)!=0) return -1; /* ignore content */
  free(banner);
  if (nip5f_write_frame(fd, hello, strlen(hello))!=0) return -1;
  const char *req = "{\"id\":\"1\",\"method\":\"get_public_key\",\"params\":null}";
  if (nip5f_write_frame(fd, req, strlen(req))!=0) return -1;
  char *resp=NULL; size_t rlen=0;
  if (nip5f_read_frame(fd, &resp, &rlen)!=0) return -1;
  /* Extract result string using nostr_json_get_string on the raw JSON */
  char *rid=NULL; (void)nostr_json_get_string(resp, "id", &rid);
  if (!rid || strcmp(rid, "1")!=0) { free(rid); free(resp); return -1; }
  free(rid);
  char *pub=NULL; if (nostr_json_get_string(resp, "result", &pub)!=0 || !pub) { free(resp); return -1; }
  *out_pub = pub; free(resp); return 0;
}

int main(int argc, char **argv) {
  const char *host = "127.0.0.1";
  unsigned short port = 5897;
  const char *token = getenv("NOSTR_SIGNER_TOKEN");
  for (int i=1;i<argc;i++) {
    if (!strncmp(argv[i], "--host=", 7)) host = argv[i]+7;
    else if (!strncmp(argv[i], "--port=", 7)) port = (unsigned short)atoi(argv[i]+7);
    else if (!strncmp(argv[i], "--token=", 8)) token = argv[i]+8;
  }
  if (!token || !*token) {
    fprintf(stderr, "Missing token. Set --token or NOSTR_SIGNER_TOKEN.\n");
    return 2;
  }
  int fd = tcp_connect(host, port);
  if (fd < 0) { fprintf(stderr, "connect failed: %s\n", strerror(errno)); return 1; }
  /* Send AUTH line then switch to framed protocol */
  dprintf(fd, "AUTH %s\n", token);
  char *pub=NULL;
  int rc = do_jsonrpc_get_pubkey(fd, &pub);
  if (rc!=0) { fprintf(stderr, "RPC failed\n"); close(fd); return 1; }
  printf("pubkey: %s\n", pub);
  free(pub);
  close(fd);
  return 0;
}
