/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_VAULT_READER_H
#define NH_SYNCD_VAULT_READER_H
#include "nh_syncd_vault_config.h"
#include "nh_syncd_vault_session.h"
typedef struct nh_vault_reader nh_vault_reader;
/* 1: not owned, 0: immutable verified handle, negative: owned but unavailable.
 * Public fallback fetches sealed bytes without identity/signing custody. */
int nh_vault_reader_open(const nh_syncd_vault_config *, const char *state_dir,
                         const uint8_t home_key[32], nh_syncd_cache *,
                         const vault_blob_io *public_io, const char *path, uint64_t generation,
                         uint64_t size, const char *const *chunks, size_t count,
                         nh_vault_reader **);
int nh_vault_reader_read(nh_vault_reader *, uint64_t, uint8_t *, size_t, size_t *);
void nh_vault_reader_close(nh_vault_reader *);
#endif
