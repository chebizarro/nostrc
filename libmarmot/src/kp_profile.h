/*
 * libmarmot - adopted-spec KeyPackage profile internals (nostrc-prqu.9).
 * See kp_profile.c and MarmotKeyPackageProfile in marmot.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MARMOT_KP_PROFILE_H
#define MARMOT_KP_PROFILE_H

#include <marmot/marmot.h>
#include "mls/mls_key_package.h"
#include "mls/mls-internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Registries (foundation/registries.md @26fa6a6). */
#define MARMOT_EXT_APP_DATA_DICTIONARY      0x0006
#define MARMOT_PROPOSAL_APP_DATA_UPDATE     0x0008
#define MARMOT_COMPONENT_APP_COMPONENTS     0x0001
#define MARMOT_COMPONENT_SAFE_AAD           0x0002
#define MARMOT_COMPONENT_LAST_RESORT_KP     0x0004
#define MARMOT_COMPONENT_ACCOUNT_PROOF_V2   0x8009

/* RFC 9420 §6 */
#define MARMOT_MLS_VERSION_10               0x0001
#define MARMOT_MLS_WIRE_FORMAT_KEY_PACKAGE  0x0005
/* MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519 signs with ed25519 */
#define MARMOT_SIGNATURE_SCHEME_ED25519     0x0807

/* app-components/account-identity-proof-v2.md */
#define MARMOT_KIND_ACCOUNT_PROOF           450
#define MARMOT_ACCOUNT_PROOF_LEN            104
#define MARMOT_ACCOUNT_PROOF_CONTENT        "Authorize this MLS leaf key for my Marmot account"

/* foundation/key-packages.md: not_after - not_before upper bound */
#define MARMOT_KP_LIFETIME_MAX_RANGE        7261200

typedef struct {
    uint16_t       component_id;
    const uint8_t *data;   /* borrowed */
    size_t         len;
} MarmotComponentData;

/* ComponentsList { ComponentID component_ids<V>; } */
int  marmot_components_list_encode(const uint16_t *ids, size_t n, MlsTlsBuf *out);
int  marmot_components_list_decode(const uint8_t *data, size_t len,
                                   uint16_t **ids_out, size_t *n_out);
bool marmot_u16_list_contains(const uint16_t *ids, size_t n, uint16_t id);

/* AppDataDictionary { ComponentData component_data<V>; }, entries strictly
 * ascending by component_id (encode refuses, parse rejects otherwise).
 * Parsed entries borrow from @data; free(*entries_out). */
int marmot_app_data_dict_encode(const MarmotComponentData *entries, size_t n, MlsTlsBuf *out);
int marmot_app_data_dict_parse(const uint8_t *data, size_t len,
                               MarmotComponentData **entries_out, size_t *n_out);

/* In an Extension list (the bytes inside extensions<V>): the first
 * extension of @type (borrowed) and how many there are. */
int marmot_extensions_find(const uint8_t *exts, size_t len, uint16_t type,
                           const uint8_t **data_out, size_t *dlen_out, size_t *count_out);

/* MLSMessage { mls10, mls_key_package, KeyPackage } */
int marmot_mls_message_frame_key_package(const MlsKeyPackage *kp, MlsTlsBuf *out);
int marmot_mls_message_unframe_key_package(const uint8_t *data, size_t len,
                                           MlsKeyPackage *kp_out);

/* Account-identity proof v2: signed by @account_sk, else via @sign_fn. */
MarmotError marmot_account_proof_create(const uint8_t account_pk[32], const uint8_t *account_sk,
                                        MarmotAccountSignFunc sign_fn, void *sign_data,
                                        uint16_t ciphersuite, uint16_t signature_scheme,
                                        const uint8_t *sig_key, size_t sig_key_len,
                                        uint64_t created_at,
                                        uint8_t out[MARMOT_ACCOUNT_PROOF_LEN]);
MarmotError marmot_account_proof_verify(const uint8_t *proof, size_t proof_len,
                                        const uint8_t identity[32], uint16_t ciphersuite,
                                        uint16_t signature_scheme, const uint8_t *sig_key,
                                        size_t sig_key_len);
/* NIP-01 id (hex) of the signing template, for tests. */
char *marmot_account_proof_template_id(const uint8_t account_pk[32], uint64_t created_at,
                                       uint16_t ciphersuite, uint16_t signature_scheme,
                                       const uint8_t *sig_key, size_t sig_key_len);

/* ADOPTED producer without the MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER
 * build gate, for libmarmot's own tests only. */
MarmotError marmot_create_key_package_adopted_internal(Marmot *m, const uint8_t nostr_pubkey[32],
                                                       const uint8_t nostr_sk[32],
                                                       MarmotAccountSignFunc account_sign,
                                                       void *sign_data,
                                                       MarmotKeyPackageResult *result);

#endif /* MARMOT_KP_PROFILE_H */
