#include <fips/proto/framing.h>
#include <fips/proto/link.h>

/* Every payload limit the node reports derives from these figures; a framing
 * change that moves one has to be made on purpose (framing.rs const assert). */
_Static_assert(FIPS_FRAMING_NOISE_EPOCH_ENCRYPTED_SIZE ==
    FIPS_FRAMING_NOISE_EPOCH_SIZE + FIPS_FRAMING_NOISE_TAG_SIZE, "noise epoch");
_Static_assert(FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE ==
    2 * FIPS_FRAMING_NOISE_PUBKEY_SIZE + FIPS_FRAMING_NOISE_TAG_SIZE +
    FIPS_FRAMING_NOISE_EPOCH_ENCRYPTED_SIZE, "noise msg1");
_Static_assert(FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE ==
    FIPS_FRAMING_NOISE_PUBKEY_SIZE + FIPS_FRAMING_NOISE_EPOCH_ENCRYPTED_SIZE, "noise msg2");
_Static_assert(FIPS_FRAMING_FMP_MSG1_WIRE_SIZE ==
    FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 4 + FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE, "fmp msg1");
_Static_assert(FIPS_FRAMING_FMP_MSG2_WIRE_SIZE ==
    FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 4 + 4 + FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE, "fmp msg2");
_Static_assert(FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE ==
    FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE + FIPS_FRAMING_NOISE_TAG_SIZE, "fmp min");
_Static_assert(FIPS_FRAMING_FMP_FRAME_OVERHEAD ==
    FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE + (FIPS_FRAMING_FMP_INNER_HEADER_SIZE - 1) +
    FIPS_FRAMING_NOISE_TAG_SIZE, "fmp frame overhead");
_Static_assert(FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE ==
    FIPS_FRAMING_FSP_HEADER_SIZE + FIPS_FRAMING_FSP_TAG_SIZE, "fsp min");
/* The datagram's msg_type byte is also the last FMP inner-header byte: once. */
_Static_assert(FIPS_OVERHEAD ==
    FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE + FIPS_FRAMING_FMP_INNER_HEADER_SIZE +
    (FIPS_SESSION_DATAGRAM_HEADER_SIZE - 1) + FIPS_FRAMING_FSP_HEADER_SIZE +
    FIPS_FRAMING_FSP_INNER_HEADER_SIZE + FIPS_FRAMING_FSP_TAG_SIZE + FIPS_FRAMING_NOISE_TAG_SIZE,
    "the FIPS framing overhead is no longer 106 bytes");
_Static_assert(FIPS_OVERHEAD == 106, "the FIPS framing overhead is no longer 106 bytes");

static uint16_t saturating_sub(uint16_t a, uint16_t b)
{
    return a > b ? (uint16_t)(a - b) : 0;
}

uint16_t fips_framing_payload_limit(uint16_t path_mtu)
{
    return saturating_sub(saturating_sub(path_mtu, FIPS_OVERHEAD),
                          FIPS_FRAMING_FSP_PORT_HEADER_SIZE);
}
