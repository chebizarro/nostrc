/*
 * MDK vector loaders for vector classes beyond key-schedule/crypto-basics.
 *
 * These are intentionally small parsers for the checked-in vector shape.  They
 * populate test structs or fail; they never count braces as coverage.
 */
#include "mdk_vector_loader.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDK_UNDEF_NODE UINT32_MAX

static char *
read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 100 * 1024 * 1024) { fclose(f); return NULL; }
    char *json = malloc((size_t)size + 1);
    if (!json) { fclose(f); return NULL; }
    size_t got = fread(json, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(json); return NULL; }
    json[size] = '\0';
    if (out_size) *out_size = (size_t)size;
    return json;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *
json_find_matching(const char *start, const char *end, char open_ch, char close_ch)
{
    int depth = 0;
    int in_string = 0;
    int esc = 0;
    for (const char *p = start; p < end && *p; p++) {
        if (in_string) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') in_string = 0;
            continue;
        }
        if (*p == '"') in_string = 1;
        else if (*p == open_ch) depth++;
        else if (*p == close_ch) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

static const char *
json_find_key_value(const char *start, const char *end, const char *key)
{
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t pat_len = strlen(pattern);
    const char *p = start;
    while (p && p < end) {
        p = strstr(p, pattern);
        if (!p || p >= end) return NULL;
        const char *q = skip_ws(p + pat_len, end);
        if (q < end && *q == ':') return skip_ws(q + 1, end);
        p += pat_len;
    }
    return NULL;
}

static int json_get_u32(const char *start, const char *end, const char *key, uint32_t *out)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p) return -1;
    char *num_end = NULL;
    unsigned long v = strtoul(p, &num_end, 10);
    if (num_end == p || num_end > end || v > UINT32_MAX) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int json_get_u64(const char *start, const char *end, const char *key, uint64_t *out)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p) return -1;
    char *num_end = NULL;
    unsigned long long v = strtoull(p, &num_end, 10);
    if (num_end == p || num_end > end) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int json_get_hex_var(const char *start, const char *end, const char *key,
                            uint8_t *out, size_t out_cap, size_t *out_len)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p || p >= end || *p != '"') return -1;
    p++;
    const char *q = p;
    while (q < end && *q && *q != '"') q++;
    if (q >= end || *q != '"') return -1;
    size_t hex_len = (size_t)(q - p);
    if ((hex_len & 1) != 0) return -1;
    size_t byte_len = hex_len / 2;
    if (byte_len > out_cap) return -1;
    char *hex = malloc(hex_len + 1);
    if (!hex) return -1;
    memcpy(hex, p, hex_len);
    hex[hex_len] = '\0';
    int ok = mdk_hex_decode(out, hex, byte_len) ? 0 : -1;
    free(hex);
    if (ok == 0 && out_len) *out_len = byte_len;
    return ok;
}

static int json_get_hex_fixed(const char *start, const char *end, const char *key,
                              uint8_t *out, size_t len)
{
    size_t got = 0;
    if (json_get_hex_var(start, end, key, out, len, &got) != 0) return -1;
    return got == len ? 0 : -1;
}

static int json_get_hex_optional(const char *start, const char *end, const char *key,
                                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p || p >= end || !strncmp(p, "null", 4)) {
        if (out_len) *out_len = 0;
        return 0;
    }
    return json_get_hex_var(start, end, key, out, out_cap, out_len);
}

static const char *json_array_for_key(const char *start, const char *end,
                                      const char *key, const char **array_end)
{
    const char *arr = json_find_key_value(start, end, key);
    if (!arr || arr >= end || *arr != '[') return NULL;
    const char *ae = json_find_matching(arr, end, '[', ']');
    if (!ae) return NULL;
    if (array_end) *array_end = ae;
    return arr;
}

static const char *json_object_for_key(const char *start, const char *end,
                                       const char *key, const char **object_end)
{
    const char *obj = json_find_key_value(start, end, key);
    if (!obj || obj >= end || *obj != '{') return NULL;
    const char *oe = json_find_matching(obj, end, '{', '}');
    if (!oe) return NULL;
    if (object_end) *object_end = oe;
    return obj;
}

