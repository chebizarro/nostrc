#include "model.h"
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
const char *const ft_labels[FT_TABS] = {"Node", "Peers", "Transports", "Sessions", "Tree",
    "Filters", "Performance", "Routing", "Graphs", "Gateway", "Links", "Cache"};
const char *const ft_commands[FT_TABS] = {"show_status", "show_peers", "show_transports",
    "show_sessions", "show_tree", "show_bloom", "show_mmp", "show_routing",
    "show_stats_all_history", "show_gateway", "show_links", "show_cache"};
const char *const ft_windows[4] = {"1m", "10m", "1h", "24h"};
const char *const ft_granularities[4] = {"1s", "1s", "1s", "1m"};
const char *const ft_metrics[10] = {"mesh_size", "tree_depth", "peer_count", "parent_switches",
    "bytes_in", "bytes_out", "packets_in", "packets_out", "loss_rate", "active_sessions"};
const char *const ft_peer_metrics[7] = {"srtt_ms", "loss_rate", "bytes_in", "bytes_out",
    "packets_in", "packets_out", "ecn_ce"};
json_t *ft_get(const json_t *data, const char *path)
{
    const json_t *v = data;
    while (v && *path) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= 128) return NULL;
        char key[128]; memcpy(key, path, n); key[n] = 0;
        v = json_object_get(v, key);
        if (!dot) break;
        path = dot + 1;
    }
    return (json_t *)v;
}
const char *ft_str(const json_t *data, const char *path)
{
    const char *s = json_string_value(ft_get(data, path));
    return s ? s : "-";
}
uint64_t ft_uint(const json_t *data, const char *path)
{
    json_t *v = ft_get(data, path);
    const char *s = json_string_value(v);
    if (s && !strncmp(s, FIPSCTL_U64_TAG, strlen(FIPSCTL_U64_TAG)))
        return strtoull(s + strlen(FIPSCTL_U64_TAG), NULL, 10);
    json_int_t n = json_integer_value(v);
    return n < 0 ? 0 : (uint64_t)n;
}
double ft_num(const json_t *data, const char *path)
{
    json_t *v = ft_get(data, path);
    return json_is_number(v) ? json_number_value(v) : NAN;
}
void ft_value(const json_t *data, const char *path, char *out, size_t cap)
{
    json_t *v = ft_get(data, path);
    if (json_is_string(v)) {
        const char *s = json_string_value(v);
        if (!strncmp(s, FIPSCTL_U64_TAG, strlen(FIPSCTL_U64_TAG))) s += strlen(FIPSCTL_U64_TAG);
        snprintf(out, cap, "%s", s);
    } else if (json_is_boolean(v)) snprintf(out, cap, "%s", json_is_true(v) ? "yes" : "no");
    else if (json_is_integer(v)) snprintf(out, cap, "%" JSON_INTEGER_FORMAT, json_integer_value(v));
    else if (json_is_real(v)) snprintf(out, cap, "%.3f", json_real_value(v));
    else snprintf(out, cap, "-");
}
void ft_bytes(uint64_t n, char out[FT_TEXT])
{
    if (n < 1024) snprintf(out, FT_TEXT, "%" PRIu64 "B", n);
    else if (n < 1048576) snprintf(out, FT_TEXT, "%.1fKB", (double)n / 1024.0);
    else if (n < 1073741824) snprintf(out, FT_TEXT, "%.1fMB", (double)n / 1048576.0);
    else snprintf(out, FT_TEXT, "%.2fGB", (double)n / 1073741824.0);
}
void ft_throughput(double n, char out[FT_TEXT])
{
    if (!isfinite(n) || n < 0) { snprintf(out, FT_TEXT, "0 B/s"); return; }
    const char *unit = "B/s";
    if (n >= 1e9) { n /= 1e9; unit = "GB/s"; }
    else if (n >= 1e6) { n /= 1e6; unit = "MB/s"; }
    else if (n >= 1e3) { n /= 1e3; unit = "KB/s"; }
    snprintf(out, FT_TEXT, "%.*f %s", n >= 100 ? 0 : n >= 10 ? 1 : 2, n, unit);
}
void ft_duration(uint64_t ms, char out[FT_TEXT])
{
    if (ms < 1000) snprintf(out, FT_TEXT, "%" PRIu64 "ms", ms);
    else if (ms < 60000) snprintf(out, FT_TEXT, "%.1fs", (double)ms / 1000);
    else if (ms < 3600000) snprintf(out, FT_TEXT, "%.1fm", (double)ms / 60000);
    else snprintf(out, FT_TEXT, "%.1fh", (double)ms / 3600000);
}
void ft_elapsed(uint64_t ms, uint64_t now, char out[FT_TEXT])
{
    if (!ms || ms > now) snprintf(out, FT_TEXT, "-");
    else ft_duration(now - ms, out);
}
void ft_uptime(uint64_t secs, char out[FT_TEXT])
{
    uint64_t d = secs / 86400, h = secs % 86400 / 3600, m = secs % 3600 / 60, s = secs % 60;
    if (d) snprintf(out, FT_TEXT, "%" PRIu64 "d %" PRIu64 "h %" PRIu64 "m %" PRIu64 "s", d, h, m, s);
    else if (h) snprintf(out, FT_TEXT, "%" PRIu64 "h %" PRIu64 "m %" PRIu64 "s", h, m, s);
    else if (m) snprintf(out, FT_TEXT, "%" PRIu64 "m %" PRIu64 "s", m, s);
    else snprintf(out, FT_TEXT, "%" PRIu64 "s", s);
}
char *ft_truncate(const char *s, size_t width, bool pad)
{
    size_t n = fipsctl_utf8_chars(s);
    size_t take = n > width ? (width ? width - 1 : 0) : n;
    char *head = fipsctl_take_chars(s, take);
    if (!head) return NULL;
    size_t spaces = pad && n < width ? width - n : 0;
    size_t cap = strlen(head) + spaces + 4;
    char *out = malloc(cap);
    if (out) {
        snprintf(out, cap, "%s%s", head, n > width && width ? "…" : "");
        size_t len = strlen(out);
        memset(out + len, ' ', spaces); out[len + spaces] = 0;
    }
    free(head); return out;
}
size_t ft_clamp_scroll(size_t off, size_t rows, size_t visible)
{ size_t max = rows > visible ? rows - visible : 0; return off < max ? off : max; }
unsigned ft_group(const json_t *p)
{ return json_is_true(json_object_get(p, "is_parent")) ? 0 : json_is_true(json_object_get(p, "is_child")) ? 1 : 2; }
static int peer_cmp(const json_t *a, const json_t *b)
{
    unsigned ga = ft_group(a), gb = ft_group(b);
    if (ga != gb) return ga < gb ? -1 : 1;
    double x = ft_num(a, "mmp.lqi"), y = ft_num(b, "mmp.lqi");
    if (isnan(x)) return isnan(y) ? 0 : 1;
    if (isnan(y)) return -1;
    return x < y ? -1 : x > y ? 1 : 0;
}
json_t **ft_peer_order(const json_t *peers, size_t *count)
{
    *count = json_array_size(peers);
    json_t **out = calloc(*count ? *count : 1, sizeof(*out));
    if (!out) { *count = 0; return NULL; }
    for (size_t i = 0; i < *count; ++i) {
        json_t *v = json_array_get(peers, i); size_t j = i;
        while (j && peer_cmp(out[j - 1], v) > 0) { out[j] = out[j - 1]; --j; }
        out[j] = v;
    }
    return out;
}
size_t ft_series(const json_t *values, double **out)
{
    size_t n = json_array_size(values);
    *out = calloc(n ? n : 1, sizeof(**out));
    if (!*out) return 0;
    for (size_t i = 0; i < n; ++i) {
        json_t *v = json_array_get(values, i);
        (*out)[i] = json_is_number(v) ? json_number_value(v) : NAN;
    }
    return n;
}
ft_summary ft_summarize(const double *v, size_t n)
{
    ft_summary s = {INFINITY, -INFINITY, n ? NAN : 0, n};
    for (size_t i = 0; i < n; ++i) if (isfinite(v[i])) {
        s.min = fmin(s.min, v[i]); s.max = fmax(s.max, v[i]); s.last = v[i];
    }
    if (!isfinite(s.min)) s.min = s.max = 0;
    return s;
}
void ft_resample(const double *v, size_t n, double *out, size_t columns)
{
    for (size_t i = 0; i < columns; ++i) {
        if (!n) { out[i] = NAN; continue; }
        if (n == 1) { out[i] = v[0]; continue; }
        if (n == columns) { out[i] = v[i]; continue; }
        double src = (double)i * (double)(n - 1) / (double)(columns > 1 ? columns - 1 : 1);
        size_t lo = (size_t)floor(src), hi = (size_t)ceil(src);
        if (hi >= n) hi = n - 1;
        double frac = src - (double)lo;
        out[i] = v[lo] * (1 - frac) + v[hi] * frac;
    }
}
char *ft_spark(const json_t *values)
{
    double *v = NULL; size_t n = ft_series(values, &v);
    ft_summary s = ft_summarize(v, n);
    char *out = fipsctl_sparkline(v, n, s.min, s.max);
    free(v); return out;
}

char *ft_truncate_hex(const char *s, size_t width)
{
    if(strlen(s)<=width) return strdup(s);
    char *head=fipsctl_take_chars(s,width>3?width-3:0);
    if(!head) return NULL;
    char *out=fipsctl_aprintf("%s...",head); free(head); return out;
}

bool ft_locale(void)
{
    /* Numeric output remains Rust-compatible, independent of the UI locale. */
    setlocale(LC_NUMERIC,"C");
    setlocale(LC_CTYPE,"");
    if(MB_CUR_MAX>1) return true;
    if(setlocale(LC_CTYPE,"C.UTF-8") && MB_CUR_MAX>1) return true;
    if(setlocale(LC_CTYPE,"en_US.UTF-8") && MB_CUR_MAX>1) return true;
    return false;
}
