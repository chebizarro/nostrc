/*
 * MDK Test Vector Loader Implementation
 *
 * Small, bounded JSON reader for the checked-in MDK vectors.  It is not a
 * general JSON parser, but it does validate object/array boundaries, hex
 * lengths, and required fields so tests cannot pass with unpopulated vectors.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mdk_vector_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Helper: decode hex string to bytes */
bool
mdk_hex_decode(uint8_t *out, const char *hex, size_t out_len)
{
    if (!hex || (!out && out_len > 0)) return false;

    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return false;

    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1)
            return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

/* Helper: find JSON string value by key (legacy helper; first match only). */
const char *
mdk_json_find_string(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos = strchr(pos + strlen(pattern), '"');
    if (!pos) return NULL;

    return pos + 1;
}

/* Helper: find JSON number value by key (legacy helper; first match only). */
int
mdk_json_find_number(const char *json, const char *key, uint32_t *out)
{
    if (!json || !key || !out) return -1;

    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return -1;

    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) pos++;

    if (sscanf(pos, "%u", out) != 1)
        return -1;

    return 0;
}

/* Helper: read entire file into memory */
static char *
read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > 100 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    if (out_size) *out_size = (size_t)size;
    return buf;
}

static const char *
skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *
json_find_matching(const char *start, const char *end, char open_ch, char close_ch)
{
    int depth = 0;
    bool in_string = false;
    bool esc = false;
    for (const char *p = start; p < end && *p; p++) {
        if (in_string) {
            if (esc) esc = false;
            else if (*p == '\\') esc = true;
            else if (*p == '"') in_string = false;
            continue;
        }
        if (*p == '"') {
            in_string = true;
        } else if (*p == open_ch) {
            depth++;
        } else if (*p == close_ch) {
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

static bool
json_get_u32(const char *start, const char *end, const char *key, uint32_t *out)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p) return false;
    char *num_end = NULL;
    unsigned long v = strtoul(p, &num_end, 10);
    if (num_end == p || num_end > end || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool
json_copy_string(const char *start, const char *end, const char *key,
                 char *out, size_t out_cap)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p || p >= end || *p != '"' || out_cap == 0) return false;
    p++;
    size_t n = 0;
    while (p < end && *p && *p != '"') {
        if (*p == '\\') return false; /* vectors do not need escaped strings */
        if (n + 1 >= out_cap) return false;
        out[n++] = *p++;
    }
    if (p >= end || *p != '"') return false;
    out[n] = '\0';
    return true;
}

static bool
json_get_hex_var(const char *start, const char *end, const char *key,
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    const char *p = json_find_key_value(start, end, key);
    if (!p || p >= end || *p != '"') return false;
    p++;
    const char *q = p;
    while (q < end && *q && *q != '"') q++;
    if (q >= end || *q != '"') return false;
    size_t hex_len = (size_t)(q - p);
    if ((hex_len & 1) != 0) return false;
    size_t byte_len = hex_len / 2;
    if (byte_len > out_cap) return false;

    char *hex = malloc(hex_len + 1);
    if (!hex) return false;
    memcpy(hex, p, hex_len);
    hex[hex_len] = '\0';
    bool ok = mdk_hex_decode(out, hex, byte_len);
    free(hex);
    if (!ok) return false;
    if (out_len) *out_len = byte_len;
    return true;
}

static bool
json_get_hex_fixed(const char *start, const char *end, const char *key,
                   uint8_t *out, size_t len)
{
    size_t got = 0;
    return json_get_hex_var(start, end, key, out, len, &got) && got == len;
}

static const char *
json_array_for_key(const char *start, const char *end, const char *key,
                   const char **array_end)
{
    const char *arr = json_find_key_value(start, end, key);
    if (!arr || arr >= end || *arr != '[') return NULL;
    const char *ae = json_find_matching(arr, end, '[', ']');
    if (!ae) return NULL;
    if (array_end) *array_end = ae;
    return arr;
}

static const char *
json_object_for_key(const char *start, const char *end, const char *key,
                    const char **object_end)
{
    const char *obj = json_find_key_value(start, end, key);
    if (!obj || obj >= end || *obj != '{') return NULL;
    const char *oe = json_find_matching(obj, end, '{', '}');
    if (!oe) return NULL;
    if (object_end) *object_end = oe;
    return obj;
}

/* Load key-schedule vectors from JSON file */
int
mdk_load_key_schedule_vectors(const char *path,
                               MdkKeyScheduleVector *vectors,
                               size_t *count,
                               size_t max_count)
{
    if (!path || !vectors || !count) return -1;

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

        uint32_t cs = 0;
        if (!json_get_u32(pos, obj_end, "cipher_suite", &cs)) { free(json); return -1; }
        if (cs != 1) { pos = obj_end + 1; continue; } /* libmarmot implements suite 0x0001 */

        MdkKeyScheduleVector *vec = &vectors[*count];
        memset(vec, 0, sizeof(*vec));
        vec->cipher_suite = cs;
        if (!json_get_hex_var(pos, obj_end, "group_id", vec->group_id,
                              sizeof(vec->group_id), &vec->group_id_len) ||
            !json_get_hex_fixed(pos, obj_end, "initial_init_secret",
                                vec->initial_init_secret, 32)) {
            free(json); return -1;
        }

        const char *epochs_end = NULL;
        const char *epochs = json_array_for_key(pos, obj_end, "epochs", &epochs_end);
        if (!epochs) { free(json); return -1; }
        const char *epos = epochs + 1;
        while (vec->epoch_count < MAX_EPOCHS) {
            epos = skip_ws(epos, epochs_end);
            if (epos >= epochs_end || *epos == ']') break;
            if (*epos == ',') { epos++; continue; }
            if (*epos != '{') { free(json); return -1; }
            const char *epoch_end = json_find_matching(epos, epochs_end + 1, '{', '}');
            if (!epoch_end) { free(json); return -1; }

            MdkEpochVector *epoch = &vec->epochs[vec->epoch_count];
            memset(epoch, 0, sizeof(*epoch));
            if (!json_get_hex_fixed(epos, epoch_end, "commit_secret", epoch->commit_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "confirmation_key", epoch->confirmation_key, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "encryption_secret", epoch->encryption_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "exporter_secret", epoch->exporter_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "init_secret", epoch->init_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "joiner_secret", epoch->joiner_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "psk_secret", epoch->psk_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "membership_key", epoch->membership_key, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "sender_data_secret", epoch->sender_data_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "welcome_secret", epoch->welcome_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "epoch_authenticator", epoch->epoch_authenticator, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "resumption_psk", epoch->resumption_psk, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "external_secret", epoch->external_secret, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "external_pub", epoch->external_pub, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "tree_hash", epoch->tree_hash, 32) ||
                !json_get_hex_fixed(epos, epoch_end, "confirmed_transcript_hash", epoch->confirmed_transcript_hash, 32) ||
                !json_get_hex_var(epos, epoch_end, "group_context", epoch->group_context,
                                  sizeof(epoch->group_context), &epoch->group_context_len)) {
                free(json); return -1;
            }

            const char *exporter_end = NULL;
            const char *exporter = json_object_for_key(epos, epoch_end, "exporter", &exporter_end);
            if (exporter) {
                if (!json_copy_string(exporter, exporter_end, "label",
                                      epoch->exporter_label, sizeof(epoch->exporter_label)) ||
                    !json_get_hex_fixed(exporter, exporter_end, "context", epoch->exporter_context, 32) ||
                    !json_get_hex_fixed(exporter, exporter_end, "secret", epoch->exporter_secret_out, 32) ||
                    !json_get_u32(exporter, exporter_end, "length", &epoch->exporter_length)) {
                    free(json); return -1;
                }
            }

            vec->epoch_count++;
            epos = epoch_end + 1;
        }
        if (vec->epoch_count == 0) { free(json); return -1; }
        (*count)++;
        pos = obj_end + 1;
    }

    free(json);
    return 0;
}

