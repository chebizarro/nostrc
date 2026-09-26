/* Test helpers: signed events and a mock session relay (WebSocket on a
 * private AF_UNIX socket, no network). */
#ifndef NSP_TESTUTIL_H
#define NSP_TESTUTIL_H

#include <gio/gio.h>

#define NSP_TEST_SK_ALICE "0000000000000000000000000000000000000000000000000000000000000001"
#define NSP_TEST_SK_BOB "0000000000000000000000000000000000000000000000000000000000000002"

/* Signed event JSON. @tags: flat NULL-terminated list of key,value pairs. */
char *nsp_test_event(const char *sk_hex, int kind, gint64 created_at, const char *content,
                     const char *const *tags);
char *nsp_test_pubkey(const char *sk_hex);
char *nsp_test_event_id(const char *event_json);

typedef enum {
  NSP_MOCK_ANSWER,  /* matching events, then EOSE */
  NSP_MOCK_SILENT,  /* never answers a REQ (cache-less session relay) */
  NSP_MOCK_NO_EOSE, /* matching events, but never EOSE */
} NspMockMode;

typedef struct NspMockRelay NspMockRelay;

/* @nip50: honour "search"; otherwise CLOSED "unsupported: search" as
 * relayd does without a search-capable store. */
NspMockRelay *nsp_mock_relay_new(NspMockMode mode, gboolean nip50);
void nsp_mock_relay_free(NspMockRelay *m);
const char *nsp_mock_relay_socket(NspMockRelay *m);
void nsp_mock_relay_add(NspMockRelay *m, const char *event_json);
guint nsp_mock_relay_reqs(NspMockRelay *m);
guint nsp_mock_relay_search_reqs(NspMockRelay *m);

/* Spin the default main context until *flag or @timeout_ms. */
gboolean nsp_test_wait(gboolean *flag, guint timeout_ms);

#endif
