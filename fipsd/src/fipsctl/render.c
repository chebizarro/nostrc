/* Output: the serde_json pretty printer fipsctl's JSON is byte-compatible
 * with, the response printer, the stats sparkline plot and small string
 * helpers shared by the other modules. */
#include "fipsctl.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *fipsctl_aprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) { va_end(ap); return NULL; }
    char *out = malloc((size_t)n + 1);
    if (out) vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

char *fipsctl_strerror(int err)
{
    return fipsctl_aprintf("%s (os error %d)", strerror(err), err);
}

size_t fipsctl_utf8_chars(const char *text)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

char *fipsctl_take_chars(const char *text, size_t count)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t seen = 0, bytes = 0;
    while (p[bytes]) {
        if ((p[bytes] & 0xC0) != 0x80) {
            if (seen == count) break;
            ++seen;
        }
        ++bytes;
    }
    char *out = malloc(bytes + 1);
    if (!out) return NULL;
    memcpy(out, text, bytes);
    out[bytes] = 0;
    return out;
}

/* ---- a growable string -------------------------------------------------- */

typedef struct strbuf { char *data; size_t len, cap; bool failed; } strbuf;

static void sb_reserve(strbuf *b, size_t extra)
{
    if (b->failed) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *grown = realloc(b->data, cap);
    if (!grown) { b->failed = true; return; }
    b->data = grown; b->cap = cap;
}

static void sb_putn(strbuf *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    if (b->failed) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void sb_puts(strbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(strbuf *b, char c) { sb_putn(b, &c, 1); }

static char *sb_finish(strbuf *b)
{
    if (b->failed) { free(b->data); return NULL; }
    if (!b->data) return strdup("");
    return b->data;
}

/* ---- numbers -------------------------------------------------------------- */

/* The shortest digit string that reads back as `value`, laid out as ryu's
 * pretty formatter (what serde_json prints for an f64): plain decimal
 * notation with a `.0` for integral values up to 1e16, otherwise `d.ddde-N`
 * with the sign of the exponent spelled out (`1e+16`, `1e-6`). */
char *fipsctl_format_f64(double value)
{
    if (value == 0) return strdup(signbit(value) ? "-0.0" : "0.0");
    if (!isfinite(value)) return NULL; /* serde_json refuses these */
    char buf[64];
    int precision = 0;
    for (; precision <= 17; ++precision) {
        snprintf(buf, sizeof(buf), "%.*e", precision, value);
        if (strtod(buf, NULL) == value) break;
    }
    bool negative = buf[0] == '-';
    const char *p = buf + (negative ? 1 : 0);
    char digits[32];
    size_t length = 0;
    for (; *p && *p != 'e'; ++p)
        if (*p != '.') digits[length++] = *p;
    digits[length] = 0;
    long exp10 = strtol(p + 1, NULL, 10);
    /* value = digits * 10^k, with trailing zeros folded into k. */
    long k = exp10 - (long)(length - 1);
    while (length > 1 && digits[length - 1] == '0') { digits[--length] = 0; ++k; }
    long kk = (long)length + k;
    strbuf b = {0};
    if (negative) sb_putc(&b, '-');
    if (0 <= k && kk <= 16) {
        sb_puts(&b, digits);
        for (long i = 0; i < k; ++i) sb_putc(&b, '0');
        sb_puts(&b, ".0");
    } else if (0 < kk && kk <= 16) {
        sb_putn(&b, digits, (size_t)kk);
        sb_putc(&b, '.');
        sb_puts(&b, digits + kk);
    } else if (-5 < kk && kk <= 0) {
        sb_puts(&b, "0.");
        for (long i = 0; i < -kk; ++i) sb_putc(&b, '0');
        sb_puts(&b, digits);
    } else {
        sb_putc(&b, digits[0]);
        if (length > 1) { sb_putc(&b, '.'); sb_puts(&b, digits + 1); }
        char e[16];
        snprintf(e, sizeof(e), kk - 1 >= 0 ? "e+%ld" : "e%ld", kk - 1);
        sb_puts(&b, e);
    }
    return sb_finish(&b);
}

/* ---- JSON ----------------------------------------------------------------- */

static bool exceeds_i64(const char *digits, size_t n)
{
    if (n > 20) return false; /* beyond u64 too: let the parser refuse it */
    if (n < 19) return false;
    if (n == 19) return strncmp(digits, "9223372036854775807", 19) > 0;
    return strncmp(digits, "18446744073709551615", 20) <= 0;
}

json_t *fipsctl_json_load(const char *text, size_t len, json_error_t *error)
{
    strbuf b = {0};
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len;) {
        char c = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            sb_putc(&b, c); ++i;
            continue;
        }
        if (c == '"') { quoted = true; sb_putc(&b, c); ++i; continue; }
        if (c >= '0' && c <= '9' && (i == 0 || !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '.' || text[i - 1] == '-' || text[i - 1] == '+'))) {
            size_t j = i;
            while (j < len && text[j] >= '0' && text[j] <= '9') ++j;
            bool integer = j >= len || (text[j] != '.' && text[j] != 'e' && text[j] != 'E');
            if (integer && exceeds_i64(text + i, j - i)) {
                sb_puts(&b, "\"\\u0001u64:");
                sb_putn(&b, text + i, j - i);
                sb_putc(&b, '"');
            } else sb_putn(&b, text + i, j - i);
            i = j;
            continue;
        }
        sb_putc(&b, c); ++i;
    }
    if (b.failed) { free(b.data); return NULL; }
    json_t *value = json_loadb(b.data ? b.data : "", b.len, JSON_DECODE_ANY, error);
    free(b.data);
    return value;
}

