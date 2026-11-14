#include <fips/core.h>
#include <libgo/fiber.h>
#include <stdio.h>
#include <stdlib.h>

#include "FIPSD-version.h"

int main(void)
{
    if (gof_start_background(0) != 0) {
        fputs("fipsd: cannot start fiber runtime\n", stderr);
        return EXIT_FAILURE;
    }

    int printed = printf("fipsd %s (libfips-core %s)\n",
                         FIPSD_VERSION, fips_core_version());
    gof_request_stop();
    if (gof_join_background() != 0) {
        fputs("fipsd: cannot join fiber runtime\n", stderr);
        return EXIT_FAILURE;
    }
    return printed < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