typedef int (*ObjectParser)(const char *obj, const char *obj_end, void *dst, size_t *count, size_t max_count);

static int parse_json_array(const char *path, void *vectors, size_t elem_size,
                            size_t *count, size_t max_count, ObjectParser parser)
{
    size_t file_size = 0;
    char *json = read_file(path, &file_size);
    if (!json) return -1;
    *count = 0;
    const char *end = json + file_size;
    const char *pos = skip_ws(json, end);
    if (pos >= end || *pos != '[') { free(json); return -1; }
    const char *array_end = json_find_matching(pos, end, '[', ']');
    if (!array_end) { free(json); return -1; }
    pos++;
    while (*count < max_count) {
        pos = skip_ws(pos, array_end);
        if (pos >= array_end || *pos == ']') break;
        if (*pos == ',') { pos++; continue; }
        if (*pos != '{') { free(json); return -1; }
        const char *obj_end = json_find_matching(pos, array_end + 1, '{', '}');
        if (!obj_end) { free(json); return -1; }
        void *slot = (uint8_t *)vectors + (*count * elem_size);
        int rc = parser(pos, obj_end, slot, count, max_count);
        if (rc < 0) { free(json); return -1; }
        pos = obj_end + 1;
    }
    free(json);
    return 0;
}

static int supported_suite_or_skip(const char *obj, const char *end, uint32_t *cs)
{
    if (json_get_u32(obj, end, "cipher_suite", cs) != 0) return -1;
    return *cs == 1 ? 1 : 0;
}

static int parse_tree_math_object(const char *obj, const char *end, void *dst,
                                  size_t *count, size_t max_count)
{
    (void)max_count;
    MdkTreeMathVector *v = dst;
    memset(v, 0, sizeof(*v));
    if (json_get_u32(obj, end, "n_leaves", &v->n_leaves) != 0 ||
        json_get_u32(obj, end, "n_nodes", &v->n_nodes) != 0 ||
        json_get_u32(obj, end, "root", &v->root) != 0) return -1;
    v->array_size = v->n_nodes;
    if (v->array_size == 0 || v->array_size > 4096) return -1;
    v->left = calloc(v->array_size, sizeof(uint32_t));
    v->right = calloc(v->array_size, sizeof(uint32_t));
    v->parent = calloc(v->array_size, sizeof(uint32_t));
    v->sibling = calloc(v->array_size, sizeof(uint32_t));
    if (!v->left || !v->right || !v->parent || !v->sibling) return -1;

    struct { const char *key; uint32_t *out; } arrays[] = {
        {"left", v->left}, {"right", v->right}, {"parent", v->parent}, {"sibling", v->sibling}
    };
    for (size_t a = 0; a < 4; a++) {
        const char *arr_end = NULL;
        const char *p = json_array_for_key(obj, end, arrays[a].key, &arr_end);
        if (!p) return -1;
        p++;
        for (size_t i = 0; i < v->array_size; i++) {
            p = skip_ws(p, arr_end);
            if (!strncmp(p, "null", 4)) {
                arrays[a].out[i] = MDK_UNDEF_NODE;
                p += 4;
            } else {
                char *num_end = NULL;
                unsigned long n = strtoul(p, &num_end, 10);
                if (num_end == p || num_end > arr_end || n > UINT32_MAX) return -1;
                arrays[a].out[i] = (uint32_t)n;
                p = num_end;
            }
            p = skip_ws(p, arr_end);
            if (i + 1 < v->array_size) {
                if (*p != ',') return -1;
                p++;
            }
        }
    }
    (*count)++;
    return 0;
}

