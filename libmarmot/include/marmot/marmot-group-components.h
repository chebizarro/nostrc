/*
 * libmarmot - adopted-profile group components: the agent text stream and
 * encrypted-media policies, and the read side of a group's GroupContext
 * (nostrc-qp24.5.2, nostrc-m6tp).
 *
 * Spec: marmot-protocol/marmot 07da8ffb, app-components/
 * agent-text-stream-quic-v1.md (0x8006) and group-encrypted-media-v2.md
 * (0x800b); checked against MDK v0.11.0 (946e0547) cgka-traits
 * (agent_text_stream.rs, app_components/encrypted_media_v2.rs).
 *
 * Every White Noise (MDK 0.11 marmot-app) group requires both components
 * and SelfRemove.  libmarmot admits such groups as a member with the
 * agent-stream `receive` role only: it advertises the role capability
 * (MLS extension type 0xF2D1), which the spec defines as "understands this
 * component and the MLS-delivered start/final stream anchors"; a receive
 * member may ignore the raw QUIC live previews and wait for the durable
 * kind-9 final message, which is ordinary chat content.  libmarmot opens no
 * QUIC stream, and Groundhog renders only the final message (nostrc-ji2j).
 * A group that requires the `send` or `fanout` role is refused
 * (MARMOT_ERR_UNSUPPORTED).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GROUP_COMPONENTS_H
#define MARMOT_GROUP_COMPONENTS_H

#include "marmot-error.h"
#include "marmot-types.h"
#include "marmot-media.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 0x8006 marmot.group.agent-text-stream.quic.v1 ─────────────────────── */

#define MARMOT_COMPONENT_AGENT_TEXT_STREAM_QUIC_V1  0x8006u

/* Role-mask bits and the MLS extension type a leaf lists in its
 * capabilities to advertise each role. */
#define MARMOT_AGENT_STREAM_ROLE_RECEIVE  0x01u
#define MARMOT_AGENT_STREAM_ROLE_SEND     0x02u
#define MARMOT_AGENT_STREAM_ROLE_FANOUT   0x04u
#define MARMOT_AGENT_STREAM_ROLE_MASK     0x07u
#define MARMOT_EXT_AGENT_STREAM_RECEIVE   0xF2D1u
#define MARMOT_EXT_AGENT_STREAM_SEND      0xF2D2u
#define MARMOT_EXT_AGENT_STREAM_FANOUT    0xF2D4u

/* The v1 bounds (MDK AGENT_TEXT_STREAM_* constants). */
#define MARMOT_AGENT_STREAM_STATE_LEN        12u
#define MARMOT_AGENT_STREAM_MAX_FRAME_LEN    65519u
#define MARMOT_AGENT_STREAM_MAX_REPLAY_TTL   300u
#define MARMOT_AGENT_STREAM_MAX_PADDING      4096u

/**
 * MarmotAgentTextStreamPolicy: the 12-byte component state
 * (u8 required_member_roles, u8 allowed_member_roles,
 * u32 max_plaintext_frame_len, u32 replay_ttl_secs,
 * u16 padding_bucket_bytes; big-endian).
 */
typedef struct {
    uint8_t  required_member_roles;
    uint8_t  allowed_member_roles;
    uint32_t max_plaintext_frame_len;
    uint32_t replay_ttl_secs;
    uint16_t padding_bucket_bytes;
} MarmotAgentTextStreamPolicy;

/**
 * Decode and validate exactly as MDK v0.11.0
 * (AgentTextStreamQuicPolicyV1::decode_component_state): 12 bytes; a
 * non-empty required mask, both masks within receive|send|fanout, required
 * a subset of allowed, 1 <= max_plaintext_frame_len <= 65519,
 * replay_ttl_secs <= 300, padding_bucket_bytes <= 4096.
 * MARMOT_ERR_EXTENSION_FORMAT otherwise.
 */
MarmotError marmot_agent_text_stream_policy_decode(const uint8_t *data, size_t len,
                                                   MarmotAgentTextStreamPolicy *out);

/** The 12 state bytes of a valid policy (MARMOT_ERR_INVALID_INPUT otherwise). */
MarmotError marmot_agent_text_stream_policy_encode(const MarmotAgentTextStreamPolicy *policy,
                                                   uint8_t out[MARMOT_AGENT_STREAM_STATE_LEN]);

/** MDK's AgentTextStreamQuicPolicyV1::user_to_agent_default(), which every
 *  White Noise group carries: requires receive, allows receive|send,
 *  4096-byte frames, no replay, no padding. */
MarmotAgentTextStreamPolicy marmot_agent_text_stream_policy_user_to_agent_default(void);

/* ── 0x800b marmot.group.encrypted-media.v2 ────────────────────────────── */

#define MARMOT_MEDIA_POLICY_MAX_LOCATOR_KINDS   16u
#define MARMOT_MEDIA_POLICY_MAX_ENDPOINTS       16u
#define MARMOT_MEDIA_POLICY_LOCATOR_KIND_MAX    64u
#define MARMOT_MEDIA_POLICY_ENDPOINT_URL_MAX    2048u

