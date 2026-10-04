/* A local Blossom server (BUD-01 GET /<sha256>, BUD-02 PUT /upload) on
 * SoupServer at 127.0.0.1 for the G21 attachment tests (privacy charter
 * §8.2 G21: "a local Blossom fixture on SoupServer"). It keeps blobs in
 * memory only and records every request, including the kind-24242
 * authorization it was sent, checked the way a server does: valid id and
 * signature, kind 24242, ["t", "upload"], ["x"] equal to the body's SHA-256
 * and an expiration in the future. An upload without a valid authorization
 * is refused (401, with an X-Reason), and so is one whose pubkey is not the
 * required one when a pubkey is required. It can be scripted to lie in its
 * descriptor, to hold its answers, to serve a blob in chunks without a
 * Content-Length, and to stall after the first chunk (AT-3, AT-8).
 * Runs on the thread-default main context it was made on. */
#ifndef GH_TEST_BLOSSOM_FIXTURE_H
#define GH_TEST_BLOSSOM_FIXTURE_H

#include <gio/gio.h>

typedef struct {
  gchar *method;       /* "PUT", "GET", ... */
  gchar *path;
  gboolean has_auth;   /* an Authorization header was sent */
  gboolean auth_valid; /* it was a valid kind-24242 upload authorization */
  gchar *auth_pubkey;  /* its pubkey (NULL without a parsable event) */
  gchar *auth_x;
  gchar *auth_server;
  gchar *content_type;
  gchar *x_sha256;
  gboolean auth_base64url;
  gint64 auth_expiration;
  gsize body_size;
} BlossomRequest;

typedef struct _BlossomFixture BlossomFixture;

BlossomFixture *blossom_fixture_new(void);
void blossom_fixture_free(BlossomFixture *fixture);
/* "http://127.0.0.1:<port>" */
const gchar *blossom_fixture_url(BlossomFixture *fixture);
guint16 blossom_fixture_port(BlossomFixture *fixture);

/* Uploads are accepted only when signed by pubkey (NULL: any valid key). */
void blossom_fixture_require_pubkey(BlossomFixture *fixture, const gchar *pubkey);
/* Enforce BUD-11 Base64url and BUD-02 ciphertext headers. */
void blossom_fixture_set_strict_upload(BlossomFixture *fixture, gboolean strict);
/* Simulate a media-only server: body signatures, not declared MIME, decide;
 * opaque bytes get HTTP 415/X-Reason even if labeled image/png. */
void blossom_fixture_reject_opaque(BlossomFixture *fixture, gboolean reject);
/* The descriptor names this sha256 instead of the real one (NULL: honest). */
void blossom_fixture_set_lie(BlossomFixture *fixture, const gchar *sha256);
/* Answers wait until released (blossom_fixture_release_held). */
void blossom_fixture_set_hold(BlossomFixture *fixture, gboolean hold);
void blossom_fixture_release_held(BlossomFixture *fixture);
/* GETs are streamed in chunks without a Content-Length. */
void blossom_fixture_set_chunked(BlossomFixture *fixture, gboolean chunked);
/* GETs stall after the first chunk until released (implies chunked). */
void blossom_fixture_set_stall(BlossomFixture *fixture, gboolean stall);

/* Stores bytes under sha256 as if uploaded (e.g. a tampered copy). */
void blossom_fixture_put_blob(BlossomFixture *fixture, const gchar *sha256, GBytes *bytes);
/* The blob kept under sha256, or NULL. */
GBytes *blossom_fixture_get_blob(BlossomFixture *fixture, const gchar *sha256);

/* BlossomRequest, in arrival order. */
GPtrArray *blossom_fixture_requests(BlossomFixture *fixture);
guint blossom_fixture_count(BlossomFixture *fixture, const gchar *method);
/* A counter a test can wait on: requests with a held answer. */
guint blossom_fixture_held(BlossomFixture *fixture);

#endif