static int parse_messages_object(const char *obj, const char *end, void *dst,
                                 size_t *count, size_t max_count)
{
    (void)max_count;
    MdkMessagesVector *v = dst;
    memset(v, 0, sizeof(*v));
#define REQ_HEX(field) do { \
    if (json_get_hex_var(obj, end, #field, v->field, sizeof(v->field), &v->field##_len) != 0) return -1; \
} while (0)
    REQ_HEX(mls_welcome);
    REQ_HEX(mls_group_info);
    REQ_HEX(mls_key_package);
    REQ_HEX(ratchet_tree);
    REQ_HEX(group_secrets);
    REQ_HEX(add_proposal);
    REQ_HEX(update_proposal);
    REQ_HEX(remove_proposal);
    REQ_HEX(pre_shared_key_proposal);
    REQ_HEX(re_init_proposal);
    REQ_HEX(external_init_proposal);
    REQ_HEX(group_context_extensions_proposal);
    REQ_HEX(commit);
    REQ_HEX(public_message_application);
    REQ_HEX(public_message_proposal);
    REQ_HEX(public_message_commit);
    REQ_HEX(private_message);
#undef REQ_HEX
    (*count)++;
    return 0;
}

static int parse_deserialization_object(const char *obj, const char *end, void *dst,
                                        size_t *count, size_t max_count)
{
    (void)max_count;
    MdkDeserializationVector *v = dst;
    memset(v, 0, sizeof(*v));
    if (json_get_hex_var(obj, end, "vlbytes_header", v->vlbytes_header,
                         sizeof(v->vlbytes_header), &v->vlbytes_header_len) != 0 ||
        json_get_u32(obj, end, "length", &v->length) != 0) return -1;
    (*count)++;
    return 0;
}

static int parse_psk_secret_object(const char *obj, const char *end, void *dst,
                                   size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkPskSecretVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_fixed(obj, end, "psk_secret", v->psk_secret, 32) != 0) return -1;
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, "psks", &arr_end);
    if (!p) return -1;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') return -1;
        if (v->psk_count >= 16) return -1;
        const char *oe = json_find_matching(p, arr_end + 1, '{', '}');
        if (!oe) return -1;
        if (json_get_hex_var(p, oe, "psk_id", v->psks[v->psk_count].psk_id,
                             sizeof(v->psks[v->psk_count].psk_id),
                             &v->psks[v->psk_count].psk_id_len) != 0 ||
            json_get_hex_fixed(p, oe, "psk", v->psks[v->psk_count].psk, 32) != 0 ||
            json_get_hex_fixed(p, oe, "psk_nonce", v->psks[v->psk_count].psk_nonce, 32) != 0) return -1;
        v->psk_count++;
        p = oe + 1;
    }
    (*count)++;
    return 0;
}

static int parse_secret_tree_object(const char *obj, const char *end, void *dst,
                                    size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkSecretTreeVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_fixed(obj, end, "encryption_secret", v->encryption_secret, 32) != 0) return -1;
    const char *sd_end = NULL;
    const char *sd = json_object_for_key(obj, end, "sender_data", &sd_end);
    if (!sd) return -1;
    if (json_get_hex_fixed(sd, sd_end, "sender_data_secret", v->sender_data_secret, 32) != 0 ||
        json_get_hex_var(sd, sd_end, "ciphertext", v->sender_data_ciphertext,
                         sizeof(v->sender_data_ciphertext), &v->sender_data_ciphertext_len) != 0 ||
        json_get_hex_fixed(sd, sd_end, "key", v->sender_data_key, 16) != 0 ||
        json_get_hex_fixed(sd, sd_end, "nonce", v->sender_data_nonce, 12) != 0) return -1;

    const char *leaves_end = NULL;
    const char *leaf_arr = json_array_for_key(obj, end, "leaves", &leaves_end);
    if (!leaf_arr) return -1;
    const char *lp = leaf_arr + 1;
    while (v->n_leaves < MAX_SECRET_TREE_LEAVES) {
        lp = skip_ws(lp, leaves_end);
        if (lp >= leaves_end || *lp == ']') break;
        if (*lp == ',') { lp++; continue; }
        if (*lp != '[') return -1;
        const char *gens_end = json_find_matching(lp, leaves_end + 1, '[', ']');
        if (!gens_end) return -1;
        const char *gp = lp + 1;
        while (v->generation_count[v->n_leaves] < MAX_SECRET_TREE_GENERATIONS) {
            gp = skip_ws(gp, gens_end);
            if (gp >= gens_end || *gp == ']') break;
            if (*gp == ',') { gp++; continue; }
            if (*gp != '{') return -1;
            const char *ge = json_find_matching(gp, gens_end + 1, '{', '}');
            if (!ge) return -1;
            size_t gi = v->generation_count[v->n_leaves];
            MdkSecretTreeGenerationVector *g = &v->generations[v->n_leaves][gi];
            if (json_get_u32(gp, ge, "generation", &g->generation) != 0 ||
                json_get_hex_fixed(gp, ge, "application_key", g->application_key, 16) != 0 ||
                json_get_hex_fixed(gp, ge, "application_nonce", g->application_nonce, 12) != 0 ||
                json_get_hex_fixed(gp, ge, "handshake_key", g->handshake_key, 16) != 0 ||
                json_get_hex_fixed(gp, ge, "handshake_nonce", g->handshake_nonce, 12) != 0) return -1;
            v->generation_count[v->n_leaves]++;
            gp = ge + 1;
        }
        if (v->generation_count[v->n_leaves] == 0) return -1;
        v->n_leaves++;
        lp = gens_end + 1;
    }
    if (v->n_leaves == 0) return -1;
    (*count)++;
    return 0;
}

