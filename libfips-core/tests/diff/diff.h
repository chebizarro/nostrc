#ifndef FIPS_DIFF_H
#define FIPS_DIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owned, lossless C representation of the exporter records. No json_t escapes
 * loader.c. Schema validation converts hex fields to bytes/uint64_t in place. */
typedef struct diff_bytes { unsigned char *data; size_t len; } diff_bytes;
typedef enum diff_type {
    D_NULL, D_BOOL, D_UINT, D_INT, D_TEXT, D_BYTES, D_ARRAY, D_OBJECT
} diff_type;
typedef struct diff_value diff_value;
struct diff_value {
    diff_type type;
    char *name;
    union {
        bool boolean;
        uint64_t number;
        int64_t signed_number;
        char *text;
        diff_bytes bytes;
        struct { diff_value **items; size_t len; } children;
    } as;
};
typedef struct diff_context {
    const char *dir, *pin, *version;
    char id[256];
    char error[512];
} diff_context;

diff_value *diff_load(diff_context *ctx, const char *family);
void diff_free(diff_value *v);
diff_value *diff_get(const diff_value *v, const char *name);
bool diff_error(diff_context *ctx, const char *format, ...);
bool diff_hex(diff_context *ctx, const char *text, diff_bytes *out);
bool diff_word(diff_context *ctx, const char *text, uint64_t *out);
bool diff_compare(const char *id, const unsigned char *expected, size_t en,
                  const unsigned char *actual, size_t an);
/* Contract transcribed from tools/fips-vectors/src/{basic,noise,proto,rekey}.rs. */
bool diff_schema(diff_context *ctx, const char *family, diff_value *cases);
int diff_dispatch(diff_context *ctx, const char *family, diff_value *cases);
int diff_mutate(const char *mode, const char *dir);

#define FIELD(v, key) diff_get((v), (key))
#define COUNT(v) ((v)->as.children.len)
#define ITEM(v, i) ((v)->as.children.items[(i)])
#define TEXT(v, key) (FIELD((v), (key))->as.text)
#define UINT(v, key) (FIELD((v), (key))->as.number)
#define BYTES(v, key) (FIELD((v), (key))->as.bytes)
#endif
