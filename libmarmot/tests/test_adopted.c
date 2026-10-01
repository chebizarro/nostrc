/*
 * libmarmot tests: adopted-profile group admission and creation
 * (nostrc-qp24.5.1, nostrc-qp24.5.1.1).
 *
 * Fixtures are real MDK v0.11.0 / pinned-OpenMLS captures
 * (tests/vectors/mdk-0.11, provenance in its README).  Every refusal is
 * checked for its specific error and for leaving nothing stored.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "adopted.h"
#include "commits.h"
#include "proposals.h"
#include "kp_profile.h"
#include "test_enroll.h"
#include "mls/mls_group.h"
#include "mls/mls_app_components.h"
#include "mls/mls_key_package.h"
#include "mls/mls_tree.h"
#include "mls/mls-internal.h"
#include "vectors/mdk-0.11/adopted_fixture.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "\n  FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
            abort();                                                        \
        }                                                                   \
    } while (0)

#define OK(expr)                                                            \
    do {                                                                    \
        MarmotError e_ = (expr);                                            \
        CHECK(e_ == MARMOT_OK, "%s -> %d (%s)", #expr, e_,                  \
              marmot_error_string(e_));                                     \
    } while (0)

#define EXPECT_ERR(expr, want)                                              \
    do {                                                                    \
        int e_ = (expr);                                                    \
        CHECK(e_ == (want), "%s -> %d (%s), want %d (%s)", #expr, e_,       \
              marmot_error_string((MarmotError)e_), (int)(want),            \
              marmot_error_string((MarmotError)(want)));                    \
    } while (0)

static const char *g_only;
#define RUN(fn)                                                             \
    do {                                                                    \
        if (g_only && strcmp(g_only, #fn) != 0) break;                      \
        printf("  %-58s", #fn); fflush(stdout); fn(); printf("PASS\n");    \
    } while (0)


/* ── Result helpers ───────────────────────────────────────────────────── */

static void
groups_free(MarmotGroup **groups, size_t n)
{
    for (size_t i = 0; i < n; i++) marmot_group_free(groups[i]);
    free(groups);
}

static void
welcomes_free(MarmotWelcome **w, size_t n)
{
    for (size_t i = 0; i < n; i++) marmot_welcome_free(w[i]);
    free(w);
}

static void
relays_free(MarmotGroupRelay *r, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(r[i].relay_url);
        marmot_group_id_free(&r[i].mls_group_id);
    }
    free(r);
}

/* memmem() is a GNU extension (hidden under -std=c11 on glibc). */
static uint8_t *
find_bytes(uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t n)
{
    for (size_t i = 0; n <= hay_len && i + n <= hay_len; i++)
        if (memcmp(hay + i, needle, n) == 0) return hay + i;
    return NULL;
}

/* The account key MDK's test support derives from a seed
 * (cgka-engine/tests/support/mod.rs signing_key): the fixtures' identities. */
static void
mdk_test_identity(const char *seed, uint8_t out[32])
{
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    for (uint64_t counter = 0;; counter++) {
        uint8_t sk[32], be[8];
        for (int i = 0; i < 8; i++) be[i] = (uint8_t)(counter >> (56 - 8 * i));
        crypto_hash_sha256_state st;
        crypto_hash_sha256_init(&st);
        crypto_hash_sha256_update(&st, (const uint8_t *)"cgka-engine-test-identity-v1", 28);
        crypto_hash_sha256_update(&st, (const uint8_t *)seed, strlen(seed));
        crypto_hash_sha256_update(&st, be, 8);
        crypto_hash_sha256_final(&st, sk);
        if (!secp256k1_ec_seckey_verify(ctx, sk)) continue;
        secp256k1_keypair kp;
        secp256k1_xonly_pubkey x;
        CHECK(secp256k1_keypair_create(ctx, &kp, sk) &&
              secp256k1_keypair_xonly_pub(ctx, &x, NULL, &kp) &&
              secp256k1_xonly_pubkey_serialize(ctx, out, &x), "identity");
        break;
    }
    secp256k1_context_destroy(ctx);
}

/* ── Bytes ────────────────────────────────────────────────────────────── */

static uint8_t *
unhex(const char *hex, size_t *len)
{
    size_t n = strlen(hex) / 2;
    uint8_t *out = malloc(n ? n : 1);
    CHECK(out && sodium_hex2bin(out, n, hex, 2 * n, NULL, len, NULL) == 0 && *len == n,
          "hex");
    return out;
}

static void
unhex_into(const char *hex, uint8_t *out, size_t n)
{
    size_t len = 0;
    CHECK(strlen(hex) == 2 * n && sodium_hex2bin(out, n, hex, 2 * n, NULL, &len, NULL) == 0 &&
          len == n, "hex %zu", n);
}

/* The MDK GroupContext extensions come with their vector length; the MLS
 * state keeps the entries only. */
static const uint8_t *
strip_vec(const uint8_t *data, size_t len, size_t *out_len)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    size_t n = 0;
    CHECK(mls_tls_read_vli(&r, &n) == 0 && n == mls_tls_reader_remaining(&r), "vector");
    *out_len = n;
    return data + r.pos;
}

/* ── GroupContext builder: the MDK capture taken apart and put together ─ */

typedef struct {
    uint16_t       id;
    const uint8_t *data;
    size_t         len;
    const uint8_t *raw;     /* pre-encoded entry, written verbatim */
    size_t         raw_len;
} Comp;

typedef struct {
    uint16_t ext[8], prop[8], cred[4];
    size_t   n_ext, n_prop, n_cred;
    bool     no_caps, no_dict;
    Comp     entries[16];
    size_t   n_entries;
    uint16_t extra_ext[2];
    size_t   n_extra_ext;
} GcSpec;

static const uint8_t *g_fixture_exts;
static size_t g_fixture_exts_len;

/* Parse an (adopted) GroupContext extension list into a spec. */
static void
gc_spec_from(const uint8_t *exts, size_t len, GcSpec *s)
{
    memset(s, 0, sizeof(*s));
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, len);
    while (!mls_tls_reader_done(&r)) {
        uint16_t type = 0;
        size_t dlen = 0;
        CHECK(mls_tls_read_u16(&r, &type) == 0 && mls_tls_read_vli(&r, &dlen) == 0, "ext");
        const uint8_t *d = r.data + r.pos;
        r.pos += dlen;
        MlsTlsReader dr;
        mls_tls_reader_init(&dr, d, dlen);
        if (type == 0x0003) {
            uint16_t *lists[3] = {s->ext, s->prop, s->cred};
            size_t *counts[3] = {&s->n_ext, &s->n_prop, &s->n_cred};
            for (int l = 0; l < 3; l++) {
                size_t bytes = 0;
                CHECK(mls_tls_read_vli(&dr, &bytes) == 0, "caps");
                for (size_t i = 0; i < bytes / 2; i++)
                    CHECK(mls_tls_read_u16(&dr, &lists[l][(*counts[l])++]) == 0, "cap");
            }
        } else if (type == 0x0006) {
            size_t entries_len = 0;
            CHECK(mls_tls_read_vli(&dr, &entries_len) == 0, "dict");
            while (!mls_tls_reader_done(&dr)) {
                Comp *c = &s->entries[s->n_entries++];
                CHECK(mls_tls_read_u16(&dr, &c->id) == 0 && mls_tls_read_vli(&dr, &c->len) == 0,
                      "entry");
                c->data = dr.data + dr.pos;
                dr.pos += c->len;
            }
        }
    }
}

static uint8_t *
gc_build(const GcSpec *s, size_t *out_len)
{
    MlsTlsBuf caps, entries, dict, exts;
    CHECK(mls_tls_buf_init(&caps, 32) == 0 && mls_tls_buf_init(&entries, 512) == 0 &&
          mls_tls_buf_init(&dict, 512) == 0 && mls_tls_buf_init(&exts, 512) == 0, "bufs");
    const uint16_t *lists[3] = {s->ext, s->prop, s->cred};
    const size_t counts[3] = {s->n_ext, s->n_prop, s->n_cred};
    for (int l = 0; l < 3; l++) {
        CHECK(mls_tls_write_vli(&caps, counts[l] * 2) == 0, "caps");
        for (size_t i = 0; i < counts[l]; i++) CHECK(mls_tls_write_u16(&caps, lists[l][i]) == 0, "c");
    }
    for (size_t i = 0; i < s->n_entries; i++) {
        const Comp *c = &s->entries[i];
        if (c->raw) {
            CHECK(mls_tls_buf_append(&entries, c->raw, c->raw_len) == 0, "raw");
            continue;
        }
        CHECK(mls_tls_write_u16(&entries, c->id) == 0 &&
              mls_tls_write_opaque32(&entries, c->data, c->len) == 0, "entry");
    }
    CHECK(mls_tls_write_opaque32(&dict, entries.data, entries.len) == 0, "dict");
    if (!s->no_caps)
        CHECK(mls_tls_write_u16(&exts, 0x0003) == 0 &&
              mls_tls_write_opaque32(&exts, caps.data, caps.len) == 0, "caps ext");
    if (!s->no_dict)
        CHECK(mls_tls_write_u16(&exts, 0x0006) == 0 &&
              mls_tls_write_opaque32(&exts, dict.data, dict.len) == 0, "dict ext");
    for (size_t i = 0; i < s->n_extra_ext; i++) {
        static const uint8_t two[] = {0x00, 0x02};
        CHECK(mls_tls_write_u16(&exts, s->extra_ext[i]) == 0 &&
              mls_tls_write_opaque32(&exts, two, sizeof(two)) == 0, "extra");
    }
    mls_tls_buf_free(&caps);
    mls_tls_buf_free(&entries);
    mls_tls_buf_free(&dict);
    *out_len = exts.len;
    return exts.data;
}

static Comp *
gc_entry(GcSpec *s, uint16_t id)
{
    for (size_t i = 0; i < s->n_entries; i++)
        if (s->entries[i].id == id) return &s->entries[i];
    return NULL;
}

static void
gc_drop(GcSpec *s, uint16_t id)
{
    for (size_t i = 0; i < s->n_entries; i++)
        if (s->entries[i].id == id) {
            memmove(&s->entries[i], &s->entries[i + 1],
                    (s->n_entries - i - 1) * sizeof(Comp));
            s->n_entries--;
            return;
        }
}

/* Insert keeping the ascending order. */
static void
gc_add(GcSpec *s, uint16_t id, const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i < s->n_entries && s->entries[i].id < id) i++;
    memmove(&s->entries[i + 1], &s->entries[i], (s->n_entries - i) * sizeof(Comp));
    s->entries[i] = (Comp){.id = id, .data = data, .len = len};
    s->n_entries++;
}

static int
gc_parse_spec(const GcSpec *s)
{
    size_t len = 0;
    uint8_t *exts = gc_build(s, &len);
    MlsAdoptedGroupContext gc;
    int rc = mls_adopted_group_context_parse(exts, len, &gc);
    free(exts);
    return rc;
}

/* A ComponentsList of @ids, in the given order (QUIC varint length). */
static size_t
comp_list(uint8_t *out, const uint16_t *ids, size_t n)
{
    out[0] = (uint8_t)(n * 2);
    for (size_t i = 0; i < n; i++) {
        out[1 + 2 * i] = (uint8_t)(ids[i] >> 8);
        out[2 + 2 * i] = (uint8_t)(ids[i] & 0xff);
    }
    return 1 + 2 * n;
}

/* ── Members ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    Marmot     *m;
    uint8_t     sk[32], pk[32];
    char        sk_hex[65];
} Member;

static void
member_init(Member *x, const char *name)
{
    memset(x, 0, sizeof(*x));
    x->name = name;
    /* Fixture Welcomes are dated at capture: no age limit in these tests. */
    MarmotConfig cfg = marmot_config_default();
    cfg.max_event_age_secs = 0;
    x->m = marmot_new_with_config(marmot_storage_memory_new(), &cfg);
    CHECK(x->m, "marmot_new");
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    do randombytes_buf(x->sk, 32); while (!secp256k1_ec_seckey_verify(ctx, x->sk));
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xonly;
    CHECK(secp256k1_keypair_create(ctx, &kp, x->sk) &&
          secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &kp) &&
          secp256k1_xonly_pubkey_serialize(ctx, x->pk, &xonly), "keypair");
    secp256k1_context_destroy(ctx);
    for (int i = 0; i < 32; i++) snprintf(x->sk_hex + 2 * i, 3, "%02x", x->sk[i]);
}

static void
member_free(Member *x)
{
    marmot_free(x->m);
    sodium_memzero(x->sk, sizeof(x->sk));
}

/* MarmotAccountSignFunc: a signer that holds the account key (or refuses,
 * or signs with another key). */
typedef struct {
    const char *sk_hex;
    int         calls;
    bool        refuse;
} Signer;

static int
sign_cb(void *ud, const char *unsigned_json, char **out)
{
    Signer *s = ud;
    s->calls++;
    if (s->refuse) return 1;
    NostrEvent *ev = nostr_event_new();
    int rc = 1;
    if (ev && nostr_event_deserialize_compact(ev, unsigned_json, NULL) &&
        nostr_event_sign(ev, s->sk_hex) == 0) {
        *out = nostr_event_serialize_compact(ev);
        rc = *out ? 0 : 1;
    }
    if (ev) nostr_event_free(ev);
    return rc;
}

static char *
adopted_key_package(Member *x)
{
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package_adopted_internal(x->m, x->pk, x->sk, NULL, NULL, &r));
    char *json = strdup(r.event_json);
    marmot_key_package_result_free(&r);
    return json;
}

static MarmotError
join(Member *x, const char *rumor, MarmotWelcome **out)
{
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    MarmotError err = marmot_process_welcome(x->m, wrapper, rumor, &w);
    if (err == MARMOT_OK) err = marmot_accept_welcome(x->m, w);
    if (out) *out = w;
    else marmot_welcome_free(w);
    return err;
}

/* Install a KeyPackage's private material the way marmot_create_key_package()
 * stores it ("kp_priv" = init || encryption || Ed25519 secret; "kp_full"). */
static void
install_key_package(Member *x, const uint8_t *kp_bytes, size_t kp_len, const char *init_hex,
                    const char *enc_hex, const char *seed_hex, const char *pub_hex,
                    const char *want_ref_hex)
{
    MlsKeyPackage kp;
    memset(&kp, 0, sizeof(kp));
    MlsTlsReader r;
    mls_tls_reader_init(&r, kp_bytes, kp_len);
    CHECK(mls_key_package_deserialize(&r, &kp) == 0 && mls_tls_reader_done(&r), "kp");
    uint8_t ref[32];
    CHECK(mls_key_package_ref(&kp, ref) == 0, "ref");
    if (want_ref_hex) {
        uint8_t want[32];
        unhex_into(want_ref_hex, want, 32);
        CHECK(memcmp(ref, want, 32) == 0, "libmarmot computes MDK's KeyPackageRef");
    }
    uint8_t priv[32 + 32 + 64], seed[32], pk[32];
    unhex_into(init_hex, priv, 32);
    unhex_into(enc_hex, priv + 32, 32);
    unhex_into(seed_hex, seed, 32);
    unhex_into(pub_hex, pk, 32);
    uint8_t derived_pk[32];
    CHECK(crypto_sign_seed_keypair(derived_pk, priv + 64, seed) == 0 &&
          memcmp(derived_pk, pk, 32) == 0 &&
          memcmp(kp.leaf_node.signature_key, pk, 32) == 0, "signature key");
    OK(x->m->storage->mls_store(x->m->storage->ctx, "kp_priv", ref, 32, priv, sizeof(priv)));
    OK(x->m->storage->mls_store(x->m->storage->ctx, "kp_full", ref, 32, kp_bytes, kp_len));
    sodium_memzero(priv, sizeof(priv));
    mls_key_package_clear(&kp);
}

static void
load_mls(Member *x, const MarmotGroupId *gid, uint8_t **blob, size_t *len)
{
    OK(x->m->storage->mls_load(x->m->storage->ctx, "mls_group", gid->data, gid->len, blob, len));
}

