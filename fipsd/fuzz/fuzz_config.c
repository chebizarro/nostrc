#include "config/config.h"
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fipsd_config_log log = {0};
    /* The file loader calls this same bounded YAML/normalization pipeline.
     * No filesystem writes, environment mutations, daemon or identity startup. */
    fipsd_config *config = fipsd_config_parse((const char *)data, size, &log);
    fipsd_config_free(config);
    fipsd_config_log_clear(&log);
    return 0;
}
