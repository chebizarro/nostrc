/*
 * test_fuse_key.c — read-once seed drop reader.
 */

#define _GNU_SOURCE
#include "nh_fuse_key.h"
#include "nh_porthome_crypto.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    /* Env-inline path. */
    setenv("NH_FUSE_SEED_HEX",
           "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", 1);
    uint8_t hk1[NH_PORTHOME_KEY_LEN];
    assert(nh_fuse_key_load(hk1) == 0);
    unsetenv("NH_FUSE_SEED_HEX");

    /* File path — write, verify read + unlink. */
    char tmpl[] = "/tmp/nhfuse_key_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    char path[512]; snprintf(path, sizeof path, "%s/seed", tmpl);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    const char *hex64 = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\n";
    (void)!write(fd, hex64, strlen(hex64));
    close(fd);

    setenv("NH_FUSE_SEED_FILE", path, 1);
    uint8_t hk2[NH_PORTHOME_KEY_LEN];
    assert(nh_fuse_key_load(hk2) == 0);
    assert(memcmp(hk1, hk2, NH_PORTHOME_KEY_LEN) == 0);

    /* File must be unlinked. */
    struct stat s;
    assert(stat(path, &s) != 0);

    /* Second call fails cleanly. */
    assert(nh_fuse_key_load(hk2) != 0);

    unsetenv("NH_FUSE_SEED_FILE");
    printf("test_fuse_key OK\n");
    return 0;
}
