#include "nostr/nip19.h"
#include "bech32.h"
#include "event.h"
#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper function to convert hex to binary
static int hex_to_bin(const char *hex, uint8_t *bin, size_t bin_len) {
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || bin_len < hex_len / 2) {
        return -1;
    }
    for (size_t i = 0; i < hex_len; i += 2) {
        sscanf(hex + i, "%2hhx", &bin[i / 2]);
    }
    return 0;
}

// Helper function to convert binary to hex
static void bin_to_hex(const uint8_t *bin, size_t bin_len, char *hex) {
    for (size_t i = 0; i < bin_len; i++) {
        sprintf(hex + i * 2, "%02x", bin[i]);
    }
}

int nip19_decode(const char *bech32_string, char *prefix, size_t prefix_len, void **value, size_t *value_len) {
    char hrp[84];
    size_t data_len = 0;
    uint8_t data[65];
    int result = bech32_decode(hrp, data, &data_len, bech32_string);
    if (result == -1) {
        return -1;
    }

    // Convert 5-bit data to 8-bit data
    uint8_t bin[65];
    size_t bin_len = 0;
    if (bech32_convert_bits(bin, &bin_len, 8, data, data_len, 5, 0) == -1) {
        return -1;
    }

    strncpy(prefix, hrp, prefix_len);
    *value = malloc(bin_len);
    if (*value == NULL) {
        return -1;
    }
    memcpy(*value, bin, bin_len);
    *value_len = bin_len;

    return 0;
}

char *nip19_encode_private_key(const char *private_key_hex) {
    uint8_t bin[32];
    if (hex_to_bin(private_key_hex, bin, sizeof(bin)) == -1) {
        return NULL;
    }

    // Convert 8-bit data to 5-bit data
    uint8_t data[52];
    size_t data_len = 0;
    if (bech32_convert_bits(data, &data_len, 5, bin, sizeof(bin), 8, 1) == -1) {
        return NULL;
    }

    return bech32_encode("nsec", data, data_len);
}

char *nip19_encode_public_key(const char *public_key_hex) {
    uint8_t bin[32];
    if (hex_to_bin(public_key_hex, bin, sizeof(bin)) == -1) {
        return NULL;
    }

    // Convert 8-bit data to 5-bit data
    uint8_t data[52];
    size_t data_len = 0;
    if (bech32_convert_bits(data, &data_len, 5, bin, sizeof(bin), 8, 1) == -1) {
        return NULL;
    }

    return bech32_encode("npub", data, data_len);
}

char *nip19_encode_note_id(const char *event_id_hex) {
    uint8_t bin[32];
    if (hex_to_bin(event_id_hex, bin, sizeof(bin)) == -1) {
        return NULL;
    }

    // Convert 8-bit data to 5-bit data
    uint8_t data[52];
    size_t data_len = 0;
    if (bech32_convert_bits(data, &data_len, 5, bin, sizeof(bin), 8, 1) == -1) {
        return NULL;
    }

    return bech32_encode("note", data, data_len);
}

char *nip19_encode_profile(const char *public_key_hex, const char *relays[], size_t relays_len) {
    uint8_t bin[32];
    if (hex_to_bin(public_key_hex, bin, sizeof(bin)) == -1) {
        return NULL;
    }

    // Prepare TLV encoded data
    uint8_t tlv_data[1024];
    size_t tlv_len = 0;
    tlv_data[tlv_len++] = TLV_DEFAULT;
    tlv_data[tlv_len++] = 32;
    memcpy(tlv_data + tlv_len, bin, 32);
    tlv_len += 32;

    for (size_t i = 0; i < relays_len; i++) {
        size_t relay_len = strlen(relays[i]);
        tlv_data[tlv_len++] = TLV_RELAY;
        tlv_data[tlv_len++] = relay_len;
        memcpy(tlv_data + tlv_len, relays[i], relay_len);
        tlv_len += relay_len;
    }

    // Convert 8-bit data to 5-bit data
    uint8_t data[1024];
    size_t data_len = 0;
    if (bech32_convert_bits(data, &data_len, 5, tlv_data, tlv_len, 8, 1) == -1) {
        return NULL;
    }

    return bech32_encode("nprofile", data, data_len);
}

char *nip19_encode_event(const char *event_id_hex, const char *relays[], size_t relays_len, const char *author_hex) {
    uint8_t bin[32];
    if (hex_to_bin(event_id_hex, bin, sizeof(bin)) == -1) {
        return NULL;
    }

    // Prepare TLV encoded data
    uint8_t tlv_data[1024];
    size_t tlv_len = 0;
    tlv_data[tlv_len++] = TLV_DEFAULT;
    tlv_data[tlv_len++] = 32;
    memcpy(tlv_data + tlv_len, bin, 32);
    tlv_len += 32;

    for (size_t i = 0; i < relays_len; i++) {
        size_t relay_len = strlen(relays[i]);
        tlv_data[tlv_len++] = TLV_RELAY;
        tlv_data[tlv_len++] = relay_len;
        memcpy(tlv_data + tlv_len, relays[i], relay_len);
        tlv_len += relay_len;
    }

    if (hex_to_bin(author_hex, bin, sizeof(bin)) == 0) {
        tlv_data[tlv_len++] = TLV_AUTHOR;
        tlv_data[tlv_len++] = 32;
        memcpy(tlv_data + tlv_len, bin, 32);
        tlv_len += 32;
    }

    // Convert 8-bit data to 5-bit data
    uint8_t data[1024];
    size_t data_len = 0;
    if (bech32_convert_bits(data, &data_len, 5, tlv_data, tlv_len, 8, 1) == -1) {
        return NULL;
    }

    return bech32_encode("nevent", data, data_len);
}

char *nip19_encode_entity(const char *public_key_hex, int kind, const char *identifier, const char *relays[], size_t relays_len) {
    // Prepare TLV encoded data
    uint8_t tlv_data[1024];
    size_t tlv_len = 0;
    size_t id_len = strlen(identifier);
    tlv_data[tlv_len++] = TLV_DEFAULT;
    tlv_data[tlv_len++] = id_len;
    memcpy(tlv_data + tlv_len, identifier, id_len);
    tlv_len += id_len;

    for (size_t i = 0; i < relays_len; i++) {
        size_t relay_len = strlen(relays[i]);
        tlv_data[tlv_len++] = TLV_RELAY;
        tlv_data[tlv_len++] = relay_len;
        memcpy(tlv_data + tlv_len, relays[i], relay_len);
        tlv_len += relay_len;
    }

    uint8_t bin[32];
    if (hex_to_bin(public_key_hex, bin, sizeof(bin)) == 0) {
        tlv_data[tlv_len++] = TLV_AUTHOR;
        tlv_data[tlv_len++] = 32;
        memcpy(tlv_data + tlv_len, bin, 32);
        tlv_len += 32;
    }

    uint8_t kind_bytes[4];
    kind_bytes[0] = (uint8_t)(kind >> 24);
    kind_bytes[1] = (uint8_t)(kind >> 16);
    kind_bytes[2] = (uint8_t)(kind >> 8);
    kind_bytes[3] = (uint8_t)kind;
    tlv_data[tlv_len++] = TLV_KIND;
    tlv_data[tlv_len++] = 4;
    memcpy(tlv_data + tlv_len, kind_bytes, 4);
    tlv_len += 4;

    // Convert 8-bit data to 5-bit data
    uint8_t data[1024];
    size_t data_len = 0;
    if (bech32_convert_bits(data, &data_len, 5, tlv_data, tlv_len, 8, 1) == -1) {
        return NULL;
    }

    return bech32_encode("naddr", data, data_len);
}
