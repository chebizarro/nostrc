#include "diff.h"
#include <jansson.h>
#include <openssl/evp.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool diff_error(diff_context *ctx, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(ctx->error, sizeof(ctx->error), format, args);
    va_end(args);
    return false;
}

static void *allocate(size_t size)
{
    void *p = calloc(1, size ? size : 1);
    if (!p) { fputs("FAIL: out of memory\n", stderr); exit(1); }
    return p;
}

static char *copy_text(const char *text)
{
    char *p = allocate(strlen(text) + 1);
    strcpy(p, text);
    return p;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool diff_hex(diff_context *ctx, const char *text, diff_bytes *out)
{
    size_t n = strlen(text);
    if (n % 2) return diff_error(ctx, "odd hex length");
    unsigned char *data = allocate(n / 2);
    for (size_t i = 0; i < n; i += 2) {
        int a = nibble(text[i]), b = nibble(text[i + 1]);
        if (a < 0 || b < 0) { free(data); return diff_error(ctx, "invalid lowercase hex"); }
        data[i / 2] = (unsigned char)(16 * a + b);
    }
    *out = (diff_bytes){data, n / 2};
    return true;
}

bool diff_word(diff_context *ctx, const char *text, uint64_t *out)
{
    if (strlen(text) != 16) return diff_error(ctx, "expected 16-digit hex word");
    uint64_t word = 0;
    for (size_t i = 0; i < 16; ++i) {
        int digit = nibble(text[i]);
        if (digit < 0) return diff_error(ctx, "invalid hex word");
        word = (word << 4) | (unsigned int)digit;
    }
    *out = word;
    return true;
}

static bool read_file(diff_context *ctx, const char *path, diff_bytes *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return diff_error(ctx, "cannot read %s: %s", path, strerror(errno));
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && n <= 16 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
            unsigned char *data = allocate((size_t)n + 1);
            if (fread(data, 1, (size_t)n, f) == (size_t)n && !ferror(f)) {
                *out = (diff_bytes){data, (size_t)n};
                ok = true;
            } else free(data);
        }
    }
    fclose(f);
    return ok || diff_error(ctx, "cannot read bounded fixture %s (max 16 MiB)", path);
}

static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char *path = allocate(n);
    snprintf(path, n, "%s/%s", dir, name);
    return path;
}

bool diff_compare(const char *id, const unsigned char *expected, size_t en,
                  const unsigned char *actual, size_t an)
{
    size_t n = en < an ? en : an, offset = 0;
    while (offset < n && expected[offset] == actual[offset]) ++offset;
    if (offset == n && en == an) return true;
    fprintf(stderr, "FAIL %s: first differing offset %zu (0x%zx), expected length %zu, actual %zu\n",
            id, offset, offset, en, an);
    size_t start = offset > 16 ? offset - 16 : 0;
    size_t end = offset + 17;
    if (end > (en > an ? en : an)) end = en > an ? en : an;
    for (size_t row = start; row < end; row += 16) {
        fprintf(stderr, "  %08zx expected:", row);
        for (size_t i = row; i < row + 16 && i < end; ++i)
            if (i < en) fprintf(stderr, " %02x", expected[i]); else fputs(" --", stderr);
        fputs("\n           actual:  ", stderr);
        for (size_t i = row; i < row + 16 && i < end; ++i)
            if (i < an) fprintf(stderr, " %02x", actual[i]); else fputs(" --", stderr);
        fputc('\n', stderr);
    }
    return false;
}

void diff_free(diff_value *v)
{
    if (!v) return;
    free(v->name);
    if (v->type == D_TEXT) free(v->as.text);
    if (v->type == D_BYTES) free(v->as.bytes.data);
    if (v->type == D_ARRAY || v->type == D_OBJECT) {
        for (size_t i = 0; i < COUNT(v); ++i) diff_free(ITEM(v, i));
        free(v->as.children.items);
    }
    free(v);
}

diff_value *diff_get(const diff_value *v, const char *name)
{
    if (v && v->type == D_OBJECT)
        for (size_t i = 0; i < COUNT(v); ++i)
            if (strcmp(ITEM(v, i)->name, name) == 0) return ITEM(v, i);
    return NULL;
}

/* Jansson deliberately rejects unsigned values above INT64_MAX. Quote ONLY
 * those integer tokens before parsing, never use JSON_DECODE_INT_AS_REAL.
 * The internal marker is forbidden in source strings (also after unescaping). */