static bool
group_stored(Member *x, const MarmotGroupId *gid)
{
    MarmotGroup *g = NULL;
    MarmotError err = marmot_get_group(x->m, gid, &g); /* OK + NULL: not found */
    bool found = err == MARMOT_OK && g != NULL;
    marmot_group_free(g);
    return found;
}

static size_t
count_tags(NostrEvent *ev, const char *key)
{
    size_t n = 0;
    for (size_t i = 0; ev->tags && i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (nostr_tag_size(t) >= 1 && strcmp(nostr_tag_get_key(t), key) == 0) n++;
    }
    return n;
}

static NostrTag *
tag(NostrEvent *ev, const char *key)
{
    for (size_t i = 0; ev->tags && i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (nostr_tag_size(t) >= 1 && strcmp(nostr_tag_get_key(t), key) == 0) return t;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GroupContext admission (structural)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_mdk_group_context_admitted(void)
{
    const AdoptedMdkFixture *f = &MDK011_ENGINE_DEFAULT;
    CHECK(mls_group_context_profile_of(g_fixture_exts, g_fixture_exts_len) ==
              MARMOT_GROUP_PROFILE_ADOPTED, "classified adopted");
    MlsAdoptedGroupContext gc;
    OK(mls_adopted_group_context_parse(g_fixture_exts, g_fixture_exts_len, &gc));
    static const uint16_t want[] = {0x8001, 0x8003, 0x8004, 0x8009, 0x800c};
    CHECK(gc.n_components == 5 && memcmp(gc.components, want, sizeof(want)) == 0,
          "MDK requires the components libmarmot supports");
    CHECK(gc.n_ext_types == 1 && gc.ext_types[0] == 0x0006 && gc.n_proposal_types == 1 &&
          gc.proposal_types[0] == 0x0008 && gc.n_credential_types == 0, "required_capabilities");

    const uint8_t *name, *desc;
    size_t nl, dl;
    OK(mls_group_profile_v1_decode(gc.profile, gc.profile_len, &name, &nl, &desc, &dl));
    CHECK(nl == 20 && memcmp(name, "W24-E engine-default", 20) == 0 && dl == 19 &&
          memcmp(desc, "MDK v0.11.0 fixture", 19) == 0, "profile");
    const uint8_t *keys;
    size_t n_keys;
    uint8_t creator[32];
    unhex_into(f->creator_pub, creator, 32);
    OK(mls_admin_policy_v1_decode(gc.admins, gc.admins_len, &keys, &n_keys));
    CHECK(n_keys == 1 && memcmp(keys, creator, 32) == 0, "admin = creator");
    const uint8_t *ngid;
    MlsRelaySpan relays[MARMOT_NOSTR_ROUTING_MAX_RELAYS];
    size_t n_relays;
    uint8_t want_ngid[32];
    unhex_into(f->nostr_group_id, want_ngid, 32);
    OK(mls_nostr_routing_v1_decode(gc.routing, gc.routing_len, &ngid, relays, &n_relays));
    CHECK(memcmp(ngid, want_ngid, 32) == 0 && n_relays == 2 &&
          relays[0].len == strlen(f->relay_a) && memcmp(relays[0].url, f->relay_a, relays[0].len) == 0 &&
          memcmp(relays[1].url, f->relay_b, relays[1].len) == 0, "routing");
    CHECK(gc.has_lifecycle, "lifecycle active");

    /* The builder reproduces the capture byte for byte, so the mutations
     * below change exactly what they say. */
    GcSpec s;
    gc_spec_from(g_fixture_exts, g_fixture_exts_len, &s);
    size_t len = 0;
    uint8_t *rebuilt = gc_build(&s, &len);
    CHECK(len == g_fixture_exts_len && memcmp(rebuilt, g_fixture_exts, len) == 0, "rebuild");
    free(rebuilt);

    /* What libmarmot builds for its own groups is admitted too. */
    const char *rel[] = {"wss://relay-a.example.com", "wss://relay-b.example.com"};
    uint8_t *own = NULL;
    size_t own_len = 0;
    OK(marmot_adopted_group_context_build("n", "d", (const uint8_t (*)[32])creator, 1, want_ngid,
                                          rel, 2, &own, &own_len));
    CHECK(mls_group_context_profile_of(own, own_len) == MARMOT_GROUP_PROFILE_ADOPTED, "own");
    free(own);
}

/* The White Noise fixture's GroupContext extensions (entries only). */
static uint8_t *
wn_exts(size_t *len, const uint8_t **exts)
{
    size_t raw_len = 0;
    uint8_t *raw = unhex(MDK011_WHITE_NOISE_APP.group_context, &raw_len);
    *exts = strip_vec(raw, raw_len, len);
    return raw;
}

static void
test_white_noise_group_context_admitted(void)
{
    /* Every White Noise (MDK 0.11 marmot-app) group requires SelfRemove
     * (0x000a), the agent text stream (0x8006, receive role) and encrypted
     * media v2 (0x800b): admitted since nostrc-qp24.5.2. */
    size_t len = 0;
    const uint8_t *exts = NULL;
    uint8_t *raw = wn_exts(&len, &exts);
    CHECK(mls_group_context_profile_of(exts, len) == MARMOT_GROUP_PROFILE_ADOPTED, "adopted");
    MlsAdoptedGroupContext gc;
    OK(mls_adopted_group_context_parse(exts, len, &gc));
    static const uint16_t want[] = {0x8001, 0x8003, 0x8004, 0x8006, 0x8009, 0x800b, 0x800c};
    CHECK(gc.n_components == 7 && memcmp(gc.components, want, sizeof(want)) == 0,
          "White Noise requires 0x8006 and 0x800b");
    CHECK(gc.n_proposal_types == 2 && gc.proposal_types[0] == 0x0008 &&
              gc.proposal_types[1] == 0x000a && gc.n_ext_types == 1 && gc.ext_types[0] == 0x0006,
          "SelfRemove required, the receive role not folded into required_capabilities");
    CHECK(gc.required_member_roles == MARMOT_AGENT_STREAM_ROLE_RECEIVE && gc.agent_stream &&
              gc.media_policy && !gc.image && !gc.avatar, "component states");

    /* The read side (nostrc-m6tp). */
    MarmotGroupComponents c;
    OK(marmot_adopted_components_from_extensions(exts, len, 1, &c));
    CHECK(c.epoch == 1 && strcmp(c.name, "W24-E white-noise-app") == 0 &&
              strcmp(c.description, "MDK v0.11.0 fixture") == 0, "profile 0x8001");
    CHECK(c.required_component_count == 7 &&
              memcmp(c.required_components, want, sizeof(want)) == 0, "required list");
    MarmotAgentTextStreamPolicy def = marmot_agent_text_stream_policy_user_to_agent_default();
    /* Field by field: the struct has padding. */
    const MarmotAgentTextStreamPolicy *a = &c.agent_text_stream;
    CHECK(c.has_agent_text_stream && a->required_member_roles == def.required_member_roles &&
              a->allowed_member_roles == def.allowed_member_roles &&
              a->max_plaintext_frame_len == def.max_plaintext_frame_len &&
              a->replay_ttl_secs == def.replay_ttl_secs &&
              a->padding_bucket_bytes == def.padding_bucket_bytes,
          "user_to_agent_default");
    CHECK(c.has_media_policy && c.media_policy.allowed_locator_kind_count == 1 &&
              strcmp(c.media_policy.allowed_locator_kinds[0], "blossom-v1") == 0 &&
              c.media_policy.default_blob_endpoint_count == 1 &&
              strcmp(c.media_policy.default_blob_endpoints[0].base_url,
                     "https://blossom.example.com/") == 0,
          "media policy 0x800b");
    CHECK(!c.image.present && !c.avatar_url.url && c.avatar_source == MARMOT_GROUP_AVATAR_NONE,
          "no image");
    marmot_group_components_clear(&c);

    /* SelfRemove alone, or the components alone, are each admitted. */
    GcSpec s;
    gc_spec_from(exts, len, &s);
    CHECK(s.n_prop == 2 && s.prop[1] == 0x000a, "SelfRemove required");
    s.n_prop = 1;
    OK(gc_parse_spec(&s));
    free(raw);
}

static void
test_white_noise_component_negatives(void)
{
    size_t len = 0;
    const uint8_t *exts = NULL;
    uint8_t *raw = wn_exts(&len, &exts);
    GcSpec base, s;
    gc_spec_from(exts, len, &base);
    OK(gc_parse_spec(&base));

    /* Malformed 0x8006 (MDK AgentTextStreamQuicPolicyV1::validate). */
    static const uint8_t agent_bad[][13] = {
        {0, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0},          /* no required role */
        {9, 15, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0},         /* unknown required bit */
        {1, 0x81, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0},       /* unknown allowed bit */
        {1, 2, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0},          /* required not allowed */
        {1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},           /* frame 0 */
        {1, 3, 0, 0, 0xff, 0xf0, 0, 0, 0, 0, 0, 0},     /* frame 65520 */
        {1, 3, 0, 0, 16, 0, 0, 0, 1, 0x2d, 0, 0},       /* ttl 301 */
        {1, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0x10, 0x01},    /* padding 4097 */
    };
    for (size_t i = 0; i < sizeof(agent_bad) / sizeof(agent_bad[0]); i++) {
        s = base;
        gc_entry(&s, 0x8006)->data = agent_bad[i];
        gc_entry(&s, 0x8006)->len = 12;
        EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    }
    s = base;
    gc_entry(&s, 0x8006)->len = 11;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    static const uint8_t agent13[13] = {1, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0};
    gc_entry(&s, 0x8006)->data = agent13;
    gc_entry(&s, 0x8006)->len = 13;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* The v1 bounds themselves are valid. */
    static const uint8_t agent_max[12] = {1, 7, 0, 0, 0xff, 0xef, 0, 0, 1, 0x2c, 0x10, 0};
    s = base;
    gc_entry(&s, 0x8006)->data = agent_max;
    OK(gc_parse_spec(&s));
    /* A group that requires a role libmarmot does not play: send, fanout. */
    static const uint8_t agent_send[12] = {3, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t agent_fanout[12] = {5, 7, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0};
    gc_entry(&s, 0x8006)->data = agent_send;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    gc_entry(&s, 0x8006)->data = agent_fanout;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    /* Required, but no state. */
    s = base;
    gc_drop(&s, 0x8006);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    gc_drop(&s, 0x800b);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);

    /* Malformed 0x800b. */
    const Comp *media = gc_entry(&base, 0x800b);
    uint8_t buf[160];
    CHECK(media->len < sizeof(buf) - 1, "0x800b size");
    s = base;
    memcpy(buf, media->data, media->len);
    buf[media->len] = 0x00; /* trailing byte */
    gc_entry(&s, 0x800b)->data = buf;
    gc_entry(&s, 0x800b)->len = media->len + 1;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    memcpy(buf, media->data, media->len);
    buf[1 + 17] = '1'; /* "encrypted-media-v1" */
    gc_entry(&s, 0x800b)->data = buf;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    memcpy(buf, media->data, media->len);
    buf[media->len - 1] = 'x'; /* "https://blossom.example.comx": no path */
    gc_entry(&s, 0x800b)->data = buf;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    memcpy(buf, media->data, media->len);
    buf[21] = 'B'; /* allowed kind "Blossom-v1" (buf[20] is its length) */
    gc_entry(&s, 0x800b)->data = buf;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    gc_entry(&s, 0x800b)->len = media->len - 1; /* truncated */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);

    /* Known components nobody requires are validated too (MDK
     * validate_app_component_dictionary), and read. */
    static const uint8_t bad_avatar[] = {0x08, 'h', 't', 't', 'p', ':', '/', '/', 'x', 0x00, 0x00};
    s = base;
    gc_add(&s, 0x8007, bad_avatar, sizeof(bad_avatar));
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    static const uint8_t partial_image[] = {0x01, 0xaa, 0x00, 0x00, 0x00, 0x00};
    s = base;
    gc_add(&s, 0x8002, partial_image, sizeof(partial_image));
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    static const uint8_t retention7[7] = {0};
    static const uint8_t retention[8] = {0, 0, 0, 0, 0, 0, 0x0e, 0x10};
    s = base;
    gc_add(&s, 0x8005, retention7, sizeof(retention7));
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    gc_add(&s, 0x8005, retention, sizeof(retention));
    OK(gc_parse_spec(&s)); /* kept, not honoured (nostrc-b55p) */
    /* ... but a group that requires disappearing messages is refused. */
    uint8_t list[32];
    static const uint16_t with_retention[] = {0x8001, 0x8003, 0x8004, 0x8005, 0x8006,
                                              0x8009, 0x800b, 0x800c};
    gc_entry(&s, 0x0001)->data = list;
    gc_entry(&s, 0x0001)->len = comp_list(list, with_retention, 8);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);

    /* An image and an avatar URL nobody requires: valid, and read. */
    MarmotGroupBlossomImage img = {.present = true, .media_type = "image/png"};
    memset(img.image_hash, 0x11, 32);
    memset(img.image_key, 0x22, 32);
    memset(img.image_nonce, 0x33, 12);
    memset(img.image_upload_key, 0x44, 32);
    uint8_t *img_bytes = NULL, *av_bytes = NULL;
    size_t img_len = 0, av_len = 0;
    OK(marmot_group_blossom_image_encode(&img, &img_bytes, &img_len));
    MarmotGroupAvatarUrl av = {.url = "https://xn--bcher-kva.example/a.png", .url_unverified = true};
    /* An unverified URL is valid stored state but never produced: build it. */
    size_t ul = strlen(av.url);
    av_len = 1 + ul + 2;
    av_bytes = malloc(av_len);
    av_bytes[0] = (uint8_t)ul;
    memcpy(av_bytes + 1, av.url, ul);
    av_bytes[1 + ul] = 0;
    av_bytes[2 + ul] = 0;
    s = base;
    gc_add(&s, 0x8002, img_bytes, img_len);
    gc_add(&s, 0x8007, av_bytes, av_len);
    size_t elen = 0;
    uint8_t *e = gc_build(&s, &elen);
    MarmotGroupComponents c;
    OK(marmot_adopted_components_from_extensions(e, elen, 7, &c));
    CHECK(c.image.present && strcmp(c.image.media_type, "image/png") == 0 &&
              c.image.image_key[0] == 0x22 && c.image.image_upload_key[31] == 0x44,
          "0x8002 read");
    CHECK(c.avatar_url.url && strcmp(c.avatar_url.url, av.url) == 0 && c.avatar_url.url_unverified,
          "0x8007 read, unverified kept byte for byte");
    CHECK(c.avatar_source == MARMOT_GROUP_AVATAR_URL_PLACEHOLDER,
          "the URL avatar wins, as a placeholder");
    marmot_group_components_clear(&c);
    free(e);
    free(img_bytes);
    free(av_bytes);

    /* required_capabilities: the receive role may be required outright
     * (libmarmot advertises it); the other two may not. */
    s = base;
    s.ext[s.n_ext++] = 0xF2D1;
    OK(gc_parse_spec(&s));
    s = base;
    s.ext[s.n_ext++] = 0xF2D2;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    free(raw);
}

