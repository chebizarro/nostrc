/*
 * libmarmot - adopted-profile GroupContext and member-leaf admission
 * (nostrc-qp24.5.1).  See mls_app_components.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_app_components.h"
#include <marmot/marmot-media.h>
#include <marmot/marmot-group-components.h>
#include <stdlib.h>
#include <string.h>

const uint16_t MLS_ADOPTED_SUPPORTED_COMPONENTS[MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT] = {
    MARMOT_COMPONENT_GROUP_PROFILE_V1,   /* 0x8001 marmot.group.profile.v1 */
    MLS_COMPONENT_BLOSSOM_IMAGE_V1,      /* 0x8002 marmot.group.blossom-image.v1 (nostrc-l2ln) */
    MARMOT_COMPONENT_ADMIN_POLICY_V1,    /* 0x8003 marmot.group.admin-policy.v1 */
    MARMOT_COMPONENT_NOSTR_ROUTING_V1,   /* 0x8004 marmot.transport.nostr.routing.v1 */
    MLS_COMPONENT_AGENT_TEXT_STREAM_V1,  /* 0x8006 agent-text-stream.quic.v1 (receive) */
    MLS_COMPONENT_AVATAR_URL_V1,         /* 0x8007 marmot.group.avatar-url.v1 (nostrc-l2ln) */
    MLS_COMPONENT_ACCOUNT_PROOF_V2,      /* 0x8009 (LeafNode only) */
    MLS_COMPONENT_ENCRYPTED_MEDIA_V2,    /* 0x800b marmot.group.encrypted-media.v2 */
    MARMOT_COMPONENT_GROUP_LIFECYCLE_V1, /* 0x800c marmot.group.lifecycle.v1 (active) */
};

const uint16_t MLS_ADOPTED_CREATE_COMPONENTS[MLS_ADOPTED_CREATE_COMPONENT_COUNT] = {
    MARMOT_COMPONENT_GROUP_PROFILE_V1,
    MARMOT_COMPONENT_ADMIN_POLICY_V1,
    MARMOT_COMPONENT_NOSTR_ROUTING_V1,
    MLS_COMPONENT_ACCOUNT_PROOF_V2,
    MARMOT_COMPONENT_GROUP_LIFECYCLE_V1,
};

/* The two spellings of each id (MLS layer, public header) agree. */
_Static_assert(MLS_COMPONENT_BLOSSOM_IMAGE_V1 == MARMOT_COMPONENT_GROUP_BLOSSOM_IMAGE_V1, "0x8002");
_Static_assert(MLS_COMPONENT_AVATAR_URL_V1 == MARMOT_COMPONENT_GROUP_AVATAR_URL_V1, "0x8007");
_Static_assert(MLS_COMPONENT_ENCRYPTED_MEDIA_V2 == MARMOT_COMPONENT_GROUP_ENCRYPTED_MEDIA_V2,
               "0x800b");
_Static_assert(MLS_COMPONENT_AGENT_TEXT_STREAM_V1 == MARMOT_COMPONENT_AGENT_TEXT_STREAM_QUIC_V1,
               "0x8006");
_Static_assert(MLS_EXT_AGENT_STREAM_RECEIVE == MARMOT_EXT_AGENT_STREAM_RECEIVE &&
               MLS_EXT_AGENT_STREAM_SEND == MARMOT_EXT_AGENT_STREAM_SEND &&
               MLS_EXT_AGENT_STREAM_FANOUT == MARMOT_EXT_AGENT_STREAM_FANOUT,
               "agent-stream role capabilities");

/* ──────────────────────────────────────────────────────────────────────────
 * Primitives
 * ──────────────────────────────────────────────────────────────────────── */

static bool
ids_contain(const uint16_t *ids, size_t n, uint16_t id)
{
    for (size_t i = 0; i < n; i++)
        if (ids[i] == id) return true;
    return false;
}

static bool
list_contains(const uint16_t *list, size_t n, uint16_t id)
{
    return (n == 0 || list) && ids_contain(list, n, id);
}

/* One vector: a minimal QUIC varint length (mls_tls_read_vli refuses
 * non-minimal encodings) and that many bytes, borrowed. */
static int
read_vec(MlsTlsReader *r, const uint8_t **out, size_t *len)
{
    size_t n = 0;
    if (mls_tls_read_vli(r, &n) != 0 || n > mls_tls_reader_remaining(r)) return -1;
    *out = r->data + r->pos;
    *len = n;
    r->pos += n;
    return 0;
}

static bool
ext_type_is_default(uint16_t t)
{
    return t >= 0x0001 && t <= 0x0005; /* RFC 9420 §7.2 */
}

static bool
proposal_type_is_default(uint16_t t)
{
    return t >= 0x0001 && t <= 0x0007; /* RFC 9420 §7.2 */
}

/* What libmarmot's adopted leaves advertise beyond the RFC 9420 defaults
 * (mls_leaf_node_set_adopted_capabilities()). */