static char *normalize(diff_context *ctx, const diff_bytes *input)
{
    char *out = allocate(input->len * 2 + 1);
    size_t used = 0;
    for (size_t i = 0; i < input->len;) {
        unsigned char c = input->data[i];
        if (c == '"') {
            size_t start = i;
            out[used++] = (char)input->data[i++];
            while (i < input->len) {
                c = input->data[i++]; out[used++] = (char)c;
                if (c == '\\' && i < input->len) out[used++] = (char)input->data[i++];
                else if (c == '"') break;
            }
            json_error_t error;
            json_t *literal = json_loadb((const char *)input->data + start, i - start,
                                        JSON_DECODE_ANY, &error);
            const char *text = json_string_value(literal);
            bool reserved = text && strncmp(text, "~u64:", 5) == 0;
            json_decref(literal);
            if (reserved) { free(out); diff_error(ctx, "reserved internal integer marker in JSON string"); return NULL; }
        } else if (c >= '0' && c <= '9') {
            size_t start = i;
            while (i < input->len && input->data[i] >= '0' && input->data[i] <= '9') ++i;
            size_t n = i - start;
            bool negative = start && input->data[start - 1] == '-';
            bool large = !negative && (n > 19 || (n == 19 &&
                memcmp(input->data + start, "9223372036854775807", 19) > 0));
            if (large) {
                if (input->data[start] == '0' ||
                    (i < input->len && (input->data[i] == '.' || input->data[i] == 'e' || input->data[i] == 'E'))) {
                    free(out); diff_error(ctx, "invalid unsigned integer token"); return NULL;
                }
                memcpy(out + used, "\"~u64:", 6); used += 6;
            }
            memcpy(out + used, input->data + start, n); used += n;
            if (large) out[used++] = '"';
        } else { out[used++] = (char)c; ++i; }
    }
    out[used] = '\0';
    return out;
}

static const char *string_field(json_t *v, const char *name)
{
    return json_string_value(json_object_get(v, name));
}

static bool same_field(diff_context *ctx, json_t *v, const char *key, const char *expected)
{
    const char *got = string_field(v, key);
    return (got && strcmp(got, expected) == 0) ||
        diff_error(ctx, "%s mismatch: expected %s, got %s", key, expected, got ? got : "<missing>");
}

static bool blob(diff_context *ctx, json_t *j, diff_value *v)
{
    const char *file = string_field(j, "file"), *hex = string_field(j, "hex");
    const char *sha = string_field(j, "sha256");
    json_t *length = json_object_get(j, "length");
    if (!same_field(ctx, j, "pin", ctx->pin) || !same_field(ctx, j, "generator_version", ctx->version)) return false;
    if (!file || !*file || strchr(file, '/') || strchr(file, '\\') || strstr(file, "..") ||
        strlen(file) < 5 || strcmp(file + strlen(file) - 4, ".bin") || !hex || !sha ||
        !json_is_integer(length) || json_integer_value(length) < 0)
        return diff_error(ctx, "invalid byte descriptor");
    char *path = join(ctx->dir, file);
    diff_bytes bytes = {0}, expected = {0}, hash = {0};
    bool ok = read_file(ctx, path, &bytes);
    free(path);
    if (ok && (uint64_t)json_integer_value(length) != bytes.len)
        ok = diff_error(ctx, "%s length mismatch", file);
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dn = 0;
    if (ok) ok = diff_hex(ctx, sha, &hash);
    if (ok && (hash.len != 32 || !EVP_Digest(bytes.data, bytes.len, digest, &dn, EVP_sha256(), NULL) || dn != 32 ||
               memcmp(hash.data, digest, 32)))
        ok = diff_error(ctx, "%s sha256 mismatch", file);
    if (ok) ok = diff_hex(ctx, hex, &expected);
    if (ok && !diff_compare(ctx->id, expected.data, expected.len, bytes.data, bytes.len))
        ok = diff_error(ctx, "%s hex/sidecar mismatch", file);
    free(expected.data); free(hash.data);
    if (!ok) { free(bytes.data); return false; }
    v->type = D_BYTES; v->as.bytes = bytes;
    return true;
}

