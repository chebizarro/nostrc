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
#define MLS_PROPOSAL_TYPE_SELF_REMOVE        0x000a
#define MLS_COMPONENT_SAFE_AAD               0x0002
/* Marmot component ids (foundation/registries.md) */
#define MARMOT_COMPONENT_GROUP_PROFILE_V1    0x8001
#define MLS_COMPONENT_BLOSSOM_IMAGE_V1       0x8002
#define MARMOT_COMPONENT_NOSTR_ROUTING_V1    0x8004
#define MLS_COMPONENT_MESSAGE_RETENTION_V1   0x8005
#define MLS_COMPONENT_AGENT_TEXT_STREAM_V1   0x8006
#define MLS_COMPONENT_AVATAR_URL_V1          0x8007
#define MARMOT_COMPONENT_ENCRYPTED_MEDIA_V1  0x8008
#define MLS_COMPONENT_ACCOUNT_PROOF_V2       0x8009
#define MLS_COMPONENT_ENCRYPTED_MEDIA_V2     0x800b
#define MARMOT_COMPONENT_GROUP_LIFECYCLE_V1  0x800c
/* The agent-text-stream-QUIC receive role capability (an MLS extension
 * type in LeafNode capabilities; agent-text-stream-quic-v1.md). */
#define MLS_EXT_AGENT_STREAM_RECEIVE         0xF2D1
#define MLS_EXT_AGENT_STREAM_SEND            0xF2D2
#define MLS_EXT_AGENT_STREAM_FANOUT          0xF2D4
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
 * same list is what libmarmot's adopted leaves advertise.  Since 0.12.0
 * (nostrc-qp24.5.2) it covers what every White Noise group requires:
 * the agent text stream (0x8006, receive role only) and encrypted media v2
 * (0x800b).  0x8002 (Blossom image) and 0x8007 (avatar URL) are also
 * advertised (nostrc-l2ln).
 */
#define MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT 9
extern const uint16_t MLS_ADOPTED_SUPPORTED_COMPONENTS[MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT];

/*
 * What a group libmarmot creates requires (0x8001 0x8003 0x8004 0x8009
 * 0x800c, as an MDK 0.11 cgka-engine creator does).  A subset of the
 * supported list: libmarmot does not create agent-stream or media-policy
 * state.
 */
#define MLS_ADOPTED_CREATE_COMPONENT_COUNT 5
extern const uint16_t MLS_ADOPTED_CREATE_COMPONENTS[MLS_ADOPTED_CREATE_COMPONENT_COUNT];

/*
 * Beyond the RFC 9420 defaults, an adopted group may require of its
 * members' capabilities exactly what libmarmot's adopted leaves advertise
 * (mls_leaf_node_set_adopted_capabilities()): extension types 0x0006 and
 * the agent-stream receive role 0xF2D1, proposal types 0x0008 and
 * SelfRemove 0x000a (nostrc-2um6; every White Noise group requires it).
 */

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

    /* Component states (borrowed from the extension bytes; NULL if absent),
     * each valid when present, required or not (MDK
     * validate_app_component_dictionary). */
    const uint8_t *profile;   size_t profile_len;    /* 0x8001 */
    const uint8_t *image;     size_t image_len;      /* 0x8002 */
    const uint8_t *admins;    size_t admins_len;     /* 0x8003 */
    const uint8_t *routing;   size_t routing_len;    /* 0x8004 */
    const uint8_t *agent_stream; size_t agent_stream_len; /* 0x8006 */
    const uint8_t *avatar;    size_t avatar_len;     /* 0x8007 */
    const uint8_t *media_policy; size_t media_policy_len; /* 0x800b */
    bool           has_lifecycle;                    /* 0x800c (always active) */
    /* The agent-stream role mask every member must advertise (0x8006
     * required_member_roles; 0 without the component). */
    uint8_t        required_member_roles;
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
 *    required component, e.g. message retention 0x8005; safe_aad framing;
 *    an agent stream requiring the send or fanout role).
 * Known components are validated wherever they are, required or not, as
 * MDK v0.11.0 does: 0x8002 and 0x8007 with the marmot-media.h codecs (an
 * unverified avatar URL is valid state), 0x8005 (8 bytes), 0x8006 and
 * 0x800b with the marmot-group-components.h codecs.
 */
int mls_adopted_group_context_parse(const uint8_t *exts, size_t len,
                                    MlsAdoptedGroupContext *out);

/*
 * Whether @data is a valid state of component @id in an adopted
 * GroupContext, by exactly the rules mls_adopted_group_context_parse()
 * applies to each dictionary entry (the per-entry half; requirement lists
 * and "required, but no state" are the caller's).  For a state replaced
 * or added by an AppDataUpdate.
 *
 * W24 slice H (nostrc-qp24.5.1): the AppDataUpdate path MUST call this
 * instead of its own adopted_component_valid().  One definition keeps
 * admission (Welcome, load, entered epoch) and Commit validation from ever
 * disagreeing: a valid 0x8006 or 0x800b update returns 0 here and must be
 * applied, not refused as unsupported (review of slice I, M1).  Keep the
 * removal and lifecycle rules in the caller.
 *
 * Returns 0 (also for an id libmarmot does not interpret: opaque, any
 * bytes), or:
 *  - MARMOT_ERR_EXTENSION_FORMAT: malformed state of a known component
 *    (0x0001 app_components, 0x8001 profile, 0x8002 Blossom image, 0x8003
 *    admin policy, 0x8004 Nostr routing, 0x8005 retention, 0x8006 agent
 *    text stream, 0x8007 avatar URL, 0x800b encrypted media v2, 0x800c
 *    lifecycle), and any 0x8009 state (LeafNode only);
 *  - MARMOT_ERR_UNSUPPORTED: safe_aad (0x0002) state; a 0x8006 policy
 *    requiring the send or fanout role;
 *  - MARMOT_ERR_VALIDATION: frozen encrypted media v1 (0x8008); a
 *    disbanded lifecycle;
 *  - MARMOT_ERR_MEMORY.
 * An avatar or media endpoint URL outside libmarmot's verifiable subset is
 * valid (unverified, never contacted).
 */
int mls_adopted_component_state_valid(uint16_t id, const uint8_t *data, size_t len);

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
 * as in MDK).  When the GroupContext carries 0x8006, every leaf also
 * advertises the role capability (MLS extension type) of each role in its
 * required_member_roles: agent-text-stream-quic-v1.md makes that a
 * resulting-epoch invariant of every member.  MDK v0.11.0 enforces it only
 * on invitees and on a joiner's own client (not in its resulting-epoch
 * check): a deliberate, stricter divergence (README "Adopted profile";
 * nostrc-qp24.5.2 review L1).  The proofs' signatures are NOT verified
 * here.  0 or
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