static bool
adopted_ext_type_supported(uint16_t t)
{
    return t == MLS_EXTENSION_APP_DATA_DICTIONARY || t == MLS_EXT_AGENT_STREAM_RECEIVE ||
           ext_type_is_default(t);
}

static bool
adopted_proposal_supported(uint16_t t)
{
    return t == MLS_PROPOSAL_TYPE_APP_DATA_UPDATE || t == MLS_PROPOSAL_TYPE_SELF_REMOVE ||
           proposal_type_is_default(t);
}

/* The role capability (MLS extension type) of each agent-stream role bit. */
static const struct {
    uint8_t  bit;
    uint16_t ext;
} AGENT_STREAM_ROLES[] = {
    {MARMOT_AGENT_STREAM_ROLE_RECEIVE, MLS_EXT_AGENT_STREAM_RECEIVE},
    {MARMOT_AGENT_STREAM_ROLE_SEND, MLS_EXT_AGENT_STREAM_SEND},
    {MARMOT_AGENT_STREAM_ROLE_FANOUT, MLS_EXT_AGENT_STREAM_FANOUT},
};

static bool
component_supported(uint16_t id)
{
    return ids_contain(MLS_ADOPTED_SUPPORTED_COMPONENTS, MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT,
                       id);
}

bool
mls_utf8_valid(const uint8_t *s, size_t len)
{
    if (len > 0 && !s) return false;
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t n;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) {
            n = 1; cp = c & 0x1F; min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2; cp = c & 0x0F; min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3; cp = c & 0x07; min = 0x10000;
        } else {
            return false;
        }
        if (len - i - 1 < n) return false;
        for (size_t k = 1; k <= n; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

static bool
ascii_prefix_ci(const uint8_t *s, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    if (len < n) return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
        if (c != (uint8_t)prefix[i]) return false;
    }
    return true;
}

static bool
all_digits(const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool
is_hex_digit(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Strict dotted-quad: exactly four decimal parts 0..255, no leading zeros
 * (the WHATWG parser reads those as octal; we refuse rather than guess). */
static bool
ipv4_dotted_valid(const uint8_t *s, size_t len)
{
    size_t parts = 0, i = 0;
    while (parts < 4) {
        size_t start = i;
        unsigned v = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9' && i - start < 3) v = v * 10 + (s[i++] - '0');
        size_t n = i - start;
        if (n == 0 || v > 255 || (n > 1 && s[start] == '0')) return false;
        parts++;
        if (parts < 4) {
            if (i >= len || s[i] != '.') return false;
            i++;
        }
    }
    return i == len;
}

/* An IPv6 literal between the brackets (RFC 4291 §2.2 text form, as the
 * WHATWG URL parser accepts it; no zone id): up to eight groups of 1..4 hex
 * digits with at most one "::", optionally ending in a dotted IPv4 that
 * counts as two groups (W24 review N1). */
static bool
ipv6_literal_valid(const uint8_t *s, size_t len)
{
    if (len < 2) return false;
    size_t groups = 0, i = 0;
    bool compressed = false;
    if (s[0] == ':') {
        if (s[1] != ':') return false;
        compressed = true;
        i = 2;
        if (i == len) return true; /* "::" */
    }
    while (i < len) {
        size_t start = i;
        while (i < len && is_hex_digit(s[i]) && i - start < 5) i++;
        if (i < len && s[i] == '.') {
            /* Embedded IPv4: must be last and take two groups. */
            if (!ipv4_dotted_valid(s + start, len - start)) return false;
            groups += 2;
            i = len;
            break;
        }
        size_t n = i - start;
        if (n == 0 || n > 4) return false;
        groups++;
        if (i == len) break;
        if (s[i] != ':') return false;
        i++;
        if (i < len && s[i] == ':') {
            if (compressed) return false;
            compressed = true;
            i++;
            if (i == len) break;
        } else if (i == len) {
            return false; /* trailing single ':' */
        }
    }
    return compressed ? groups < 8 : groups == 8;
}

/* A registered-name host, conservatively (W24 review N1): ASCII only (a
 * URL parser would IDNA-map anything else; MDK serializes the punycode),
 * none of the WHATWG forbidden host / domain code points, and, if it ends
 * in a number, a strict dotted-quad IPv4 address. */
static bool
host_name_valid(const uint8_t *h, size_t len)
{
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = h[i];
        if (c >= 0x80 || c <= 0x20 || c == 0x7f) return false;
        if (strchr("#/:<>?@[\\]^|%", c)) return false;
    }
    /* The last label, ignoring one trailing dot. */
    size_t end = len;
    if (h[end - 1] == '.') end--;
    size_t start = end;
    while (start > 0 && h[start - 1] != '.') start--;
    if (end == start) return false; /* empty label: "a..", "." */
    bool numeric = all_digits(h + start, end - start) ||
                   (end - start >= 2 && h[start] == '0' && (h[start + 1] | 0x20) == 'x');
    if (numeric) return ipv4_dotted_valid(h, len);
    return true;
}

bool
mls_relay_url_valid(const uint8_t *url, size_t len)
{
    if (!url || len == 0 || len > MARMOT_NOSTR_RELAY_URL_MAX || !mls_utf8_valid(url, len))
        return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = url[i];
        /* whitespace and controls (a parser would strip or re-encode them),
         * backslash (read as '/' in ws/wss URLs), fragment. */
        if (b <= 0x20 || b == 0x7f || b == '\\' || b == '#') return false;
    }
    size_t p;
    if (ascii_prefix_ci(url, len, "wss://")) p = 6;
    else if (ascii_prefix_ci(url, len, "ws://")) p = 5;
    else return false;
    size_t q = p;
    while (q < len && url[q] != '/' && url[q] != '?') q++;
    const uint8_t *auth = url + p;
    size_t auth_len = q - p;
    if (auth_len == 0) return false;
    if (memchr(auth, '@', auth_len)) return false; /* userinfo */
    const uint8_t *port = NULL;
    size_t port_len = 0;
    if (auth[0] == '[') {
        const uint8_t *close = memchr(auth, ']', auth_len);
        if (!close || close == auth + 1) return false;
        if (!ipv6_literal_valid(auth + 1, (size_t)(close - auth) - 1)) return false;
        size_t after = auth_len - (size_t)(close - auth) - 1;
        if (after > 0) {
            if (close[1] != ':') return false;
            port = close + 2;
            port_len = after - 1;
        }
    } else {
        if (memchr(auth, '[', auth_len) || memchr(auth, ']', auth_len)) return false;
        const uint8_t *colon = memchr(auth, ':', auth_len);
        size_t host_len = colon ? (size_t)(colon - auth) : auth_len;
        if (!host_name_valid(auth, host_len)) return false;
        if (colon) {
            port = colon + 1;
            port_len = auth_len - host_len - 1;
            if (memchr(port, ':', port_len)) return false;
        }
    }
    if (port) {
        if (port_len > 5 || !all_digits(port, port_len)) return false;
        unsigned long v = 0;
        for (size_t i = 0; i < port_len; i++) v = v * 10 + (unsigned long)(port[i] - '0');
        if (v > 65535) return false;
    }
    return true;
}

