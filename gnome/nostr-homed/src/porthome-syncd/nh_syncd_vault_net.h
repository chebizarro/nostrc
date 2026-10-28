/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_VAULT_NET_H
#define NH_SYNCD_VAULT_NET_H
#include <vault/tree.h>
#include <nostr-event.h>
typedef struct nh_vault_net nh_vault_net;
/* URLs and identity remain borrowed until close. HTTP/private-address Blossom
 * transport is enabled only by the daemon's explicit test-mode switch. */
int nh_vault_net_open(const char *secret, const char *const *relays, size_t,
                      const char *const *blossom, size_t, size_t replication, int test_mode,
                      nh_vault_net **);
void nh_vault_net_close(nh_vault_net *);
int nh_vault_net_reconcile(void *, const uint8_t[32], const uint8_t *, size_t);
vault_blob_io nh_vault_net_blobs(nh_vault_net *);
int nh_vault_net_cache_only(const nh_vault_net *);
void nh_vault_net_read_limit(nh_vault_net *, size_t);
/* Catch-up is bounded by EOSE from every configured relay, not elapsed quiet.
 * A deadline is failure. stop/join drains callbacks before returning events. */
int nh_vault_net_catchup(nh_vault_net *, const uint8_t vault_id[32], const uint8_t (*writers)[32],
                         size_t, NostrEvent ***, size_t *);
/* GC discovery: signed addressable heads only, every relay must reach EOSE.
 * NULL tag discovers this identity's home/vault addresses. Saturation refuses. */
int nh_vault_net_gc_heads(nh_vault_net *, const char *tag, const uint8_t (*writers)[32],
                          size_t, NostrEvent ***, size_t *);
int nh_vault_net_listen(nh_vault_net *, const uint8_t vault_id[32], const uint8_t (*writers)[32],
                        size_t);
int nh_vault_net_fd(const nh_vault_net *);
int nh_vault_net_drain(nh_vault_net *, NostrEvent ***, size_t *);
int nh_vault_net_publish(nh_vault_net *, NostrEvent *);
#endif
