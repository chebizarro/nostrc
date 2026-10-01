#ifndef GH_BLOSSOM_CLIENT_H
#define GH_BLOSSOM_CLIENT_H

#include "gh-net-http.h"

G_BEGIN_DECLS

/*
 * GhBlossomClient: encrypted-attachment transfers with Blossom servers
 * (BUD-01 GET /<sha256>, BUD-02 PUT /upload; privacy charter §6, D6, G21).
 * Every request goes through GhNetHttp (gh-net-http.h), so the network mode,
 * Tor (a fresh circuit per request: charter §4.3 "Media ... random per
 * fetch"), the https-only rule (http only to .onion in Tor mode, or to a
 * loopback test server), no redirects, cookies, cache or TLS resumption all
 * hold. Only ciphertext ever passes through here.
 *
 * Servers (D6). The blossom-servers setting, empty by default: with no
 * server, an upload fails with GH_BLOSSOM_ERROR_NO_SERVER before anything is
 * contacted. Servers are tried in order; the first that accepts the file and
 * answers with a matching descriptor (sha256 and size) is used, and the file
 * URL is <server>/<sha256> (BUD-01), so no server-chosen address or
 * redirect is ever put in a message. A server that refuses the upload's
 * credentials ends the upload (GH_BLOSSOM_ERROR_AUTH_REQUIRED): the file
 * goes to no other server until the user decides.
 *
 * Upload authorization (kind 24242; charter §6 step 4, §4.3, AT-6). Signed by
 * a fresh throwaway key per upload, generated here and wiped once signed, so
 * an upload never names the account. Only for a server the user consented to
 * (gh_blossom_client_set_account_consent(), e.g. one that accepts only known
 * pubkeys) is the account asked to sign, through the account signer; without
 * consent the account signer is never called. The event carries
 * ["t", "upload"], ["x", <sha256>], ["expiration", now + 5 min] and
 * ["server", <host>] (BUD-11), so it is good for this file on this server
 * only, briefly.
 *
 * Download (charter §6 "Download"). Only ever the user's explicit action
 * (PD-2, AT-7): nothing here runs on its own. The answer is read into
 * memory, capped at min(size, cap + 16) + 1 KiB (AT-3), and never written to
 * disk. No authorization is sent. The address comes from the sender, so it
 * must be a Blossom blob on the public network (W17 review #3): its last
 * path segment is the file's x (BUD-01 GET /<sha256>, an extension
 * allowed), with no query or fragment, and its host is a public name or
 * address or a .onion; loopback, private, link-local, shared (CGNAT),
 * multicast and unspecified addresses, single-label names and localhost,
 * .local, .lan, .internal, .home.arpa and .localdomain names are refused
 * before any request (G_IO_ERROR_PERMISSION_DENIED), so a message cannot aim
 * Download at a service on the user's own machine or network. (A public
 * name that resolves to a private address is not caught here.)
 *
 * Main context only. One client per account generation: the account signer
 * and the consents belong to it.
 */

#define GH_BLOSSOM_MAX_FILE_SIZE (25 * 1024 * 1024) /* D6: of the file itself */
#define GH_BLOSSOM_AUTH_KIND 24242
#define GH_BLOSSOM_AUTH_LIFETIME_S 300
#define GH_BLOSSOM_DOWNLOAD_SLACK 1024

typedef enum {
  GH_BLOSSOM_ERROR_NO_SERVER = 1, /* no attachment server chosen (D6) */
  GH_BLOSSOM_ERROR_TOO_LARGE,     /* over the size cap */
  GH_BLOSSOM_ERROR_AUTH_REQUIRED, /* the server refused the throwaway key */
  GH_BLOSSOM_ERROR_REFUSED,       /* the server refused the upload otherwise */
  GH_BLOSSOM_ERROR_BAD_ANSWER,    /* the descriptor does not match the upload */
  GH_BLOSSOM_ERROR_SIGNER         /* the account signer failed or refused */
} GhBlossomError;
#define GH_BLOSSOM_ERROR gh_blossom_error_quark()
GQuark gh_blossom_error_quark(void);

/* Signs an unsigned event (JSON, pubkey set) with the account and completes
 * on the caller's thread-default context; the shape of
 * gh_account_controller_sign_with_cancellable_async()/_finish() (the same as
 * GhRelayAuthSignAsyncFunc in gh-relay-auth.h). */
typedef void (*GhBlossomSignAsyncFunc)(gpointer user_data, const gchar *unsigned_event_json,
                                       GCancellable *cancellable, GAsyncReadyCallback callback,
                                       gpointer callback_data);
typedef gchar *(*GhBlossomSignFinishFunc)(GAsyncResult *result, GError **error);

#define GH_TYPE_BLOSSOM_CLIENT (gh_blossom_client_get_type())
G_DECLARE_FINAL_TYPE(GhBlossomClient, gh_blossom_client, GH, BLOSSOM_CLIENT, GObject)

/* settings (nullable) supplies blossom-servers, read at each upload; http is
 * the app's GhNetHttp (a reference is kept). */
GhBlossomClient *gh_blossom_client_new(GSettings *settings, GhNetHttp *http);

