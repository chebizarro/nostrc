#ifndef NOSTR_HOMECTL_H
#define NOSTR_HOMECTL_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

int nh_open_session(const char *username);
int nh_close_session(const char *username);
int nh_warm_cache(const char *npub_hex);
int nh_get_status(const char *username, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_HOMECTL_H */
