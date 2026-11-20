/* Upstream transport/ble/psm.rs: the L2CAP listener PSM on the air. A platform
 * that assigns PSMs (CoreBluetooth, Android) cannot be dialled at a configured
 * constant, so the listener's PSM rides the primary advertisement as Service
 * Data keyed on the 16-bit UUID 0x9C90 (the leading 16 bits of the FIPS
 * service UUID), two bytes little-endian; a backend that cannot advertise
 * service data serves the same bytes from the GATT PSM characteristic of the
 * FIPS service instead. State-free: learning and caching belong to the probe
 * loop. A reader ignores trailing bytes; absent or truncated data decodes to
 * nothing, which every legacy UUID-only advertiser produces. */
#include "transport/ble/internal.h"
/* Flags 3 + 128-bit UUID list 18 + 16-bit keyed service data 6 = 27 bytes:
 * a 128-bit key would need 20 for the service data and overflow by 10. */
_Static_assert(FIPSD_BLE_ADV_FLAGS_BYTES + FIPSD_BLE_ADV_UUID128_BYTES + FIPSD_BLE_ADV_PSM_BYTES <=
    FIPSD_BLE_LEGACY_ADV_BYTES, "PSM advert layout exceeds the 31-byte legacy advertising PDU");
_Static_assert(FIPSD_BLE_ADV_PSM_BYTES == 2 + 2 + 2, "service data AD: length, type, 16-bit key, PSM");
void fipsd_ble_encode_psm(uint16_t psm, uint8_t out[2]) { out[0] = (uint8_t)psm; out[1] = (uint8_t)(psm >> 8); }
bool fipsd_ble_decode_psm(const uint8_t *p, size_t n, uint16_t *psm)
{
    if (!p || n < 2) return false;
    *psm = (uint16_t)(p[0] | (p[1] << 8));
    return true;
}
/* Zero is not a valid PSM: dialling it would fail where falling back to the
 * configured PSM might not. */
bool fipsd_ble_decode_gatt_psm(const uint8_t *p, size_t n, uint16_t *psm)
{
    uint16_t v;
    if (!fipsd_ble_decode_psm(p, n, &v) || !v) return false;
    *psm = v;
    return true;
}