int
mls_components_list_decode_strict(const uint8_t *data, size_t len,
                                  uint16_t *ids, size_t cap, size_t *n)
{
    if (!n || (len > 0 && !data)) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    const uint8_t *v = NULL;
    size_t vlen = 0;
    if (read_vec(&r, &v, &vlen) != 0 || !mls_tls_reader_done(&r) || vlen % 2 != 0)
        return -1;
    size_t count = vlen / 2;
    if (count > cap || (count > 0 && !ids)) return -1;
    for (size_t i = 0; i < count; i++) {
        uint16_t id = (uint16_t)((v[2 * i] << 8) | v[2 * i + 1]);
        if (i > 0 && id <= ids[i - 1]) return -1; /* sorted, unique */
        ids[i] = id;
    }
    *n = count;
    return 0;
}

/* A strict ComponentsList (any length) containing every id of @need. */
static int
components_list_covers(const uint8_t *data, size_t len, const uint16_t *need, size_t n_need)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    const uint8_t *v = NULL;
    size_t vlen = 0;
    if (!data || read_vec(&r, &v, &vlen) != 0 || !mls_tls_reader_done(&r) || vlen % 2 != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    size_t found = 0;
    uint16_t prev = 0;
    for (size_t i = 0; i < vlen / 2; i++) {
        uint16_t id = (uint16_t)((v[2 * i] << 8) | v[2 * i + 1]);
        if (i > 0 && id <= prev) return MARMOT_ERR_EXTENSION_FORMAT;
        if (ids_contain(need, n_need, id)) found++;
        prev = id;
    }
    return found == n_need ? 0 : MARMOT_ERR_VALIDATION;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Component codecs
 * ──────────────────────────────────────────────────────────────────────── */

int
mls_group_profile_v1_decode(const uint8_t *data, size_t len,
                            const uint8_t **name, size_t *name_len,
                            const uint8_t **description, size_t *description_len)
{
    if (!data || !name || !name_len || !description || !description_len)
        return MARMOT_ERR_EXTENSION_FORMAT;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    if (read_vec(&r, name, name_len) != 0 || *name_len > MARMOT_GROUP_PROFILE_NAME_MAX ||
        read_vec(&r, description, description_len) != 0 ||
        *description_len > MARMOT_GROUP_PROFILE_DESCRIPTION_MAX ||
        !mls_tls_reader_done(&r) || !mls_utf8_valid(*name, *name_len) ||
        !mls_utf8_valid(*description, *description_len))
        return MARMOT_ERR_EXTENSION_FORMAT;
    return 0;
}

int
mls_admin_policy_v1_decode(const uint8_t *data, size_t len,
                           const uint8_t **keys, size_t *count)
{
    if (!data || !keys || !count) return MARMOT_ERR_EXTENSION_FORMAT;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    const uint8_t *v = NULL;
    size_t vlen = 0;
    if (read_vec(&r, &v, &vlen) != 0 || !mls_tls_reader_done(&r) || vlen == 0 ||
        vlen % 32 != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t i = 32; i < vlen; i += 32)
        if (memcmp(v + i - 32, v + i, 32) >= 0) return MARMOT_ERR_EXTENSION_FORMAT;
    *keys = v;
    *count = vlen / 32;
    return 0;
}

/* Rust's `[u8]` ordering: bytewise, a proper prefix first. */
static int
bytes_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    int c = memcmp(a, b, alen < blen ? alen : blen);
    if (c != 0) return c;
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

int
mls_nostr_routing_v1_decode(const uint8_t *data, size_t len,
                            const uint8_t **nostr_group_id,
                            MlsRelaySpan relays[MARMOT_NOSTR_ROUTING_MAX_RELAYS],
                            size_t *relay_count)
{
    if (!data || !nostr_group_id || !relays || !relay_count || len < 32)
        return MARMOT_ERR_EXTENSION_FORMAT;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data + 32, len - 32);
    const uint8_t *vec = NULL;
    size_t vlen = 0;
    if (read_vec(&r, &vec, &vlen) != 0 || !mls_tls_reader_done(&r) ||
        vlen > MARMOT_NOSTR_ROUTING_MAX_RELAYS * (MARMOT_NOSTR_RELAY_URL_MAX + 2))
        return MARMOT_ERR_EXTENSION_FORMAT;
    MlsTlsReader e;
    mls_tls_reader_init(&e, vec, vlen);
    size_t n = 0;
    while (!mls_tls_reader_done(&e)) {
        if (n == MARMOT_NOSTR_ROUTING_MAX_RELAYS) return MARMOT_ERR_EXTENSION_FORMAT;
        const uint8_t *url = NULL;
        size_t ulen = 0;
        if (read_vec(&e, &url, &ulen) != 0 || !mls_relay_url_valid(url, ulen))
            return MARMOT_ERR_EXTENSION_FORMAT;
        if (n > 0 && bytes_cmp(relays[n - 1].url, relays[n - 1].len, url, ulen) >= 0)
            return MARMOT_ERR_EXTENSION_FORMAT; /* sorted, unique */
        relays[n].url = url;
        relays[n].len = ulen;
        n++;
    }
    if (n == 0) return MARMOT_ERR_EXTENSION_FORMAT;
    *nostr_group_id = data;
    *relay_count = n;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Classification
 * ──────────────────────────────────────────────────────────────────────── */

MarmotGroupProfile
mls_group_context_profile_of(const uint8_t *exts, size_t len)
{
    if (len > 0 && !exts) return MARMOT_GROUP_PROFILE_LEGACY;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, len);
    /* Legacy groups historically keep opaque extension bytes even when the
     * list is not canonically encoded: an undecodable tail ends the scan.
     * Only an app_data_dictionary extension -- the marker the pre-0.12.0
     * gate refused -- makes a group adopted: a legacy group's
     * required_capabilities may list 0x0006 (libmarmot legacy leaves
     * advertise it for their proof) without changing its profile. */
    while (!mls_tls_reader_done(&r)) {
        uint16_t type = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&r, &type) != 0) break;
        /* Recognized by its type alone, before its length: a truncated or
         * non-minimally encoded 0x0006 is adopted -- and then refused by the
         * strict parse -- never legacy (W24 review L1). */
        if (type == MLS_EXTENSION_APP_DATA_DICTIONARY) return MARMOT_GROUP_PROFILE_ADOPTED;
        if (read_vec(&r, &d, &dlen) != 0) break;
    }
    return MARMOT_GROUP_PROFILE_LEGACY;
}

