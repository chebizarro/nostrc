#ifndef FIPSD_CONTROL_PROTOCOL_H
#define FIPSD_CONTROL_PROTOCOL_H
#include "control/snapshot.h"
#define FIPSD_CONTROL_MAX_REQUEST 4096u
#define FIPSD_CONTROL_MAX_DEPTH 64u
#define FIPSD_CONTROL_IO_TIMEOUT_MS 5000
/* Owned JSON responses; caller decrefs. params/request are borrowed. */
json_t *fipsd_control_ok(json_t *data); /* consumes data */
json_t *fipsd_control_error(const char *message);
json_t *fipsd_control_dispatch(const fipsd_control_provider *provider, const json_t *request);
json_t *fipsd_control_parse(const fipsd_control_provider *provider, const char *line, size_t len);
/* Serde Response struct order: status, data/message; nested serde_json::Value
 * maps use lexical key order. Never sort the outer Response struct. */
char *fipsd_control_encode(const json_t *response);
json_t *fipsd_control_parse_context(const fipsd_control_provider *provider, const char *line, size_t len,
                                    const fipsd_control_request_context *context);
json_t *fipsd_control_query(const fipsd_control_snapshot *s, const char *name, const json_t *params);
const char *const *fipsd_control_commands(size_t *count);
json_t *fipsd_control_series(const fips_stats_series *series);
#endif
