#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nostr/nip19/nip19.h"

/* Spec: docs/nips/19.md (Bare keys and ids, lines 13–25)
 * Bech32 (not m) per BIP-0173. */

static const char *CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static int8_t CHARSET_REV[128];

static uint32_t polymod(const uint8_t *values, size_t len) {
    uint32_t chk = 1;
    for (size_t i = 0; i < len; ++i) {
        uint8_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ values[i];
        if (top & 1) chk ^= 0x3b6a57b2;
        if (top & 2) chk ^= 0x26508e6d;
        if (top & 4) chk ^= 0x1ea119fa;
        if (top & 8) chk ^= 0x3d4233dd;
        if (top & 16) chk ^= 0x2a1462b3;
    }
    return chk;
}

static void hrp_expand(const char *hrp, uint8_t **out, size_t *out_len) {
    size_t hlen = strlen(hrp);
    uint8_t *buf = (uint8_t *)calloc(hlen * 2 + 1, 1);
    for (size_t i = 0; i < hlen; ++i) buf[i] = (uint8_t)(tolower((unsigned char)hrp[i]) >> 5);
    buf[hlen] = 0;
    for (size_t i = 0; i < hlen; ++i) buf[hlen + 1 + i] = (uint8_t)(tolower((unsigned char)hrp[i]) & 31);
    *out = buf;
    *out_len = hlen * 2 + 1;
}

static int verify_checksum(const char *hrp, const uint8_t *data, size_t data_len) {
    uint8_t *exp = NULL; size_t exp_len = 0;
    hrp_expand(hrp, &exp, &exp_len);
    size_t n = exp_len + data_len;
    uint8_t *vals = (uint8_t *)malloc(n);
    memcpy(vals, exp, exp_len);
    memcpy(vals + exp_len, data, data_len);
    uint32_t pm = polymod(vals, n);
    free(exp); free(vals);
    return pm == 1 ? 0 : -1;
}

static void create_checksum(const char *hrp, const uint8_t *data, size_t data_len, uint8_t out[6]) {
    uint8_t *exp = NULL; size_t exp_len = 0;
    hrp_expand(hrp, &exp, &exp_len);
    size_t n = exp_len + data_len + 6;
    uint8_t *vals = (uint8_t *)malloc(n);
    memcpy(vals, exp, exp_len);
    memcpy(vals + exp_len, data, data_len);
    memset(vals + exp_len + data_len, 0, 6);
    uint32_t pm = polymod(vals, n) ^ 1;
    free(exp); free(vals);
    for (int i = 0; i < 6; ++i) out[i] = (pm >> (5 * (5 - i))) & 31;
}

static void ensure_rev_table(void) {
    static int inited = 0; if (inited) return; inited = 1;
    memset(CHARSET_REV, -1, sizeof(CHARSET_REV));
    for (int i = 0; CHARSET[i]; ++i) CHARSET_REV[(int)CHARSET[i]] = i;
}

int nostr_b32_encode(const char *hrp, const uint8_t *data5, size_t data5_len, char **out_bech) {
    if (!hrp || !data5 || !out_bech) return -1;
    *out_bech = NULL;
    size_t hlen = strlen(hrp);
    size_t out_len = hlen + 1 + data5_len + 6; // hrp + '1' + data + checksum
    char *out = (char *)malloc(out_len + 1);
    for (size_t i = 0; i < hlen; ++i) out[i] = (char)tolower((unsigned char)hrp[i]);
    out[hlen] = '1';
    uint8_t sum[6]; create_checksum(hrp, data5, data5_len, sum);
    for (size_t i = 0; i < data5_len; ++i) out[hlen + 1 + i] = CHARSET[data5[i]];
    for (int i = 0; i < 6; ++i) out[hlen + 1 + data5_len + i] = CHARSET[sum[i]];
    out[out_len] = '\0';
    *out_bech = out;
    return 0;
}