/* The form in which servers are compared, and consents kept (G22,
 * nostrc-dnsc): scheme://host[:port][/path], scheme and host lowercased,
 * without a trailing slash; NULL for anything but an http(s) URL with a host
 * and without user info, query or fragment. out_host (nullable) receives the
 * host. */
gchar *gh_blossom_client_normalize_server(const gchar *server, gchar **out_host);

/* The configured servers (a copy): the override if set, else blossom-servers,
 * without blanks. */
GStrv gh_blossom_client_dup_servers(GhBlossomClient *self);
/* Replaces the servers read from settings (NULL: back to the settings). */
void gh_blossom_client_set_servers(GhBlossomClient *self, const gchar *const *servers);

/* The account signer (NULL sign_async: none) and the account pubkey it signs
 * as (64 hex, required with a signer). Setting it clears every consent. */
void gh_blossom_client_set_account_signer(GhBlossomClient *self, const gchar *account_pubkey,
                                          GhBlossomSignAsyncFunc sign_async,
                                          GhBlossomSignFinishFunc sign_finish,
                                          gpointer user_data, GDestroyNotify user_data_destroy);
/* The user's per-server consent to upload to server as the account (charter
 * §6 step 4). Kept in memory for this client only (nostrc-dnsc persists
 * it). */
void gh_blossom_client_set_account_consent(GhBlossomClient *self, const gchar *server,
                                           gboolean consent);
gboolean gh_blossom_client_get_account_consent(GhBlossomClient *self, const gchar *server);

/* Tests only: lets downloads reach loopback and other non-public hosts (the
 * local Blossom fixtures). Nothing in the application calls it. */
void gh_blossom_client_set_allow_private_hosts(GhBlossomClient *self, gboolean allow);

/* The largest file (plaintext) accepted; GH_BLOSSOM_MAX_FILE_SIZE by default.
 * Lowering it is for tests (AT-3 without 25 MiB transfers). */
void gh_blossom_client_set_max_file_size(GhBlossomClient *self, gsize max_file_size);
gsize gh_blossom_client_get_max_file_size(GhBlossomClient *self);

/* Uploads ciphertext, whose SHA-256 is sha256_hex, to the first server that
 * takes it. Finishes with the file URL (<server>/<sha256>) and, in
 * out_server (nullable), the server used (normalized). Errors:
 * GH_BLOSSOM_ERROR_*, the network's G_IO_ERROR_*, G_IO_ERROR_CANCELLED; with
 * GH_BLOSSOM_ERROR_AUTH_REQUIRED out_server names the server that asked for
 * an account it knows, the one to ask the user's consent for. */
void gh_blossom_client_upload_async(GhBlossomClient *self, GBytes *ciphertext,
                                    const gchar *sha256_hex, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data);
gchar *gh_blossom_client_upload_finish(GhBlossomClient *self, GAsyncResult *result,
                                       gchar **out_server, GError **error);

/* As upload_async(), for a blob another key owns (W25, nostrc-m6tp): an
 * encrypted group picture, whose 0x8002 state names the secp256k1
 * upload_key (copied into locked memory, wiped when done) that may replace
 * or delete it, as MDK uploads it. The authorization is signed with that
 * key, never the account's (no consent applies), and the servers are the
 * given list (the group's own media servers), not blossom-servers. Those
 * are someone else's choice, so they get the download rule (W25 review M1):
 * a server whose host is not public (loopback, private, link-local, local
 * names; gh_blossom_client_dup_public_servers()) is never contacted, and the
 * connection itself is public-only (GhNetHttpRequest.public_only). With no
 * server left: GH_BLOSSOM_ERROR_NO_SERVER, before any request. Finishes with
 * gh_blossom_client_upload_finish(). */
void gh_blossom_client_upload_keyed_async(GhBlossomClient *self, const gchar *const *servers,
                                          GBytes *ciphertext, const gchar *sha256_hex,
                                          const guint8 upload_key[32],
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data);

/* The servers of a list someone else named that this client may contact:
 * http(s) URLs whose host is public (as for downloads; any host with
 * gh_blossom_client_set_allow_private_hosts(), tests only), in order. */
GStrv gh_blossom_client_dup_public_servers(GhBlossomClient *self,
                                           const gchar *const *servers);

/* Downloads the ciphertext at url (the kind-15 content), whose SHA-256 is
 * sha256_hex (the message's x), into memory. size is the message's size tag
 * (0: none). An address that is not a public Blossom blob of that x (see
 * above) fails with G_IO_ERROR_PERMISSION_DENIED, and a size over the cap
 * with GH_BLOSSOM_ERROR_TOO_LARGE, both before any request. A host name that
 * resolves to no public address when connecting (System and No Proxy modes;
 * gh_net_http_get_public_async()) also fails with
 * G_IO_ERROR_PERMISSION_DENIED, without a connection. A longer answer
 * is cut off at min(size, cap + 16) + GH_BLOSSOM_DOWNLOAD_SLACK bytes
 * (G_IO_ERROR_MESSAGE_TOO_LARGE). Only for the user's explicit Download. */
void gh_blossom_client_download_async(GhBlossomClient *self, const gchar *url,
                                      const gchar *sha256_hex, guint64 size,
                                      GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer user_data);
GBytes *gh_blossom_client_download_finish(GhBlossomClient *self, GAsyncResult *result,
                                          GError **error);

G_END_DECLS
#endif
