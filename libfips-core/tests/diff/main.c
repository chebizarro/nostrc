#include "diff.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--mutate")) return diff_mutate(argv[2], argv[3]);
    if (argc < 2 || argc > 3) {
        fputs("usage: fips_diff <entropy|identity|codec|noise|proto|rekey> [vectors-dir]\n", stderr);
        return 2;
    }
    const char *family = argv[1];
    if (strcmp(family, "entropy") && strcmp(family, "identity") && strcmp(family, "codec") &&
        strcmp(family, "noise") && strcmp(family, "proto") && strcmp(family, "rekey")) {
        fprintf(stderr, "unknown family: %s\n", family); return 2;
    }
    diff_context ctx = {.dir = argc == 3 ? argv[2] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "%s", family);
    diff_value *cases = diff_load(&ctx, family);
    if (!cases) {
        fprintf(stderr, "FAIL %s: %s\n%s: 0 checked, 0 skipped, 1 failed (fixture integrity/schema)\n", ctx.id, ctx.error, family);
        return 1;
    }
    int result = diff_dispatch(&ctx, family, cases);
    diff_free(cases);
    return result;
}
