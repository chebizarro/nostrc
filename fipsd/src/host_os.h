#ifndef FIPSD_HOST_OS_H
#define FIPSD_HOST_OS_H

#include <fips/host.h>

/* Stateless adapters with process-lifetime ops; no clock/RNG read at construction.
 * CLOCK_MONOTONIC is available on supported POSIX hosts, including modern macOS.
 */
fips_clock fipsd_host_os_clock(void);
fips_entropy fipsd_host_os_entropy(void);

#endif
