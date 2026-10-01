/*
 * libmarmot - adopted-profile GroupContext and member-leaf admission
 * (nostrc-qp24.5.1).
 *
 * The structural half of admitting an adopted-profile group (marmot-protocol/
 * marmot @07da8ff: protocol-core/group-setup.md, the app-components docs,
 * foundation/registries.md; MDK v0.11.0 cgka-engine app_components.rs):
 * the GroupContext's required_capabilities and app_data_dictionary, the
 * Marmot component states libmarmot understands, and each member leaf's
 * capabilities and account-proof carrier.  Every check here is about bytes
 * and is cheap enough to repeat whenever a group state is created, joined,
 * loaded or cloned.  The BIP-340 verification of each leaf's account proof
 * is the Marmot layer's (kp_profile.c, adopted.c).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_APP_COMPONENTS_H
#define MLS_APP_COMPONENTS_H

#include "mls-internal.h"
#include "mls_tree.h"
#include "mls_app_data_update.h"
#include <marmot/marmot-group-profile.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RFC 9420 §17.3 */
#define MLS_EXT_REQUIRED_CAPABILITIES        0x0003
/* foundation/registries.md: draft-ietf-mls-extensions-10 code points */
#define MLS_PROPOSAL_TYPE_APP_DATA_UPDATE    0x0008
#define MLS_COMPONENT_SAFE_AAD               0x0002
/* Marmot component ids (foundation/registries.md) */
#define MARMOT_COMPONENT_GROUP_PROFILE_V1    0x8001
#define MARMOT_COMPONENT_NOSTR_ROUTING_V1    0x8004
#define MARMOT_COMPONENT_ENCRYPTED_MEDIA_V1  0x8008
#define MLS_COMPONENT_ACCOUNT_PROOF_V2       0x8009
#define MARMOT_COMPONENT_GROUP_LIFECYCLE_V1  0x800c
/* Legacy-profile markers: MIP-01 group data, superseded proof v1. */
#define MLS_LEGACY_EXT_GROUP_DATA            0xF2EE
#define MLS_LEGACY_EXT_ACCOUNT_PROOF_V1      0xF2F1

/* MarmotAuthorizationProof (foundation/authorization-proofs.md) */
#define MLS_ACCOUNT_PROOF_V2_LEN             104

/* Component state bounds (the app-components docs, MDK cgka-traits). */
#define MARMOT_GROUP_PROFILE_NAME_MAX        256
#define MARMOT_GROUP_PROFILE_DESCRIPTION_MAX 4096
#define MARMOT_NOSTR_ROUTING_MAX_RELAYS      16
#define MARMOT_NOSTR_RELAY_URL_MAX           512

/*
 * The adopted components libmarmot can be required to support, i.e. the
 * GroupContext state it validates and keeps, sorted.  A group requiring any
 * other component is refused (MARMOT_ERR_UNSUPPORTED): "a member that does
 * not support every required component id MUST NOT join the group".  The
 * same list is what libmarmot's adopted leaves advertise.
 */
#define MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT 5
extern const uint16_t MLS_ADOPTED_SUPPORTED_COMPONENTS[MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT];

/*
 * Proposal types beyond app_data_update (0x0008) and the RFC 9420 defaults
 * that an adopted group may require.  None yet: SelfRemove (0x000a), which
 * every White Noise group requires, is W24 slice B (nostrc-2um6); add it
 * here once libmarmot advertises and processes it.
 */
#define MLS_ADOPTED_EXTRA_PROPOSAL_COUNT 0

/* Bounded parse results (anything larger contains an unsupported or
 * repeated id and is refused before it is stored). */
#define MLS_ADOPTED_MAX_IDS 16

typedef struct {
    /* required_capabilities, as listed */
    uint16_t ext_types[MLS_ADOPTED_MAX_IDS];
    size_t   n_ext_types;
    uint16_t proposal_types[MLS_ADOPTED_MAX_IDS];
    size_t   n_proposal_types;
    uint16_t credential_types[MLS_ADOPTED_MAX_IDS];
    size_t   n_credential_types;

    /* GroupContext app_components: the required component ids, ascending */
    uint16_t components[MLS_ADOPTED_MAX_IDS];
    size_t   n_components;

    /* Component states (borrowed from the extension bytes; NULL if absent) */
    const uint8_t *profile;   size_t profile_len;    /* 0x8001 */
    const uint8_t *admins;    size_t admins_len;     /* 0x8003 */
    const uint8_t *routing;   size_t routing_len;    /* 0x8004 */
    bool           has_lifecycle;                    /* 0x800c (always active) */
} MlsAdoptedGroupContext;

/*
 * The profile a GroupContext extension list (concatenated Extension entries,
 * as MlsGroup.extensions_data) claims.  One carrying an app_data_dictionary
 * extension -- exactly what libmarmot refused before 0.12.0 -- is ADOPTED
 * and must then pass mls_adopted_group_context_parse().  Everything else is
 * LEGACY and keeps its historical read behaviour (opaque, even
 * non-canonical, bytes; Marmot-layer GroupData rules).
 */
MarmotGroupProfile mls_group_context_profile_of(const uint8_t *exts, size_t len);