/* ──────────────────────────────────────────────────────────────────────────
 * GroupContext
 * ──────────────────────────────────────────────────────────────────────── */

/* One u16 list of RequiredCapabilities: canonical, no repeats. */
static int
read_type_list(MlsTlsReader *r, uint16_t *out, size_t *n)
{
    const uint8_t *v = NULL;
    size_t vlen = 0;
    if (read_vec(r, &v, &vlen) != 0 || vlen % 2 != 0) return MARMOT_ERR_EXTENSION_FORMAT;
    size_t count = vlen / 2;
    /* More than the bound necessarily repeats or names an unsupported type. */
    if (count > MLS_ADOPTED_MAX_IDS) return MARMOT_ERR_UNSUPPORTED;
    for (size_t i = 0; i < count; i++) {
        uint16_t t = (uint16_t)((v[2 * i] << 8) | v[2 * i + 1]);
        if (ids_contain(out, i, t)) return MARMOT_ERR_EXTENSION_FORMAT;
        out[i] = t;
    }
    *n = count;
    return 0;
}

static int
parse_required_capabilities(const uint8_t *data, size_t len, MlsAdoptedGroupContext *gc)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    int rc;
    if ((rc = read_type_list(&r, gc->ext_types, &gc->n_ext_types)) != 0 ||
        (rc = read_type_list(&r, gc->proposal_types, &gc->n_proposal_types)) != 0 ||
        (rc = read_type_list(&r, gc->credential_types, &gc->n_credential_types)) != 0)
        return rc;
    if (!mls_tls_reader_done(&r)) return MARMOT_ERR_EXTENSION_FORMAT;

    /* A legacy-profile requirement next to the adopted ones: a mixed group. */
    if (ids_contain(gc->ext_types, gc->n_ext_types, MLS_LEGACY_EXT_GROUP_DATA) ||
        ids_contain(gc->ext_types, gc->n_ext_types, MLS_LEGACY_EXT_ACCOUNT_PROOF_V1))
        return MARMOT_ERR_VALIDATION;
    /* group-setup.md: every current-profile group requires both. */
    if (!ids_contain(gc->ext_types, gc->n_ext_types, MLS_EXTENSION_APP_DATA_DICTIONARY) ||
        !ids_contain(gc->proposal_types, gc->n_proposal_types,
                     MLS_PROPOSAL_TYPE_APP_DATA_UPDATE))
        return MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t i = 0; i < gc->n_ext_types; i++)
        if (!adopted_ext_type_supported(gc->ext_types[i])) return MARMOT_ERR_UNSUPPORTED;
    for (size_t i = 0; i < gc->n_proposal_types; i++)
        if (!adopted_proposal_supported(gc->proposal_types[i]))
            return MARMOT_ERR_UNSUPPORTED;
    for (size_t i = 0; i < gc->n_credential_types; i++)
        if (gc->credential_types[i] != MLS_CREDENTIAL_BASIC) return MARMOT_ERR_UNSUPPORTED;
    return 0;
}

