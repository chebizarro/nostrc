/*
 * Regression coverage for the cfg.listen host+port parser and validator.
 *
 * Before this parser existed, relayd_main.c only extracted the port from
 * cfg.listen via `strrchr(":")` and passed no interface hint to libwebsockets,
 * so any documented loopback bind (e.g. `127.0.0.1:4848` in
 * relay.toml.example) silently became a wide `0.0.0.0` bind. The parser plus
 * the validator hook now:
 *   (a) split cfg.listen into a real host string used as `info.iface`;
 *   (b) reject bare port forms so mis-configured relays fail at startup
 *       rather than silently opening the whole network.
 *
 * The full ss(1) socket assertion covering (a)+(b) end-to-end lives in the
 * companion shell test `run_bind_iface_integ.sh` — this unit test is the
 * parser contract every code path depends on.
 */
#include "relayd_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void expect_parse_ok(const char *listen, const char *expect_host,
                            int expect_port) {
  char host[64];
  int port = -1;
  int rc = relayd_config_parse_listen(listen, host, sizeof(host), &port);
  if (rc != 0) {
    fprintf(stderr, "FAIL: expected %s to parse, got rc=%d\n", listen, rc);
    exit(1);
  }
  if (strcmp(host, expect_host) != 0) {
    fprintf(stderr, "FAIL: %s -> host=%s (want %s)\n", listen, host,
            expect_host);
    exit(1);
  }
  if (port != expect_port) {
    fprintf(stderr, "FAIL: %s -> port=%d (want %d)\n", listen, port,
            expect_port);
    exit(1);
  }
}

static void expect_parse_reject(const char *listen) {
  char host[64];
  int port = -1;
  int rc = relayd_config_parse_listen(listen, host, sizeof(host), &port);
  if (rc == 0) {
    fprintf(stderr, "FAIL: expected %s to be rejected, got host=%s port=%d\n",
            listen ? listen : "<null>", host, port);
    exit(1);
  }
}

int main(void) {
  /* --- accept: canonical, wide, IPv6-bracketed, hostname forms ---------- */
  expect_parse_ok("127.0.0.1:4848", "127.0.0.1", 4848);
  expect_parse_ok("0.0.0.0:4848", "0.0.0.0", 4848);
  expect_parse_ok("localhost:8080", "localhost", 8080);
  expect_parse_ok("[::1]:4848", "::1", 4848);
  expect_parse_ok("[::]:65535", "::", 65535);
  expect_parse_ok("[fe80::1]:1", "fe80::1", 1);

  /* --- reject: port-only, empty, malformed ports, malformed brackets ---- */
  expect_parse_reject(NULL);
  expect_parse_reject("");
  expect_parse_reject("4848");           /* port-only */
  expect_parse_reject(":4848");          /* empty host */
  expect_parse_reject("127.0.0.1:");     /* empty port */
  expect_parse_reject("127.0.0.1");      /* no port */
  expect_parse_reject("127.0.0.1:0");    /* out-of-range low */
  expect_parse_reject("127.0.0.1:70000"); /* out-of-range high */
  expect_parse_reject("127.0.0.1:-1");   /* negative */
  expect_parse_reject("127.0.0.1:abc");  /* non-numeric port */
  expect_parse_reject("127.0.0.1: 4848"); /* whitespace in port */
  expect_parse_reject("::1:4848");        /* bare IPv6 must be bracketed */
  expect_parse_reject("[::1:4848");       /* unterminated bracket */
  expect_parse_reject("[]:4848");         /* empty bracketed host */
  expect_parse_reject("[::1]4848");       /* missing colon after ']' */
  expect_parse_reject("[::1]:");          /* bracketed, empty port */

  /* --- caller-supplied buffer too small ---------------------------------- */
  {
    char tiny[4];
    int port = -1;
    int rc = relayd_config_parse_listen("127.0.0.1:4848", tiny, sizeof(tiny),
                                        &port);
    expect(rc != 0, "tiny host buffer must be rejected, not truncated");
  }

  /* --- caller may pass NULL outputs to just validate ---------------------- */
  expect(relayd_config_parse_listen("127.0.0.1:4848", NULL, 0, NULL) == 0,
         "validate-only NULL outputs accept a valid listen string");
  expect(relayd_config_parse_listen("4848", NULL, 0, NULL) != 0,
         "validate-only NULL outputs reject a bare port");

  /* --- validator hook: default config valid; port-only rejected --------- */
  {
    RelaydConfig cfg;
    expect(relayd_config_load(NULL, &cfg) == 0,
           "default config with listen=127.0.0.1:4848 is valid");
    snprintf(cfg.listen, sizeof(cfg.listen), "4848");
    char err[128];
    expect(relayd_config_validate(&cfg, err, sizeof(err)) != 0,
           "port-only listen is rejected");
    expect(strstr(err, "listen") != NULL,
           "validator error message mentions the listen field");
  }

  /* --- validator hook: bracketed IPv6 in cfg accepted -------------------- */
  {
    RelaydConfig cfg;
    expect(relayd_config_load(NULL, &cfg) == 0, "reload default config");
    snprintf(cfg.listen, sizeof(cfg.listen), "[::1]:4848");
    expect(relayd_config_validate(&cfg, NULL, 0) == 0,
           "bracketed IPv6 listen is accepted");
  }

  puts("OK");
  return 0;
}
