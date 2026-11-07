#include "signet/health_server.h"
#include "../src/http_listen.h"
#include "test_http_client.h"
#include <ifaddrs.h>
#include <net/if.h>

static void check_nonloop(unsigned short port, bool allowed) {
  struct ifaddrs *all = NULL;
  CHECK(getifaddrs(&all) == 0);
  bool checked = false;
  for (struct ifaddrs *it = all; it; it = it->ifa_next) {
    if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET ||
        !(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
    char ip[INET_ADDRSTRLEN];
    CHECK(inet_ntop(AF_INET, &((struct sockaddr_in *)it->ifa_addr)->sin_addr, ip, sizeof(ip)));
    int fd = http_connect(ip, port);
    CHECK((fd >= 0) == allowed);
    if (fd >= 0) close(fd);
    checked = true;
  }
  freeifaddrs(all);
  if (!checked) fprintf(stderr, "CAPABILITY: no active non-loopback IPv4 interface\n");
}

int main(void) {
  SignetHttpListen addr;
  const char *bad[] = {"localhost:80", ":80", "127.0.0.1:", "127.0.0.1:-1",
    "127.0.0.1:+2", "127.0.0.1: 2", "127.0.0.1:65536", "::1:80", "[::1:80",
    "[::1]80", "[127.0.0.1]:80", "127.0.0.1:1x", "127.0.0.1:9999999999999999999999"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
    CHECK(signet_http_listen_parse(bad[i], &addr) != 0);
  CHECK(signet_http_listen_parse("127.0.0.1:0", &addr) == 0);
  CHECK(addr.family == AF_INET);
  SignetHealthServerConfig cfg = {.listen = "127.0.0.1:0"};
  SignetHealthServer *hs = signet_health_server_new(&cfg);
  CHECK(hs);
  CHECK(signet_health_server_bound_port(hs) == 0);
  CHECK(signet_health_server_start(hs) == 0);
  CHECK(signet_health_server_start(hs) == 0);
  unsigned short port = signet_health_server_bound_port(hs);
  CHECK(port);
  char response[4096];
  CHECK(http_get(port, "/health", NULL, response, sizeof(response)) == 503);
  check_nonloop(port, false);
  signet_health_server_stop(hs);
  CHECK(signet_health_server_bound_port(hs) == 0);
  signet_health_server_stop(hs);
  signet_health_server_free(hs);
  cfg.listen = "0.0.0.0:0";
  hs = signet_health_server_new(&cfg);
  CHECK(hs && signet_health_server_start(hs) == 0);
  check_nonloop(signet_health_server_bound_port(hs), true);
  signet_health_server_free(hs);
  cfg.listen = "[::1]:0";
  hs = signet_health_server_new(&cfg);
  CHECK(hs);
  if (signet_health_server_start(hs) == 0) {
    port = signet_health_server_bound_port(hs);
    CHECK(port);
    int fd = http_connect("::1", port);
    CHECK(fd >= 0); close(fd);
    CHECK(http_connect("127.0.0.1", port) < 0);
  } else {
    CHECK(signet_health_server_bound_port(hs) == 0);
    fprintf(stderr, "CAPABILITY: IPv6 loopback listener explicitly unavailable\n");
  }
  signet_health_server_free(hs);
  return 0;
}
