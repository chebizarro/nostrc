#ifndef FIPSTOP_MODEL_H
#define FIPSTOP_MODEL_H
#include "fipsctl.h"
#include <stddef.h>
#define FT_TEXT 96
/* Display order and cross-reference-only surfaces match upstream App::Tab. */
typedef enum ft_tab { FT_NODE, FT_PEERS, FT_TRANSPORTS, FT_SESSIONS, FT_TREE,
    FT_BLOOM, FT_MMP, FT_ROUTING, FT_GRAPHS, FT_GATEWAY, FT_LINKS, FT_CACHE, FT_TABS } ft_tab;
extern const char *const ft_labels[FT_TABS];
extern const char *const ft_commands[FT_TABS];
extern const char *const ft_windows[4];
extern const char *const ft_granularities[4];
extern const char *const ft_metrics[10];
extern const char *const ft_peer_metrics[7];
json_t *ft_get(const json_t *data, const char *path);
const char *ft_str(const json_t *data, const char *path);
uint64_t ft_uint(const json_t *data, const char *path);
double ft_num(const json_t *data, const char *path);
void ft_value(const json_t *data, const char *path, char *out, size_t cap);
void ft_bytes(uint64_t bytes, char out[FT_TEXT]);
void ft_throughput(double rate, char out[FT_TEXT]);
void ft_duration(uint64_t ms, char out[FT_TEXT]);
void ft_elapsed(uint64_t ms, uint64_t now, char out[FT_TEXT]);
void ft_uptime(uint64_t secs, char out[FT_TEXT]);
char *ft_truncate_hex(const char *s, size_t width);
char *ft_truncate(const char *s, size_t width, bool pad);
size_t ft_clamp_scroll(size_t offset, size_t content, size_t visible);
unsigned ft_group(const json_t *peer);
/* Stable ordering: role then LQI (missing sorts last). Borrowed row refs. */
json_t **ft_peer_order(const json_t *peers, size_t *count);
/* Histories already contain rates, not byte counters; null means no sample. */
size_t ft_series(const json_t *values, double **out);
typedef struct ft_summary { double min, max, last; size_t n; } ft_summary;
ft_summary ft_summarize(const double *values, size_t n);
void ft_resample(const double *values, size_t n, double *out, size_t columns);
char *ft_spark(const json_t *values);
#endif