static diff_value *decode(diff_context *ctx, json_t *j, size_t depth, const char *id)
{
    if (depth > 64) { diff_error(ctx, "fixture nesting too deep"); return NULL; }
    snprintf(ctx->id, sizeof(ctx->id), "%s", id);
    diff_value *v = allocate(sizeof(*v));
    if (json_is_object(j) && json_object_get(j, "file")) {
        if (!blob(ctx, j, v)) goto fail;
    } else if (json_is_array(j) || json_is_object(j)) {
        v->type = json_is_array(j) ? D_ARRAY : D_OBJECT;
        size_t n = json_is_array(j) ? json_array_size(j) : json_object_size(j);
        v->as.children.items = allocate(n * sizeof(diff_value *));
        const char *key; json_t *item;
        if (v->type == D_ARRAY) {
            for (size_t i = 0; i < n; ++i) {
                char child_id[256];
                snprintf(child_id, sizeof(child_id), "%s/%zu", id, i);
                diff_value *child = decode(ctx, json_array_get(j, i), depth + 1, child_id);
                if (!child) goto fail;
                v->as.children.items[v->as.children.len++] = child;
            }
        } else json_object_foreach(j, key, item) {
            char child_id[256];
            snprintf(child_id, sizeof(child_id), "%s/%s", id, key);
            diff_value *child = decode(ctx, item, depth + 1, child_id);
            if (!child) goto fail;
            child->name = copy_text(key);
            v->as.children.items[v->as.children.len++] = child;
        }
    } else if (json_is_string(j)) {
        const char *s = json_string_value(j);
        if (strlen(s) != json_string_length(j)) { diff_error(ctx, "embedded NUL in fixture string"); goto fail; }
        if (strncmp(s, "~u64:", 5) == 0) {
            errno = 0; char *end;
            uintmax_t number = strtoumax(s + 5, &end, 10);
            if (errno || *end || end == s + 5 || number > UINT64_MAX) { diff_error(ctx, "u64 overflow"); goto fail; }
            v->type = D_UINT; v->as.number = (uint64_t)number;
        } else { v->type = D_TEXT; v->as.text = copy_text(s); }
    } else if (json_is_integer(j)) {
        json_int_t n = json_integer_value(j);
        if (n < 0) { v->type = D_INT; v->as.signed_number = (int64_t)n; }
        else { v->type = D_UINT; v->as.number = (uint64_t)n; }
    } else if (json_is_boolean(j)) { v->type = D_BOOL; v->as.boolean = json_is_true(j); }
    else if (json_is_null(j)) v->type = D_NULL;
    else { diff_error(ctx, "unexpected JSON value (integers must be lossless)"); goto fail; }
    return v;
fail:
    diff_free(v);
    return NULL;
}

diff_value *diff_load(diff_context *ctx, const char *family)
{
    char name[64];
    snprintf(name, sizeof(name), "%s.json", family);
    char *path = join(ctx->dir, name);
    diff_bytes raw = {0};
    bool ok = read_file(ctx, path, &raw); free(path);
    if (!ok) return NULL;
    json_error_t error;
    if (memchr(raw.data, 0, raw.len)) {
        free(raw.data); diff_error(ctx, "NUL in JSON document"); return NULL;
    }
    char *normalized = normalize(ctx, &raw);
    free(raw.data);
    if (!normalized) return NULL;
    json_t *j = json_loads(normalized, JSON_REJECT_DUPLICATES, &error);
    free(normalized);
    if (!j) { diff_error(ctx, "JSON line %d: %s", error.line, error.text); return NULL; }
    diff_value *v = NULL;
    json_t *schema = json_object_get(j, "schema_version");
    const char *version = string_field(j, "generator_version");
    if (!same_field(ctx, j, "pin", ctx->pin) || !same_field(ctx, j, "family", family) ||
        !same_field(ctx, j, "generator", "fips-vectors")) goto done;
    if (!json_is_integer(schema) || json_integer_value(schema) != 1 || !version || !*version ||
        !json_is_array(json_object_get(j, "gaps")) || !json_object_get(j, "cases")) {
        diff_error(ctx, "invalid schema_version/generator_version/gaps/cases"); goto done;
    }
    size_t i; json_t *gap;
    json_array_foreach(json_object_get(j, "gaps"), i, gap) {
        if (!json_is_string(gap)) { diff_error(ctx, "gap must be text"); goto done; }
        printf("GAP %s: %s\n", family, json_string_value(gap));
    }
    ctx->version = version;
    v = decode(ctx, json_object_get(j, "cases"), 0, family);
    ctx->version = NULL;
    if (v && !diff_schema(ctx, family, v)) { diff_free(v); v = NULL; }
done:
    json_decref(j);
    return v;
}

/* Used only by the negative CTest, on its private COPY of entropy fixtures. */
int diff_mutate(const char *mode, const char *dir)
{
    char *path = join(dir, "entropy.json");
    json_error_t error;
    json_t *root = json_load_file(path, JSON_REJECT_DUPLICATES, &error);
    if (!root) { free(path); return 1; }
    json_t *stream = json_object_get(json_array_get(json_object_get(root, "cases"), 0), "stream");
    int result = 1;
    if (strcmp(mode, "hex") == 0) {
        char *hex = copy_text(string_field(stream, "hex"));
        hex[0] = hex[0] == '0' ? '1' : '0';
        if (json_object_set_new(stream, "hex", json_string(hex)) == 0)
            result = json_dump_file(root, path, JSON_INDENT(2));
        free(hex);
    } else if (strcmp(mode, "bin") == 0) {
        char *bin = join(dir, string_field(stream, "file"));
        FILE *f = fopen(bin, "r+b");
        if (f) {
            int c = fgetc(f);
            if (c != EOF && fseek(f, 0, SEEK_SET) == 0 && fputc(c ^ 1, f) != EOF) result = 0;
            if (fclose(f) != 0) result = 1;
        }
        free(bin);
    }
    json_decref(root); free(path);
    return result == 0 ? 0 : 1;
}
