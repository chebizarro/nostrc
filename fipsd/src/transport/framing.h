#ifndef FIPSD_FRAMING_H
#define FIPSD_FRAMING_H
#include <fips/proto/fmp/wire.h>
/* FMP itself is the framing: no TCP/Tor-specific envelope. Outputs unchanged
 * on error. Stream flags and ciphertext are validated by the node, not here. */
fips_status fipsd_frame_size(const uint8_t prefix[4], uint16_t ceiling, size_t *size);
fips_status fipsd_datagram_frame(const uint8_t *, size_t, uint16_t ceiling);
#endif
