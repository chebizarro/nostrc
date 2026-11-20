#include <fips/proto/mmp/limits.h>
_Static_assert(FIPS_MMP_MIN_OWD_WINDOW_SIZE >= 2, "regression needs two samples");
_Static_assert(FIPS_MMP_MIN_REACTIVE_PATH_MTU == FIPS_MMP_MIN_ACTIONABLE_PATH_MTU, "carrier floors");