static int
validate_component_state(uint16_t id, const uint8_t *data, size_t len,
                         MlsAdoptedGroupContext *gc)
{
    switch (id) {
    case MLS_COMPONENT_APP_COMPONENTS:
        return mls_components_list_decode_strict(data, len, gc->components,
                                                 MLS_ADOPTED_MAX_IDS, &gc->n_components) == 0
                   ? 0 : MARMOT_ERR_EXTENSION_FORMAT;
    case MLS_COMPONENT_SAFE_AAD:
        /* A GroupContext safe_aad entry changes the framing of every
         * message's authenticated_data; libmarmot does not implement it. */
        return MARMOT_ERR_UNSUPPORTED;
    case MLS_COMPONENT_ACCOUNT_PROOF_V2:
        /* LeafNode-only component (account-identity-proof-v2.md). */
        return MARMOT_ERR_EXTENSION_FORMAT;
    case MARMOT_COMPONENT_ENCRYPTED_MEDIA_V1:
        /* Frozen; not permitted in a current-profile group (MDK 0.11). */
        return MARMOT_ERR_VALIDATION;
    case MARMOT_COMPONENT_GROUP_PROFILE_V1: {
        const uint8_t *n, *d;
        size_t nl, dl;
        if (mls_group_profile_v1_decode(data, len, &n, &nl, &d, &dl) != 0)
            return MARMOT_ERR_EXTENSION_FORMAT;
        gc->profile = data;
        gc->profile_len = len;
        return 0;
    }
    case MARMOT_COMPONENT_ADMIN_POLICY_V1: {
        const uint8_t *keys;
        size_t count;
        if (mls_admin_policy_v1_decode(data, len, &keys, &count) != 0)
            return MARMOT_ERR_EXTENSION_FORMAT;
        gc->admins = data;
        gc->admins_len = len;
        return 0;
    }
    case MARMOT_COMPONENT_NOSTR_ROUTING_V1: {
        const uint8_t *ngid;
        MlsRelaySpan relays[MARMOT_NOSTR_ROUTING_MAX_RELAYS];
        size_t n;
        if (mls_nostr_routing_v1_decode(data, len, &ngid, relays, &n) != 0)
            return MARMOT_ERR_EXTENSION_FORMAT;
        gc->routing = data;
        gc->routing_len = len;
        return 0;
    }
    case MARMOT_COMPONENT_GROUP_LIFECYCLE_V1:
        if (len != 1 || data[0] > 1) return MARMOT_ERR_EXTENSION_FORMAT;
        /* A disbanded group is terminal: nothing can be joined or loaded. */
        if (data[0] != 0) return MARMOT_ERR_VALIDATION;
        gc->has_lifecycle = true;
        return 0;
    case MLS_COMPONENT_BLOSSOM_IMAGE_V1: {
        /* nostrc-m6tp: the group image; its key is a group secret. */
        MarmotGroupBlossomImage img;
        MarmotError err = marmot_group_blossom_image_decode(data, len, &img);
        marmot_group_blossom_image_clear(&img);
        if (err == MARMOT_ERR_MEMORY) return MARMOT_ERR_MEMORY;
        if (err != MARMOT_OK) return MARMOT_ERR_EXTENSION_FORMAT;
        gc->image = data;
        gc->image_len = len;
        return 0;
    }
    case MLS_COMPONENT_AVATAR_URL_V1: {
        /* Three-way (nostrc-u7cb review M3): an unverified URL is valid
         * state, kept byte for byte; only a provably invalid one refuses. */
        MarmotGroupAvatarUrl av;
        MarmotError err = marmot_group_avatar_url_decode(data, len, &av);
        marmot_group_avatar_url_clear(&av);
        if (err == MARMOT_ERR_MEMORY) return MARMOT_ERR_MEMORY;
        if (err != MARMOT_OK) return MARMOT_ERR_EXTENSION_FORMAT;
        gc->avatar = data;
        gc->avatar_len = len;
        return 0;
    }
    case MLS_COMPONENT_MESSAGE_RETENTION_V1:
        /* MDK decode_message_retention: a u64 of seconds.  Not honoured
         * (nostrc-b55p): a group requiring it is refused below. */
        return len == 8 ? 0 : MARMOT_ERR_EXTENSION_FORMAT;
    case MLS_COMPONENT_AGENT_TEXT_STREAM_V1: {
        MarmotAgentTextStreamPolicy p;
        if (marmot_agent_text_stream_policy_decode(data, len, &p) != MARMOT_OK)
            return MARMOT_ERR_EXTENSION_FORMAT;
        gc->agent_stream = data;
        gc->agent_stream_len = len;
        gc->required_member_roles = p.required_member_roles;
        /* libmarmot is a receive member only (marmot-group-components.h):
         * "a joiner that does not support every role capability named by
         * required_member_roles MUST NOT join the group". */
        if (p.required_member_roles & ~MARMOT_AGENT_STREAM_ROLE_RECEIVE)
            return MARMOT_ERR_UNSUPPORTED;
        return 0;
    }
    case MLS_COMPONENT_ENCRYPTED_MEDIA_V2: {
        MarmotGroupMediaPolicy p;
        MarmotError err = marmot_group_media_policy_decode(data, len, &p);
        marmot_group_media_policy_clear(&p);
        if (err == MARMOT_ERR_MEMORY) return MARMOT_ERR_MEMORY;
        if (err != MARMOT_OK) return MARMOT_ERR_EXTENSION_FORMAT;
        gc->media_policy = data;
        gc->media_policy_len = len;
        return 0;
    }
    default:
        /* Not one libmarmot implements and (checked below) not required:
         * kept byte-for-byte, never interpreted (app-components/README.md
         * "Unknown Data"). */
        return 0;
    }
}

