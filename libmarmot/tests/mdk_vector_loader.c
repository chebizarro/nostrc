/*
 * MDK Test Vector Loader Implementation
 *
 * Simple JSON parser for MDK test vectors without external dependencies.
 * This is intentionally minimal - just enough to parse the specific
 * vector format we need for cross-validation.
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
    if (!hex || !out) return false;
    
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

/* Helper: find JSON string value by key (returns pointer into json buffer) */
const char *
mdk_json_find_string(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    
    /* Build search pattern: "key": " */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    
    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;
    
    /* Skip to opening quote */
    pos = strchr(pos + strlen(pattern), '"');
    if (!pos) return NULL;
    
    return pos + 1; /* Return start of string value */
}

/* Helper: find JSON number value by key */
int
mdk_json_find_number(const char *json, const char *key, uint32_t *out)
{
    if (!json || !key || !out) return -1;
    
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    
    const char *pos = strstr(json, pattern);
    if (!pos) return -1;
    
    pos += strlen(pattern);
    while (*pos && isspace(*pos)) pos++;
    
    if (sscanf(pos, "%u", out) != 1)
        return -1;
    
    return 0;
}

/* Helper: extract hex field from JSON */
static bool
extract_hex_field(const char *json, const char *key, uint8_t *out, size_t len)
{
    const char *hex = mdk_json_find_string(json, key);
    if (!hex) return false;
    
    /* Copy hex string (up to closing quote) */
    char hex_buf[1024];
    size_t i = 0;
    while (hex[i] && hex[i] != '"' && i < sizeof(hex_buf) - 1) {
        hex_buf[i] = hex[i];
        i++;
    }
    hex_buf[i] = '\0';
    
    return mdk_hex_decode(out, hex_buf, len);
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
    
    if (size < 0 || size > 100 * 1024 * 1024) { /* 100MB limit */
        fclose(f);
        return NULL;
    }
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t read = fread(buf, 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        free(buf);
        return NULL;
    }
    
    buf[size] = '\0';
    if (out_size) *out_size = size;
    return buf;
}

