#ifndef FIPSD_CONTROL_RENDER_H
#define FIPSD_CONTROL_RENDER_H
#include <jansson.h>
#include <stdint.h>
/* Serialization is CPU-only: no yielding, callbacks or blocking I/O between
 * begin/end. Nested series/report builders share the caller's error guard. */
void fipsd_control_render_begin(void);
json_t *fipsd_control_render_end(json_t *result);
void fipsd_control_put(json_t *object, const char *key, json_t *value);
void fipsd_control_append(json_t *array, json_t *value);
void fipsd_control_render_check(int result);
json_t *fipsd_control_uint(uint64_t value);
#endif