int nostr_b32_decode(const char *bech, char **out_hrp, uint8_t **out_data5, size_t *out_data5_len) {
    if (!bech || !out_hrp || !out_data5) return -1;
    *out_hrp = NULL; *out_data5 = NULL; if (out_data5_len) *out_data5_len = 0;
    ensure_rev_table();
    size_t len = strlen(bech);
    if (len < 8) return -1; // hrp(1)+'1'+data(1)+checksum(6)
    // Check case rules and find separator
    int has_lower = 0, has_upper = 0; ssize_t pos1 = -1;
    for (size_t i = 0; i < len; ++i) {
        char c = bech[i];
        if (c >= 'a' && c <= 'z') has_lower = 1;
        else if (c >= 'A' && c <= 'Z') has_upper = 1;
        if (c == '1') pos1 = (ssize_t)i;
    }
    if (has_lower && has_upper) return -1;
    if (pos1 < 1 || (size_t)pos1 + 7 > len) return -1;
    // HRP
    size_t hlen = (size_t)pos1;
    char *hrp = (char *)malloc(hlen + 1);
    for (size_t i = 0; i < hlen; ++i) {
        char c = bech[i];
        if (c < 33 || c > 126) { free(hrp); return -1; }
        hrp[i] = (char)tolower((unsigned char)c);
    }
    hrp[hlen] = '\0';
    // Data+checksum
    size_t dlen = len - hlen - 1;
    if (dlen < 6) { free(hrp); return -1; }
    size_t datalen = dlen - 6;
    uint8_t *data = (uint8_t *)malloc(dlen);
    for (size_t i = 0; i < dlen; ++i) {
        unsigned char c = (unsigned char)bech[hlen + 1 + i];
        int8_t v = (c <= 127) ? CHARSET_REV[(int)c] : -1;
        if (v < 0) { free(hrp); free(data); return -1; }
        data[i] = (uint8_t)v;
    }
    if (verify_checksum(hrp, data, dlen) != 0) { free(hrp); free(data); return -1; }
    uint8_t *payload = NULL;
    if (datalen > 0) {
        payload = (uint8_t *)malloc(datalen);
        memcpy(payload, data, datalen);
    }
    free(data);
    *out_hrp = hrp; *out_data5 = payload; if (out_data5_len) *out_data5_len = datalen;
    return 0;
}

int nostr_b32_to_5bit(const uint8_t *in8, size_t in8_len, uint8_t **out5, size_t *out5_len) {
    if (!in8 || !out5) return -1; *out5 = NULL; if (out5_len) *out5_len = 0;
    size_t outcap = (in8_len * 8 + 4) / 5;
    uint8_t *out = (uint8_t *)malloc(outcap);
    size_t outn = 0; int acc = 0; int bits = 0;
    for (size_t i = 0; i < in8_len; ++i) {
        acc = (acc << 8) | in8[i]; bits += 8;
        while (bits >= 5) {
            out[outn++] = (acc >> (bits - 5)) & 31; bits -= 5;
        }
    }
    if (bits > 0) out[outn++] = (acc << (5 - bits)) & 31; // pad
    *out5 = out; if (out5_len) *out5_len = outn;
    return 0;
}

int nostr_b32_to_8bit(const uint8_t *in5, size_t in5_len, uint8_t **out8, size_t *out8_len) {
    if (!in5 || !out8) return -1; *out8 = NULL; if (out8_len) *out8_len = 0;
    size_t outcap = (in5_len * 5) / 8 + 1;
    uint8_t *out = (uint8_t *)malloc(outcap);
    size_t outn = 0; int acc = 0; int bits = 0;
    for (size_t i = 0; i < in5_len; ++i) {
        if (in5[i] >> 5) { free(out); return -1; }
        acc = (acc << 5) | in5[i]; bits += 5;
        if (bits >= 8) {
            out[outn++] = (acc >> (bits - 8)) & 0xff; bits -= 8;
        }
    }
    *out8 = out; if (out8_len) *out8_len = outn;
    return 0;
}