static int parse_transcript_hashes_object(const char *obj, const char *end, void *dst,
                                          size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkTranscriptHashesVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_fixed(obj, end, "confirmation_key", v->confirmation_key, 32) != 0 ||
        json_get_hex_var(obj, end, "authenticated_content", v->authenticated_content,
                         sizeof(v->authenticated_content), &v->authenticated_content_len) != 0 ||
        json_get_hex_fixed(obj, end, "interim_transcript_hash_before", v->interim_transcript_hash_before, 32) != 0 ||
        json_get_hex_fixed(obj, end, "confirmed_transcript_hash_after", v->confirmed_transcript_hash_after, 32) != 0 ||
        json_get_hex_fixed(obj, end, "interim_transcript_hash_after", v->interim_transcript_hash_after, 32) != 0) return -1;
    (*count)++;
    return 0;
}

static int parse_welcome_object(const char *obj, const char *end, void *dst,
                                size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkWelcomeVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_var(obj, end, "init_priv", v->init_priv, sizeof(v->init_priv), &v->init_priv_len) != 0 ||
        json_get_hex_var(obj, end, "signer_pub", v->signer_pub, sizeof(v->signer_pub), &v->signer_pub_len) != 0 ||
        json_get_hex_var(obj, end, "key_package", v->key_package, sizeof(v->key_package), &v->key_package_len) != 0 ||
        json_get_hex_var(obj, end, "welcome", v->welcome, sizeof(v->welcome), &v->welcome_len) != 0) return -1;
    (*count)++;
    return 0;
}

static int parse_message_protection_object(const char *obj, const char *end, void *dst,
                                           size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkMessageProtectionVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_var(obj, end, "group_id", v->group_id, sizeof(v->group_id), &v->group_id_len) != 0 ||
        json_get_u64(obj, end, "epoch", &v->epoch) != 0 ||
        json_get_hex_fixed(obj, end, "tree_hash", v->tree_hash, 32) != 0 ||
        json_get_hex_fixed(obj, end, "confirmed_transcript_hash", v->confirmed_transcript_hash, 32) != 0 ||
        json_get_hex_var(obj, end, "signature_priv", v->signature_priv, sizeof(v->signature_priv), &v->signature_priv_len) != 0 ||
        json_get_hex_var(obj, end, "signature_pub", v->signature_pub, sizeof(v->signature_pub), &v->signature_pub_len) != 0 ||
        json_get_hex_fixed(obj, end, "encryption_secret", v->encryption_secret, 32) != 0 ||
        json_get_hex_fixed(obj, end, "sender_data_secret", v->sender_data_secret, 32) != 0 ||
        json_get_hex_fixed(obj, end, "membership_key", v->membership_key, 32) != 0 ||
        json_get_hex_var(obj, end, "proposal", v->proposal, sizeof(v->proposal), &v->proposal_len) != 0 ||
        json_get_hex_var(obj, end, "proposal_pub", v->proposal_pub, sizeof(v->proposal_pub), &v->proposal_pub_len) != 0 ||
        json_get_hex_var(obj, end, "proposal_priv", v->proposal_priv, sizeof(v->proposal_priv), &v->proposal_priv_len) != 0 ||
        json_get_hex_var(obj, end, "commit", v->commit, sizeof(v->commit), &v->commit_len) != 0 ||
        json_get_hex_var(obj, end, "commit_pub", v->commit_pub, sizeof(v->commit_pub), &v->commit_pub_len) != 0 ||
        json_get_hex_var(obj, end, "commit_priv", v->commit_priv, sizeof(v->commit_priv), &v->commit_priv_len) != 0 ||
        json_get_hex_var(obj, end, "application", v->application, sizeof(v->application), &v->application_len) != 0 ||
        json_get_hex_var(obj, end, "application_priv", v->application_priv, sizeof(v->application_priv), &v->application_priv_len) != 0) return -1;
    (*count)++;
    return 0;
}

