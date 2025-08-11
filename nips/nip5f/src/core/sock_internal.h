#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIP5F_MAX_FRAME (1024 * 1024)

int nip5f_read_frame(int fd, char **out_json, size_t *out_len);
int nip5f_write_frame(int fd, const char *json, size_t len);

/* Resolve socket path: env NOSTR_SIGNER_SOCK or default $HOME/.local/share/nostr/signer.sock.
 * Returns newly-allocated string or NULL. */
char *nip5f_resolve_socket_path(void);

/* Ensure parent directory exists with 0700 perms. Returns 0 on success. */
int nip5f_ensure_socket_dirs(const char *socket_path);

#ifdef __cplusplus
}
#endif