int
mls_adopted_component_state_valid(uint16_t id, const uint8_t *data, size_t len)
{
    if (len > 0 && !data) return MARMOT_ERR_INVALID_ARG;
    static const uint8_t empty[1] = {0};
    /* The admission rules exactly, with their side effects on a scratch
     * context (review M1: one definition for Welcome, load, entered epoch
     * and AppDataUpdate). */
    MlsAdoptedGroupContext scratch;
    memset(&scratch, 0, sizeof(scratch));
    return validate_component_state(id, data ? data : empty, len, &scratch);
}

int
mls_adopted_group_context_parse(const uint8_t *exts, size_t len, MlsAdoptedGroupContext *out)
{
    if (!out || (len > 0 && !exts)) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* 1. The extension list itself: canonical, each type at most once. */
    const uint8_t *caps = NULL, *dict = NULL;
    size_t caps_len = 0, dict_len = 0;
    bool mixed = false, unsupported = false;
    uint16_t seen[MLS_ADOPTED_MAX_IDS];
    size_t n_seen = 0;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, len);
    while (!mls_tls_reader_done(&r)) {
        uint16_t type = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&r, &type) != 0 || read_vec(&r, &d, &dlen) != 0 ||
            ids_contain(seen, n_seen, type))
            return MARMOT_ERR_EXTENSION_FORMAT;
        if (n_seen == MLS_ADOPTED_MAX_IDS) return MARMOT_ERR_UNSUPPORTED;
        seen[n_seen++] = type;
        if (type == MLS_EXT_REQUIRED_CAPABILITIES) {
            caps = d;
            caps_len = dlen;
        } else if (type == MLS_EXTENSION_APP_DATA_DICTIONARY) {
            dict = d;
            dict_len = dlen;
        } else if (type == MLS_LEGACY_EXT_GROUP_DATA || type == MLS_LEGACY_EXT_ACCOUNT_PROOF_V1) {
            mixed = true;
        } else {
            unsupported = true;
        }
    }
    /* 2. No legacy group data beside it; nothing libmarmot cannot apply. */
    if (mixed) return MARMOT_ERR_VALIDATION;
    if (unsupported) return MARMOT_ERR_UNSUPPORTED;
    if (!caps || !dict) return MARMOT_ERR_EXTENSION_FORMAT;

    /* 3. required_capabilities */
    int rc = parse_required_capabilities(caps, caps_len, out);
    if (rc != 0) return rc;

    /* 4. AppDataDictionary { ComponentData component_data<V>; }: exactly
     *    covered, entries strictly ascending by id. */
    MlsTlsReader dr;
    mls_tls_reader_init(&dr, dict, dict_len);
    const uint8_t *entries = NULL;
    size_t entries_len = 0;
    if (read_vec(&dr, &entries, &entries_len) != 0 || !mls_tls_reader_done(&dr))
        return MARMOT_ERR_EXTENSION_FORMAT;
    MlsTlsReader er;
    mls_tls_reader_init(&er, entries, entries_len);
    bool have_components = false, first = true;
    uint16_t prev = 0;
    uint16_t present[MLS_ADOPTED_MAX_IDS];
    size_t n_present = 0;
    int deferred = 0; /* the first semantic (non-format) refusal */
    while (!mls_tls_reader_done(&er)) {
        uint16_t id = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&er, &id) != 0 || read_vec(&er, &d, &dlen) != 0 ||
            (!first && id <= prev))
            return MARMOT_ERR_EXTENSION_FORMAT;
        first = false;
        prev = id;
        if (id == MLS_COMPONENT_APP_COMPONENTS) have_components = true;
        rc = validate_component_state(id, d, dlen, out);
        if (rc == MARMOT_ERR_EXTENSION_FORMAT || rc == MARMOT_ERR_MEMORY) return rc;
        if (rc != 0 && deferred == 0) deferred = rc;
        if (component_supported(id) && n_present < MLS_ADOPTED_MAX_IDS)
            present[n_present++] = id;
    }
    if (!have_components) return MARMOT_ERR_EXTENSION_FORMAT;

    /* 5. The required components: the lifetime invariants first. */
    if (!ids_contain(out->components, out->n_components, MARMOT_COMPONENT_ADMIN_POLICY_V1) ||
        !ids_contain(out->components, out->n_components, MLS_COMPONENT_ACCOUNT_PROOF_V2))
        return MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t i = 0; i < out->n_components; i++) {
        uint16_t id = out->components[i];
        if (id == MLS_COMPONENT_ACCOUNT_PROOF_V2) continue; /* leaf state only */
        if (component_supported(id) && !ids_contain(present, n_present, id))
            return MARMOT_ERR_EXTENSION_FORMAT; /* required, but no state */
    }
    if (deferred != 0) return deferred;
    for (size_t i = 0; i < out->n_components; i++) {
        uint16_t id = out->components[i];
        if (id == MARMOT_COMPONENT_ENCRYPTED_MEDIA_V1) return MARMOT_ERR_VALIDATION;
        if (!component_supported(id)) return MARMOT_ERR_UNSUPPORTED;
    }
    /* libmarmot routes over Nostr only: it needs the signed routing state. */
    if (!ids_contain(out->components, out->n_components, MARMOT_COMPONENT_NOSTR_ROUTING_V1))
        return MARMOT_ERR_UNSUPPORTED;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Members
 * ──────────────────────────────────────────────────────────────────────── */

