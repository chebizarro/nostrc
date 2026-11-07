#ifndef SIGNET_HTTP_LISTEN_H
#define SIGNET_HTTP_LISTEN_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct {
  struct sockaddr_storage address;
  socklen_t length;
  int family;
} SignetHttpListen;
typedef struct {
  unsigned char address[16];
  unsigned int prefix;
  int family;
} SignetHttpCidr;
int signet_http_listen_parse(const char *text, SignetHttpListen *out);
int signet_http_cidr_parse(const char *text, SignetHttpCidr *out);
bool signet_http_cidr_matches(const SignetHttpCidr *mask, const struct sockaddr *peer);
#endif
