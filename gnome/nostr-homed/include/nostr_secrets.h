#ifndef NOSTR_SECRETS_H
#define NOSTR_SECRETS_H

#ifdef __cplusplus
extern "C" {
#endif

int nh_secrets_mount_tmpfs(const char *path);
int nh_secrets_decrypt_via_signer(const char *ciphertext, char **plaintext_out);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_SECRETS_H */
