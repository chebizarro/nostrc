/*
 * nh_fuse_key.c — seed drop reader (design §D6a).
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "nh_fuse_key.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_hex64_from_file(const char *path, char out_hex[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[96];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 64) { OPENSSL_cleanse(buf, sizeof buf); return -1; }
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t')) buf[--n] = '\0';
    if (n < 64) { OPENSSL_cleanse(buf, sizeof buf); return -1; }
    for (size_t i = 0; i < 64; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            OPENSSL_cleanse(buf, sizeof buf);
            return -1;
        }
    }
    memcpy(out_hex, buf, 64);
    out_hex[64] = '\0';
    OPENSSL_cleanse(buf, sizeof buf);
    return 0;
}

static int hex_to_bytes(const char hex[64], uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        char h = hex[i*2], l = hex[i*2 + 1];
        int hv = (h >= '0' && h <= '9') ? h - '0'
               : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : -1;
        int lv = (l >= '0' && l <= '9') ? l - '0'
               : (l >= 'a' && l <= 'f') ? l - 'a' + 10 : -1;
        if (hv < 0 || lv < 0) return -1;
        out[i] = (uint8_t)((hv << 4) | lv);
    }
    return 0;
}

int nh_fuse_key_load(uint8_t out_home_key[NH_PORTHOME_KEY_LEN]) {
    if (!out_home_key) return -1;
    /* Suppress core dumps before we ever touch the seed. */
    struct rlimit rc = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &rc);

    char hex[65] = {0};
    int got = -1;

    /* 1. Explicit test / operator override via env var (inline). */
    const char *hex_env = getenv("NH_FUSE_SEED_HEX");
    if (hex_env && strlen(hex_env) >= 64) {
        memcpy(hex, hex_env, 64);
        hex[64] = '\0';
        /* validate */
        int ok = 1;
        for (int i = 0; i < 64; i++) {
            char c = hex[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = 0; break; }
        }
        if (ok) got = 0;
    }

    /* 2. Path override or default broker drop. */
    if (got != 0) {
        const char *ovr = getenv("NH_FUSE_SEED_FILE");
        char path[512];
        if (ovr && *ovr) snprintf(path, sizeof path, "%s", ovr);
        else snprintf(path, sizeof path,
                      "/run/nostr-auth/session/%u/home_seed.fuse",
                      (unsigned)getuid());
        if (read_hex64_from_file(path, hex) == 0) {
            got = 0;
            /* Read-once-and-unlink discipline. Best-effort. */
            (void)unlink(path);
        }
    }
    if (got != 0) return -1;

    uint8_t seed[32];
    if (hex_to_bytes(hex, seed) != 0) {
        OPENSSL_cleanse(hex, sizeof hex);
        return -1;
    }
    OPENSSL_cleanse(hex, sizeof hex);
    int dr = nh_porthome_key_derive(seed, out_home_key);
    OPENSSL_cleanse(seed, sizeof seed);
    return dr == 0 ? 0 : -1;
}
