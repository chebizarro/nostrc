/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_VAULT_CONFIG_H
#define NH_SYNCD_VAULT_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    char *path; /* HOME-relative directory, without trailing slash */
    uint8_t vault_id[32];
    bool is_owner;
} nh_syncd_vault_config_entry;
typedef struct {
    nh_syncd_vault_config_entry *vaults;
    size_t len;
} nh_syncd_vault_config;
/* Strict JSON {"vaults":[{"path":...,"vault_id":64hex,"is_owner":bool}]}.
 * NULL means no configuration; an explicit missing/invalid file is an error.
 * Nested/duplicate paths and repeated IDs are ambiguous generation sources. */
int nh_syncd_vault_config_load(const char *, nh_syncd_vault_config *);
void nh_syncd_vault_config_clear(nh_syncd_vault_config *);
/* Matches the complete subtree, not lexical prefix neighbours. Also keeps the
 * shared conflict directory out of the legacy personal-home generation. */
bool nh_syncd_vault_config_excludes(void *, const char *);
#endif