/* Load crypto-basics vectors from JSON file */
int
mdk_load_crypto_basics_vectors(const char *path,
                                MdkCryptoBasicsVector *vectors,
                                size_t *count,
                                size_t max_count)
{
    if (!path || !vectors || !count) return -1;

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

        uint32_t cs = 0;
        if (!json_get_u32(pos, obj_end, "cipher_suite", &cs)) { free(json); return -1; }
        if (cs != 1) { pos = obj_end + 1; continue; }

        MdkCryptoBasicsVector *vec = &vectors[*count];
        memset(vec, 0, sizeof(*vec));
        vec->cipher_suite = cs;

        const char *expand_end = NULL;
        const char *expand = json_object_for_key(pos, obj_end, "expand_with_label", &expand_end);
        const char *derive_end = NULL;
        const char *derive = json_object_for_key(pos, obj_end, "derive_secret", &derive_end);
        if (!expand || !derive) { free(json); return -1; }

        size_t expand_out_len = 0;
        if (!json_get_hex_fixed(expand, expand_end, "secret", vec->expand_secret, 32) ||
            !json_get_hex_fixed(expand, expand_end, "context", vec->expand_context, 32) ||
            !json_copy_string(expand, expand_end, "label", vec->expand_label, sizeof(vec->expand_label)) ||
            !json_get_u32(expand, expand_end, "length", &vec->expand_length) ||
            vec->expand_length > sizeof(vec->expand_out) ||
            !json_get_hex_var(expand, expand_end, "out", vec->expand_out,
                              sizeof(vec->expand_out), &expand_out_len) ||
            expand_out_len != vec->expand_length) {
            free(json); return -1;
        }

        if (!json_get_hex_fixed(derive, derive_end, "secret", vec->derive_secret, 32) ||
            !json_get_hex_fixed(derive, derive_end, "out", vec->derive_out, 32) ||
            !json_copy_string(derive, derive_end, "label", vec->derive_label, sizeof(vec->derive_label))) {
            free(json); return -1;
        }

        (*count)++;
        pos = obj_end + 1;
    }

    free(json);
    return 0;
}