/**
 * MarmotMediaBlobEndpoint: one default blob endpoint.
 *
 * base_url_unverified (decode only): as for the 0x8007 avatar URL
 * (MarmotGroupAvatarUrl.url_unverified; nostrc-u7cb review M3), the stored
 * URL is accepted state outside the subset of the WHATWG serializer
 * libmarmot can verify (an IDNA or '_' host, ...).  It is kept byte for
 * byte and must never be contacted: fetch fallbacks skip it.
 */
typedef struct {
    char *locator_kind;
    char *base_url;
    bool  base_url_unverified;
} MarmotMediaBlobEndpoint;

/**
 * MarmotGroupMediaPolicy: the component state.  media_format is always
 * MARMOT_MEDIA_V2_VERSION.  allowed_locator_kinds is a NULL-terminated
 * array (allowed_locator_kind_count entries plus NULL), in state order, so
 * it can be passed as is to marmot_media_imeta_build(); default endpoints
 * in state order (marmot_media_blossom_fallback_url() builds a fetch URL).
 */
typedef struct {
    char                   **allowed_locator_kinds;
    size_t                   allowed_locator_kind_count;
    MarmotMediaBlobEndpoint *default_blob_endpoints;
    size_t                   default_blob_endpoint_count;
} MarmotGroupMediaPolicy;

/**
 * Strict decode, as MDK v0.11.0 decode_encrypted_media_policy_v2: three
 * QUIC-varint vectors (media_format = "encrypted-media-v2",
 * allowed_locator_kinds, default_blob_endpoints), shortest length prefixes,
 * no trailing bytes; 1..16 unique locator kinds of 1..64 bytes of
 * [a-z0-9-]; 1..16 unique endpoints whose kind is allowed and whose base URL
 * is an http(s) URL of at most 2048 bytes with no userinfo, query or
 * fragment, stored as its WHATWG serialization.  The URL judgement is
 * three-way (see MarmotMediaBlobEndpoint): a URL no serializer can have
 * produced is MARMOT_ERR_MEDIA_INVALID_REFERENCE, one outside libmarmot's
 * verifiable subset is accepted unverified.  Every other violation is
 * MARMOT_ERR_MEDIA_INVALID_REFERENCE too.
 */
MarmotError marmot_group_media_policy_decode(const uint8_t *data, size_t len,
                                             MarmotGroupMediaPolicy *out);

/**
 * Encode as MDK's EncryptedMediaPolicyV2::new() then
 * encode_encrypted_media_policy_v2: locator kinds trimmed, lowercased and
 * deduplicated; endpoint URLs normalized (libmarmot's strict subset: an
 * URL it cannot verify is MARMOT_ERR_INVALID_INPUT, as is an unverified
 * endpoint) and deduplicated.  *out is malloc'd.
 */
MarmotError marmot_group_media_policy_encode(const MarmotGroupMediaPolicy *policy,
                                             uint8_t **out, size_t *out_len);

void marmot_group_media_policy_clear(MarmotGroupMediaPolicy *policy);

/* ── Read side: the components of a joined adopted group (nostrc-m6tp) ── */

/**
 * MarmotGroupComponents: what the group's current GroupContext says, read
 * from the stored MLS state, so it reflects every epoch the group has
 * entered (creation, Welcome, Commit) and survives a reload.  All owned;
 * free with marmot_group_components_clear().
 *
 *  - name, description: 0x8001 marmot.group.profile.v1 (empty strings when
 *    the group has no profile component);
 *  - image: 0x8002 marmot.group.blossom.image.v1 (image.present false when
 *    absent or in its empty state).  image_key and image_upload_key are
 *    group secrets;
 *  - avatar_url: 0x8007 marmot.group.avatar-url.v1 (url NULL when absent
 *    or empty); avatar_source: the rendering precedence
 *    (marmot_group_avatar_select(): a URL avatar wins, an unverified one is
 *    a placeholder);
 *  - media_policy: 0x800b, when has_media_policy;
 *  - agent_text_stream: 0x8006, when has_agent_text_stream;
 *  - required_components: the GroupContext app_components list.
 */
typedef struct {
    uint64_t                    epoch;
    char                       *name;
    char                       *description;
    MarmotGroupBlossomImage     image;
    MarmotGroupAvatarUrl        avatar_url;
    MarmotGroupAvatarSource     avatar_source;
    bool                        has_media_policy;
    MarmotGroupMediaPolicy      media_policy;
    bool                        has_agent_text_stream;
    MarmotAgentTextStreamPolicy agent_text_stream;
    uint16_t                   *required_components;
    size_t                      required_component_count;
} MarmotGroupComponents;

/**
 * The components of adopted group @mls_group_id.  MARMOT_ERR_GROUP_NOT_FOUND
 * if there is no such group, MARMOT_ERR_UNSUPPORTED for a legacy (0xF2EE)
 * group, whose metadata is in MarmotGroup; a stored state that no longer
 * validates is MARMOT_ERR_DESERIALIZATION.
 *
 * Writing them (AppDataUpdate) is not this API (nostrc-qp24.5.1 live
 * Commits).
 */
MarmotError marmot_get_group_components(Marmot *m, const MarmotGroupId *mls_group_id,
                                        MarmotGroupComponents *out);

void marmot_group_components_clear(MarmotGroupComponents *components);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_GROUP_COMPONENTS_H */