int
mls_adopted_leaf_check(const MlsLeafNode *leaf, const MlsAdoptedGroupContext *gc)
{
    if (leaf->credential_type != MLS_CREDENTIAL_BASIC || !leaf->credential_identity ||
        leaf->credential_identity_len != 32)
        return MARMOT_ERR_VALIDATION;

    /* Capabilities (RFC 9420 §7.2, §12.4.3.1 / MDK
     * validate_resulting_leaf_capabilities): the group's version and
     * ciphersuite, and every required extension, proposal and credential
     * type -- the RFC 9420 defaults being implied. */
    if (!list_contains(leaf->versions, leaf->version_count, 0x0001) ||
        !list_contains(leaf->ciphersuites, leaf->ciphersuite_count, MARMOT_CIPHERSUITE) ||
        !list_contains(leaf->cap_credentials, leaf->cap_credential_count, MLS_CREDENTIAL_BASIC))
        return MARMOT_ERR_VALIDATION;
    for (size_t i = 0; i < gc->n_ext_types; i++)
        if (!ext_type_is_default(gc->ext_types[i]) &&
            !list_contains(leaf->cap_extensions, leaf->cap_extension_count, gc->ext_types[i]))
            return MARMOT_ERR_VALIDATION;
    for (size_t i = 0; i < gc->n_proposal_types; i++)
        if (!proposal_type_is_default(gc->proposal_types[i]) &&
            !list_contains(leaf->proposals, leaf->proposal_count, gc->proposal_types[i]))
            return MARMOT_ERR_VALIDATION;
    for (size_t i = 0; i < gc->n_credential_types; i++)
        if (!list_contains(leaf->cap_credentials, leaf->cap_credential_count,
                           gc->credential_types[i]))
            return MARMOT_ERR_VALIDATION;

    /* Its only LeafNode extension: one canonical app_data_dictionary. */
    MlsTlsReader r;
    mls_tls_reader_init(&r, leaf->extensions_data, leaf->extensions_len);
    uint16_t type = 0;
    const uint8_t *dict = NULL;
    size_t dict_len = 0;
    if (!leaf->extensions_data || mls_tls_read_u16(&r, &type) != 0 ||
        read_vec(&r, &dict, &dict_len) != 0)
        return MARMOT_ERR_VALIDATION; /* no proof carrier at all */
    if (type != MLS_EXTENSION_APP_DATA_DICTIONARY || !mls_tls_reader_done(&r))
        return MARMOT_ERR_VALIDATION; /* e.g. a 0xF2F1 proof: mixed */

    MlsTlsReader dr;
    mls_tls_reader_init(&dr, dict, dict_len);
    const uint8_t *entries = NULL;
    size_t entries_len = 0;
    if (read_vec(&dr, &entries, &entries_len) != 0 || !mls_tls_reader_done(&dr))
        return MARMOT_ERR_EXTENSION_FORMAT;
    MlsTlsReader er;
    mls_tls_reader_init(&er, entries, entries_len);
    const uint8_t *advertised = NULL, *proof = NULL;
    size_t advertised_len = 0, proof_len = 0;
    bool first = true;
    uint16_t prev = 0;
    while (!mls_tls_reader_done(&er)) {
        uint16_t id = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&er, &id) != 0 || read_vec(&er, &d, &dlen) != 0 ||
            (!first && id <= prev))
            return MARMOT_ERR_EXTENSION_FORMAT;
        first = false;
        prev = id;
        if (id == MLS_COMPONENT_APP_COMPONENTS) {
            advertised = d;
            advertised_len = dlen;
        } else if (id == MLS_COMPONENT_ACCOUNT_PROOF_V2) {
            proof = d;
            proof_len = dlen;
        } else if (id == MLS_COMPONENT_SAFE_AAD) {
            /* The leaf's safe_aad support list: any canonical list. */
            if (components_list_covers(d, dlen, NULL, 0) != 0) return MARMOT_ERR_EXTENSION_FORMAT;
        }
    }
    /* app_components advertises every required component (incl. 0x8009). */
    if (!advertised) return MARMOT_ERR_VALIDATION;
    int rc = components_list_covers(advertised, advertised_len, gc->components,
                                    gc->n_components);
    if (rc != 0) return rc;
    /* Every agent-stream role the group requires of its members, as an
     * advertised capability (agent-text-stream-quic-v1.md "Validation"). */
    for (size_t i = 0; i < sizeof(AGENT_STREAM_ROLES) / sizeof(AGENT_STREAM_ROLES[0]); i++)
        if ((gc->required_member_roles & AGENT_STREAM_ROLES[i].bit) &&
            !list_contains(leaf->cap_extensions, leaf->cap_extension_count,
                           AGENT_STREAM_ROLES[i].ext))
            return MARMOT_ERR_VALIDATION;
    /* Exactly one 104-byte proof (the dictionary admits one entry per id),
     * naming this leaf's own account. */
    if (!proof || proof_len != MLS_ACCOUNT_PROOF_V2_LEN ||
        memcmp(proof, leaf->credential_identity, 32) != 0)
        return MARMOT_ERR_VALIDATION;
    return 0;
}