static int parse_tree_operations_object(const char *obj, const char *end, void *dst,
                                        size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkTreeOperationsVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_var(obj, end, "tree_before", v->tree_before, sizeof(v->tree_before), &v->tree_before_len) != 0 ||
        json_get_hex_var(obj, end, "proposal", v->proposal, sizeof(v->proposal), &v->proposal_len) != 0 ||
        json_get_u32(obj, end, "proposal_sender", &v->proposal_sender) != 0 ||
        json_get_hex_fixed(obj, end, "tree_hash_before", v->tree_hash_before, 32) != 0 ||
        json_get_hex_var(obj, end, "tree_after", v->tree_after, sizeof(v->tree_after), &v->tree_after_len) != 0 ||
        json_get_hex_fixed(obj, end, "tree_hash_after", v->tree_hash_after, 32) != 0) return -1;
    (*count)++;
    return 0;
}

static int parse_hex_hash_array(const char *obj, const char *end, const char *key,
                                uint8_t (**out_hashes)[32], size_t *out_count)
{
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, key, &arr_end);
    if (!p || !out_hashes || !out_count) return -1;
    *out_hashes = NULL;
    *out_count = 0;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') return -1;
        p++;
        const char *q = p;
        while (q < arr_end && *q && *q != '"') q++;
        if (q >= arr_end || *q != '"') return -1;
        if ((size_t)(q - p) != 64) return -1;
        uint8_t (*new_hashes)[32] = realloc(*out_hashes, (*out_count + 1) * 32);
        if (!new_hashes) return -1;
        *out_hashes = new_hashes;
        char hex[65];
        memcpy(hex, p, 64);
        hex[64] = '\0';
        if (!mdk_hex_decode((*out_hashes)[*out_count], hex, 32)) return -1;
        (*out_count)++;
        p = q + 1;
    }
    return *out_count > 0 ? 0 : -1;
}

static int parse_u32_array(const char *arr, const char *arr_end,
                           uint32_t **out_nodes, size_t *out_count)
{
    if (!arr || !arr_end || !out_nodes || !out_count) return -1;
    *out_nodes = NULL;
    *out_count = 0;
    const char *p = arr + 1;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        char *num_end = NULL;
        unsigned long n = strtoul(p, &num_end, 10);
        if (num_end == p || num_end > arr_end || n > UINT32_MAX) return -1;
        uint32_t *new_nodes = realloc(*out_nodes, (*out_count + 1) * sizeof(uint32_t));
        if (!new_nodes) return -1;
        *out_nodes = new_nodes;
        (*out_nodes)[(*out_count)++] = (uint32_t)n;
        p = num_end;
    }
    return 0;
}

