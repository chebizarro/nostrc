#include "control/render.h"
#include "control/protocol.h"
#include <limits.h>
#include <stdbool.h>
/* No fiber park/migration occurs in a render guard. These are not shared
 * failure flags and do not change Jansson's process-wide allocator hooks. */
static _Thread_local unsigned depth;
static _Thread_local bool failed, range_error;
void fipsd_control_render_begin(void)
{
    if (depth++ == 0) { failed = false; range_error = false; }
}
json_t *fipsd_control_render_end(json_t *result)
{
    if (!result) failed = true;
    --depth;
    if (!failed) return result;
    json_decref(result);
    if (depth) return NULL;
    return fipsd_control_error(range_error ? "value exceeds JSON integer range" : "response construction failed");
}
void fipsd_control_render_check(int result) { if (result < 0) failed = true; }
void fipsd_control_put(json_t *object, const char *key, json_t *value)
{
    fipsd_control_render_check(json_object_set_new(object, key, value));
}
void fipsd_control_append(json_t *array, json_t *value)
{
    fipsd_control_render_check(json_array_append_new(array, value));
}
json_t *fipsd_control_uint(uint64_t value)
{
    if (value > INT64_MAX) { failed = true; range_error = true; return NULL; }
    return json_integer((json_int_t)value);
}