static void write_string(strbuf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                char esc[7] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15], 0};
                sb_puts(b, esc);
            } else sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

static int compare_keys(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void indent(strbuf *b, int depth)
{
    for (int i = 0; i < depth * 2; ++i) sb_putc(b, ' ');
}

static void write_value(strbuf *b, const json_t *v, int depth, bool pretty)
{
    switch (json_typeof(v)) {
    case JSON_OBJECT: {
        size_t n = json_object_size(v);
        if (!n) { sb_puts(b, "{}"); return; }
        const char **keys = malloc(n * sizeof(*keys));
        if (!keys) { b->failed = true; return; }
        size_t i = 0;
        const char *k; json_t *item;
        json_object_foreach((json_t *)v, k, item) keys[i++] = k;
        qsort(keys, n, sizeof(*keys), compare_keys);
        sb_putc(b, '{');
        for (i = 0; i < n; ++i) {
            if (i) sb_putc(b, ',');
            if (pretty) { sb_putc(b, '\n'); indent(b, depth + 1); }
            write_string(b, keys[i]);
            sb_puts(b, pretty ? ": " : ":");
            write_value(b, json_object_get(v, keys[i]), depth + 1, pretty);
        }
        free(keys);
        if (pretty) { sb_putc(b, '\n'); indent(b, depth); }
        sb_putc(b, '}');
        return;
    }
    case JSON_ARRAY: {
        size_t n = json_array_size(v);
        if (!n) { sb_puts(b, "[]"); return; }
        sb_putc(b, '[');
        for (size_t i = 0; i < n; ++i) {
            if (i) sb_putc(b, ',');
            if (pretty) { sb_putc(b, '\n'); indent(b, depth + 1); }
            write_value(b, json_array_get(v, i), depth + 1, pretty);
        }
        if (pretty) { sb_putc(b, '\n'); indent(b, depth); }
        sb_putc(b, ']');
        return;
    }
    case JSON_STRING: {
        const char *s = json_string_value(v);
        if (!strncmp(s, FIPSCTL_U64_TAG, sizeof(FIPSCTL_U64_TAG) - 1)) sb_puts(b, s + sizeof(FIPSCTL_U64_TAG) - 1);
        else write_string(b, s);
        return;
    }
    case JSON_INTEGER: {
        char num[32];
        snprintf(num, sizeof(num), "%lld", (long long)json_integer_value(v));
        sb_puts(b, num);
        return;
    }
    case JSON_REAL: {
        char *num = fipsctl_format_f64(json_real_value(v));
        if (!num) { sb_puts(b, "null"); return; }
        sb_puts(b, num);
        free(num);
        return;
    }
    case JSON_TRUE: sb_puts(b, "true"); return;
    case JSON_FALSE: sb_puts(b, "false"); return;
    case JSON_NULL: default: sb_puts(b, "null"); return;
    }
}

char *fipsctl_json_pretty(const json_t *value)
{
    strbuf b = {0};
    write_value(&b, value, 0, true);
    return sb_finish(&b);
}

char *fipsctl_json_compact(const json_t *value)
{
    strbuf b = {0};
    write_value(&b, value, 0, false);
    return sb_finish(&b);
}

/* ---- responses ------------------------------------------------------------ */

int fipsctl_print_response(FILE *out, FILE *err, const json_t *value)
{
    const char *status = json_string_value(json_object_get(value, "status"));
    if (!status) status = "unknown";
    if (!strcmp(status, "error")) {
        const char *message = json_string_value(json_object_get(value, "message"));
        fprintf(err, "error: %s\n", message ? message : "unknown error");
        return 1;
    }
    const json_t *data = json_object_get(value, "data");
    char *text = fipsctl_json_pretty(data ? data : value);
    if (!text) { fprintf(err, "error: out of memory\n"); return 1; }
    fprintf(out, "%s\n", text);
    free(text);
    return 0;
}

/* Eight block levels; a constant series and an empty one render at `▄`. */
char *fipsctl_sparkline(const double *values, size_t count, double min, double max)
{
    static const char *const BLOCKS[8] = {"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                          "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"};
    double range = max - min;
    strbuf b = {0};
    for (size_t i = 0; i < count; ++i) {
        double v = values[i];
        if (isnan(v)) sb_putc(&b, ' ');
        else if (!isfinite(range) || range <= 0.0) sb_puts(&b, BLOCKS[3]);
        else {
            double norm = (v - min) / range;
            if (norm < 0.0) norm = 0.0;
            if (norm > 1.0) norm = 1.0;
            size_t idx = (size_t)round(norm * 7.0);
            sb_puts(&b, BLOCKS[idx > 7 ? 7 : idx]);
        }
    }
    return sb_finish(&b);
}

int fipsctl_print_plot(FILE *out, FILE *err, const json_t *value, const char *metric)
{
    const char *status = json_string_value(json_object_get(value, "status"));
    if (status && !strcmp(status, "error")) {
        const char *message = json_string_value(json_object_get(value, "message"));
        fprintf(err, "error: %s\n", message ? message : "unknown error");
        return 1;
    }
    const json_t *data = json_object_get(value, "data");
    if (!data) { fprintf(err, "error: no data in response\n"); return 1; }
    const json_t *array = json_object_get(data, "values");
    size_t count = json_is_array(array) ? json_array_size(array) : 0;
    const char *unit = json_string_value(json_object_get(data, "unit"));
    if (!unit) unit = "";
    const json_t *g = json_object_get(data, "granularity_seconds");
    uint64_t granularity = json_is_integer(g) && json_integer_value(g) >= 0 ? (uint64_t)json_integer_value(g) : 1;
    if (!count) { fprintf(out, "%s: no data yet\n", metric); return 0; }
    double *values = malloc(count * sizeof(*values));
    if (!values) { fprintf(err, "error: out of memory\n"); return 1; }
    double min = INFINITY, max = -INFINITY, last = NAN;
    size_t gaps = 0;
    for (size_t i = 0; i < count; ++i) {
        const json_t *v = json_array_get(array, i);
        values[i] = json_is_number(v) ? json_number_value(v) : NAN;
        if (isnan(values[i])) { ++gaps; continue; }
        if (values[i] < min) min = values[i];
        if (values[i] > max) max = values[i];
    }
    if (!isfinite(min)) { min = 0.0; max = 0.0; }
    for (size_t i = count; i > 0; --i)
        if (!isnan(values[i - 1])) { last = values[i - 1]; break; }
    fprintf(out, "%s (%s) \xe2\x80\x94 %zu samples @ %llus = %llus window", metric, unit, count,
            (unsigned long long)granularity, (unsigned long long)((uint64_t)count * granularity));
    if (gaps) fprintf(out, " (%zu gaps)", gaps);
    fputc('\n', out);
    if (isnan(last)) fprintf(out, "  min=%.3f max=%.3f last=-\n", min, max);
    else fprintf(out, "  min=%.3f max=%.3f last=%.3f\n", min, max, last);
    char *spark = fipsctl_sparkline(values, count, min, max);
    fprintf(out, "  %s\n", spark ? spark : "");
    free(spark);
    free(values);
    return 0;
}