static int parse_resolution_array(const char *obj, const char *end,
                                  MdkTreeValidationResolution **out_res,
                                  size_t *out_count)
{
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, "resolutions", &arr_end);
    if (!p || !out_res || !out_count) return -1;
    *out_res = NULL;
    *out_count = 0;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '[') return -1;
        const char *inner_end = json_find_matching(p, arr_end + 1, '[', ']');
        if (!inner_end) return -1;
        MdkTreeValidationResolution *new_res = realloc(*out_res, (*out_count + 1) * sizeof(**out_res));
        if (!new_res) return -1;
        *out_res = new_res;
        MdkTreeValidationResolution *r = &(*out_res)[*out_count];
        memset(r, 0, sizeof(*r));
        if (parse_u32_array(p, inner_end, &r->nodes, &r->count) != 0) return -1;
        (*out_count)++;
        p = inner_end + 1;
    }
    return *out_count > 0 ? 0 : -1;
}

static int parse_treekem_node_secrets(const char *obj, const char *end,
                                      MdkTreeKEMNodeSecret *out,
                                      size_t *out_count, size_t max_count)
{
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, "path_secrets", &arr_end);
    if (!p || !out || !out_count) return -1;
    *out_count = 0;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{' || *out_count >= max_count) return -1;
        const char *oe = json_find_matching(p, arr_end + 1, '{', '}');
        if (!oe) return -1;
        MdkTreeKEMNodeSecret *secret = &out[*out_count];
        if (json_get_u32(p, oe, "node", &secret->node) != 0 ||
            json_get_hex_fixed(p, oe, "path_secret", secret->path_secret, 32) != 0)
            return -1;
        (*out_count)++;
        p = oe + 1;
    }
    return 0;
}

static int parse_treekem_leaf_private_array(const char *obj, const char *end,
                                            MdkTreeKEMLeafPrivate *out,
                                            size_t *out_count, size_t max_count)
{
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, "leaves_private", &arr_end);
    if (!p || !out || !out_count) return -1;
    *out_count = 0;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{' || *out_count >= max_count) return -1;
        const char *oe = json_find_matching(p, arr_end + 1, '{', '}');
        if (!oe) return -1;
        MdkTreeKEMLeafPrivate *leaf = &out[*out_count];
        memset(leaf, 0, sizeof(*leaf));
        if (json_get_u32(p, oe, "index", &leaf->index) != 0 ||
            json_get_hex_var(p, oe, "signature_priv", leaf->signature_priv,
                             sizeof(leaf->signature_priv), &leaf->signature_priv_len) != 0 ||
            json_get_hex_var(p, oe, "encryption_priv", leaf->encryption_priv,
                             sizeof(leaf->encryption_priv), &leaf->encryption_priv_len) != 0 ||
            leaf->encryption_priv_len != 32 ||
            parse_treekem_node_secrets(p, oe, leaf->path_secrets,
                                       &leaf->path_secret_count,
                                       MAX_TREEKEM_NODE_SECRETS) != 0)
            return -1;
        (*out_count)++;
        p = oe + 1;
    }
    return *out_count > 0 ? 0 : -1;
}

static int parse_hex_string_token(const char *p, const char *end,
                                  uint8_t *out, size_t out_len,
                                  const char **next)
{
    if (!p || p >= end || *p != '"' || !out || !next) return -1;
    p++;
    const char *q = p;
    while (q < end && *q && *q != '"') q++;
    if (q >= end || *q != '"') return -1;
    size_t hex_len = (size_t)(q - p);
    if (hex_len != out_len * 2) return -1;
    char *hex = malloc(hex_len + 1);
    if (!hex) return -1;
    memcpy(hex, p, hex_len);
    hex[hex_len] = '\0';
    int rc = mdk_hex_decode(out, hex, out_len) ? 0 : -1;
    free(hex);
    if (rc == 0) *next = q + 1;
    return rc;
}

static int parse_treekem_leaf_path_secret_array(const char *obj, const char *end,
                                                MdkTreeKEMLeafPathSecret **out,
                                                size_t *out_count)
{
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, "path_secrets", &arr_end);
    if (!p || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        MdkTreeKEMLeafPathSecret *new_items = realloc(*out, (*out_count + 1) * sizeof(**out));
        if (!new_items) return -1;
        *out = new_items;
        MdkTreeKEMLeafPathSecret *slot = &(*out)[*out_count];
        memset(slot, 0, sizeof(*slot));
        if (!strncmp(p, "null", 4)) {
            slot->present = false;
            p += 4;
        } else if (*p == '"') {
            slot->present = true;
            if (parse_hex_string_token(p, arr_end, slot->path_secret, 32, &p) != 0)
                return -1;
        } else {
            return -1;
        }
        (*out_count)++;
    }
    return *out_count > 0 ? 0 : -1;
}

