/*
 * test_syncd_lazy.c — cover nh_syncd_lazy_new_from_string /
 * nh_syncd_lazy_covers (Phase 4 P4-I, bead nostrc-1u55).
 */

#include "nh_syncd.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_covers(const nh_syncd_lazy *lz, const char *rel, bool want) {
    bool got = nh_syncd_lazy_covers(lz, rel);
    if (got != want) {
        fprintf(stderr, "expect %s -> %d, got %d\n", rel, (int)want, (int)got);
        assert(0);
    }
}

int main(void) {
    /* Empty matcher covers nothing. */
    nh_syncd_lazy *empty = NULL;
    assert(nh_syncd_lazy_new_empty(&empty) == 0);
    expect_covers(empty, "Portable/foo.bin", false);
    expect_covers(empty, "Portable", false);
    nh_syncd_lazy_free(empty);

    /* Single-prefix comma matcher. */
    nh_syncd_lazy *lz = NULL;
    assert(nh_syncd_lazy_new_from_string("Portable/", &lz) == 0);
    assert(nh_syncd_lazy_prefix_count(lz) == 1);
    expect_covers(lz, "Portable", true);
    expect_covers(lz, "Portable/foo.bin", true);
    expect_covers(lz, "Portable/a/b/c.bin", true);
    expect_covers(lz, "PortableX/foo", false);      /* partial prefix ≠ boundary */
    expect_covers(lz, "portable/foo", false);       /* case-sensitive */
    expect_covers(lz, "Documents/foo", false);
    nh_syncd_lazy_free(lz);

    /* Comma + newline separator, comment, trailing slash normalisation. */
    lz = NULL;
    assert(nh_syncd_lazy_new_from_string(
        "Portable/,Videos\n# comment\nArchive/////\n", &lz) == 0);
    assert(nh_syncd_lazy_prefix_count(lz) == 3);
    expect_covers(lz, "Portable/x", true);
    expect_covers(lz, "Videos/movie.mkv", true);
    expect_covers(lz, "Archive/2020/report.pdf", true);
    expect_covers(lz, "Videos", true);
    nh_syncd_lazy_free(lz);

    /* Hostile inputs are silently dropped, not crashed. */
    lz = NULL;
    assert(nh_syncd_lazy_new_from_string("../bad,,\n,,,", &lz) == 0);
    assert(nh_syncd_lazy_prefix_count(lz) == 0);
    nh_syncd_lazy_free(lz);

    printf("test_syncd_lazy OK\n");
    return 0;
}
