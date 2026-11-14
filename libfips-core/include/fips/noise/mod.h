#ifndef FIPS_NOISE_MOD_H
#define FIPS_NOISE_MOD_H

/* Pinned upstream noise::{TAG_SIZE, aead::NONCE_SIZE, MAX_MESSAGE_SIZE}.
 * MAX_MESSAGE_SIZE includes the authentication tag. Handshake sizes follow later. */
#define FIPS_NOISE_TAG_SIZE 16u
#define FIPS_NOISE_NONCE_SIZE 12u
#define FIPS_NOISE_MAX_MESSAGE_SIZE 65535u

#endif
