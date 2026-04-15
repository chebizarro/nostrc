/*
 * libmarmot - MLS Key Schedule RFC 9420 test vector validation
 *
 * Validates the key schedule implementation against the official
 * RFC 9420 Appendix D test vectors (cipher suite 0x0001:
 * MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519).
 *
 * Test vectors from: libmarmot/tests/vectors/mdk/key-schedule.json
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_key_schedule.h"
#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Hex decoding utility
 * ══════════════════════════════════════════════════════════════════════════ */

static int
hex_char_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/** Decode hex string into bytes. Returns decoded length. */
static size_t
hex_decode(const char *hex, uint8_t *out, size_t out_max)
{
    size_t hex_len = strlen(hex);
    size_t byte_len = hex_len / 2;
    assert(byte_len <= out_max);
    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_char_val(hex[2*i]);
        int lo = hex_char_val(hex[2*i + 1]);
        assert(hi >= 0 && lo >= 0);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return byte_len;
}

/** Decode hex string into a malloc'd buffer. Caller frees. */
static uint8_t *
hex_decode_alloc(const char *hex, size_t *out_len)
{
    size_t hex_len = strlen(hex);
    size_t byte_len = hex_len / 2;
    uint8_t *buf = malloc(byte_len);
    assert(buf);
    hex_decode(hex, buf, byte_len);
    *out_len = byte_len;
    return buf;
}

#define HEXDEC(name, hex_str) \
    uint8_t name[MLS_HASH_LEN]; \
    do { size_t _n = hex_decode(hex_str, name, sizeof(name)); (void)_n; assert(_n == MLS_HASH_LEN); } while(0)

/** Compare a byte array against a hex string. Returns 0 on match. */
static int
assert_hex_eq(const char *label, const uint8_t *actual, size_t actual_len, const char *expected_hex)
{
    size_t expected_len = strlen(expected_hex) / 2;
    if (actual_len != expected_len) {
        fprintf(stderr, "FAIL %s: length mismatch (got %zu, expected %zu)\n",
                label, actual_len, expected_len);
        return -1;
    }
    uint8_t expected[256];
    assert(expected_len <= sizeof(expected));
    hex_decode(expected_hex, expected, sizeof(expected));
    if (memcmp(actual, expected, expected_len) != 0) {
        fprintf(stderr, "FAIL %s: value mismatch\n  got:      ", label);
        for (size_t i = 0; i < expected_len; i++) fprintf(stderr, "%02x", actual[i]);
        fprintf(stderr, "\n  expected: %s\n", expected_hex);
        return -1;
    }
    return 0;
}

#define ASSERT_HEX(label, actual, len, hex) \
    assert(assert_hex_eq(label, actual, len, hex) == 0)

/* ══════════════════════════════════════════════════════════════════════════
 * RFC 9420 Appendix D test vectors (cipher suite 1, 5 epochs)
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *INITIAL_INIT_SECRET =
    "a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e";

typedef struct {
    const char *commit_secret;
    const char *psk_secret;
    const char *group_context;
    const char *joiner_secret;
    const char *welcome_secret;
    const char *sender_data_secret;
    const char *encryption_secret;
    const char *exporter_secret;
    const char *external_secret;
    const char *confirmation_key;
    const char *membership_key;
    const char *resumption_psk;
    const char *epoch_authenticator;
    const char *init_secret;
    /* Exporter test */
    const char *exporter_label;
    const char *exporter_context;
    int         exporter_length;
    const char *exporter_output;
} EpochVector;