static void
test_group_context_negatives(void)
{
    GcSpec base, s;
    gc_spec_from(g_fixture_exts, g_fixture_exts_len, &base);
    OK(gc_parse_spec(&base));

    /* Non-canonical dictionary: entries out of order, repeated. */
    s = base;
    Comp tmp = s.entries[2];
    s.entries[2] = s.entries[3];
    s.entries[3] = tmp; /* 0x8004 before 0x8003 */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    s.entries[s.n_entries] = *gc_entry(&s, 0x8003);
    memmove(&s.entries[3], &s.entries[2], (s.n_entries - 2) * sizeof(Comp));
    s.n_entries++;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* Non-minimal length encoding of an entry (0x800c, 2-byte varint 1). */
    s = base;
    static const uint8_t nonminimal[] = {0x80, 0x0c, 0x40, 0x01, 0x00};
    Comp *life = gc_entry(&s, 0x800c);
    life->raw = nonminimal;
    life->raw_len = sizeof(nonminimal);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);

    /* Truncated components. */
    s = base;
    gc_entry(&s, 0x8004)->len -= 1; /* routing loses its last byte */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    gc_entry(&s, 0x8001)->len -= 1;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    gc_entry(&s, 0x8003)->len = 20; /* admin list cut mid-key */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* Trailing bytes after a component. */
    uint8_t routing_plus[160];
    s = base;
    Comp *route = gc_entry(&s, 0x8004);
    memcpy(routing_plus, route->data, route->len);
    routing_plus[route->len] = 0x00;
    route->data = routing_plus;
    route->len += 1;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    uint8_t profile_plus[64];
    s = base;
    Comp *prof = gc_entry(&s, 0x8001);
    memcpy(profile_plus, prof->data, prof->len);
    profile_plus[prof->len] = 0x00;
    prof->data = profile_plus;
    prof->len += 1;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);

    /* Missing required capabilities. */
    s = base;
    s.n_prop = 0; /* app_data_update not required */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    s.n_ext = 0; /* app_data_dictionary not required */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    s.no_caps = true;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    s.prop[s.n_prop++] = 0x0008; /* repeated */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* SelfRemove may be required (nostrc-qp24.5.2). */
    s = base;
    s.prop[s.n_prop++] = 0x000a;
    OK(gc_parse_spec(&s));
    /* Requirements libmarmot cannot honour. */
    s = base;
    s.prop[s.n_prop++] = 0x000b; /* unassigned */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    s = base;
    s.cred[s.n_cred++] = 0x0002; /* x509 */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    s = base;
    s.extra_ext[s.n_extra_ext++] = 0x0005; /* external_senders */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);

    /* Mixed profile: legacy group data or a v1 proof requirement. */
    s = base;
    s.extra_ext[s.n_extra_ext++] = 0xF2EE;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_VALIDATION);
    s = base;
    s.ext[s.n_ext++] = 0xF2F1;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_VALIDATION);

    /* app_components: lifetime invariants and canonical form. */
    uint8_t list[32];
    static const uint16_t no_proof[] = {0x8001, 0x8003, 0x8004, 0x800c};
    static const uint16_t no_admin[] = {0x8001, 0x8004, 0x8009, 0x800c};
    static const uint16_t no_route[] = {0x8001, 0x8003, 0x8009, 0x800c};
    static const uint16_t unsorted[] = {0x8003, 0x8001, 0x8004, 0x8009, 0x800c};
    static const uint16_t agent[] = {0x8001, 0x8003, 0x8004, 0x8006, 0x8009, 0x800c};
    s = base;
    gc_entry(&s, 0x0001)->data = list;
    gc_entry(&s, 0x0001)->len = comp_list(list, no_proof, 4);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    gc_entry(&s, 0x0001)->len = comp_list(list, no_admin, 4);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    gc_entry(&s, 0x0001)->len = comp_list(list, no_route, 4);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    gc_entry(&s, 0x0001)->len = comp_list(list, unsorted, 5);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* The agent text stream, receive role (nostrc-qp24.5.2). */
    static const uint8_t agent_state[] = {1, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0};
    gc_entry(&s, 0x0001)->len = comp_list(list, agent, 6);
    gc_add(&s, 0x8006, agent_state, sizeof(agent_state));
    OK(gc_parse_spec(&s));
    /* A component libmarmot does not implement, required: refused. */
    static const uint16_t retention_req[] = {0x8001, 0x8003, 0x8004, 0x8005, 0x8009, 0x800c};
    static const uint8_t retention_state[8] = {0};
    s = base;
    gc_entry(&s, 0x0001)->data = list;
    gc_entry(&s, 0x0001)->len = comp_list(list, retention_req, 6);
    gc_add(&s, 0x8005, retention_state, sizeof(retention_state));
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    s = base;
    gc_drop(&s, 0x0001);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* A required component without its state. */
    s = base;
    gc_drop(&s, 0x8001);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    s = base;
    gc_drop(&s, 0x8003);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* State in the wrong place or of the wrong kind. */
    static const uint8_t proof104[104] = {0};
    s = base;
    gc_add(&s, 0x8009, proof104, sizeof(proof104));
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    static const uint8_t empty_list[] = {0x00};
    s = base;
    gc_add(&s, 0x0002, empty_list, sizeof(empty_list)); /* safe_aad framing */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_UNSUPPORTED);
    s = base;
    gc_add(&s, 0x8008, empty_list, sizeof(empty_list)); /* frozen media v1 */
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_VALIDATION);
    static const uint8_t disbanded[] = {0x01}, bad_life[] = {0x02}, long_life[] = {0x00, 0x00};
    s = base;
    gc_entry(&s, 0x800c)->data = disbanded;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_VALIDATION);
    gc_entry(&s, 0x800c)->data = bad_life;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    gc_entry(&s, 0x800c)->data = long_life;
    gc_entry(&s, 0x800c)->len = 2;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* Profile: invalid UTF-8, over-long name. */
    static const uint8_t bad_utf8[] = {0x02, 0xc3, 0x28, 0x00};
    s = base;
    gc_entry(&s, 0x8001)->data = bad_utf8;
    gc_entry(&s, 0x8001)->len = sizeof(bad_utf8);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    uint8_t long_name[2 + 257 + 1];
    long_name[0] = 0x41; /* varint 257 */
    long_name[1] = 0x01;
    memset(long_name + 2, 'a', 257);
    long_name[2 + 257] = 0x00;
    s = base;
    gc_entry(&s, 0x8001)->data = long_name;
    gc_entry(&s, 0x8001)->len = sizeof(long_name);
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    /* Admin policy: empty, unsorted. */
    uint8_t admins2[1 + 64];
    admins2[0] = 0x40;
    s = base;
    gc_entry(&s, 0x8003)->data = admins2;
    gc_entry(&s, 0x8003)->len = 1;
    admins2[0] = 0x00;
    EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    admins2[0] = 0x40; /* 64 bytes: two keys */
    gc_entry(&s, 0x8003)->data = admins2;
    gc_entry(&s, 0x8003)->len = 0; /* placeholder, set below */
    {
        uint8_t blob[2 + 64];
        blob[0] = 0x40;
        blob[1] = 0x40;
        memset(blob + 2, 0x22, 32);
        memset(blob + 34, 0x11, 32); /* descending */
        gc_entry(&s, 0x8003)->data = blob;
        gc_entry(&s, 0x8003)->len = sizeof(blob);
        EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    }
    /* Routing: relay URL profile, order, count. */
    {
        uint8_t r[32 + 2 + 64];
        const char *bad[] = {"https://relay.example", "wss://user@relay.example",
                             "wss://relay.example/#frag", "wss://", "wss://relay example",
                             "wss://[zzz]", "wss://ex<ample.com", "wss://exa%zzmple.com"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            size_t ul = strlen(bad[i]);
            memset(r, 0x42, 32);
            r[32] = (uint8_t)(ul + 1);
            r[33] = (uint8_t)ul;
            memcpy(r + 34, bad[i], ul);
            s = base;
            gc_entry(&s, 0x8004)->data = r;
            gc_entry(&s, 0x8004)->len = 34 + ul;
            EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
        }
        /* b before a */
        const char *a = "wss://a.example", *b = "wss://b.example";
        memset(r, 0x42, 32);
        r[32] = (uint8_t)(2 + strlen(a) + strlen(b));
        r[33] = (uint8_t)strlen(b);
        memcpy(r + 34, b, strlen(b));
        r[34 + strlen(b)] = (uint8_t)strlen(a);
        memcpy(r + 35 + strlen(b), a, strlen(a));
        s = base;
        gc_entry(&s, 0x8004)->data = r;
        gc_entry(&s, 0x8004)->len = 35 + strlen(a) + strlen(b);
        EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
        r[32] = 0; /* no relay */
        gc_entry(&s, 0x8004)->len = 33;
        EXPECT_ERR(gc_parse_spec(&s), MARMOT_ERR_EXTENSION_FORMAT);
    }
    /* An unknown component nobody requires is kept, not interpreted. */
    static const uint8_t junk[] = {0xff, 0xfe, 0xfd};
    s = base;
    gc_add(&s, 0x9001, junk, sizeof(junk));
    OK(gc_parse_spec(&s));

    /* A legacy GroupContext is classified legacy and left alone. */
    static const uint8_t legacy[] = {0xf2, 0xee, 0x02, 0x00, 0x02};
    CHECK(mls_group_context_profile_of(legacy, sizeof(legacy)) == MARMOT_GROUP_PROFILE_LEGACY,
          "legacy");
    /* A recognizable but malformed 0x0006 after legacy group data --
     * truncated, or with a non-minimal length -- is adopted and refused, at
     * classification, creation and load alike (W24 review L1). */
    static const uint8_t legacy_trunc[] = {0xf2, 0xee, 0x02, 0x00, 0x02, 0x00, 0x06, 0x05, 0x00, 0x00};
    static const uint8_t legacy_nonmin[] = {0xf2, 0xee, 0x02, 0x00, 0x02, 0x00, 0x06, 0x40, 0x01, 0x00};
    const struct { const uint8_t *b; size_t n; } bad_dict[] = {
        {legacy_trunc, sizeof(legacy_trunc)}, {legacy_nonmin, sizeof(legacy_nonmin)}};
    for (size_t i = 0; i < 2; i++) {
        CHECK(mls_group_context_profile_of(bad_dict[i].b, bad_dict[i].n) ==
                  MARMOT_GROUP_PROFILE_ADOPTED, "malformed 0x0006 classified adopted");
        MlsAdoptedGroupContext bad_gc;
        EXPECT_ERR(mls_adopted_group_context_parse(bad_dict[i].b, bad_dict[i].n, &bad_gc),
                   MARMOT_ERR_EXTENSION_FORMAT); /* undecodable list: format first */
        uint8_t gid[32] = {1}, ident[32] = {2}, gsk[MLS_SIG_SK_LEN], gpk[MLS_SIG_PK_LEN];
        crypto_sign_keypair(gpk, gsk);
        MlsGroup g;
        CHECK(mls_group_create(&g, gid, 32, ident, 32, gsk, bad_dict[i].b, bad_dict[i].n) != 0,
              "creation refused");
        sodium_memzero(gsk, sizeof(gsk));
        /* Without the legacy entry the malformed dictionary itself fails. */
        EXPECT_ERR(mls_adopted_group_context_parse(bad_dict[i].b + 5, bad_dict[i].n - 5, &bad_gc),
                   MARMOT_ERR_EXTENSION_FORMAT);
    }

    /* Only an app_data_dictionary makes a group adopted: a legacy group's
     * required_capabilities may name 0x0006 / 0x0008 (e.g. computed from
     * its members' capabilities) and stays legacy, as before 0.12.0. */
    static const uint8_t caps_only[] = {0x00, 0x03, 0x07, 0x02, 0x00, 0x06,
                                        0x02, 0x00, 0x08, 0x00};
    CHECK(mls_group_context_profile_of(caps_only, sizeof(caps_only)) ==
              MARMOT_GROUP_PROFILE_LEGACY, "caps alone stay legacy");
    MlsAdoptedGroupContext gc;
    EXPECT_ERR(mls_adopted_group_context_parse(caps_only, sizeof(caps_only), &gc),
               MARMOT_ERR_EXTENSION_FORMAT);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Members (structural + proofs) on the MDK tree
 * ══════════════════════════════════════════════════════════════════════════ */

static void
leaf_dict_rebuild(MlsLeafNode *leaf, const uint16_t *advertised, size_t n_adv,
                  const uint8_t *proof, size_t proof_len, bool extra_v1_ext)
{
    MlsTlsBuf list, entries, dict, exts;
    CHECK(mls_tls_buf_init(&list, 32) == 0 && mls_tls_buf_init(&entries, 256) == 0 &&
          mls_tls_buf_init(&dict, 256) == 0 && mls_tls_buf_init(&exts, 256) == 0, "bufs");
    CHECK(mls_tls_write_vli(&list, n_adv * 2) == 0, "list");
    for (size_t i = 0; i < n_adv; i++) CHECK(mls_tls_write_u16(&list, advertised[i]) == 0, "id");
    static const uint8_t empty[] = {0x00};
    CHECK(mls_tls_write_u16(&entries, 0x0001) == 0 &&
          mls_tls_write_opaque32(&entries, list.data, list.len) == 0 &&
          mls_tls_write_u16(&entries, 0x0002) == 0 &&
          mls_tls_write_opaque32(&entries, empty, 1) == 0, "entries");
    if (proof)
        CHECK(mls_tls_write_u16(&entries, 0x8009) == 0 &&
              mls_tls_write_opaque32(&entries, proof, proof_len) == 0, "proof");
    CHECK(mls_tls_write_opaque32(&dict, entries.data, entries.len) == 0 &&
          mls_tls_write_u16(&exts, 0x0006) == 0 &&
          mls_tls_write_opaque32(&exts, dict.data, dict.len) == 0, "exts");
    if (extra_v1_ext)
        CHECK(mls_tls_write_u16(&exts, 0xF2F1) == 0 && mls_tls_write_opaque32(&exts, empty, 1) == 0,
              "v1");
    free(leaf->extensions_data);
    leaf->extensions_data = exts.data;
    leaf->extensions_len = exts.len;
    mls_tls_buf_free(&list);
    mls_tls_buf_free(&entries);
    mls_tls_buf_free(&dict);
}

/* The proof bytes of a leaf (borrowed). */
static const uint8_t *
leaf_proof(const MlsLeafNode *leaf)
{
    const uint8_t *dict = NULL;
    size_t dlen = 0, count = 0;
    CHECK(marmot_extensions_find(leaf->extensions_data, leaf->extensions_len, 0x0006, &dict,
                                 &dlen, &count) == 0 && count == 1, "dict");
    MarmotComponentData *e = NULL;
    size_t n = 0;
    CHECK(marmot_app_data_dict_parse(dict, dlen, &e, &n) == 0, "parse");
    const uint8_t *p = NULL;
    for (size_t i = 0; i < n; i++)
        if (e[i].component_id == 0x8009 && e[i].len == 104) p = e[i].data;
    free(e);
    CHECK(p, "proof");
    return p;
}

/* W24 review N1: the relay URL profile refuses every host MDK's url::Url
 * refuses, so libmarmot cannot create a group MDK will not join. */
static void
test_relay_url_profile(void)
{
    static const char *good[] = {
        "wss://relay.example.com", "ws://localhost:7777", "wss://relay.example.com.",
        "wss://127.0.0.1:7777", "wss://[::1]", "wss://[::1]:7777", "wss://[2001:db8::1]",
        "wss://[2001:db8:0:0:0:0:0:1]", "wss://[::ffff:192.0.2.1]", "wss://[1::]",
        "wss://relay.example.com/path?x=1", "wss://a-b_c*d.example",
        "wss://[::1]:", /* empty port: WHATWG accepts it */
    };
    static const char *bad[] = {
        /* the review's three */
        "wss://[zzz]", "wss://ex<ample.com", "wss://exa%zzmple.com",
        /* IPv6 literals */
        "wss://[]", "wss://[:]", "wss://[:::]", "wss://[1:2]", "wss://[1::2::3]",
        "wss://[12345::1]", "wss://[1:2:3:4:5:6:7:8:9]", "wss://[::1%25eth0]",
        "wss://[::1.2.3]", "wss://[::1.2.3.256]", "wss://[1:]", "wss://[:1]",
        "wss://[::1]x",
        /* forbidden host / domain code points, non-ASCII */
        "wss://ex>ample.com", "wss://ex^ample.com", "wss://ex|ample.com",
        "wss://ex%41mple.com", "wss://rel\xc3\xa9y.example",
        /* hosts that end in a number */
        "wss://999.1.1.1", "wss://1.2.3", "wss://127.1", "wss://010.0.0.1", "wss://a.0x1",
        "wss://example.123", "wss://1.2.3.4.5",
        /* empty labels */
        "wss://.", "wss://a..",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        CHECK(mls_relay_url_valid((const uint8_t *)good[i], strlen(good[i])), "accepts %s",
              good[i]);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!mls_relay_url_valid((const uint8_t *)bad[i], strlen(bad[i])), "refuses %s",
              bad[i]);
}