/* Load key-schedule vectors from JSON file */
int
mdk_load_key_schedule_vectors(const char *path,
                               MdkKeyScheduleVector *vectors,
                               size_t *count,
                               size_t max_count)
{
    if (!path || !vectors || !count) return -1;
    
    size_t file_size;
    char *json = read_file(path, &file_size);
    if (!json) return -1;
    
    *count = 0;
    
    /* Find first test case (after opening '[') */
    const char *pos = strchr(json, '[');
    if (!pos) {
        free(json);
        return -1;
    }
    pos++;
    
    /* Parse each test case */
    while (*count < max_count) {
        /* Find next object */
        pos = strchr(pos, '{');
        if (!pos) break;
        
        /* Find end of this object (simple brace matching) */
        const char *obj_start = pos;
        int brace_depth = 0;
        const char *obj_end = pos;
        while (*obj_end) {
            if (*obj_end == '{') brace_depth++;
            else if (*obj_end == '}') {
                brace_depth--;
                if (brace_depth == 0) break;
            }
            obj_end++;
        }
        
        if (brace_depth != 0) break;
        
        /* Extract cipher_suite */
        MdkKeyScheduleVector *vec = &vectors[*count];
        memset(vec, 0, sizeof(*vec));
        
        if (mdk_json_find_number(obj_start, "cipher_suite", &vec->cipher_suite) != 0) {
            pos = obj_end + 1;
            continue;
        }
        
        /* Extract group_id and initial_init_secret */
        const char *gid_hex = mdk_json_find_string(obj_start, "group_id");
        if (gid_hex) {
            char hex_buf[128];
            size_t i = 0;
            while (gid_hex[i] && gid_hex[i] != '"' && i < sizeof(hex_buf) - 1) {
                hex_buf[i] = gid_hex[i];
                i++;
            }
            hex_buf[i] = '\0';
            vec->group_id_len = strlen(hex_buf) / 2;
            if (vec->group_id_len <= sizeof(vec->group_id)) {
                mdk_hex_decode(vec->group_id, hex_buf, vec->group_id_len);
            }
        }
        extract_hex_field(obj_start, "initial_init_secret", vec->initial_init_secret, 32);
        
        /* Find epochs array */
        const char *epochs_start = strstr(obj_start, "\"epochs\"");
        if (!epochs_start || epochs_start > obj_end) {
            pos = obj_end + 1;
            continue;
        }
        
        epochs_start = strchr(epochs_start, '[');
        if (!epochs_start) {
            pos = obj_end + 1;
            continue;
        }
        
        /* Parse each epoch */
        const char *epoch_pos = epochs_start + 1;
        vec->epoch_count = 0;
        
        while (vec->epoch_count < MAX_EPOCHS) {
            epoch_pos = strchr(epoch_pos, '{');
            if (!epoch_pos || epoch_pos > obj_end) break;
            
            /* Find end of epoch object */
            const char *epoch_start = epoch_pos;
            int depth = 0;
            const char *epoch_end = epoch_pos;
            while (*epoch_end && epoch_end < obj_end) {
                if (*epoch_end == '{') depth++;
                else if (*epoch_end == '}') {
                    depth--;
                    if (depth == 0) break;
                }
                epoch_end++;
            }
            
            if (depth != 0) break;
            
            MdkEpochVector *epoch = &vec->epochs[vec->epoch_count];
            
            /* Extract all hex fields */
            extract_hex_field(epoch_start, "commit_secret", epoch->commit_secret, 32);
            extract_hex_field(epoch_start, "confirmation_key", epoch->confirmation_key, 32);
            extract_hex_field(epoch_start, "encryption_secret", epoch->encryption_secret, 32);
            extract_hex_field(epoch_start, "exporter_secret", epoch->exporter_secret, 32);
            extract_hex_field(epoch_start, "init_secret", epoch->init_secret, 32);
            extract_hex_field(epoch_start, "joiner_secret", epoch->joiner_secret, 32);
            extract_hex_field(epoch_start, "membership_key", epoch->membership_key, 32);
            extract_hex_field(epoch_start, "sender_data_secret", epoch->sender_data_secret, 32);
            extract_hex_field(epoch_start, "welcome_secret", epoch->welcome_secret, 32);
            extract_hex_field(epoch_start, "epoch_authenticator", epoch->epoch_authenticator, 32);
            extract_hex_field(epoch_start, "resumption_psk", epoch->resumption_psk, 32);
            extract_hex_field(epoch_start, "external_secret", epoch->external_secret, 32);
            extract_hex_field(epoch_start, "external_pub", epoch->external_pub, 32);
            extract_hex_field(epoch_start, "tree_hash", epoch->tree_hash, 32);
            extract_hex_field(epoch_start, "confirmed_transcript_hash", epoch->confirmed_transcript_hash, 32);
            
            /* Extract group_context */
            const char *gc_hex = mdk_json_find_string(epoch_start, "group_context");
            if (gc_hex) {
                char hex_buf[1024];
                size_t i = 0;
                while (gc_hex[i] && gc_hex[i] != '"' && i < sizeof(hex_buf) - 1) {
                    hex_buf[i] = gc_hex[i];
                    i++;
                }
                hex_buf[i] = '\0';
                epoch->group_context_len = strlen(hex_buf) / 2;
                if (epoch->group_context_len <= sizeof(epoch->group_context)) {
                    mdk_hex_decode(epoch->group_context, hex_buf, epoch->group_context_len);
                }
            }
            
            /* Extract exporter test data */
            const char *exporter_obj = strstr(epoch_start, "\"exporter\"");
            if (exporter_obj && exporter_obj < epoch_end) {
                /* Label is a string per MLS spec, not hex */
                const char *label = mdk_json_find_string(exporter_obj, "label");
                if (label) {
                    size_t i = 0;
                    while (label[i] && label[i] != '"' && i < sizeof(epoch->exporter_label) - 1) {
                        epoch->exporter_label[i] = label[i];
                        i++;
                    }
                    epoch->exporter_label[i] = '\0';
                }
                extract_hex_field(exporter_obj, "context", epoch->exporter_context, 32);
                extract_hex_field(exporter_obj, "secret", epoch->exporter_secret_out, 32);
                mdk_json_find_number(exporter_obj, "length", &epoch->exporter_length);
            }
            
            vec->epoch_count++;
            epoch_pos = epoch_end + 1;
        }
        
        if (vec->epoch_count > 0) {
            (*count)++;
        }
        
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
    
    size_t file_size;
    char *json = read_file(path, &file_size);
    if (!json) return -1;
    
    *count = 0;
    
    /* Find first test case */
    const char *pos = strchr(json, '[');
    if (!pos) {
        free(json);
        return -1;
    }
    pos++;
    
    /* Parse each test case */
    while (*count < max_count) {
        pos = strchr(pos, '{');
        if (!pos) break;
        
        /* Find end of object */
        const char *obj_start = pos;
        int brace_depth = 0;
        const char *obj_end = pos;
        while (*obj_end) {
            if (*obj_end == '{') brace_depth++;
            else if (*obj_end == '}') {
                brace_depth--;
                if (brace_depth == 0) break;
            }
            obj_end++;
        }
        
        if (brace_depth != 0) break;
        
        MdkCryptoBasicsVector *vec = &vectors[*count];
        memset(vec, 0, sizeof(*vec));
        
        if (mdk_json_find_number(obj_start, "cipher_suite", &vec->cipher_suite) != 0) {
            pos = obj_end + 1;
            continue;
        }
        
        /* Extract expand_with_label test */
        const char *expand_obj = strstr(obj_start, "\"expand_with_label\"");
        if (expand_obj && expand_obj < obj_end) {
            extract_hex_field(expand_obj, "secret", vec->expand_secret, 32);
            extract_hex_field(expand_obj, "context", vec->expand_context, 32);
            extract_hex_field(expand_obj, "out", vec->expand_out, 32);
            
            const char *label = mdk_json_find_string(expand_obj, "label");
            if (label) {
                size_t i = 0;
                while (label[i] && label[i] != '"' && i < sizeof(vec->expand_label) - 1) {
                    vec->expand_label[i] = label[i];
                    i++;
                }
                vec->expand_label[i] = '\0';
            }
            
            mdk_json_find_number(expand_obj, "length", &vec->expand_length);
        }
        
        /* Extract derive_secret test */
        const char *derive_obj = strstr(obj_start, "\"derive_secret\"");
        if (derive_obj && derive_obj < obj_end) {
            extract_hex_field(derive_obj, "secret", vec->derive_secret, 32);
            extract_hex_field(derive_obj, "out", vec->derive_out, 32);
            
            const char *label = mdk_json_find_string(derive_obj, "label");
            if (label) {
                size_t i = 0;
                while (label[i] && label[i] != '"' && i < sizeof(vec->derive_label) - 1) {
                    vec->derive_label[i] = label[i];
                    i++;
                }
                vec->derive_label[i] = '\0';
            }
        }
        
        (*count)++;
        pos = obj_end + 1;
    }
    
    free(json);
    return 0;
}
