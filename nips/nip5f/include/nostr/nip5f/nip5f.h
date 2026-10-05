#ifndef NIPS_NIP5F_NOSTR_NIP5F_NIP5F_H
#define NIPS_NIP5F_NOSTR_NIP5F_NIP5F_H

/* See SPEC source: nips/nip5f/SPEC_SOURCE -> ../../docs/proposals/5F.md */

#ifdef __cplusplus
extern "C" {
#endif

/* Error domain for NIP-5F: errors are returned as int codes per SPEC. */

/* Server handlers */
typedef int (*Nip5fGetPubFn)(void *ud, char **out_pub_hex);
typedef int (*Nip5fSignEventFn)(void *ud, const char *event_json, const char *pubkey_hex, char **out_signed_event_json);
typedef int (*Nip5fNip44EncFn)(void *ud, const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64);
typedef int (*Nip5fNip44DecFn)(void *ud, const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext);
typedef int (*Nip5fListKeysFn)(void *ud, char **out_keys_json);

/* Error codes (docs/proposals/5F.md). */
enum {
  NIP5F_ERR_INVALID_REQUEST = 1,
  NIP5F_ERR_UNSUPPORTED     = 2,
  NIP5F_ERR_INVALID_PARAMS  = 3,
  NIP5F_ERR_NO_KEY          = 4,
  NIP5F_ERR_DECLINED        = 5,
  NIP5F_ERR_INTERNAL        = 10
};

/* The process at the other end of a connection, as the kernel reported it
 * when the connection was accepted (Linux SO_PEERCRED / SO_PEERPIDFD, macOS
 * getpeereid + LOCAL_PEERPID). Never taken from anything the client sends.
 * have_creds == 0 for transports without credentials (TCP). */
typedef struct {
  int have_creds;
  unsigned int uid;
  int pid;      /* 0 = unknown */
  int pidfd;    /* Linux pidfd of the connecting process, or -1; owned by the server */
  int conn_fd;  /* the connection; for liveness checks only (never read/write it) */
} Nip5fPeer;

/* Servers that decide per caller (grotto-daemon) install hooks; then
 * every method goes through @request and the built-in handlers are never used.
 *   open    - called once per connection, on its thread, right after accept
 *             (before the handshake: nothing read from the client yet).
 *             Returns per-connection state, or NULL to refuse the connection.
 *   request - one request. May block (e.g. while the user is asked). Params:
 *               get_public_key, list_public_keys: a = b = NULL
 *               sign_event:    a = event JSON, b = pubkey hex or NULL
 *               nip44_encrypt: a = peer pubkey, b = plaintext
 *               nip44_decrypt: a = peer pubkey, b = base64 payload
 *             Returns 0 with *out_result_json = the raw JSON result (malloc),
 *             or a NIP5F_ERR_* code with optional *out_message (malloc).
 *   close   - the connection ended; free @conn. */
typedef struct {
  void *(*open)(void *ud, const Nip5fPeer *peer);
  int   (*request)(void *ud, void *conn, const char *method, const char *a, const char *b,
                   char **out_result_json, char **out_message);
  void  (*close)(void *ud, void *conn);
} Nip5fServerHooks;

/* Server control. Every server accepts only peers of its own uid. */
int nostr_nip5f_server_start(const char *socket_path, void **out_handle);
/* Start with @hooks installed before the first accept (see above). */
int nostr_nip5f_server_start_with_hooks(const char *socket_path, const Nip5fServerHooks *hooks,
                                        void *user_data, void **out_handle);
/* Serve one connection a transport accepted and authenticated itself (the
 * daemon's TCP lane) through @hooks, on a new detached thread. Takes
 * ownership of @fd and of peer->pidfd. Returns 0 on success. */
int nostr_nip5f_serve_connection(int fd, const Nip5fPeer *peer, const Nip5fServerHooks *hooks,
                                 void *user_data);
int nostr_nip5f_server_stop(void *handle);
int nostr_nip5f_server_set_handlers(void *handle,
  Nip5fGetPubFn get_pub, Nip5fSignEventFn sign_event,
  Nip5fNip44EncFn enc44, Nip5fNip44DecFn dec44,
  Nip5fListKeysFn list_keys, void *user_data);

/* Built-in default handlers using libnostr primitives */
int nostr_nip5f_builtin_get_public_key(char **out_pub_hex);
int nostr_nip5f_builtin_sign_event(const char *event_json, const char *pubkey_hex, char **out_signed_event_json);
int nostr_nip5f_builtin_nip44_encrypt(const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64);
int nostr_nip5f_builtin_nip44_decrypt(const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext);
int nostr_nip5f_builtin_list_public_keys(char **out_keys_json);

/* Client helpers */
int nostr_nip5f_client_connect(const char *socket_path, void **out_conn);
int nostr_nip5f_client_close(void *conn);
int nostr_nip5f_client_get_public_key(void *conn, char **out_pub_hex);
int nostr_nip5f_client_sign_event(void *conn, const char *event_json, const char *pubkey_hex, char **out_signed_event_json);
int nostr_nip5f_client_nip44_encrypt(void *conn, const char *peer_pub_hex, const char *plaintext, char **out_cipher_b64);
int nostr_nip5f_client_nip44_decrypt(void *conn, const char *peer_pub_hex, const char *cipher_b64, char **out_plaintext);
int nostr_nip5f_client_list_public_keys(void *conn, char **out_keys_json);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP5F_NOSTR_NIP5F_NIP5F_H */