static void
test_mdk_tree_members(void)
{
    size_t tlen = 0;
    uint8_t *tree_bytes = unhex(MDK011_ENGINE_DEFAULT.ratchet_tree, &tlen);
    MlsRatchetTree tree;
    memset(&tree, 0, sizeof(tree));
    CHECK(mls_ratchet_tree_deserialize(tree_bytes, tlen, &tree) == 0 && tree.n_leaves == 2,
          "MDK tree");
    MlsAdoptedGroupContext gc;
    OK(mls_adopted_group_context_parse(g_fixture_exts, g_fixture_exts_len, &gc));
    OK(mls_adopted_tree_check(&tree, &gc, true));
    for (uint32_t i = 0; i < 2; i++)
        CHECK(marmot_leaf_proof_status(&tree.nodes[2 * i].leaf, MARMOT_CIPHERSUITE) ==
                  MARMOT_LEAF_PROOF_VALID, "MDK proof v2 verifies (leaf %u)", i);

    MlsLeafNode *leaf = &tree.nodes[2].leaf; /* the joiner */
    uint8_t proof[104];
    memcpy(proof, leaf_proof(leaf), 104);
    uint8_t *saved = malloc(leaf->extensions_len);
    size_t saved_len = leaf->extensions_len;
    memcpy(saved, leaf->extensions_data, saved_len);
#define RESTORE()                                                              \
    do {                                                                       \
        free(leaf->extensions_data);                                           \
        leaf->extensions_data = malloc(saved_len);                             \
        memcpy(leaf->extensions_data, saved, saved_len);                       \
        leaf->extensions_len = saved_len;                                      \
    } while (0)

    static const uint16_t full[] = {0x0001, 0x8001, 0x8003, 0x8004, 0x8009, 0x800c};
    static const uint16_t no_life[] = {0x0001, 0x8001, 0x8003, 0x8004, 0x8009};
    /* Same content rebuilt: still valid (the rebuild is faithful). */
    leaf_dict_rebuild(leaf, full, 6, proof, 104, false);
    OK(mls_adopted_tree_check(&tree, &gc, true));
    /* Missing proof. */
    leaf_dict_rebuild(leaf, full, 6, NULL, 0, false);
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    /* A proof naming another account. */
    uint8_t other[104];
    memcpy(other, proof, 104);
    other[0] ^= 0x01;
    leaf_dict_rebuild(leaf, full, 6, other, 104, false);
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    /* A proof of the wrong length. */
    leaf_dict_rebuild(leaf, full, 6, proof, 103, false);
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    /* A required component not advertised. */
    leaf_dict_rebuild(leaf, no_life, 5, proof, 104, false);
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    /* Mixed leaf: a v1 (0xF2F1) proof extension beside the dictionary. */
    leaf_dict_rebuild(leaf, full, 6, proof, 104, true);
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    RESTORE();
    OK(mls_adopted_tree_check(&tree, &gc, true));

    /* Bad proof (structurally fine, signature does not verify). */
    uint8_t *p = (uint8_t *)leaf_proof(leaf);
    p[50] ^= 0x01;
    OK(mls_adopted_tree_check(&tree, &gc, true));
    CHECK(marmot_leaf_proof_status(leaf, MARMOT_CIPHERSUITE) == MARMOT_LEAF_PROOF_INVALID,
          "tampered proof does not verify");
    p[50] ^= 0x01;

    /* A required MLS capability the leaf does not advertise. */
    size_t np = leaf->proposal_count;
    leaf->proposal_count = 0;
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    leaf->proposal_count = np;

    /* An admin that is no member. */
    GcSpec s;
    gc_spec_from(g_fixture_exts, g_fixture_exts_len, &s);
    uint8_t stranger[33];
    stranger[0] = 0x20;
    memset(stranger + 1, 0x77, 32);
    gc_entry(&s, 0x8003)->data = stranger;
    gc_entry(&s, 0x8003)->len = sizeof(stranger);
    size_t len = 0;
    uint8_t *exts = gc_build(&s, &len);
    MlsAdoptedGroupContext gc2;
    OK(mls_adopted_group_context_parse(exts, len, &gc2));
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc2, true), MARMOT_ERR_VALIDATION);
    /* Only for an epoch the group enters: the founding epoch-0 state lists
     * co-admins who join with the founding Commit. */
    OK(mls_adopted_tree_check(&tree, &gc2, false));
    free(exts);
#undef RESTORE
    free(saved);
    mls_tree_free(&tree);
    free(tree_bytes);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Welcome-join of real MDK v0.11.0 groups
 * ══════════════════════════════════════════════════════════════════════════ */

static void
install_mdk_joiner(Member *x, const AdoptedMdkFixture *f)
{
    size_t kp_len = 0;
    uint8_t *framed = unhex(f->kp_mls_message, &kp_len);
    CHECK(kp_len > 4 && framed[0] == 0 && framed[1] == 1 && framed[2] == 0 && framed[3] == 5,
          "MLSMessage(mls_key_package)");
    install_key_package(x, framed + 4, kp_len - 4, f->init_sk, f->enc_sk, f->sig_seed,
                        f->sig_pub, f->kp_ref);
    free(framed);
}

static MarmotGroupId
fixture_gid(const AdoptedMdkFixture *f)
{
    size_t len = 0;
    uint8_t *id = unhex(f->group_id, &len);
    MarmotGroupId gid = marmot_group_id_new(id, len);
    free(id);
    return gid;
}

static void
test_mdk_welcome_join(void)
{
    const AdoptedMdkFixture *f = &MDK011_ENGINE_DEFAULT;
    Member bob;
    member_init(&bob, "bob");
    install_mdk_joiner(&bob, f);

    /* The MDK rumor: e + relays, base64 MLSMessage, no encoding tag. */
    NostrEvent *rumor = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(rumor, f->rumor_json, NULL) && rumor->kind == 444 &&
          count_tags(rumor, "encoding") == 0 && count_tags(rumor, "e") == 1 &&
          count_tags(rumor, "relays") == 1, "MDK rumor shape");
    nostr_event_free(rumor);

    /* The pending invitation already shows the signed group profile. */
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    OK(marmot_process_welcome(bob.m, wrapper, f->rumor_json, &w));
    uint8_t want_ngid[32];
    unhex_into(f->nostr_group_id, want_ngid, 32);
    CHECK(w->group_name && strcmp(w->group_name, "W24-E engine-default") == 0 &&
          w->group_description && strcmp(w->group_description, "MDK v0.11.0 fixture") == 0 &&
          w->group_admin_count == 1 && w->member_count == 2 &&
          memcmp(w->nostr_group_id, want_ngid, 32) == 0, "signed preview");
    OK(marmot_accept_welcome(bob.m, w));
    marmot_welcome_free(w);

    MarmotGroupId gid = fixture_gid(f);
    MarmotGroupProfile profile = MARMOT_GROUP_PROFILE_LEGACY;
    OK(marmot_get_group_profile(bob.m, &gid, &profile));
    CHECK(profile == MARMOT_GROUP_PROFILE_ADOPTED, "joined an adopted group");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(bob.m, &gid, &g));
    uint8_t ngid[32], creator[32];
    unhex_into(f->nostr_group_id, ngid, 32);
    unhex_into(f->creator_pub, creator, 32);
    CHECK(memcmp(g->nostr_group_id, ngid, 32) == 0, "nostr_group_id from 0x8004");
    CHECK(g->name && strcmp(g->name, "W24-E engine-default") == 0 && g->description &&
          strcmp(g->description, "MDK v0.11.0 fixture") == 0, "profile from 0x8001");
    CHECK(g->admin_count == 1 && memcmp(g->admin_pubkeys[0], creator, 32) == 0,
          "admins from 0x8003");
    CHECK(g->epoch == f->epoch && g->state == MARMOT_GROUP_STATE_ACTIVE, "epoch");
    marmot_group_free(g);
    MarmotGroupRelay *relays = NULL;
    size_t n_relays = 0;
    OK(marmot_get_group_relay_urls(bob.m, &gid, &relays, &n_relays));
    CHECK(n_relays == 2, "relays from 0x8004");
    bool a = false, b = false;
    for (size_t i = 0; i < n_relays; i++) {
        a |= strcmp(relays[i].relay_url, f->relay_a) == 0;
        b |= strcmp(relays[i].relay_url, f->relay_b) == 0;
    }
    CHECK(a && b, "relay urls");
    relays_free(relays, n_relays);
    uint8_t (*members)[32] = NULL;
    size_t n_members = 0;
    OK(marmot_get_group_members(bob.m, &gid, &members, &n_members));
    CHECK(n_members == 2, "two members");
    free(members);

    /* A second copy of the same Welcome does not replace the joined state. */
    EXPECT_ERR(join(&bob, f->rumor_json, NULL), MARMOT_ERR_WELCOME_ALREADY_ACCEPTED);

    marmot_group_id_free(&gid);
    member_free(&bob);
}

static void
test_persist_load_clone(void)
{
    const AdoptedMdkFixture *f = &MDK011_ENGINE_DEFAULT;
    Member bob;
    member_init(&bob, "bob");
    install_mdk_joiner(&bob, f);
    OK(join(&bob, f->rumor_json, NULL));
    MarmotGroupId gid = fixture_gid(f);

    uint8_t *blob = NULL;
    size_t len = 0;
    load_mls(&bob, &gid, &blob, &len);
    /* Version 4: the profile is persisted. */
    CHECK(len > 9 && blob[4] == 0 && blob[5] == 0 && blob[6] == 0 && blob[7] == 4 &&
          blob[len - 1] == 0x01, "serial version 4 + its on-disk adopted byte 0x01");
    MlsGroup g;
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "load");
    CHECK(g.profile == MARMOT_GROUP_PROFILE_ADOPTED && g.extensions_len == g_fixture_exts_len &&
          memcmp(g.extensions_data, g_fixture_exts, g_fixture_exts_len) == 0,
          "GroupContext kept byte for byte");
    /* Clone (what every Commit stage does) and re-save: same state. */
    uint8_t *again = NULL;
    size_t again_len = 0;
    CHECK(mls_group_serialize(&g, &again, &again_len) == 0 && again_len == len &&
          memcmp(again, blob, len) == 0, "stable round trip");
    MlsGroup clone;
    CHECK(mls_group_deserialize(again, again_len, &clone) == 0 &&
          clone.profile == MARMOT_GROUP_PROFILE_ADOPTED, "clone");
    OK(mls_group_profile_check(&clone));
    mls_group_free(&clone);
    free(again);

    /* Tampered state fails closed: no fallback to legacy. */
    uint8_t *t = malloc(len);
    memcpy(t, blob, len);
    t[len - 1] = 0x00; /* v4 with any other profile byte */
    CHECK(mls_group_deserialize(t, len, &clone) != 0, "v4 legacy profile refused");
    memcpy(t, blob, len);
    t[7] = 3; /* an adopted GroupContext in a v3 (legacy) blob */
    CHECK(mls_group_deserialize(t, len - 1, &clone) != 0, "adopted state as v3 refused");
    memcpy(t, blob, len);
    uint8_t *gc_at = find_bytes(t, len, g_fixture_exts, g_fixture_exts_len);
    CHECK(gc_at, "GroupContext in blob");
    gc_at[g_fixture_exts_len - 1] = 0x01; /* lifecycle -> disbanded */
    CHECK(mls_group_deserialize(t, len, &clone) != 0, "disbanded state refused");
    gc_at[g_fixture_exts_len - 1] = 0x00;
    /* A member leaf that lost its proof carrier. */
    size_t sig_off = 0;
    uint8_t joiner_pub[32];
    unhex_into(f->joiner_pub, joiner_pub, 32);
    uint8_t *proof_at = find_bytes(t, len, joiner_pub, 32);
    /* ... 0x80 0x09 (component 0x8009), varint 104 (0x40 0x68), proof ... */
    while (proof_at && (proof_at[-1] != 0x68 || proof_at[-2] != 0x40 || proof_at[-3] != 0x09 ||
                        proof_at[-4] != 0x80)) {
        sig_off = (size_t)(proof_at - t) + 1;
        proof_at = find_bytes(t + sig_off, len - sig_off, joiner_pub, 32);
    }
    CHECK(proof_at, "proof entry of the joiner in blob");
    proof_at[-3] = 0x08; /* 0x8009 -> 0x8008: the leaf has no proof any more */
    CHECK(mls_group_deserialize(t, len, &clone) != 0, "leaf without proof refused");
    free(t);

    /* Through the API: a stored state that no longer validates. */
    t = malloc(len);
    memcpy(t, blob, len);
    t[len - 1] = 0x02;
    OK(bob.m->storage->mls_store(bob.m->storage->ctx, "mls_group", gid.data, gid.len, t, len));
    MarmotGroupProfile profile;
    EXPECT_ERR(marmot_get_group_profile(bob.m, &gid, &profile), MARMOT_ERR_DESERIALIZATION);
    free(t);

    mls_group_free(&g);
    sodium_memzero(blob, len);
    free(blob);
    marmot_group_id_free(&gid);
    member_free(&bob);
}

static void
expect_refused(Member *x, const char *rumor, const MarmotGroupId *gid, MarmotError want)
{
    MarmotWelcome *w = NULL;
    EXPECT_ERR(join(x, rumor, &w), want);
    CHECK(!group_stored(x, gid), "nothing stored");
    MarmotGroupProfile p;
    EXPECT_ERR(marmot_get_group_profile(x->m, gid, &p), MARMOT_ERR_GROUP_NOT_FOUND);
    /* Refused when it arrives: never an invitation that can only fail. */
    CHECK(!w, "no pending Welcome returned");
    MarmotWelcome **pending = NULL;
    size_t n_pending = 0;
    MarmotPagination page = marmot_pagination_default();
    OK(marmot_get_pending_welcomes(x->m, &page, &pending, &n_pending));
    CHECK(n_pending == 0, "refused Welcome listed as pending");
    welcomes_free(pending, n_pending);
    marmot_welcome_free(w);
}

/* A storage whose "kp_priv" lookup fails with a (transient) storage error
 * while g_kp_fault is set. */
static bool g_kp_fault;
static MarmotError (*g_real_mls_load)(void *, const char *, const uint8_t *, size_t,
                                      uint8_t **, size_t *);

static MarmotError
faulty_mls_load(void *ctx, const char *label, const uint8_t *key, size_t key_len,
                uint8_t **out, size_t *out_len)
{
    if (g_kp_fault && strcmp(label, "kp_priv") == 0) {
        *out = NULL;
        *out_len = 0;
        return MARMOT_ERR_STORAGE;
    }
    return g_real_mls_load(ctx, label, key, key_len, out, out_len);
}

static void
test_welcome_transient_storage_error(void)
{
    /* W24 review L3: a busy store while an adopted Welcome arrives (or is
     * accepted) must not fail it for good. */
    const AdoptedMdkFixture *f = &MDK011_ENGINE_DEFAULT;
    Member bob;
    member_init(&bob, "bob");
    install_mdk_joiner(&bob, f);
    g_real_mls_load = bob.m->storage->mls_load;
    bob.m->storage->mls_load = faulty_mls_load;

    /* Arrival: stored as a pending invitation, unchecked. */
    g_kp_fault = true;
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    OK(marmot_process_welcome(bob.m, wrapper, f->rumor_json, &w));
    CHECK(w && w->state == MARMOT_WELCOME_STATE_PENDING, "pending");
    /* Accept while still busy: the error, nothing recorded, still pending. */
    EXPECT_ERR(marmot_accept_welcome(bob.m, w), MARMOT_ERR_STORAGE);
    MarmotWelcome **pending = NULL;
    size_t n_pending = 0;
    MarmotPagination page = marmot_pagination_default();
    OK(marmot_get_pending_welcomes(bob.m, &page, &pending, &n_pending));
    CHECK(n_pending == 1, "still pending after a transient failure");
    welcomes_free(pending, n_pending);
    /* The store recovers: the same invitation is accepted. */
    g_kp_fault = false;
    OK(marmot_accept_welcome(bob.m, w));
    marmot_welcome_free(w);
    MarmotGroupId gid = fixture_gid(f);
    CHECK(group_stored(&bob, &gid), "joined");
    marmot_group_id_free(&gid);
    bob.m->storage->mls_load = g_real_mls_load;
    member_free(&bob);
}