static const EpochVector EPOCH_VECTORS[5] = {
    /* Epoch 0 */
    {
        .commit_secret        = "a22606222e350fd7f0937168fe7548fb06626ab143cba7611d641693b1447509",
        .psk_secret           = "e871b247379522395689182736cb3d1e7b108d6ae934b802223975de8dc3f80b",
        .group_context        = "0001000120a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e0000000000000000209769e302a99c457350a8e636009b12a2fee068664004606d6318eb3a1977d818205e57c9364dc71f0f71b19ffe561ab77257c490708a47e29f8f73f2b318201d2f00",
        .joiner_secret        = "4fb996ba26b29a70f3ce6c310151ce8701cb812d027f4d4bbf5cc4e9f884638d",
        .welcome_secret       = "ddcd9ced2d264798f876cbd00a200cdc4d77311dfef96975257efb66b0ef2c4d",
        .sender_data_secret   = "9b3995e08589548b75e149190060cf35228df0eefe3527ea2fb39e49a84125b4",
        .encryption_secret    = "01588615c93d02c83bda0b587473303b1637a92bf80783206d963f9197c40a13",
        .exporter_secret      = "5a097e149f2a375d0b9e1d1f4dc3a9c6c1788df888e5441f41a8791f4dc56cea",
        .external_secret      = "b5cb5666cfb9c501ed76715c6ed1cafbed5061cd6b86898ae5d3fd4cb05abb26",
        .confirmation_key     = "feabd690de3b4ce985a3dfad86a4c4e6a0be9b84e7cc764842784f2a6b938b75",
        .membership_key       = "970744ba7edd21700a3e106cb4e2b4c657cef6b41a1fe5b5a1418f86e76e037e",
        .resumption_psk       = "d78ca815e192823f5c7c94b0156bdc7af4791cfb3f240fff613c0c03c01dabd5",
        .epoch_authenticator  = "7375d449cde2c5a856c13c8eb52c16bf9ef29eceef59b09d1f946bd1bac24643",
        .init_secret          = "505be2ce2ff922aa11e0a03d76346dda2981f1d9edf5cf98ecfc8757f69b00c9",
        .exporter_label       = "9ba13d54ecdec7cbefcb47b4268d7b1990fabc6d6e67681e167959389d84e4e4",
        .exporter_context     = "884f1af892ab002f5be4c5d5081ade9e0e6418c6ea7a9a92e90534f19dcef785",
        .exporter_length      = 32,
        .exporter_output      = "dbce4e25e59ab4dfa6f6200f113ed08393cf6e7286d024811141c6a4dd11c0cb",
    },
    /* Epoch 1 */
    {
        .commit_secret        = "7b3027aa5d2224aab7e2a18660bbf57930e2e21d95e02b849c704d970e3e28c5",
        .psk_secret           = "ca7a68f2a8a52147d70f1eb7195de968d2e182b93596bc5a61393861e91180e4",
        .group_context        = "0001000120a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e000000000000000120826a4d3b0956277ce5e272e4d18fdca023ffb63ea4cea636e34cc837ae7c5c5d2014a2985ea47db0685924a74d47ac8a08ec241f843b536dd1348e3ffb2d78184e00",
        .joiner_secret        = "7ba2c5eed466d6fa8de0b0f33553c7b336a2580c03820e79f22e9416efc5b9f9",
        .welcome_secret       = "161b3d492aed45f9d9cde3ad8c4e6c936ca1c104977b7668727a014374afe489",
        .sender_data_secret   = "64035464638ac7cf16583644e8117a84ca3c101eaa34a86ad4ead9524f8fb9bc",
        .encryption_secret    = "c607312fab6423cd728a25fd91e9905e058518d1bf171984ed5f4e4e057fa3be",
        .exporter_secret      = "047d983048b132b79ea4e2e578afd02a0f4717d166cefe46e43e2e965b5c9f4e",
        .external_secret      = "19a4869847b2637e3700599304be769023c729334462919c395481f6ef174010",
        .confirmation_key     = "04ab9a788afe377d34b0fac1dc26085c85ac55a63e44b88da39fb4e58a898979",
        .membership_key       = "1eb0202e445ebc744c00eb42951ad67638c51cc9a468d9035be06612ff5cd89a",
        .resumption_psk       = "3b83961159a16b5d7c5f9a52344edfeea5ea92cf7b2fb069a53eedb90fe51de6",
        .epoch_authenticator  = "4bdbe62402b3caaadaf5c6fafd89db4db5ac7c7532f3e47d35c82b3998570361",
        .init_secret          = "88586b2252f06838106a97f5ad1f3357d99d718be8f44f61ab103be653fc608a",
        .exporter_label       = "ed66d7f1da52171ac9448f0f902edcfefa4ebbda843a43bd3d173cb7c5b4331e",
        .exporter_context     = "02dc18fc5bc4d9093cf41fa0053521653775b123784d40ac7d46cc5a72ef4d46",
        .exporter_length      = 32,
        .exporter_output      = "a702c3e70a89c06eae51aec3675918a4ee7698ae88c596dfb7abd1ed3a9ecf5e",
    },
    /* Epoch 2 */
    {
        .commit_secret        = "d2825785628f1ea7404d6761f27272af5f99416ea28cc9d335df47ed2b0097d4",
        .psk_secret           = "599aa672406270914c60d30b7a31d2f2e217c3b5298b279b79e34c65a60e5f24",
        .group_context        = "0001000120a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e000000000000000220661ce3bd9ebec8608fa97bf5413a4588f50a8f9face225ec67a6d29c862b251620b5d7ec8c9d8b6a28c9467fe4918844be6acc08e98c1e10c71122e95f9a5e78c500",
        .joiner_secret        = "5af09c405a0354e09b03714d5dd5d512cc93aff33979917b00bcf8ff364239f3",
        .welcome_secret       = "89d13b67d2b4f36eaca621454ea345c3a67a76ee2ffb78e153a5243f2677a0a2",
        .sender_data_secret   = "e86474fdaaef42f5bd0913e06dab01ce63ae6c1f7a177e7acf100265b692466d",
        .encryption_secret    = "e5ebb4f0b362e5ecb1753785d79802c2eb30b89033068a46c022fb9591be8c05",
        .exporter_secret      = "69cb23d2ff46b36c466ad48911fd56ee108ab8d4ec1c91028622e9a93c067710",
        .external_secret      = "fc55b63c93d9c8b5596e5c314c7a04f684ae51f45d18571f52c0f3f3c0b3488b",
        .confirmation_key     = "1a1c0cdbc1d8545116cf181c08467f1e8de1c6a4d7d865c73628b3c989625b8f",
        .membership_key       = "8838cc5d52d7828fd87dcc3ca98a45e9169426e71545f4068726679d15d195c7",
        .resumption_psk       = "69ca921c0bc0796ea4d476219ff929413f0936dba5474c76adf4fc466714f84f",
        .epoch_authenticator  = "408990a9228b3303b8cf89979d8698836fed7a4092220f91ec1753d56be14df6",
        .init_secret          = "ca13f80a46a3dff48551759bdeb081fcbf0a0be6fc74e33fc56209b377922a83",
        .exporter_label       = "06f549e9bf966d7b7135b6ef6d4e3032a1c720adf0281c3ef1c0beac0da88621",
        .exporter_context     = "b0fa7e3f0f2199278a55267d551d43946bbfc6d847632867dd86abd1217982a5",
        .exporter_length      = 32,
        .exporter_output      = "07dd60ff9acb0278f80cae4c6f4ee69c745481f5bf967b21d237c18e48d78b4b",
    },
    /* Epoch 3 */
    {
        .commit_secret        = "f652baa9151c9719ecb2716240a2a5ed9aeede1df19de0de862ded166a724783",
        .psk_secret           = "4106e07ffe8f0bfdbfd317d92e37a1fc6c4d1fba53ee054b7acf8587013d533b",
        .group_context        = "0001000120a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e00000000000000032008225ed7f3b0b8aa9f03b24395ed8ee7002d38209fddd7d941dd8ac629ac8a6220dab8c2f8ec97a0e5a137c55a5b9ac1ccdf5ae8329810e98e0bc3930aae0b4be100",
        .joiner_secret        = "65ee9fa09f2d0da5022b9de54fb3753b58471d03106e9d479cd53030a6f58165",
        .welcome_secret       = "e8eefa28cfb25ab8a05dd40ba81f4fac84852b4b56f79dbf55c7da0af08ebfae",
        .sender_data_secret   = "c3a28048c5b6d04872be40bff1f8e3d469250e2b9482fde4684cc40120e1bbc9",
        .encryption_secret    = "735539658afe3bf936f091f430e4a85962c7b48b138e7b3b9a70f1290e5d6ffd",
        .exporter_secret      = "9f2a260a99b352c9c9c90f081050380c9e8b5f400b41461f4e40690a2f111b03",
        .external_secret      = "c6b645ebd6b263616fbf872d9d43b3a51338d4509164d8a3fe4f52c10ac45cea",
        .confirmation_key     = "6345bb395af7657232ebab23196a690e16a90a51d87eeac7789c81935b8c2a30",
        .membership_key       = "99372e5b9b27f332d9f1b1c669619c156eee9d6bb1908d4a7ed89eb5fb511f29",
        .resumption_psk       = "b3ffe5ef8de8e569879d48cd5b64bcaef7127eafc052da4eb25e64e2b951af52",
        .epoch_authenticator  = "4705894cbf2a35f793bdc25045ddad0281dee5fd1836b3ed836c74ae6b23e7fd",
        .init_secret          = "1a034b1838c873d3c536a5f007c2ecb34b6f44ce5fa716902755d6ab64398aad",
        .exporter_label       = "b76193af29eadc6e16c66493e2a4ef8219aad79986b6d4911493741ec1e666cc",
        .exporter_context     = "941db06073e76050679d33cf1f0ec33de3e5a2cd00cb738b54c0dd90de251cdd",
        .exporter_length      = 32,
        .exporter_output      = "d379ee776bbaadb55b2bf74ffb4e53b96307611a115b6a67f165192b6c481bce",
    },
    /* Epoch 4 */
    {
        .commit_secret        = "50fb68cdd31ff76b8d86b88b80124534d845cf2a835411b002b40b7b08d28278",
        .psk_secret           = "c7e0f52b886962b1edde9e75b9ecafa0d8efeffb474732a3da298c470f1d1445",
        .group_context        = "0001000120a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e000000000000000420918e07d9bfd965f880f860830b24427d9200fcac485e973b4943e67d322c1682205e88437e7e8e91582bc440a375e93280417c94c5e38db8537963dd3750dd3e1600",
        .joiner_secret        = "4ace52a8c9dd70e8a71ec54d84a5aa64a5f7229d97f110849e49be6d93b6c65a",
        .welcome_secret       = "d015f8f6ef8b89bf9555a3796d8759ffc500e63b3f1fd7a5b55c319646388109",
        .sender_data_secret   = "981607260c472cc61770cadee57fae7f02b5433246fa57ba64e712d6ed320845",
        .encryption_secret    = "98e6bbb22b1d33ccc8d964da54abb79a6105af028c04799de6103320b42b9717",
        .exporter_secret      = "7e6f8cedec75018d137c1d08444c8695e52fe73efe0650f928007dcbcfa48b2b",
        .external_secret      = "ce214e92ce56cc0ef199984cf563f00e63114933acc529e12525ba72e07fd845",
        .confirmation_key     = "7c46de2314426dfa41f137627860ed9601d13427556bc197f1ae029b17a22ed2",
        .membership_key       = "eeba13b78014db447abd409b1fbbecfbfccd9ccb76ed4a43b47ca2d36b3a32ec",
        .resumption_psk       = "cf1b198338aee226353358a7778c72e6785743516e3b5eff11378357c2db118b",
        .epoch_authenticator  = "c60fd8cebae30f72724eee59569c0a364a7c12e617f91bced41d5615886cc9cf",
        .init_secret          = "005eec47d467da14e21f9bf1e8f79b8ca64c27ce762f94fd0da253749d150384",
        .exporter_label       = "cc9c4b25b0bd69250b6e4f9908d1b170bf62fe8cc11ad36d33e602324ac0662a",
        .exporter_context     = "a0f762fc82d5e421d8fdf8a317b2fd463008c625bf19db7fbfa4ac778b5106a2",
        .exporter_length      = 32,
        .exporter_output      = "f4698636cc032717011a186a14a42cc49e95aeeb4d9bc8ab82295fc1543735ac",
    },
};

