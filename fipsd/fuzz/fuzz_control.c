#include "control/protocol.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    json_t *response = fipsd_control_parse(NULL, (const char *)data, size);
    if (!response) return 0; /* allocation failure */
    char *wire = fipsd_control_encode(response);
    if (wire) {
        json_error_t error;
        json_t *decoded = json_loadb(wire, strlen(wire), JSON_REJECT_DUPLICATES, &error);
        if (!decoded || !json_equal(response, decoded)) abort();
        json_decref(decoded);
        free(wire);
    }
    json_decref(response);
    return 0;
}