static void
test_white_noise_welcome_joined(void)
{
    /* nostrc-qp24.5.2 acceptance: the real MDK v0.11.0 marmot-app-shaped
     * (White Noise) Welcome is joined, and its components are readable
     * from the stored state. */
    const AdoptedMdkFixture *f = &MDK011_WHITE_NOISE_APP;
    Member bob;
    member_init(&bob, "bob");
    install_mdk_joiner(&bob, f);
    MarmotGroupId gid = fixture_gid(f);

    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    OK(marmot_process_welcome(bob.m, wrapper, f->rumor_json, &w));
    CHECK(w && w->state == MARMOT_WELCOME_STATE_PENDING && w->group_name &&
              strcmp(w->group_name, "W24-E white-noise-app") == 0 && w->member_count == 2,
          "listed as an invitation with its signed profile");
    OK(marmot_accept_welcome(bob.m, w));
    marmot_welcome_free(w);

    MarmotGroupProfile profile = MARMOT_GROUP_PROFILE_LEGACY;
    OK(marmot_get_group_profile(bob.m, &gid, &profile));
    CHECK(profile == MARMOT_GROUP_PROFILE_ADOPTED, "adopted");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(bob.m, &gid, &g));
    CHECK(g && g->state == MARMOT_GROUP_STATE_ACTIVE && g->epoch == f->epoch &&
              strcmp(g->name, "W24-E white-noise-app") == 0, "group record");
    marmot_group_free(g);

    /* The read side loads (and so re-validates) the stored MLS state. */
    MarmotGroupComponents c;
    OK(marmot_get_group_components(bob.m, &gid, &c));
    CHECK(c.epoch == f->epoch && strcmp(c.name, "W24-E white-noise-app") == 0 &&
              c.has_agent_text_stream &&
              c.agent_text_stream.required_member_roles == MARMOT_AGENT_STREAM_ROLE_RECEIVE &&
              c.has_media_policy &&
              strcmp(c.media_policy.default_blob_endpoints[0].base_url,
                     "https://blossom.example.com/") == 0 &&
              c.avatar_source == MARMOT_GROUP_AVATAR_NONE,
          "components of the joined White Noise group");
    marmot_group_components_clear(&c);
    MarmotGroupId nope = marmot_group_id_new((const uint8_t *)"no such group", 13);
    EXPECT_ERR(marmot_get_group_components(bob.m, &nope, &c), MARMOT_ERR_GROUP_NOT_FOUND);
    marmot_group_id_free(&nope);

    /* Application messages, both ways, are not this slice's: a White Noise
     * group's first Commit still needs nostrc-qp24.5.1 (slice H). */
    EXPECT_ERR(join(&bob, f->rumor_json, NULL), MARMOT_ERR_WELCOME_ALREADY_ACCEPTED);
    marmot_group_id_free(&gid);
    member_free(&bob);
}

/* Review M1: the one per-component validator slice H's AppDataUpdate path
 * calls agrees with admission on every entry, and accepts the valid 0x8006
 * and 0x800b replacements an MDK admin may commit. */
static void
test_component_state_validator(void)
{
    size_t len = 0;
    const uint8_t *exts = NULL;
    uint8_t *raw = wn_exts(&len, &exts);
    GcSpec base;
    gc_spec_from(exts, len, &base);
    for (size_t i = 0; i < base.n_entries; i++)
        CHECK(mls_adopted_component_state_valid(base.entries[i].id, base.entries[i].data,
                                                base.entries[i].len) == 0,
              "White Noise entry %04x valid", base.entries[i].id);

    /* Valid replacements: another receive-only policy, new endpoints. */
    static const uint8_t agent2[12] = {1, 7, 0, 0, 0x20, 0, 0, 0, 0, 60, 0, 0};
    OK(mls_adopted_component_state_valid(0x8006, agent2, sizeof(agent2)));
    char *kinds[] = {"blossom-v1", "ipfs-v1", NULL};
    MarmotMediaBlobEndpoint eps[] = {{"blossom-v1", "https://a.example/", false},
                                     {"ipfs-v1", "http://b.example:8080/x", false}};
    MarmotGroupMediaPolicy pol = {kinds, 2, eps, 2};
    uint8_t *media = NULL;
    size_t media_len = 0;
    OK(marmot_group_media_policy_encode(&pol, &media, &media_len));
    OK(mls_adopted_component_state_valid(0x800b, media, media_len));
    /* ... and each agrees with the full parse of a GroupContext carrying it. */
    GcSpec s = base;
    gc_entry(&s, 0x8006)->data = agent2;
    gc_entry(&s, 0x800b)->data = media;
    gc_entry(&s, 0x800b)->len = media_len;
    OK(gc_parse_spec(&s));
    free(media);

    /* The documented refusals, the codes admission gives. */
    static const uint8_t agent_send[12] = {3, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t agent_bad[12] = {1, 2, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t media_bad[] = {0x12, 'e', 'n', 'c', 'r', 'y', 'p', 't', 'e', 'd', '-',
                                        'm', 'e', 'd', 'i', 'a', '-', 'v', '1', 0x00, 0x00};
    static const uint8_t retention7[7] = {0}, partial_image[] = {0x01, 0xaa, 0, 0, 0, 0};
    static const uint8_t proof104[104] = {0}, empty_list[] = {0x00}, one[] = {0x01};
    static const uint8_t junk[] = {0xff, 0xfe};
    EXPECT_ERR(mls_adopted_component_state_valid(0x8006, agent_send, 12), MARMOT_ERR_UNSUPPORTED);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8006, agent_bad, 12),
               MARMOT_ERR_EXTENSION_FORMAT);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8006, agent_bad, 11),
               MARMOT_ERR_EXTENSION_FORMAT);
    EXPECT_ERR(mls_adopted_component_state_valid(0x800b, media_bad, sizeof(media_bad)),
               MARMOT_ERR_EXTENSION_FORMAT);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8005, retention7, 7),
               MARMOT_ERR_EXTENSION_FORMAT);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8002, partial_image, sizeof(partial_image)),
               MARMOT_ERR_EXTENSION_FORMAT);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8009, proof104, 104),
               MARMOT_ERR_EXTENSION_FORMAT);
    EXPECT_ERR(mls_adopted_component_state_valid(0x0002, empty_list, 1), MARMOT_ERR_UNSUPPORTED);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8008, empty_list, 1), MARMOT_ERR_VALIDATION);
    EXPECT_ERR(mls_adopted_component_state_valid(0x800c, one, 1), MARMOT_ERR_VALIDATION);
    EXPECT_ERR(mls_adopted_component_state_valid(0x8001, NULL, 0), MARMOT_ERR_EXTENSION_FORMAT);
    OK(mls_adopted_component_state_valid(0x9001, junk, sizeof(junk))); /* opaque */
    free(raw);
}

/* agent-text-stream-quic-v1.md: every member advertises the role
 * capability of each role the group requires (here receive, 0xF2D1). */
static void
test_white_noise_member_roles(void)
{
    size_t tlen = 0, len = 0;
    uint8_t *tree_bytes = unhex(MDK011_WHITE_NOISE_APP.ratchet_tree, &tlen);
    MlsRatchetTree tree;
    memset(&tree, 0, sizeof(tree));
    CHECK(mls_ratchet_tree_deserialize(tree_bytes, tlen, &tree) == 0 && tree.n_leaves == 2,
          "MDK tree");
    const uint8_t *exts = NULL;
    uint8_t *raw = wn_exts(&len, &exts);
    MlsAdoptedGroupContext gc;
    OK(mls_adopted_group_context_parse(exts, len, &gc));
    OK(mls_adopted_tree_check(&tree, &gc, true));
    for (uint32_t i = 0; i < 2; i++) {
        const MlsLeafNode *leaf = &tree.nodes[2 * i].leaf;
        bool receive = false;
        for (size_t k = 0; k < leaf->cap_extension_count; k++)
            receive |= leaf->cap_extensions[k] == 0xF2D1;
        CHECK(receive, "MDK leaf %u advertises the receive role", i);
    }

    /* A member without the receive role: refused (a Welcome, a load, an
     * invitee's KeyPackage leaf), but only where the group requires it. */
    MlsLeafNode *joiner = &tree.nodes[2].leaf;
    uint16_t *saved = joiner->cap_extensions;
    size_t saved_n = joiner->cap_extension_count;
    uint16_t without[8];
    size_t n = 0;
    for (size_t k = 0; k < saved_n && n < 8; k++)
        if (saved[k] != 0xF2D1) without[n++] = saved[k];
    CHECK(n == saved_n - 1, "dropped 0xF2D1");
    joiner->cap_extensions = without;
    joiner->cap_extension_count = n;
    EXPECT_ERR(mls_adopted_tree_check(&tree, &gc, true), MARMOT_ERR_VALIDATION);
    EXPECT_ERR(mls_adopted_leaf_check(joiner, &gc), MARMOT_ERR_VALIDATION);
    MlsAdoptedGroupContext plain;
    OK(mls_adopted_group_context_parse(g_fixture_exts, g_fixture_exts_len, &plain));
    OK(mls_adopted_leaf_check(joiner, &plain)); /* no 0x8006: no role needed */
    joiner->cap_extensions = saved;
    joiner->cap_extension_count = saved_n;
    OK(mls_adopted_tree_check(&tree, &gc, true));

    free(raw);
    mls_tree_free(&tree);
    free(tree_bytes);
}

/* A kind:444 rumor of the adopted binding around raw Welcome bytes. */
static char *
rumor_for(const char *welcome_hex, const uint8_t sender[32])
{
    size_t len = 0;
    uint8_t *w = unhex(welcome_hex, &len);
    size_t b64_len = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(b64_len);
    sodium_bin2base64(b64, b64_len, w, len, sodium_base64_VARIANT_ORIGINAL);
    char sender_hex[65];
    for (int i = 0; i < 32; i++) snprintf(sender_hex + 2 * i, 3, "%02x", sender[i]);
    NostrEvent *ev = nostr_event_new();
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("e", "1111111111111111111111111111111111111111111111111111111111111111", NULL));
    nostr_tags_append(tags, nostr_tag_new("relays", "wss://relay-a.example.com", NULL));
    nostr_event_set_kind(ev, 444);
    nostr_event_set_pubkey(ev, sender_hex);
    nostr_event_set_created_at(ev, 1790000000);
    nostr_event_set_content(ev, b64);
    nostr_event_set_tags(ev, tags);
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    free(b64);
    free(w);
    return json;
}

static void
test_openmls_welcome_negatives(void)
{
    static const struct {
        const char *name, *welcome;
        MarmotError want;
    } cases[] = {
        {"control", OMLS_NEG_WELCOME_Control, MARMOT_OK},
        {"bad proof", OMLS_NEG_WELCOME_BadProof, MARMOT_ERR_KEY_PACKAGE_IDENTITY},
        {"missing proof", OMLS_NEG_WELCOME_MissingProof, MARMOT_ERR_VALIDATION},
        {"mixed 0xf2ee/0x8009", OMLS_NEG_WELCOME_MixedGroup, MARMOT_ERR_VALIDATION},
        {"missing required capability", OMLS_NEG_WELCOME_MissingRequiredCapability,
         MARMOT_ERR_EXTENSION_FORMAT},
        {"non-admin inviter", OMLS_NEG_WELCOME_NonAdminInviter, MARMOT_ERR_ADMIN_ONLY},
    };
    size_t kp_len = 0;
    uint8_t *kp = unhex(OMLS_NEG_JOINER_KP, &kp_len);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Member bob;
        member_init(&bob, "bob");
        install_key_package(&bob, kp, kp_len, OMLS_NEG_INIT_SK, OMLS_NEG_ENC_SK,
                            OMLS_NEG_SIG_SEED, OMLS_NEG_SIG_PUB, NULL);
        uint8_t inviter[32];
        mdk_test_identity("w24e-neg-creator", inviter);
        char *rumor = rumor_for(cases[i].welcome, inviter);
        MarmotWelcome *w = NULL;
        MarmotError err = join(&bob, rumor, &w);
        CHECK(err == cases[i].want, "%s: got %d (%s), want %d", cases[i].name, err,
              marmot_error_string(err), cases[i].want);
        MarmotGroup **groups = NULL;
        size_t n_groups = 0;
        OK(marmot_get_all_groups(bob.m, &groups, &n_groups));
        CHECK(n_groups == (cases[i].want == MARMOT_OK ? 1u : 0u), "%s: groups stored",
              cases[i].name);
        if (n_groups == 1) {
            MarmotGroupProfile p;
            OK(marmot_get_group_profile(bob.m, &groups[0]->mls_group_id, &p));
            CHECK(p == MARMOT_GROUP_PROFILE_ADOPTED, "control is adopted");
        }
        groups_free(groups, n_groups);
        marmot_welcome_free(w);
        free(rumor);
        member_free(&bob);
    }
    free(kp);
}

static void
test_adopted_rumor_shape(void)
{
    /* Without an `encoding` tag a base64 MLSMessage Welcome is the adopted
     * binding: exactly one e and one relays tag. */
    size_t kp_len = 0;
    uint8_t *kp = unhex(OMLS_NEG_JOINER_KP, &kp_len);
    Member bob;
    member_init(&bob, "bob");
    install_key_package(&bob, kp, kp_len, OMLS_NEG_INIT_SK, OMLS_NEG_ENC_SK, OMLS_NEG_SIG_SEED,
                        OMLS_NEG_SIG_PUB, NULL);
    uint8_t inviter[32];
    mdk_test_identity("w24e-neg-creator", inviter);
    char *good = rumor_for(OMLS_NEG_WELCOME_Control, inviter);
    const char *variants[] = {
        "\"tags\":[[\"relays\",\"wss://relay-a.example.com\"]]",           /* no e */
        "\"tags\":[[\"e\",\"11\"],[\"relays\",\"wss://relay-a.example.com\"]]", /* bad id */
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"]]",
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"],[\"relays\",\"https://x.example\"]]",
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"],[\"relays\",\"wss://a.example\",\"wss://a.example\"]]",
        /* Exact cardinality (nostrc-0bdg): never the first match only. */
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"],[\"e\",\"2222222222222222222222222222222222222222222222222222222222222222\"],[\"relays\",\"wss://a.example\"]]",
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"],[\"relays\",\"wss://a.example\"],[\"relays\",\"wss://b.example\"]]",
        "\"tags\":[[\"e\",\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"],[\"relays\",\"wss://a.example\"]]",
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\",\"wss://hint.example\"],[\"relays\",\"wss://a.example\"]]",
        "\"tags\":[[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"],[\"relays\"]]",
    };
    char *tags_at = strstr(good, "\"tags\":");
    char *tags_end = strstr(tags_at, "]]") + 2;
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        size_t n = strlen(good) - (size_t)(tags_end - tags_at) + strlen(variants[i]) + 1;
        char *bad = malloc(n);
        snprintf(bad, n, "%.*s%s%s", (int)(tags_at - good), good, variants[i], tags_end);
        uint8_t wrapper[32];
        randombytes_buf(wrapper, 32);
        MarmotWelcome *w = NULL;
        EXPECT_ERR(marmot_process_welcome(bob.m, wrapper, bad, &w), MARMOT_ERR_VALIDATION);
        free(bad);
    }
    /* W24 review N2: the adopted rumor MUST NOT have a `sig` field. */
    {
        const char *sig = "\"sig\":\"" 
            "00000000000000000000000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000000000000000\",";
        size_t n = strlen(good) + strlen(sig) + 1;
        char *signed_rumor = malloc(n);
        CHECK(good[0] == '{', "rumor is an object");
        snprintf(signed_rumor, n, "{%s%s", sig, good + 1);
        uint8_t wrapper[32];
        randombytes_buf(wrapper, 32);
        MarmotWelcome *w = NULL;
        EXPECT_ERR(marmot_process_welcome(bob.m, wrapper, signed_rumor, &w),
                   MARMOT_ERR_VALIDATION);
        marmot_welcome_free(w);
        free(signed_rumor);
    }
    /* No cleartext preview in the adopted binding: an unauthenticated
     * `name` tag is not shown as the group's name. */
    char *named = malloc(strlen(good) + 64);
    snprintf(named, strlen(good) + 64, "%.*s[\"name\",\"Forged\"],%s", (int)(tags_at - good + 8),
             good, tags_at + 8);
    uint8_t wrapper[32];
    randombytes_buf(wrapper, 32);
    MarmotWelcome *pw = NULL;
    OK(marmot_process_welcome(bob.m, wrapper, named, &pw));
    CHECK(pw && pw->group_name && strcmp(pw->group_name, "W24-E negative Control") == 0,
          "preview from the signed profile, not the forged tag: %s",
          pw && pw->group_name ? pw->group_name : "(null)");
    OK(marmot_decline_welcome(bob.m, pw));
    marmot_welcome_free(pw);
    free(named);
    /* A valid Welcome wrapped by someone else than its inviter. */
    char *forwarded = rumor_for(OMLS_NEG_WELCOME_Control, bob.pk);
    MarmotWelcome *fw = NULL;
    EXPECT_ERR(join(&bob, forwarded, &fw), MARMOT_ERR_AUTHOR_MISMATCH);
    marmot_welcome_free(fw);
    free(forwarded);
    OK(join(&bob, good, NULL));
    free(good);
    member_free(&bob);
    free(kp);
}

