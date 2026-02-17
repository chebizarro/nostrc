/* Stub implementations for remaining MDK vector loaders */
#include "mdk_vector_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper to count JSON objects in array */
static size_t count_json_objects(const char *json) {
    size_t count = 0;
    const char *pos = strchr(json, '[');
    if (pos) {
        while ((pos = strchr(pos + 1, '{'))) {
            count++;
            int depth = 0;
            while (*pos) {
                if (*pos == '{') depth++;
                else if (*pos == '}') {
                    depth--;
                    if (depth == 0) break;
                }
                pos++;
            }
        }
    }
    return count;
}

int mdk_load_tree_math_vectors(const char *path, MdkTreeMathVector *vectors, 
                                size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_messages_vectors(const char *path, MdkMessagesVector *vectors,
                               size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fclose(f);
    *count = 1;
    return 0;
}

int mdk_load_deserialization_vectors(const char *path, MdkDeserializationVector *vectors,
                                      size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_psk_secret_vectors(const char *path, MdkPskSecretVector *vectors,
                                 size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_secret_tree_vectors(const char *path, MdkSecretTreeVector *vectors,
                                  size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_transcript_hashes_vectors(const char *path, MdkTranscriptHashesVector *vectors,
                                        size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_welcome_vectors(const char *path, MdkWelcomeVector *vectors,
                              size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_message_protection_vectors(const char *path, MdkMessageProtectionVector *vectors,
                                         size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_tree_operations_vectors(const char *path, MdkTreeOperationsVector *vectors,
                                      size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_tree_validation_vectors(const char *path, MdkTreeValidationVector *vectors,
                                      size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_treekem_vectors(const char *path, MdkTreeKEMVector *vectors,
                              size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

int mdk_load_passive_client_vectors(const char *path, MdkPassiveClientVector *vectors,
                                     size_t *count, size_t max_count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    *count = count_json_objects(json);
    if (*count > max_count) *count = max_count;
    free(json);
    return 0;
}

void mdk_free_tree_math_vector(MdkTreeMathVector *vec) {
    if (vec) {
        free(vec->left);
        free(vec->right);
        free(vec->parent);
        free(vec->sibling);
    }
}
