#include "diff/proto_lookup.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    diff_context ctx = {.dir = argc > 1 ? argv[1] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto");
    diff_value *cases = diff_load(&ctx, "proto");
    if (!cases) {
        fprintf(stderr, "FAIL %s\n", ctx.error);
        return 1;
    }
    diff_value *wire = FIELD(cases, "wire_cases");
    size_t checked = 0;
    for (size_t i = 0; i < COUNT(wire); i++) {
        diff_value *v = ITEM(wire, i);
        const char *name = TEXT(v, "name");
        result (*check)(diff_context *, const proto_case *) = NULL;
        if (!strcmp(name, "lookup-request"))
            check = check_proto_lookup_request;
        if (!strcmp(name, "lookup-response"))
            check = check_proto_lookup_response;
        if (!check)
            continue;
        proto_case c = {name, v, FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
        snprintf(ctx.id, sizeof(ctx.id), "proto/%s", name);
        if (check(&ctx, &c) == CHECKED) {
            checked++;
            printf("PASS %s\n", ctx.id);
        } else
            fprintf(stderr, "FAIL %s: %s\n", ctx.id, ctx.error);
    }
    diff_free(cases);
    printf("Lookup vectors: %zu checked, 0 skipped, %zu failed\n", checked, 2 - checked);
    return checked == 2 ? 0 : 1;
}