/* ══════════════════════════════════════════════════════════════════════════
 * KeyPackages
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_mdk_key_package_event_validates(void)
{
    /* MDK publishes only the private-use components in `app_components`
     * (the leaf also lists 0x0001): libmarmot must accept that event. */
    const AdoptedMdkFixture *f = &MDK011_ENGINE_DEFAULT;
    size_t len = 0;
    uint8_t *framed = unhex(f->kp_mls_message, &len);
    MlsKeyPackage kp;
    CHECK(marmot_mls_message_unframe_key_package(framed, len, &kp) == 0, "unframe");
    int64_t at = (int64_t)kp.leaf_node.lifetime_not_before + 60;
    mls_key_package_clear(&kp);
    free(framed);
    uint8_t owner[32], ref[32], want_owner[32], want_ref[32];
    OK(marmot_validate_key_package_event_json(f->kp_event_json,
                                              MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, at, owner,
                                              ref));
    unhex_into(f->joiner_pub, want_owner, 32);
    unhex_into(f->kp_ref, want_ref, 32);
    CHECK(memcmp(owner, want_owner, 32) == 0 && memcmp(ref, want_ref, 32) == 0, "owner/ref");
    /* The White Noise KeyPackage (more components, SelfRemove) validates as
     * a KeyPackage too: refusing its groups is the group admission's job. */
    framed = unhex(MDK011_WHITE_NOISE_APP.kp_mls_message, &len);
    CHECK(marmot_mls_message_unframe_key_package(framed, len, &kp) == 0, "unframe");
    at = (int64_t)kp.leaf_node.lifetime_not_before + 60;
    mls_key_package_clear(&kp);
    free(framed);
    OK(marmot_validate_key_package_event_json(MDK011_WHITE_NOISE_APP.kp_event_json,
                                              MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, at, NULL,
                                              NULL));
}

/* The adopted KeyPackage producer (still build-gated OFF) advertises
 * exactly what an MDK 0.11 marmot-app (White Noise) creator requires of an
 * invitee (nostrc-qp24.5.2): extensions 0x0006 and the receive role 0xF2D1
 * (not send 0xF2D2 or fanout 0xF2D4), proposals 0x0008 and SelfRemove
 * 0x000a, components 0x8001 0x8003 0x8004 0x8006 0x8009 0x800b 0x800c.
 * MDK's own parser and invite precheck accepting it is the harness case
 * groundhog-mdk011-interop-white-noise-welcome (tests/interop/mdk). */
static void
test_adopted_key_package_white_noise_shape(void)
{
    Member bob;
    member_init(&bob, "bob");
    char *json = adopted_key_package(&bob);
    MlsKeyPackage kp;
    uint8_t owner[32];
    OK(marmot_parse_key_package_event_for_profile(json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, &kp,
                                                  owner));
    const MlsLeafNode *leaf = &kp.leaf_node;
    static const uint16_t want_ext[] = {0x0006, 0xF2D1}, want_prop[] = {0x0008, 0x000a};
    CHECK(leaf->cap_extension_count == 2 &&
              memcmp(leaf->cap_extensions, want_ext, sizeof(want_ext)) == 0 &&
              leaf->proposal_count == 2 &&
              memcmp(leaf->proposals, want_prop, sizeof(want_prop)) == 0,
          "leaf capabilities");
    /* The leaf's app_components entry, byte for byte. */
    const uint8_t *dict = NULL;
    size_t dlen = 0, count = 0;
    CHECK(marmot_extensions_find(leaf->extensions_data, leaf->extensions_len, 0x0006, &dict, &dlen,
                                 &count) == 0 && count == 1, "leaf dictionary");
    MarmotComponentData *e = NULL;
    size_t ne = 0;
    CHECK(marmot_app_data_dict_parse(dict, dlen, &e, &ne) == 0 && ne == 3 &&
              e[0].component_id == 0x0001, "entries");
    static const uint8_t want_list[] = {0x10, 0x00, 0x01, 0x80, 0x01, 0x80, 0x03, 0x80, 0x04,
                                        0x80, 0x06, 0x80, 0x09, 0x80, 0x0b, 0x80, 0x0c};
    CHECK(e[0].len == sizeof(want_list) && memcmp(e[0].data, want_list, sizeof(want_list)) == 0,
          "app_components [0x0001 0x8001 0x8003 0x8004 0x8006 0x8009 0x800b 0x800c]");
    free(e);

    /* The kind:30443 tags say the same. */
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, json, NULL), "event");
    NostrTag *t = tag(ev, "mls_extensions");
    CHECK(t && nostr_tag_size(t) == 3 && strcmp(nostr_tag_get(t, 1), "0x0006") == 0 &&
              strcmp(nostr_tag_get(t, 2), "0xf2d1") == 0, "mls_extensions tag");
    t = tag(ev, "mls_proposals");
    CHECK(t && nostr_tag_size(t) == 3 && strcmp(nostr_tag_get(t, 1), "0x0008") == 0 &&
              strcmp(nostr_tag_get(t, 2), "0x000a") == 0, "mls_proposals tag");
    t = tag(ev, "app_components");
    static const char *want_tag[] = {"0x8001", "0x8003", "0x8004", "0x8006",
                                     "0x8009", "0x800b", "0x800c"};
    CHECK(t && nostr_tag_size(t) == 8, "app_components tag");
    for (size_t i = 0; i < 7; i++)
        CHECK(strcmp(nostr_tag_get(t, i + 1), want_tag[i]) == 0, "app_components[%zu]", i);
    nostr_event_free(ev);

    /* Against the real White Noise group: our invitee leaf passes the
     * member check of its GroupContext (required_capabilities, every
     * required component, the receive role), as MDK's creator would. */
    size_t len = 0;
    const uint8_t *exts = NULL;
    uint8_t *raw = wn_exts(&len, &exts);
    MlsAdoptedGroupContext gc;
    OK(mls_adopted_group_context_parse(exts, len, &gc));
    OK(mls_adopted_leaf_check(leaf, &gc));
    /* and covers everything the White Noise KeyPackage MDK made advertises
     * that a White Noise group requires. */
    for (size_t i = 0; i < gc.n_proposal_types; i++) {
        bool has = false;
        for (size_t k = 0; k < leaf->proposal_count; k++) has |= leaf->proposals[k] == gc.proposal_types[i];
        CHECK(has || gc.proposal_types[i] <= 0x0007, "proposal %04x", gc.proposal_types[i]);
    }
    free(raw);
    mls_key_package_clear(&kp);
    free(json);
    member_free(&bob);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Creation (qp24.5.1.1)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    Member         alice, bob;
    MarmotGroupId  gid;
    uint8_t        nostr_gid[32];
} Pair;

static MarmotGroupConfig
config_for(const uint8_t (*admins)[32], size_t n_admins, const char **relays, size_t n_relays)
{
    MarmotGroupConfig c;
    memset(&c, 0, sizeof(c));
    c.name = "Adopted";
    c.description = "made by libmarmot";
    c.admin_pubkeys = (uint8_t (*)[32])admins;
    c.admin_count = n_admins;
    c.relay_urls = (char **)relays;
    c.relay_count = n_relays;
    return c;
}

static void
pair_create_ex(Pair *p, bool bob_admin)
{
    member_init(&p->alice, "alice");
    member_init(&p->bob, "bob");
    char *bob_kp = adopted_key_package(&p->bob);
    /* Unsorted, with a repeat: the routing state is canonical. */
    const char *relays[] = {"wss://relay-b.example.com", "wss://relay-a.example.com",
                            "wss://relay-b.example.com"};
    uint8_t admins[1][32];
    memcpy(admins[0], p->bob.pk, 32); /* a co-admin who is an invitee */
    MarmotGroupConfig cfg =
        config_for((const uint8_t (*)[32])admins, bob_admin ? 1 : 0, relays, 3);
    Signer signer = {.sk_hex = p->alice.sk_hex};
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    const char *kps[] = {bob_kp};
    OK(marmot_create_group_for_profile(p->alice.m, MARMOT_GROUP_PROFILE_ADOPTED, p->alice.pk,
                                       NULL, sign_cb, &signer, kps, 1, &cfg, &r));
    CHECK(signer.calls == 1, "account proof signed once through the callback");
    CHECK(r.welcome_count == 1 && r.welcome_rumor_jsons && r.welcome_rumor_jsons[0], "welcome");

    /* The adopted rumor shape (transports/nostr.md). */
    NostrEvent *rumor = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(rumor, r.welcome_rumor_jsons[0], NULL), "rumor");
    CHECK(rumor->kind == 444 && !rumor->sig && nostr_tags_size(rumor->tags) == 2 &&
          count_tags(rumor, "e") == 1 && count_tags(rumor, "relays") == 1, "e + relays only");
    NostrTag *rt = tag(rumor, "relays");
    CHECK(nostr_tag_size(rt) == 3 &&
          strcmp(nostr_tag_get(rt, 1), "wss://relay-a.example.com") == 0 &&
          strcmp(nostr_tag_get(rt, 2), "wss://relay-b.example.com") == 0,
          "relays sorted and unique");
    size_t clen = strlen(rumor->content), blen = 0;
    uint8_t *raw = malloc(clen);
    CHECK(sodium_base642bin(raw, clen, rumor->content, clen, NULL, &blen, NULL,
                            sodium_base64_VARIANT_ORIGINAL) == 0 &&
          blen > 4 && raw[0] == 0 && raw[1] == 1 && raw[2] == 0 && raw[3] == 3,
          "base64 MLSMessage(mls_welcome)");
    free(raw);
    nostr_event_free(rumor);

    p->gid = marmot_group_id_new(r.group->mls_group_id.data, r.group->mls_group_id.len);
    memcpy(p->nostr_gid, r.group->nostr_group_id, 32);
    CHECK(r.group->admin_count == (bob_admin ? 2u : 1u), "creator (+ co-admin)");
    OK(marmot_merge_pending_commit(p->alice.m, &p->gid));
    OK(join(&p->bob, r.welcome_rumor_jsons[0], NULL));
    marmot_create_group_result_free(&r);
    free(bob_kp);
}

static void
pair_create(Pair *p)
{
    pair_create_ex(p, true);
}

static void
pair_free(Pair *p)
{
    marmot_group_id_free(&p->gid);
    member_free(&p->alice);
    member_free(&p->bob);
}

static void
test_create_adopted_and_join(void)
{
    Pair p;
    pair_create(&p);
    Member *both[2] = {&p.alice, &p.bob};
    for (int i = 0; i < 2; i++) {
        MarmotGroupProfile profile;
        OK(marmot_get_group_profile(both[i]->m, &p.gid, &profile));
        CHECK(profile == MARMOT_GROUP_PROFILE_ADOPTED, "%s: adopted", both[i]->name);
        MarmotGroup *g = NULL;
        OK(marmot_get_group(both[i]->m, &p.gid, &g));
        CHECK(memcmp(g->nostr_group_id, p.nostr_gid, 32) == 0 && g->name &&
              strcmp(g->name, "Adopted") == 0 && g->description &&
              strcmp(g->description, "made by libmarmot") == 0 && g->admin_count == 2 &&
              g->epoch == 1, "%s: group record", both[i]->name);
        marmot_group_free(g);
        uint8_t *blob = NULL;
        size_t len = 0;
        load_mls(both[i], &p.gid, &blob, &len);
        MlsGroup mls;
        CHECK(mls_group_deserialize(blob, len, &mls) == 0, "load");
        OK(marmot_adopted_members_proven(&mls));
        /* A libmarmot group requires what an MDK cgka-engine creator does;
         * its leaves advertise the White Noise set too (nostrc-qp24.5.2). */
        MlsAdoptedGroupContext gc;
        OK(mls_adopted_group_context_parse(mls.extensions_data, mls.extensions_len, &gc));
        static const uint16_t five[] = {0x8001, 0x8003, 0x8004, 0x8009, 0x800c};
        CHECK(gc.n_components == 5 && memcmp(gc.components, five, sizeof(five)) == 0 &&
                  !gc.agent_stream && !gc.media_policy && gc.n_proposal_types == 1,
              "%s: required components", both[i]->name);
        for (uint32_t l = 0; l < mls.tree.n_leaves; l++) {
            const MlsLeafNode *leaf = &mls.tree.nodes[2 * l].leaf;
            CHECK(leaf->cap_extension_count == 2 && leaf->cap_extensions[1] == 0xF2D1 &&
                      leaf->proposal_count == 2 && leaf->proposals[1] == 0x000a,
                  "%s: leaf %u advertises the receive role and SelfRemove", both[i]->name, l);
        }
        MarmotGroupComponents c;
        OK(marmot_get_group_components(both[i]->m, &p.gid, &c));
        CHECK(strcmp(c.name, "Adopted") == 0 && !c.has_media_policy && !c.has_agent_text_stream &&
                  c.required_component_count == 5, "%s: components", both[i]->name);
        marmot_group_components_clear(&c);
        mls_group_free(&mls);
        sodium_memzero(blob, len);
        free(blob);
    }

    /* Application messages flow both ways in the adopted group. */
    for (int s = 0; s < 2; s++) {
        char inner[160];
        snprintf(inner, sizeof(inner),
                 "{\"kind\":9,\"content\":\"hello from %s\",\"created_at\":1700000000,\"tags\":[]}",
                 both[s]->name);
        MarmotOutgoingMessage out;
        memset(&out, 0, sizeof(out));
        OK(marmot_create_message(both[s]->m, &p.gid, inner, &out));
        MarmotMessageResult in;
        memset(&in, 0, sizeof(in));
        Member *r = both[1 - s];
        MarmotError err = marmot_process_message(r->m, out.event_json, &in);
        CHECK(err == MARMOT_OK && in.type == MARMOT_RESULT_APPLICATION_MESSAGE &&
              in.app_msg.inner_event_json && strstr(in.app_msg.inner_event_json, both[s]->name),
              "%s cannot read %s: %d", r->name, both[s]->name, err);
        marmot_message_result_free(&in);
        marmot_outgoing_message_free(&out);
    }
    pair_free(&p);
}

