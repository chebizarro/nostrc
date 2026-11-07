#ifndef SIGNET_TEST_HTTP_CLIENT_H
#define SIGNET_TEST_HTTP_CLIENT_H
#include "test_check.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int http_connect(const char *ip, unsigned short port) {
  int family = strchr(ip, ':') ? AF_INET6 : AF_INET;
  int fd = socket(family, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval timeout = {.tv_sec = 3};
  CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  int rc;
  if (family == AF_INET) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    CHECK(inet_pton(AF_INET, ip, &a.sin_addr) == 1);
    rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
  } else {
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
    CHECK(inet_pton(AF_INET6, ip, &a.sin6_addr) == 1);
    rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
  }
  if (rc) { close(fd); return -1; }
  return fd;
}

static int http_get(unsigned short port, const char *path, const char *headers,
                    char *response, size_t size) {
  int fd = http_connect("127.0.0.1", port);
  CHECK(fd >= 0);
  char request[2048];
  int n = snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n%s\r\n",
                   path, headers ? headers : "");
  CHECK(n > 0 && (size_t)n < sizeof(request));
  CHECK(send(fd, request, (size_t)n, 0) == n);
  size_t used = 0;
  for (;;) {
    ssize_t got = recv(fd, response + used, size - used - 1, 0);
    CHECK(got >= 0);
    if (!got) break;
    used += (size_t)got;
    CHECK(used < size - 1);
  }
  close(fd);
  response[used] = '\0';
  int status = 0;
  CHECK(sscanf(response, "HTTP/%*s %d", &status) == 1);
  return status;
}
#endif
