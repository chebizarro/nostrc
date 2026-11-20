#include "transport/framing.h"
fips_status fipsd_frame_size(const uint8_t p[4], uint16_t ceiling, size_t *size)
{
    if (!p || !size || (p[0] >> 4) != FIPS_FMP_VERSION) return FIPS_ERR_RANGE;
    size_t payload = (size_t)p[2] | ((size_t)p[3] << 8), n;
    switch (p[0] & 15) {
    case FIPS_FMP_PHASE_ESTABLISHED:
        n = FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE + payload;
        if (n > ceiling) return FIPS_ERR_RANGE;
        break;
    case FIPS_FMP_PHASE_MSG1:
        n = FIPS_FRAMING_FMP_MSG1_WIRE_SIZE;
        if (payload != n - 4) return FIPS_ERR_RANGE;
        break;
    case FIPS_FMP_PHASE_MSG2:
        n = FIPS_FRAMING_FMP_MSG2_WIRE_SIZE;
        if (payload != n - 4) return FIPS_ERR_RANGE;
        break;
    default: return FIPS_ERR_RANGE;
    }
    *size = n;
    return FIPS_OK;
}
fips_status fipsd_datagram_frame(const uint8_t *p, size_t len, uint16_t ceiling)
{
    size_t n;
    if (len < 4 || fipsd_frame_size(p, ceiling, &n) != FIPS_OK || n != len)
        return FIPS_ERR_RANGE;
    return fips_fmp_validate_frame(p, len, NULL);
}
