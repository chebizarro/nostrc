#include "diff/proto_bloom.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    diff_context ctx = {.dir = argc > 1 ? argv[1] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto/bloom");
    diff_value *cases = diff_load(&ctx, "proto");
    if (!cases) { fprintf(stderr, "%s\n", ctx.error); return 1; }
    diff_value *wire = FIELD(cases, "wire_cases");
    size_t checked = 0;
    const struct { const char *name; result (*check)(diff_context *, const proto_case *); } checks[] = {
        {"bloom", check_proto_bloom}, {"filter-announce", check_proto_filter_announce}
    };
    for (size_t j = 0; j < 2; ++j) {
        diff_value *v = NULL;
        for (size_t i = 0; i < COUNT(wire); ++i) if (!strcmp(TEXT(ITEM(wire, i), "name"), checks[j].name)) v = ITEM(wire, i);
        snprintf(ctx.id, sizeof(ctx.id), "proto/%s", checks[j].name); ctx.error[0] = '\0';
        const proto_case c = {checks[j].name, v, FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
        result r = v ? checks[j].check(&ctx, &c) : FAILED;
        if (r == CHECKED) { ++checked; printf("PASS %s: CHECKED\n", ctx.id); }
        else fprintf(stderr, "FAIL %s: %s\n", ctx.id, v ? ctx.error : "case missing");
    }
    diff_free(cases);
    printf("bloom subset: %zu checked, 0 skipped, %zu failed\n", checked, 2 - checked);
    return checked == 2 ? 0 : 1;
}
