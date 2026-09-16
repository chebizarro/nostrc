/* NIP-CAS-0010: standard-alphabet base64 for OTLP event content.
 *
 * Self-contained so the transport has no codec dependency: NIP-CAS-0010 §3.2
 * requires base64 content for every encoding, including `identity`. */

#include <stdlib.h>
#include <string.h>

#include "nostr/nip_cas0010/otel.h"

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* -1 marks an invalid character, -2 marks the padding character. */
static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

int nostr_otel_base64_encode(const uint8_t *data, size_t len, char **out) {
    if (!out) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out = NULL;
    if (len > 0 && !data) return NOSTR_OTEL_ERR_INVALID_ARG;
    /* Guard the 4/3 expansion against overflow before allocating. */
    if (len > (SIZE_MAX - 4) / 4 * 3) return NOSTR_OTEL_ERR_NOMEM;

    size_t out_len = ((len + 2) / 3) * 4;
    char *buf = malloc(out_len + 1);
    if (!buf) return NOSTR_OTEL_ERR_NOMEM;

    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        buf[o++] = B64_ALPHABET[(v >> 6) & 0x3F];
        buf[o++] = B64_ALPHABET[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        buf[o++] = '=';
        buf[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        buf[o++] = B64_ALPHABET[(v >> 6) & 0x3F];
        buf[o++] = '=';
    }
    buf[o] = '\0';
    *out = buf;
    return NOSTR_OTEL_OK;
}

int nostr_otel_base64_decode(const char *text, uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    if (!text) return NOSTR_OTEL_ERR_INVALID_ARG;

    size_t len = strlen(text);
    if (len % 4 != 0) return NOSTR_OTEL_ERR_MALFORMED_EVENT;

    size_t cap = len / 4 * 3;
    uint8_t *buf = malloc(cap + 1);
    if (!buf) return NOSTR_OTEL_ERR_NOMEM;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int q[4];
        for (size_t k = 0; k < 4; k++) {
            q[k] = b64_value((unsigned char)text[i + k]);
            if (q[k] == -1) {
                free(buf);
                return NOSTR_OTEL_ERR_MALFORMED_EVENT;
            }
        }
        /* Padding is only legal in the final quantum, and never in q[0]/q[1]. */
        bool last = (i + 4 == len);
        if (q[0] == -2 || q[1] == -2) {
            free(buf);
            return NOSTR_OTEL_ERR_MALFORMED_EVENT;
        }
        if ((q[2] == -2 || q[3] == -2) && !last) {
            free(buf);
            return NOSTR_OTEL_ERR_MALFORMED_EVENT;
        }
        if (q[2] == -2 && q[3] != -2) {
            free(buf);
            return NOSTR_OTEL_ERR_MALFORMED_EVENT;
        }
        uint32_t v = ((uint32_t)q[0] << 18) | ((uint32_t)q[1] << 12);
        if (q[2] != -2) v |= (uint32_t)q[2] << 6;
        if (q[3] != -2) v |= (uint32_t)q[3];

        buf[o++] = (uint8_t)((v >> 16) & 0xFF);
        if (q[2] != -2) buf[o++] = (uint8_t)((v >> 8) & 0xFF);
        if (q[3] != -2) buf[o++] = (uint8_t)(v & 0xFF);
    }
    buf[o] = '\0';
    *out = buf;
    *out_len = o;
    return NOSTR_OTEL_OK;
}