static void
test_create_proof_inputs(void)
{
    Member alice, bob, carol;
    member_init(&alice, "alice");
    member_init(&bob, "bob");
    member_init(&carol, "carol");
    char *bob_kp = adopted_key_package(&bob);
    const char *kps[] = {bob_kp};
    const char *relays[] = {"wss://relay.example.com"};
    MarmotGroupConfig cfg = config_for(NULL, 0, relays, 1);
    MarmotCreateGroupResult r;
    MarmotGroup **groups = NULL;
    size_t n = 0;

    /* No secret key, no signer, no enrollment: no proof, nothing created. */
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               NULL, NULL, NULL, kps, 1, &cfg, &r),
               MARMOT_ERR_KEY_PACKAGE_IDENTITY);
    /* A signer that refuses. */
    Signer refuse = {.sk_hex = alice.sk_hex, .refuse = true};
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               NULL, sign_cb, &refuse, kps, 1, &cfg, &r),
               MARMOT_ERR_CRYPTO);
    /* A signer answering with another account's signature. */
    Signer wrong = {.sk_hex = carol.sk_hex};
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               NULL, sign_cb, &wrong, kps, 1, &cfg, &r),
               MARMOT_ERR_VALIDATION);
    OK(marmot_get_all_groups(alice.m, &groups, &n));
    CHECK(n == 0, "nothing created");
    groups_free(groups, n);

    /* Legacy profile takes no signing input. */
    Signer ok_signer = {.sk_hex = alice.sk_hex};
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_LEGACY, alice.pk,
                                               NULL, sign_cb, &ok_signer, kps, 1, &cfg, &r),
               MARMOT_ERR_INVALID_ARG);
    /* Config: relays required and valid; admins must be members; profile bounds. */
    MarmotGroupConfig bad = config_for(NULL, 0, NULL, 0);
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               alice.sk, NULL, NULL, kps, 1, &bad, &r),
               MARMOT_ERR_INVALID_ARG);
    const char *https[] = {"https://relay.example.com"};
    bad = config_for(NULL, 0, https, 1);
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               alice.sk, NULL, NULL, kps, 1, &bad, &r),
               MARMOT_ERR_INVALID_ARG);
    /* W24 review N1: hosts MDK's url::Url refuses are refused at create. */
    static const char *mdk_refuses[] = {"wss://[zzz]", "wss://ex<ample.com",
                                        "wss://exa%zzmple.com"};
    for (size_t i = 0; i < sizeof(mdk_refuses) / sizeof(mdk_refuses[0]); i++) {
        bad = config_for(NULL, 0, &mdk_refuses[i], 1);
        EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED,
                                                   alice.pk, alice.sk, NULL, NULL, kps, 1, &bad,
                                                   &r),
                   MARMOT_ERR_INVALID_ARG);
    }
    uint8_t stranger[1][32];
    memcpy(stranger[0], carol.pk, 32);
    bad = config_for((const uint8_t (*)[32])stranger, 1, relays, 1);
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               alice.sk, NULL, NULL, kps, 1, &bad, &r),
               MARMOT_ERR_INVALID_ARG);
    char long_name[300];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    bad = config_for(NULL, 0, relays, 1);
    bad.name = long_name;
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               alice.sk, NULL, NULL, kps, 1, &bad, &r),
               MARMOT_ERR_INVALID_ARG);
    /* An MDK 0.8 (legacy) KeyPackage cannot join an adopted group. */
    MarmotKeyPackageResult legacy;
    memset(&legacy, 0, sizeof(legacy));
    OK(marmot_create_key_package(carol.m, carol.pk, carol.sk, NULL, 0, &legacy));
    const char *legacy_kps[] = {legacy.event_json};
    MarmotError e = marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED,
                                                    alice.pk, alice.sk, NULL, NULL, legacy_kps,
                                                    1, &cfg, &r);
    CHECK(e != MARMOT_OK, "legacy KeyPackage refused: %d", e);
    marmot_key_package_result_free(&legacy);
    OK(marmot_get_all_groups(alice.m, &groups, &n));
    CHECK(n == 0, "nothing created");
    groups_free(groups, n);

    /* With the account key, and with the enrolled proof. */
    memset(&r, 0, sizeof(r));
    OK(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk, alice.sk,
                                       NULL, NULL, NULL, 0, &cfg, &r));
    CHECK(r.welcome_count == 0 && r.group, "solo group");
    marmot_create_group_result_free(&r);
    OK(test_enroll(carol.m, carol.pk, carol.sk));
    memset(&r, 0, sizeof(r));
    OK(marmot_create_group_for_profile(carol.m, MARMOT_GROUP_PROFILE_ADOPTED, carol.pk, NULL,
                                       NULL, NULL, NULL, 0, &cfg, &r));
    MarmotGroupProfile profile;
    OK(marmot_get_group_profile(carol.m, &r.group->mls_group_id, &profile));
    CHECK(profile == MARMOT_GROUP_PROFILE_ADOPTED, "enrolled creator");
    marmot_create_group_result_free(&r);

    free(bob_kp);
    member_free(&alice);
    member_free(&bob);
    member_free(&carol);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Commits in adopted groups (nostrc-qp24.5.1.3; refused until then).  The
 * full coverage is tests/test_adopted_commits.c.
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_adopted_commits_applied(void)
{
    Pair p;
    pair_create(&p);

    /* Our own Commits are made: a rename by the 0x8001 AppDataUpdate (no
     * GroupData), as receivers judge it. */
    char *commit_json = NULL;
    MarmotGroupConfig rename = config_for(NULL, 0, NULL, 0);
    rename.name = "renamed";
    OK(marmot_update_group_metadata(p.alice.m, &p.gid, &rename, &commit_json));
    CHECK(commit_json, "a Commit event");
    bool superseded = false;
    char *pending = NULL;
    OK(marmot_get_pending_commit(p.alice.m, &p.gid, &pending, &superseded));
    CHECK(pending && !superseded, "pending until published");
    free(pending);
    OK(marmot_clear_pending_commit(p.alice.m, &p.gid));   /* not published */
    free(commit_json);
    commit_json = NULL;

    /* A Commit from a member (built at the MLS layer, authenticated): Bob
     * applies it. */
    uint8_t *blob = NULL;
    size_t len = 0;
    load_mls(&p.alice, &p.gid, &blob, &len);
    MlsGroup alice_mls;
    CHECK(mls_group_deserialize(blob, len, &alice_mls) == 0, "alice state");
    sodium_memzero(blob, len);
    free(blob);
    uint8_t exporter[32];
    memcpy(exporter, alice_mls.epoch_secrets.exporter_secret, 32);
    MlsCommitResult cr;
    memset(&cr, 0, sizeof(cr));
    CHECK(mls_group_self_update(&alice_mls, &cr) == 0, "MLS self-update");
    char *event = marmot_commit_build_event(cr.commit_data, cr.commit_len, exporter, p.nostr_gid,
                                            marmot_now());
    CHECK(event, "event");
    MarmotMessageResult res;
    memset(&res, 0, sizeof(res));
    MarmotError err = marmot_process_message(p.bob.m, event, &res);
    CHECK(err == MARMOT_OK && res.type == MARMOT_RESULT_COMMIT,
          "adopted self-update applied: err=%d type=%d", err, res.type);
    marmot_message_result_free(&res);
    MarmotGroup *g = NULL;
    OK(marmot_get_group(p.bob.m, &p.gid, &g));
    CHECK(g->epoch == 2, "bob at epoch 2");
    marmot_group_free(g);
    load_mls(&p.bob, &p.gid, &blob, &len);
    MlsGroup bob_mls;
    CHECK(mls_group_deserialize(blob, len, &bob_mls) == 0 && bob_mls.epoch == 2 &&
          bob_mls.profile == MARMOT_GROUP_PROFILE_ADOPTED, "bob's state advanced");
    /* Directly: marmot_commit_authorize() judges an adopted transition by
     * its components (here none changes: an ordinary self-update shape). */
    MlsGroup post;
    CHECK(mls_group_deserialize(blob, len, &post) == 0, "post");
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    OK(marmot_commit_authorize(&bob_mls, &post, 0, false, &key, &gde));
    CHECK(!key.privileged && !gde, "ordinary, no GroupData");
    mls_group_free(&post);
    mls_group_free(&bob_mls);
    sodium_memzero(blob, len);
    free(blob);
    free(event);
    mls_commit_result_clear(&cr);
    mls_group_free(&alice_mls);
    sodium_memzero(exporter, sizeof(exporter));
    pair_free(&p);
}

/* A Commit built at the MLS layer from `x`'s stored state, sealed as
 * kind:445 with that state's exporter secret: what a modified client sends. */
static char *
mls_remove_event(Member *x, const MarmotGroupId *gid, const uint8_t victim[32],
                 const uint8_t nostr_gid[32])
{
    uint8_t *blob = NULL;
    size_t len = 0;
    load_mls(x, gid, &blob, &len);
    MlsGroup g;
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
    uint32_t leaf = UINT32_MAX;
    for (uint32_t i = 0; i < g.tree.n_leaves; i++) {
        const MlsNode *n = &g.tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type == MLS_NODE_LEAF && n->leaf.credential_identity_len == 32 &&
            memcmp(n->leaf.credential_identity, victim, 32) == 0)
            leaf = i;
    }
    CHECK(leaf != UINT32_MAX, "victim leaf");
    uint8_t exporter[32];
    memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
    MlsCommitResult cr;
    memset(&cr, 0, sizeof(cr));
    CHECK(mls_group_remove_members(&g, &leaf, 1, &cr) == 0, "MLS-layer Remove");
    char *event = marmot_commit_build_event(cr.commit_data, cr.commit_len, exporter, nostr_gid,
                                            marmot_now());
    CHECK(event, "event");
    mls_commit_result_clear(&cr);
    mls_group_free(&g);
    sodium_memzero(exporter, sizeof(exporter));
    return event;
}

/* Carol's group is untouched: active, state kept, no removal recorded. */
static void
expect_untouched(Member *x, const MarmotGroupId *gid, uint64_t epoch)
{
    MarmotGroup *g = NULL;
    OK(marmot_get_group(x->m, gid, &g));
    CHECK(g && g->state == MARMOT_GROUP_STATE_ACTIVE && g->epoch == epoch,
          "%s: group still active at epoch %llu", x->name, (unsigned long long)epoch);
    marmot_group_free(g);
    uint8_t *blob = NULL;
    size_t len = 0;
    MarmotError err = x->m->storage->mls_load(x->m->storage->ctx, "mls_group", gid->data,
                                              gid->len, &blob, &len);
    CHECK(err == MARMOT_OK && blob, "%s: MLS state kept", x->name);
    sodium_memzero(blob, len);
    free(blob);
    bool removed = true;
    OK(marmot_get_group_removal(x->m, gid, &removed, NULL, NULL, NULL));
    CHECK(!removed, "%s: no removal recorded", x->name);
}

static void
test_adopted_removal_judged(void)
{
    /* W24 review H1: a Commit that removes OUR leaf never reaches
     * marmot_commit_authorize(); it was judged by the MIP-01 removal rules,
     * under which a group without GroupData made everyone an admin.  Bob, no
     * admin, could evict Carol -- and, with his key sorting first, make the
     * removal final and her keys deleted.  Since nostrc-qp24.5.1.3 an
     * adopted removal is judged by the group's 0x8003 admins. */
    Member alice, bob, carol;
    member_init(&alice, "alice");
    member_init(&bob, "bob");
    while (memcmp(bob.pk, alice.pk, 32) >= 0) {   /* the worst case: final at once */
        member_free(&bob);
        member_init(&bob, "bob");
    }
    member_init(&carol, "carol");
    char *bob_kp = adopted_key_package(&bob), *carol_kp = adopted_key_package(&carol);
    const char *kps[] = {bob_kp, carol_kp};
    const char *relays[] = {"wss://relay.example.com"};
    MarmotGroupConfig cfg = config_for(NULL, 0, relays, 1); /* Alice the only admin */
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                       alice.sk, NULL, NULL, kps, 2, &cfg, &r));
    MarmotGroupId gid = marmot_group_id_new(r.group->mls_group_id.data, r.group->mls_group_id.len);
    uint8_t ngid[32];
    memcpy(ngid, r.group->nostr_group_id, 32);
    CHECK(r.group->admin_count == 1, "one admin");
    OK(join(&bob, r.welcome_rumor_jsons[0], NULL));
    OK(join(&carol, r.welcome_rumor_jsons[1], NULL));
    marmot_create_group_result_free(&r);

    /* Bob (no admin) removes Carol: refused by everyone, nothing kept. */
    char *event = mls_remove_event(&bob, &gid, carol.pk, ngid);
    Member *recv[2] = {&carol, &alice};
    for (int i = 0; i < 2; i++) {
        MarmotMessageResult res;
        memset(&res, 0, sizeof(res));
        EXPECT_ERR(marmot_process_message(recv[i]->m, event, &res),
                   MARMOT_ERR_COMMIT_FROM_NON_ADMIN);
        marmot_message_result_free(&res);
        expect_untouched(recv[i], &gid, 1);
    }
    free(event);
    /* Carol still reads the group. */
    MarmotOutgoingMessage out;
    memset(&out, 0, sizeof(out));
    OK(marmot_create_message(alice.m, &gid,
                             "{\"kind\":9,\"content\":\"still here\",\"created_at\":1700000000,\"tags\":[]}",
                             &out));
    MarmotMessageResult res;
    memset(&res, 0, sizeof(res));
    OK(marmot_process_message(carol.m, out.event_json, &res));
    CHECK(res.type == MARMOT_RESULT_APPLICATION_MESSAGE, "message");
    marmot_message_result_free(&res);
    marmot_outgoing_message_free(&out);

    /* The admin's removal: Carol is told, her copy ends. */
    event = mls_remove_event(&alice, &gid, carol.pk, ngid);
    memset(&res, 0, sizeof(res));
    OK(marmot_process_message(carol.m, event, &res));
    CHECK(res.type == MARMOT_RESULT_COMMIT, "her removal");
    marmot_message_result_free(&res);
    bool removed = false;
    uint8_t remover[32];
    OK(marmot_get_group_removal(carol.m, &gid, &removed, remover, NULL, NULL));
    CHECK(removed && memcmp(remover, alice.pk, 32) == 0, "removed by alice");
    free(event);

    free(bob_kp);
    free(carol_kp);
    marmot_group_id_free(&gid);
    member_free(&alice);
    member_free(&bob);
    member_free(&carol);
}

static void load_group_state(Member *x, const MarmotGroupId *gid, MlsGroup *out);

/* W24 slice B (standalone proposals, SelfRemove) on an adopted group
 * (nostrc-qp24.5.1.3): the only adopted departure is SelfRemove, which every
 * leaf must support -- libmarmot's adopted leaves advertise it since W24
 * slice I (nostrc-qp24.5.2) -- and a Remove request is no adopted proposal. */
