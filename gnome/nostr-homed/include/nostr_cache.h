#ifndef NOSTR_CACHE_H
#define NOSTR_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  sqlite3 *db;
  uint32_t uid_base;
  uint32_t uid_range;
} nh_cache;

int nh_cache_open(nh_cache *c, const char *path);
void nh_cache_close(nh_cache *c);
int nh_cache_set_uid_policy(nh_cache *c, uint32_t base, uint32_t range);
uint32_t nh_cache_map_npub_to_uid(const nh_cache *c, const char *npub_hex);

/* Load configuration from a file like /etc/nss_nostr.conf:
 *   db_path=/var/lib/nostr-homed/cache.db
 *   uid_base=100000
 *   uid_range=100000
 * Missing file or keys fall back to defaults. Returns 0 on success.
 */
int nh_cache_open_configured(nh_cache *c, const char *conf_path);

/* User lookup helpers against the 'users' table. Return 0 on success, -1 on not found or error. */
int nh_cache_lookup_name(nh_cache *c, const char *name, unsigned int *uid, unsigned int *gid, char *home_out, size_t home_len);
int nh_cache_lookup_uid(nh_cache *c, unsigned int uid, char *name_out, size_t name_len, unsigned int *gid, char *home_out, size_t home_len);
int nh_cache_upsert_user(nh_cache *c, unsigned int uid, const char *npub, const char *username, unsigned int gid, const char *home);

/* Groups: minimal support for primary groups and membership */
int nh_cache_group_lookup_name(nh_cache *c, const char *name, unsigned int *gid);
int nh_cache_group_lookup_gid(nh_cache *c, unsigned int gid, char *name_out, size_t name_len);
int nh_cache_ensure_primary_group(nh_cache *c, const char *username, unsigned int gid);

/* Simple settings table helpers */
int nh_cache_set_setting(nh_cache *c, const char *key, const char *value);
int nh_cache_get_setting(nh_cache *c, const char *key, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_CACHE_H */
