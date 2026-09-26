/* nd-test-harness.h - Spawn a real NdDavServer for tests
 *
 * SPDX-License-Identifier: MIT
 *
 * Runs the server on its own thread + GMainContext (the libsoup client
 * calls in tests are synchronous), with a token store and SQLite store
 * rooted in a per-test temporary directory, configured exactly as the
 * daemon configures them: token ensured → account set → listen.
 */
#ifndef ND_TEST_HARNESS_H
#define ND_TEST_HARNESS_H

#include <glib.h>
#include <libsoup/soup.h>

#include "nd-dav-server.h"
#include "nd-store-db.h"
#include "nd-token-store.h"

G_BEGIN_DECLS

#define ND_TEST_ACCOUNT_ID "default"

typedef struct {
  gchar        *dir;          /* config + data root */
  gboolean      owns_dir;     /* remove @dir on stop */
  gchar        *token;        /* the valid bearer token */
  guint         port;
  gchar        *base_url;     /* http://127.0.0.1:<port> */
  SoupSession  *session;      /* adds valid Basic auth unless the request
                                 already carries an Authorization header */

  /* Server-thread state */
  NdTokenStore *token_store;
  NdStoreDb    *db;
  NdDavServer  *server;
  GMainLoop    *loop;
  GThread      *thread;
  GMutex        mutex;
  GCond         cond;
  gboolean      ready;
} NdTestServer;

/** Creates a fresh 0700 temporary directory. */
gchar *nd_test_make_tmpdir(void);

/** Recursively removes @dir. */
void nd_test_rm_rf(const gchar *dir);

/**
 * nd_test_server_start:
 * @dir: (nullable): root for `config/` and `data/`; NULL creates a
 *   temporary directory owned (and later removed) by the harness.
 *   Passing the same @dir across stop/start simulates a daemon restart.
 */
NdTestServer *nd_test_server_start(const gchar *dir);

/** Stops the server, closes the store, frees @ts. */
void nd_test_server_stop(NdTestServer *ts);

/** Paths the harness uses under @dir. */
gchar *nd_test_config_dir(const gchar *dir);
gchar *nd_test_db_path(const gchar *dir);

/** Returns "Basic base64(user:password)". */
gchar *nd_test_basic_auth(const gchar *user, const gchar *password);

G_END_DECLS
#endif /* ND_TEST_HARNESS_H */