/* ══════════════════════════════════════════════════════════════════════════
 * Test: validate a single epoch
 * ══════════════════════════════════════════════════════════════════════════ */

static void
validate_epoch(int epoch_num, const uint8_t *init_secret_prev,
               const EpochVector *vec, uint8_t *init_secret_out)
{
    char label[64];

    /* Decode inputs */
    HEXDEC(commit_secret, vec->commit_secret);
    HEXDEC(psk_secret, vec->psk_secret);

    size_t gc_len = 0;
    uint8_t *gc = hex_decode_alloc(vec->group_context, &gc_len);

    /* Derive epoch secrets */
    MlsEpochSecrets sec;
    int rc = mls_key_schedule_derive(init_secret_prev, commit_secret,
                                      gc, gc_len, psk_secret, &sec);
    assert(rc == 0);

    /* Validate all secrets */
    snprintf(label, sizeof(label), "epoch[%d].joiner_secret", epoch_num);
    ASSERT_HEX(label, sec.joiner_secret, MLS_HASH_LEN, vec->joiner_secret);

    snprintf(label, sizeof(label), "epoch[%d].welcome_secret", epoch_num);
    ASSERT_HEX(label, sec.welcome_secret, MLS_HASH_LEN, vec->welcome_secret);

    snprintf(label, sizeof(label), "epoch[%d].sender_data_secret", epoch_num);
    ASSERT_HEX(label, sec.sender_data_secret, MLS_HASH_LEN, vec->sender_data_secret);

    snprintf(label, sizeof(label), "epoch[%d].encryption_secret", epoch_num);
    ASSERT_HEX(label, sec.encryption_secret, MLS_HASH_LEN, vec->encryption_secret);

    snprintf(label, sizeof(label), "epoch[%d].exporter_secret", epoch_num);
    ASSERT_HEX(label, sec.exporter_secret, MLS_HASH_LEN, vec->exporter_secret);

    snprintf(label, sizeof(label), "epoch[%d].external_secret", epoch_num);
    ASSERT_HEX(label, sec.external_secret, MLS_HASH_LEN, vec->external_secret);

    snprintf(label, sizeof(label), "epoch[%d].confirmation_key", epoch_num);
    ASSERT_HEX(label, sec.confirmation_key, MLS_HASH_LEN, vec->confirmation_key);

    snprintf(label, sizeof(label), "epoch[%d].membership_key", epoch_num);
    ASSERT_HEX(label, sec.membership_key, MLS_HASH_LEN, vec->membership_key);

    snprintf(label, sizeof(label), "epoch[%d].resumption_psk", epoch_num);
    ASSERT_HEX(label, sec.resumption_psk, MLS_HASH_LEN, vec->resumption_psk);

    snprintf(label, sizeof(label), "epoch[%d].epoch_authenticator", epoch_num);
    ASSERT_HEX(label, sec.epoch_authenticator, MLS_HASH_LEN, vec->epoch_authenticator);

    snprintf(label, sizeof(label), "epoch[%d].init_secret", epoch_num);
    ASSERT_HEX(label, sec.init_secret, MLS_HASH_LEN, vec->init_secret);

    /* Copy init_secret for next epoch */
    memcpy(init_secret_out, sec.init_secret, MLS_HASH_LEN);

    free(gc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: validate exporter for a single epoch
 * ══════════════════════════════════════════════════════════════════════════ */

static void
validate_exporter(int epoch_num, const uint8_t *init_secret_prev,
                  const EpochVector *vec)
{
    char label[64];

    /* Re-derive to get exporter_secret */
    HEXDEC(commit_secret, vec->commit_secret);
    HEXDEC(psk_secret, vec->psk_secret);

    size_t gc_len = 0;
    uint8_t *gc = hex_decode_alloc(vec->group_context, &gc_len);

    MlsEpochSecrets sec;
    assert(mls_key_schedule_derive(init_secret_prev, commit_secret,
                                    gc, gc_len, psk_secret, &sec) == 0);

    /* Exporter label is an ASCII string (the hex representation IS the label).
     * Context is decoded hex bytes that get hashed inside the exporter. */
    const char *elabel = vec->exporter_label;
    size_t elabel_len = strlen(elabel);

    size_t ectx_len = 0;
    uint8_t *ectx = hex_decode_alloc(vec->exporter_context, &ectx_len);

    /* Compute MLS-Exporter using the string label API */
    uint8_t exported[64];
    assert((size_t)vec->exporter_length <= sizeof(exported));
    int rc = mls_exporter(sec.exporter_secret, elabel,
                           ectx, ectx_len, exported, (size_t)vec->exporter_length);
    (void)elabel_len;
    assert(rc == 0);

    snprintf(label, sizeof(label), "epoch[%d].exporter", epoch_num);
    ASSERT_HEX(label, exported, (size_t)vec->exporter_length, vec->exporter_output);

    free(gc);
    free(ectx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_epoch_0_secrets(void)
{
    HEXDEC(init, INITIAL_INIT_SECRET);
    uint8_t next_init[MLS_HASH_LEN];
    validate_epoch(0, init, &EPOCH_VECTORS[0], next_init);
}

static void test_epoch_1_secrets(void)
{
    /* Must chain from epoch 0 */
    HEXDEC(init0, INITIAL_INIT_SECRET);
    uint8_t init1[MLS_HASH_LEN], dummy[MLS_HASH_LEN];
    validate_epoch(0, init0, &EPOCH_VECTORS[0], init1);
    validate_epoch(1, init1, &EPOCH_VECTORS[1], dummy);
}

static void test_epoch_2_secrets(void)
{
    HEXDEC(init0, INITIAL_INIT_SECRET);
    uint8_t init[MLS_HASH_LEN], next[MLS_HASH_LEN];
    validate_epoch(0, init0, &EPOCH_VECTORS[0], init);
    validate_epoch(1, init, &EPOCH_VECTORS[1], next);
    memcpy(init, next, MLS_HASH_LEN);
    validate_epoch(2, init, &EPOCH_VECTORS[2], next);
}

static void test_epoch_3_secrets(void)
{
    HEXDEC(init0, INITIAL_INIT_SECRET);
    uint8_t init[MLS_HASH_LEN], next[MLS_HASH_LEN];
    memcpy(init, init0, MLS_HASH_LEN);
    for (int i = 0; i < 4; i++) {
        validate_epoch(i, init, &EPOCH_VECTORS[i], next);
        memcpy(init, next, MLS_HASH_LEN);
    }
}

static void test_epoch_4_secrets(void)
{
    HEXDEC(init0, INITIAL_INIT_SECRET);
    uint8_t init[MLS_HASH_LEN], next[MLS_HASH_LEN];
    memcpy(init, init0, MLS_HASH_LEN);
    for (int i = 0; i < 5; i++) {
        validate_epoch(i, init, &EPOCH_VECTORS[i], next);
        memcpy(init, next, MLS_HASH_LEN);
    }
}

static void test_full_epoch_chain(void)
{
    /* Validate all 5 epochs chained together */
    HEXDEC(init0, INITIAL_INIT_SECRET);
    uint8_t init[MLS_HASH_LEN], next[MLS_HASH_LEN];
    memcpy(init, init0, MLS_HASH_LEN);
    for (int i = 0; i < 5; i++) {
        validate_epoch(i, init, &EPOCH_VECTORS[i], next);
        memcpy(init, next, MLS_HASH_LEN);
    }
    printf("[5 epochs chained] ");
}

static void test_exporter_epoch_0(void)
{
    HEXDEC(init, INITIAL_INIT_SECRET);
    validate_exporter(0, init, &EPOCH_VECTORS[0]);
}

static void test_exporter_all_epochs(void)
{
    HEXDEC(init0, INITIAL_INIT_SECRET);
    uint8_t init[MLS_HASH_LEN], next[MLS_HASH_LEN];
    memcpy(init, init0, MLS_HASH_LEN);
    for (int i = 0; i < 5; i++) {
        validate_exporter(i, init, &EPOCH_VECTORS[i]);
        /* Advance init_secret for next epoch */
        HEXDEC(cs, EPOCH_VECTORS[i].commit_secret);
        HEXDEC(psk, EPOCH_VECTORS[i].psk_secret);
        size_t gc_len = 0;
        uint8_t *gc = hex_decode_alloc(EPOCH_VECTORS[i].group_context, &gc_len);
        MlsEpochSecrets sec;
        assert(mls_key_schedule_derive(init, cs, gc, gc_len, psk, &sec) == 0);
        memcpy(init, sec.init_secret, MLS_HASH_LEN);
        free(gc);
    }
    printf("[5 epochs] ");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: MLS Key Schedule — RFC 9420 test vectors\n");

    printf("\n  --- Epoch secret derivation (cipher suite 0x0001) ---\n");
    TEST(test_epoch_0_secrets);
    TEST(test_epoch_1_secrets);
    TEST(test_epoch_2_secrets);
    TEST(test_epoch_3_secrets);
    TEST(test_epoch_4_secrets);

    printf("\n  --- Full epoch chain ---\n");
    TEST(test_full_epoch_chain);

    printf("\n  --- MLS Exporter ---\n");
    TEST(test_exporter_epoch_0);
    TEST(test_exporter_all_epochs);

    printf("\nAll RFC 9420 key schedule vector tests passed.\n");
    return 0;
}
