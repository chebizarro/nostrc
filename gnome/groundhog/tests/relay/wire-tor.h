/* NT-10 transport parity (privacy charter §9.2, G09): registers each wire
 * test twice, once as it is (the GNostrRelay transport, what System and No
 * Proxy modes use) and, when built with GROUNDHOG_WIRE_TOR, once more under
 * "/tor" in Tor mode: the network session and relay dispatcher installed,
 * so every gh_relay_scope_new()/gh_relay_publish_new() goes through the
 * libsoup transport and the SOCKS5 fixture (H3) to the same local relays.
 * After a Tor run every connection must have gone through the fixture with
 * SOCKS credentials. */
#ifndef GH_TEST_WIRE_TOR_H
#define GH_TEST_WIRE_TOR_H

#include <glib.h>
#include <string.h>

#if GROUNDHOG_WIRE_TOR
#include "gh-relay-net.h"
#include "socks5-fixture.h"
#endif

typedef struct {
  const gchar *path;
  GTestFunc func;
  gboolean gnostr_only; /* exercises GNostrRelay itself, e.g. its signals */
} WireCase;

#if GROUNDHOG_WIRE_TOR
static void
wire_run_tor(gconstpointer data)
{
  const WireCase *wire = data;
  Socks5Fixture *socks = socks5_fixture_new();
  GhNetSession *session = gh_net_session_new(NULL);
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, socks5_fixture_address(socks));
  gh_relay_net_install(session);
  wire->func();
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, >, 0);
  for (guint i = 0; i < requests->len; i++) {
    Socks5Request *request = g_ptr_array_index(requests, i);
    g_assert_cmpuint(request->method, ==, 0x02);
    g_assert_nonnull(request->username);
    g_assert_cmpuint(strlen(request->username), ==, 16);
    g_assert_cmpstr(request->host, ==, "127.0.0.1");
  }
  gh_relay_net_install(NULL);
  g_object_unref(session);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  socks5_fixture_free(socks);
}
#endif

static void
wire_add_tests(const WireCase *cases, gsize n_cases)
{
  for (gsize i = 0; i < n_cases; i++)
    g_test_add_func(cases[i].path, cases[i].func);
#if GROUNDHOG_WIRE_TOR
  for (gsize i = 0; i < n_cases; i++) {
    if (cases[i].gnostr_only)
      continue;
    g_autofree gchar *path = g_strconcat("/tor", cases[i].path, NULL);
    g_test_add_data_func(path, &cases[i], wire_run_tor);
  }
#endif
}

#endif