static int parse_treekem_update_path_array(const char *obj, const char *end,
                                           MdkTreeKEMUpdatePathVector *out,
                                           size_t *out_count, size_t max_count)
{
    const char *arr_end = NULL;
    const char *p = json_array_for_key(obj, end, "update_paths", &arr_end);
    if (!p || !out || !out_count) return -1;
    *out_count = 0;
    p++;
    while (1) {
        p = skip_ws(p, arr_end);
        if (p >= arr_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{' || *out_count >= max_count) return -1;
        const char *oe = json_find_matching(p, arr_end + 1, '{', '}');
        if (!oe) return -1;
        MdkTreeKEMUpdatePathVector *up = &out[*out_count];
        memset(up, 0, sizeof(*up));
        if (json_get_u32(p, oe, "sender", &up->sender) != 0 ||
            json_get_hex_var(p, oe, "update_path", up->update_path,
                             sizeof(up->update_path), &up->update_path_len) != 0 ||
            json_get_hex_fixed(p, oe, "tree_hash_after", up->tree_hash_after, 32) != 0 ||
            json_get_hex_fixed(p, oe, "commit_secret", up->commit_secret, 32) != 0 ||
            parse_treekem_leaf_path_secret_array(p, oe, &up->path_secrets,
                                                 &up->path_secret_count) != 0) {
            free(up->path_secrets);
            memset(up, 0, sizeof(*up));
            return -1;
        }
        (*out_count)++;
        p = oe + 1;
    }
    return *out_count > 0 ? 0 : -1;
}

static int parse_tree_validation_object(const char *obj, const char *end, void *dst,
                                        size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkTreeValidationVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_var(obj, end, "tree", v->tree, sizeof(v->tree), &v->tree_len) != 0 ||
        json_get_hex_var(obj, end, "group_id", v->group_id, sizeof(v->group_id), &v->group_id_len) != 0 ||
        parse_hex_hash_array(obj, end, "tree_hashes", &v->tree_hashes, &v->tree_hash_count) != 0 ||
        parse_resolution_array(obj, end, &v->resolutions, &v->resolution_count) != 0) {
        mdk_free_tree_validation_vector(v);
        return -1;
    }
    (*count)++;
    return 0;
}

static int parse_treekem_object(const char *obj, const char *end, void *dst,
                                size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkTreeKEMVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_var(obj, end, "group_id", v->group_id, sizeof(v->group_id), &v->group_id_len) != 0 ||
        json_get_u64(obj, end, "epoch", &v->epoch) != 0 ||
        json_get_hex_fixed(obj, end, "confirmed_transcript_hash", v->confirmed_transcript_hash, 32) != 0 ||
        json_get_hex_var(obj, end, "ratchet_tree", v->ratchet_tree, sizeof(v->ratchet_tree), &v->ratchet_tree_len) != 0 ||
        parse_treekem_leaf_private_array(obj, end, v->leaves_private,
                                         &v->leaf_private_count,
                                         MAX_TREEKEM_LEAVES) != 0 ||
        parse_treekem_update_path_array(obj, end, v->update_paths,
                                        &v->update_path_count,
                                        MAX_TREEKEM_UPDATES) != 0) {
        mdk_free_treekem_vector(v);
        return -1;
    }
    (*count)++;
    return 0;
}