static void
test_adopted_departures(void)
{
    Pair p;
    pair_create_ex(&p, false);   /* Bob is not an admin: he can leave */

    /* A Remove request (a PrivateMessage Remove of Bob's own leaf) reaching
     * Alice: refused, not kept. */
    MlsGroup bob_mls;
    load_group_state(&p.bob, &p.gid, &bob_mls);
    uint8_t exporter[32];
    memcpy(exporter, bob_mls.epoch_secrets.exporter_secret, 32);
    uint8_t *msg = NULL;
    size_t msg_len = 0;
    MlsOpenedProposal own;
    CHECK(mls_group_remove_self_proposal(&bob_mls, &msg, &msg_len, &own) == 0,
          "MLS Remove request");
    mls_opened_proposal_clear(&own);
    char *event = marmot_commit_build_event(msg, msg_len, exporter, p.nostr_gid, marmot_now());
    free(msg);
    sodium_memzero(exporter, sizeof(exporter));
    mls_group_free(&bob_mls);
    MarmotMessageResult res;
    memset(&res, 0, sizeof(res));
    EXPECT_ERR(marmot_process_message(p.alice.m, event, &res), MARMOT_ERR_UNSUPPORTED);
    marmot_message_result_free(&res);
    free(event);

    /* Alice, the only admin, cannot leave; Bob can, by SelfRemove (every
     * leaf advertises it). */
    MarmotLeaveKind kind = MARMOT_LEAVE_SELF_REMOVE;
    EXPECT_ERR(marmot_can_self_remove(p.alice.m, &p.gid, &kind), MARMOT_ERR_ADMIN_CANNOT_LEAVE);
    kind = (MarmotLeaveKind)-1;
    OK(marmot_can_self_remove(p.bob.m, &p.gid, &kind));
    CHECK(kind == MARMOT_LEAVE_SELF_REMOVE, "an adopted leave is a SelfRemove");
    char *leave = NULL;
    OK(marmot_self_remove(p.bob.m, &p.gid, &leave));
    CHECK(leave, "leave event");
    bool leaving = false;
    OK(marmot_is_leaving(p.bob.m, &p.gid, &leaving));
    CHECK(leaving, "bob is leaving");

    /* Alice keeps it and, as any member may, commits it; Bob follows his
     * removal. */
    memset(&res, 0, sizeof(res));
    OK(marmot_process_message(p.alice.m, leave, &res));
    CHECK(res.type == MARMOT_RESULT_PROPOSAL && res.proposal.leave, "departure reported");
    marmot_message_result_free(&res);
    free(leave);
    MarmotPendingProposal *props = NULL;
    size_t n_props = 0;
    OK(marmot_get_pending_proposals(p.alice.m, &p.gid, &props, &n_props));
    CHECK(n_props == 1 && props[0].committable, "kept, committable: %zu", n_props);
    marmot_pending_proposals_free(props);
    char *commit = NULL;
    OK(marmot_commit_pending_proposals(p.alice.m, &p.gid, &commit));
    CHECK(commit, "a SelfRemove-only Commit");
    OK(marmot_merge_pending_commit(p.alice.m, &p.gid));
    memset(&res, 0, sizeof(res));
    OK(marmot_process_message(p.bob.m, commit, &res));
    CHECK(res.type == MARMOT_RESULT_COMMIT, "bob's departure committed");
    marmot_message_result_free(&res);
    free(commit);
    bool removed = false;
    OK(marmot_get_group_removal(p.bob.m, &p.gid, &removed, NULL, NULL, NULL));
    CHECK(removed, "bob has left");
    pair_free(&p);
}

static void
test_no_group_data_no_admin(void)
{
    /* W24 review H1: a group without GroupData has no admin -- never
     * "everyone is an admin".  A legacy-profile MLS group without 0xF2EE: an
     * Add is privileged and nobody may commit it. */
    uint8_t id[32], sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN], alice[32], bob[32];
    randombytes_buf(id, sizeof(id));
    randombytes_buf(alice, sizeof(alice));
    randombytes_buf(bob, sizeof(bob));
    crypto_sign_keypair(pk, sk);
    MlsGroup pre;
    CHECK(mls_group_create(&pre, id, 32, alice, 32, sk, NULL, 0) == 0 &&
          pre.profile == MARMOT_GROUP_PROFILE_LEGACY, "legacy group without GroupData");
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    CHECK(mls_key_package_create(&kp, &priv, bob, 32, NULL, 0) == 0, "kp");
    uint8_t *blob = NULL;
    size_t len = 0;
    MlsGroup post;
    CHECK(mls_group_serialize(&pre, &blob, &len) == 0 && mls_group_deserialize(blob, len, &post) == 0,
          "clone");
    sodium_memzero(blob, len);
    free(blob);
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    CHECK(mls_group_add_member(&post, &kp, &add) == 0, "add");
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    EXPECT_ERR(marmot_commit_authorize(&pre, &post, pre.own_leaf_index, true, &key, &gde),
               MARMOT_ERR_COMMIT_FROM_NON_ADMIN);
    CHECK(!gde, "no GroupData returned");
    /* Nor is a member's Remove request committable there (slice B's
     * proposal selection agrees with marmot_commit_authorize()). */
    MarmotStoredProposal req;
    memset(&req, 0, sizeof(req));
    req.epoch = post.epoch;
    req.sender_leaf = 1;
    req.type = MLS_PROPOSAL_REMOVE;
    req.target_leaf = 1;
    memcpy(req.sender, bob, 32);
    CHECK(!marmot_proposal_committable(&post, &req), "no admin commits a Remove request");
    mls_add_result_clear(&add);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
    mls_group_free(&pre);
    mls_group_free(&post);
    sodium_memzero(sk, sizeof(sk));
}

/* ══════════════════════════════════════════════════════════════════════════
 * The MLS-layer guards, each on its own (W24 review L2)
 * ══════════════════════════════════════════════════════════════════════════ */

static char *
to_hex(const uint8_t *b, size_t n)
{
    char *h = malloc(2 * n + 1);
    sodium_bin2hex(h, 2 * n + 1, b, n);
    return h;
}

static void
load_group_state(Member *x, const MarmotGroupId *gid, MlsGroup *out)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    load_mls(x, gid, &blob, &len);
    CHECK(mls_group_deserialize(blob, len, out) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
}

static void
test_commit_processor_profile_check(void)
{
    /* Real OpenMLS Commits in an adopted group, from the joined epoch, each
     * adding a third member.  The MLS-layer processor applies the valid one
     * and refuses -- by the entered-epoch profile check alone -- one whose
     * new leaf carries no proof or does not advertise a required
     * component; the state it was given stays as it was. */
    size_t kp_len = 0;
    uint8_t *kp = unhex(OMLS_NEG_JOINER_KP, &kp_len);
    Member bob;
    member_init(&bob, "bob");
    install_key_package(&bob, kp, kp_len, OMLS_NEG_INIT_SK, OMLS_NEG_ENC_SK, OMLS_NEG_SIG_SEED,
                        OMLS_NEG_SIG_PUB, NULL);
    uint8_t inviter[32];
    mdk_test_identity("w24e-neg-creator", inviter);
    char *rumor = rumor_for(OMLS_COMMITS_WELCOME, inviter);
    OK(join(&bob, rumor, NULL));
    MarmotGroup **groups = NULL;
    size_t n = 0;
    OK(marmot_get_all_groups(bob.m, &groups, &n));
    CHECK(n == 1, "joined");
    MarmotGroupId gid = marmot_group_id_new(groups[0]->mls_group_id.data,
                                            groups[0]->mls_group_id.len);
    groups_free(groups, n);
    static const struct { const char *name, *commit; int want; } cases[] = {
        {"valid add", OMLS_COMMIT_ADD_VALID, 0},
        {"add without proof", OMLS_COMMIT_ADD_WITHOUT_PROOF, MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"add missing a required component", OMLS_COMMIT_ADD_MISSING_COMPONENT,
         MARMOT_ERR_MLS_PROCESS_MESSAGE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MlsGroup g;
        load_group_state(&bob, &gid, &g);
        uint64_t epoch = g.epoch;
        size_t clen = 0;
        uint8_t *commit = unhex(cases[i].commit, &clen);
        int rc = mls_group_process_commit(&g, commit, clen, 0 /* the creator */);
        CHECK(rc == cases[i].want, "%s: rc %d, want %d", cases[i].name, rc, cases[i].want);
        if (rc == 0)
            CHECK(g.epoch == epoch + 1 && g.profile == MARMOT_GROUP_PROFILE_ADOPTED,
                  "%s: applied", cases[i].name);
        else
            CHECK(g.epoch == epoch, "%s: state unchanged", cases[i].name);
        free(commit);
        mls_group_free(&g);
    }
    marmot_group_id_free(&gid);
    free(rumor);
    free(kp);
    member_free(&bob);
}

static void
test_install_checked(void)
{
    /* A local producer cannot enter an epoch that breaks the profile: an
     * Add of a KeyPackage whose leaf has the adopted capabilities (so the
     * RFC 9420 checks pass) but no account-proof carrier is refused by the
     * checked install, and the group is unchanged. */
    Pair p;
    pair_create(&p);
    MlsGroup g;
    load_group_state(&p.alice, &p.gid, &g);
    uint64_t epoch = g.epoch;
    uint8_t identity[32];
    randombytes_buf(identity, 32);
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    CHECK(mls_key_package_create(&kp, &priv, identity, 32, NULL, 0) == 0 &&
              mls_leaf_node_set_adopted_capabilities(&kp.leaf_node) == 0 &&
              mls_key_package_sign(&kp, &priv) == 0,
          "proofless adopted-capability KeyPackage");
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    EXPECT_ERR(mls_group_add_member(&g, &kp, &add), MARMOT_ERR_VALIDATION);
    CHECK(g.epoch == epoch && !add.commit_data && !add.welcome_data, "unchanged, no output");
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
    mls_group_free(&g);
    pair_free(&p);
}

static void
test_welcome_tampered_leaf(void)
{
    /* A GroupInfo signer serves a member leaf altered after that member
     * signed it (here Bob's HPKE key): tree hash, parent hashes, GroupInfo
     * signature and Bob's account proof all still verify -- only the
     * per-leaf signature check (RFC 9420 12.4.3.1) refuses it. */
    Pair p;
    pair_create(&p);
    Member dave;
    member_init(&dave, "dave");
    char *dave_json = adopted_key_package(&dave);
    MlsKeyPackage dave_kp;
    uint8_t dave_pk[32];
    OK(marmot_parse_key_package_event_for_profile(dave_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                  0, &dave_kp, dave_pk));
    MlsGroup g;
    load_group_state(&p.alice, &p.gid, &g);
    bool tampered = false;
    for (uint32_t i = 0; i < g.tree.n_leaves; i++) {
        MlsNode *n = &g.tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type == MLS_NODE_LEAF && memcmp(n->leaf.credential_identity, p.bob.pk, 32) == 0) {
            n->leaf.encryption_key[0] ^= 0x01;
            tampered = true;
        }
    }
    CHECK(tampered, "bob's leaf");
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    OK((MarmotError)mls_group_add_member(&g, &dave_kp, &add));
    char *hex = to_hex(add.welcome_data, add.welcome_len);
    char *rumor = rumor_for(hex, p.alice.pk);
    EXPECT_ERR(join(&dave, rumor, NULL), MARMOT_ERR_MLS);
    MarmotGroup **groups = NULL;
    size_t n = 0;
    OK(marmot_get_all_groups(dave.m, &groups, &n));
    CHECK(n == 0, "nothing joined");
    groups_free(groups, n);
    free(rumor);
    free(hex);
    mls_add_result_clear(&add);
    mls_group_free(&g);
    mls_key_package_clear(&dave_kp);
    free(dave_json);
    member_free(&dave);
    pair_free(&p);
}

static void
test_create_wrong_enrolled_proof(void)
{
    /* An enrolled proof over another MLS signature key (a corrupted or
     * mismatched enrollment): creation verifies every leaf before anything
     * leaves and refuses; nothing is stored. */
    Member alice, bob;
    member_init(&alice, "alice");
    member_init(&bob, "bob");
    char *bob_kp = adopted_key_package(&bob);
    uint8_t other_pk[MLS_SIG_PK_LEN], other_sk[MLS_SIG_SK_LEN], proof[MARMOT_ACCOUNT_PROOF_LEN];
    crypto_sign_keypair(other_pk, other_sk);
    OK(marmot_account_proof_create(alice.pk, alice.sk, NULL, NULL, MARMOT_CIPHERSUITE,
                                   MARMOT_SIGNATURE_SCHEME_ED25519, other_pk, MLS_SIG_PK_LEN,
                                   1790000000, proof));
    memcpy(alice.m->account_proof_owner, alice.pk, 32);
    memcpy(alice.m->account_proof, proof, sizeof(proof));
    alice.m->account_proof_ready = true;
    const char *kps[] = {bob_kp};
    const char *relays[] = {"wss://relay.example.com"};
    MarmotGroupConfig cfg = config_for(NULL, 0, relays, 1);
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    EXPECT_ERR(marmot_create_group_for_profile(alice.m, MARMOT_GROUP_PROFILE_ADOPTED, alice.pk,
                                               NULL, NULL, NULL, kps, 1, &cfg, &r),
               MARMOT_ERR_KEY_PACKAGE_IDENTITY);
    CHECK(!r.group && !r.welcome_rumor_jsons, "no result");
    MarmotGroup **groups = NULL;
    size_t n = 0;
    OK(marmot_get_all_groups(alice.m, &groups, &n));
    CHECK(n == 0, "nothing created");
    groups_free(groups, n);
    sodium_memzero(other_sk, sizeof(other_sk));
    free(bob_kp);
    member_free(&alice);
    member_free(&bob);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Legacy groups keep working unchanged
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_legacy_unchanged(void)
{
    Member alice, bob;
    member_init(&alice, "alice");
    member_init(&bob, "bob");
    OK(test_enroll(alice.m, alice.pk, alice.sk));
    MarmotKeyPackageResult kp;
    memset(&kp, 0, sizeof(kp));
    OK(marmot_create_key_package(bob.m, bob.pk, bob.sk, NULL, 0, &kp));
    const char *relays[] = {"wss://relay.example.com"};
    MarmotGroupConfig cfg = config_for(NULL, 0, relays, 1);
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    const char *kps[] = {kp.event_json};
    OK(marmot_create_group(alice.m, alice.pk, kps, 1, &cfg, &r));
    MarmotGroupId gid = marmot_group_id_new(r.group->mls_group_id.data, r.group->mls_group_id.len);
    OK(join(&bob, r.welcome_rumor_jsons[0], NULL));
    Member *both[2] = {&alice, &bob};
    for (int i = 0; i < 2; i++) {
        MarmotGroupProfile profile = MARMOT_GROUP_PROFILE_ADOPTED;
        OK(marmot_get_group_profile(both[i]->m, &gid, &profile));
        CHECK(profile == MARMOT_GROUP_PROFILE_LEGACY, "legacy");
        uint8_t *blob = NULL;
        size_t len = 0;
        load_mls(both[i], &gid, &blob, &len);
        CHECK(blob[7] == 3, "legacy state still serial version 3");
        sodium_memzero(blob, len);
        free(blob);
        /* Components are an adopted-profile notion (MarmotGroup has the
         * legacy group data). */
        MarmotGroupComponents c;
        EXPECT_ERR(marmot_get_group_components(both[i]->m, &gid, &c), MARMOT_ERR_UNSUPPORTED);
    }
    /* And the legacy rumor still carries its encoding tag and preview. */
    NostrEvent *rumor = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(rumor, r.welcome_rumor_jsons[0], NULL) &&
          count_tags(rumor, "encoding") == 1 && count_tags(rumor, "h") == 1, "legacy rumor");
    nostr_event_free(rumor);
    marmot_create_group_result_free(&r);
    marmot_key_package_result_free(&kp);
    marmot_group_id_free(&gid);
    member_free(&alice);
    member_free(&bob);
}

int
main(int argc, char **argv)
{
    if (sodium_init() < 0) return 1;
    g_only = argc > 1 ? argv[1] : NULL;
    size_t raw_len = 0;
    uint8_t *raw = unhex(MDK011_ENGINE_DEFAULT.group_context, &raw_len);
    g_fixture_exts = strip_vec(raw, raw_len, &g_fixture_exts_len);

    printf("libmarmot: adopted-profile admission (nostrc-qp24.5.1)\n");
    RUN(test_mdk_group_context_admitted);
    RUN(test_white_noise_group_context_admitted);
    RUN(test_white_noise_component_negatives);
    RUN(test_group_context_negatives);
    RUN(test_relay_url_profile);
    RUN(test_mdk_tree_members);
    RUN(test_mdk_welcome_join);
    RUN(test_persist_load_clone);
    RUN(test_white_noise_welcome_joined);
    RUN(test_white_noise_member_roles);
    RUN(test_component_state_validator);
    RUN(test_welcome_transient_storage_error);
    RUN(test_openmls_welcome_negatives);
    RUN(test_adopted_rumor_shape);
    RUN(test_mdk_key_package_event_validates);
    RUN(test_adopted_key_package_white_noise_shape);
    RUN(test_create_adopted_and_join);
    RUN(test_create_proof_inputs);
    RUN(test_adopted_commits_applied);
    RUN(test_adopted_removal_judged);
    RUN(test_adopted_departures);
    RUN(test_no_group_data_no_admin);
    RUN(test_commit_processor_profile_check);
    RUN(test_install_checked);
    RUN(test_welcome_tampered_leaf);
    RUN(test_create_wrong_enrolled_proof);
    RUN(test_legacy_unchanged);
    free(raw);
    printf("All adopted admission tests passed.\n");
    return 0;
}
