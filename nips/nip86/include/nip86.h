#ifndef NIP86_H
#define NIP86_H

#ifdef __cplusplus
extern "C" {
#endif

/* Process a NIP-86 JSON-RPC request body.
 * app_ctx: pointer to app context (e.g., relayd RelaydCtx)
 * auth: Authorization header value (Nostr <base64(nip98-event)>)
 * body: JSON-RPC object {"method":"...","params":[...]}
 * method: HTTP method (e.g., "POST") used in the request
 * url: absolute URL of the request (scheme://host/path?query)
 * http_status: out parameter set to HTTP status (200 on success, 401 unauthorized, 400 bad request, 500 internal)
 * Returns: malloc'd JSON string {"result":...,"error":null|"msg"}
 */
char *nostr_nip86_process_request(void *app_ctx, const char *auth, const char *body, const char *method, const char *url, int *http_status);

/* Policy state getters (module-local storage) */
int nostr_nip86_is_pubkey_banned(const char *hex32);
int nostr_nip86_has_allowlist(void);
int nostr_nip86_is_pubkey_allowed(const char *hex32);
int nostr_nip86_has_allowed_kinds(void);
int nostr_nip86_is_kind_allowed(int kind);
int nostr_nip86_is_ip_blocked(const char *ip);

/* Load policy from disk (relay_policy.json or NOSTR_RELAY_POLICY). Returns 0 on success or if no file. */
int nostr_nip86_load_policy(void);

#ifdef __cplusplus
}
#endif

#endif /* NIP86_H */
