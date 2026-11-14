#ifndef FIPS_CORE_H
#define FIPS_CORE_H

#include <fips/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Return the library's semantic version as a static, immutable string. */
FIPS_CORE_API const char *fips_core_version(void);

#ifdef __cplusplus
}
#endif

#endif /* FIPS_CORE_H */
