#include "http_listen.h"
#include <arpa/inet.h>
#include <string.h>

static int decimal(const char *s, unsigned int max, unsigned int *out) {
  if (!s || !*s) return -1;
  unsigned int n = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return -1;
    n = n * 10 + (unsigned int)(*s - '0');
    if (n > max) return -1;
  }
  *out = n;
  return 0;
}

int signet_http_listen_parse(const char *text, SignetHttpListen *out) {
  if (!text || !out) return -1;
  memset(out, 0, sizeof(*out));
  char host[INET6_ADDRSTRLEN];
  const char *start = text, *end, *port;
  if (*text == '[') {
    start++;
    end = strchr(start, ']');
    if (!end || end[1] != ':') return -1;
    port = end + 2;
    out->family = AF_INET6;
  } else {
    end = strchr(start, ':');
    if (!end) return -1;
    port = end + 1;
    out->family = AF_INET;
  }
  size_t n = (size_t)(end - start);
  unsigned int p;
  if (!n || n >= sizeof(host) || decimal(port, 65535, &p)) return -1;
  memcpy(host, start, n);
  host[n] = '\0';
  if (out->family == AF_INET) {
    struct sockaddr_in *a = (struct sockaddr_in *)&out->address;
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)p);
    if (inet_pton(AF_INET, host, &a->sin_addr) != 1) return -1;
    out->length = sizeof(*a);
  } else {
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)&out->address;
    a->sin6_family = AF_INET6;
    a->sin6_port = htons((uint16_t)p);
    if (inet_pton(AF_INET6, host, &a->sin6_addr) != 1) return -1;
    out->length = sizeof(*a);
  }
  return 0;
}

int signet_http_cidr_parse(const char *text, SignetHttpCidr *out) {
  if (!text || !out) return -1;
  memset(out, 0, sizeof(*out));
  const char *slash = strchr(text, '/');
  if (!slash) return -1;
  size_t n = (size_t)(slash - text);
  char host[INET6_ADDRSTRLEN];
  if (!n || n >= sizeof(host)) return -1;
  memcpy(host, text, n);
  host[n] = '\0';
  out->family = strchr(host, ':') ? AF_INET6 : AF_INET;
  if (decimal(slash + 1, out->family == AF_INET ? 32 : 128, &out->prefix) ||
      inet_pton(out->family, host, out->address) != 1) return -1;
  return 0;
}

bool signet_http_cidr_matches(const SignetHttpCidr *mask, const struct sockaddr *peer) {
  if (!mask || !peer) return false;
  const unsigned char *a;
  if (mask->family != peer->sa_family) return false;
  if (peer->sa_family == AF_INET)
    a = (const unsigned char *)&((const struct sockaddr_in *)peer)->sin_addr;
  else if (peer->sa_family == AF_INET6)
    a = (const unsigned char *)&((const struct sockaddr_in6 *)peer)->sin6_addr;
  else return false;
  unsigned int whole = mask->prefix / 8, bits = mask->prefix % 8;
  return memcmp(a, mask->address, whole) == 0 &&
         (!bits || ((a[whole] ^ mask->address[whole]) & (0xff << (8 - bits))) == 0);
}