static int parse_passive_client_object(const char *obj, const char *end, void *dst,
                                       size_t *count, size_t max_count)
{
    (void)max_count;
    uint32_t cs = 0;
    int suite = supported_suite_or_skip(obj, end, &cs);
    if (suite < 0) return -1;
    if (suite == 0) return 0;
    MdkPassiveClientVector *v = dst;
    memset(v, 0, sizeof(*v));
    v->cipher_suite = cs;
    if (json_get_hex_var(obj, end, "key_package", v->key_package, sizeof(v->key_package), &v->key_package_len) != 0 ||
        json_get_hex_var(obj, end, "signature_priv", v->signature_priv, sizeof(v->signature_priv), &v->signature_priv_len) != 0 ||
        json_get_hex_var(obj, end, "encryption_priv", v->encryption_priv, sizeof(v->encryption_priv), &v->encryption_priv_len) != 0 ||
        json_get_hex_var(obj, end, "init_priv", v->init_priv, sizeof(v->init_priv), &v->init_priv_len) != 0 ||
        json_get_hex_var(obj, end, "welcome", v->welcome, sizeof(v->welcome), &v->welcome_len) != 0 ||
        json_get_hex_optional(obj, end, "ratchet_tree", v->ratchet_tree, sizeof(v->ratchet_tree), &v->ratchet_tree_len) != 0 ||
        json_get_hex_fixed(obj, end, "initial_epoch_authenticator", v->initial_epoch_authenticator, 32) != 0) return -1;
    const char *epochs_end = NULL;
    const char *epochs = json_array_for_key(obj, end, "epochs", &epochs_end);
    if (!epochs) return -1;
    const char *p = epochs + 1;
    while (p < epochs_end) {
        p = skip_ws(p, epochs_end);
        if (p >= epochs_end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') return -1;
        const char *oe = json_find_matching(p, epochs_end + 1, '{', '}');
        if (!oe) return -1;
        v->epoch_count++;
        p = oe + 1;
    }
    (*count)++;
    return 0;
}

int mdk_load_tree_math_vectors(const char *path, MdkTreeMathVector *vectors,
                               size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_tree_math_object);
}

int mdk_load_messages_vectors(const char *path, MdkMessagesVector *vectors,
                              size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_messages_object);
}

int mdk_load_deserialization_vectors(const char *path, MdkDeserializationVector *vectors,
                                     size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_deserialization_object);
}

int mdk_load_psk_secret_vectors(const char *path, MdkPskSecretVector *vectors,
                                size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_psk_secret_object);
}

int mdk_load_secret_tree_vectors(const char *path, MdkSecretTreeVector *vectors,
                                 size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_secret_tree_object);
}

int mdk_load_transcript_hashes_vectors(const char *path, MdkTranscriptHashesVector *vectors,
                                       size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_transcript_hashes_object);
}

int mdk_load_welcome_vectors(const char *path, MdkWelcomeVector *vectors,
                             size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_welcome_object);
}

int mdk_load_message_protection_vectors(const char *path, MdkMessageProtectionVector *vectors,
                                        size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_message_protection_object);
}

int mdk_load_tree_operations_vectors(const char *path, MdkTreeOperationsVector *vectors,
                                     size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_tree_operations_object);
}

int mdk_load_tree_validation_vectors(const char *path, MdkTreeValidationVector *vectors,
                                     size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_tree_validation_object);
}

int mdk_load_treekem_vectors(const char *path, MdkTreeKEMVector *vectors,
                             size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_treekem_object);
}

int mdk_load_passive_client_vectors(const char *path, MdkPassiveClientVector *vectors,
                                    size_t *count, size_t max_count)
{
    return parse_json_array(path, vectors, sizeof(*vectors), count, max_count, parse_passive_client_object);
}

void mdk_free_tree_math_vector(MdkTreeMathVector *vec)
{
    if (vec) {
        free(vec->left);
        free(vec->right);
        free(vec->parent);
        free(vec->sibling);
        memset(vec, 0, sizeof(*vec));
    }
}

void mdk_free_tree_validation_vector(MdkTreeValidationVector *vec)
{
    if (!vec) return;
    free(vec->tree_hashes);
    if (vec->resolutions) {
        for (size_t i = 0; i < vec->resolution_count; i++)
            free(vec->resolutions[i].nodes);
        free(vec->resolutions);
    }
    memset(vec, 0, sizeof(*vec));
}

void mdk_free_treekem_vector(MdkTreeKEMVector *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->update_path_count; i++)
        free(vec->update_paths[i].path_secrets);
    memset(vec, 0, sizeof(*vec));
}