/*
 * Strict validation of an adopted GroupContext extension list.
 * Returns 0, or:
 *  - MARMOT_ERR_EXTENSION_FORMAT: malformed, truncated, non-canonical or
 *    repeated encodings, a missing mandatory element (required_capabilities,
 *    app_data_dictionary, app_components, the 0x0006/0x0008 requirements,
 *    the 0x8003/0x8004/0x8009 component requirements, a required
 *    component's state), invalid component state, or 0x8009 state in the
 *    GroupContext;
 *  - MARMOT_ERR_VALIDATION: a mixed-profile group (0xF2EE group data or a
 *    0xF2F1 proof requirement next to the adopted ones), frozen
 *    encrypted-media v1 (0x8008), a disbanded lifecycle;
 *  - MARMOT_ERR_UNSUPPORTED: a well-formed requirement libmarmot cannot
 *    honour (another extension type, proposal type, credential type or
 *    required component; safe_aad framing).
 */
int mls_adopted_group_context_parse(const uint8_t *exts, size_t len,
                                    MlsAdoptedGroupContext *out);

/*
 * The structural member check of an adopted group: every leaf is a 32-byte
 * BasicCredential whose capabilities cover the group's required_capabilities
 * (RFC 9420 §7.2 defaults implied), whose only LeafNode extension is one
 * canonical app_data_dictionary advertising (app_components) every required
 * component and 0x8009, and which carries exactly one 104-byte 0x8009 proof
 * naming its own credential identity; with @admins_are_members, every admin
 * key names a member (admin-policy-v1.md: a property of every epoch a group
 * enters, checked where one is entered -- a Welcome, a Commit -- but not of
 * the founding epoch-0 state, whose co-admins join with the founding Commit,
 * as in MDK).  The proofs' signatures are NOT verified here.  0 or
 * MARMOT_ERR_VALIDATION (MARMOT_ERR_EXTENSION_FORMAT for an undecodable leaf
 * dictionary).
 */
int mls_adopted_tree_check(const MlsRatchetTree *tree, const MlsAdoptedGroupContext *gc,
                           bool admins_are_members);

/* mls_adopted_tree_check() for one leaf (e.g. an invitee's KeyPackage leaf
 * before it is added), without the admin cross-check. */
int mls_adopted_leaf_check(const MlsLeafNode *leaf, const MlsAdoptedGroupContext *gc);

/*
 * Classify (mls_group_context_profile_of()) and, for ADOPTED, validate the
 * GroupContext and (when @tree is non-NULL) the members.  *profile_out is
 * set on success.  Errors as the two functions above.
 */
int mls_group_profile_admit(const uint8_t *exts, size_t exts_len,
                            const MlsRatchetTree *tree, MarmotGroupProfile *profile_out);

/* ── Component codecs (Marmot canonical encoding: QUIC varint lengths) ── */

/* marmot.group.profile.v1: name<0..256>, description<0..4096>, UTF-8. */
int mls_group_profile_v1_decode(const uint8_t *data, size_t len,
                                const uint8_t **name, size_t *name_len,
                                const uint8_t **description, size_t *description_len);

/* marmot.group.admin-policy.v1: admins<V> of sorted, unique 32-byte keys,
 * at least one.  *keys borrows @data (count * 32 contiguous bytes). */
int mls_admin_policy_v1_decode(const uint8_t *data, size_t len,
                               const uint8_t **keys, size_t *count);

typedef struct {
    const uint8_t *url;   /* borrowed, not NUL-terminated */
    size_t         len;
} MlsRelaySpan;

/* marmot.transport.nostr.routing.v1: nostr_group_id[32], relays<V> of
 * 1..16 sorted, unique relay URLs (transports/nostr.md relay URL profile). */
int mls_nostr_routing_v1_decode(const uint8_t *data, size_t len,
                                const uint8_t **nostr_group_id,
                                MlsRelaySpan relays[MARMOT_NOSTR_ROUTING_MAX_RELAYS],
                                size_t *relay_count);

/* The Nostr relay URL profile (transports/nostr.md "relay URL profile"):
 * UTF-8, 1..512 bytes, absolute ws/wss URL with a host and no userinfo or
 * fragment.  Conservative: also refuses whitespace, control characters and
 * backslashes, which a URL parser would rewrite.  The host is checked so
 * that nothing MDK's url::Url refuses gets through (W24 review N1): an IPv6
 * literal must be well formed (no zone id); a name must be ASCII, free of
 * the WHATWG forbidden host/domain code points (including '%'), and, if its
 * last label is numeric, a strict dotted-quad IPv4 address.  Some URLs MDK
 * accepts are refused here (non-ASCII or percent-encoded hosts, short or
 * octal IPv4 forms, a space in the path); that direction fails closed. */
bool mls_relay_url_valid(const uint8_t *url, size_t len);

/* Strict UTF-8 (no overlongs, surrogates or code points past U+10FFFF). */
bool mls_utf8_valid(const uint8_t *s, size_t len);

/* ComponentsList { ComponentID component_ids<V>; }, strictly ascending.
 * Up to @cap ids are copied to @ids; more is an error. */
int mls_components_list_decode_strict(const uint8_t *data, size_t len,
                                      uint16_t *ids, size_t cap, size_t *n);

#endif /* MLS_APP_COMPONENTS_H */