int
mls_adopted_tree_check(const MlsRatchetTree *tree, const MlsAdoptedGroupContext *gc,
                       bool admins_are_members)
{
    if (!tree || !gc || !tree->nodes) return MARMOT_ERR_INVALID_ARG;
    size_t members = 0;
    for (uint32_t i = 0; i < tree->n_leaves; i++) {
        const MlsNode *node = &tree->nodes[mls_tree_leaf_to_node(i)];
        if (node->type != MLS_NODE_LEAF) continue;
        int rc = mls_adopted_leaf_check(&node->leaf, gc);
        if (rc != 0) return rc;
        members++;
    }
    if (members == 0) return MARMOT_ERR_VALIDATION;
    if (!admins_are_members) return 0;

    /* admin-policy-v1.md: every admin key is the account of a member. */
    const uint8_t *keys = NULL;
    size_t count = 0;
    if (!gc->admins || mls_admin_policy_v1_decode(gc->admins, gc->admins_len, &keys, &count) != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t k = 0; k < count; k++) {
        bool member = false;
        for (uint32_t i = 0; i < tree->n_leaves && !member; i++) {
            const MlsNode *node = &tree->nodes[mls_tree_leaf_to_node(i)];
            member = node->type == MLS_NODE_LEAF &&
                     node->leaf.credential_identity_len == 32 &&
                     memcmp(node->leaf.credential_identity, keys + 32 * k, 32) == 0;
        }
        if (!member) return MARMOT_ERR_VALIDATION;
    }
    return 0;
}

int
mls_group_profile_admit(const uint8_t *exts, size_t exts_len,
                        const MlsRatchetTree *tree, MarmotGroupProfile *profile_out)
{
    if (!profile_out) return MARMOT_ERR_INVALID_ARG;
    MarmotGroupProfile profile = mls_group_context_profile_of(exts, exts_len);
    if (profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        MlsAdoptedGroupContext gc;
        int rc = mls_adopted_group_context_parse(exts, exts_len, &gc);
        if (rc == 0 && tree) rc = mls_adopted_tree_check(tree, &gc, true);
        if (rc != 0) return rc;
    }
    *profile_out = profile;
    return 0;
}
