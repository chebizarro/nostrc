#include "diff/proto_path.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    diff_context ctx = {.dir = argc > 1 ? argv[1] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto");
    diff_value *proto = diff_load(&ctx, "proto");
    if (!proto) { fprintf(stderr, "%s\n", ctx.error); return 1; }
    const struct { const char *name; result (*check)(diff_context *, const proto_case *); } checks[] = {
        {"path-probe", check_proto_path_probe}, {"path-ack", check_proto_path_ack},
        {"path-close", check_proto_path_close}
    };
    size_t passed = 0, rejected = 0;
    diff_value *cases = FIELD(proto, "wire_cases");
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        proto_case c = {checks[i].name, NULL, FIELD(proto, "common_inputs"), FIELD(proto, "constants")};
        for (size_t j = 0; j < COUNT(cases); ++j)
            if (!strcmp(TEXT(ITEM(cases, j), "name"), c.name)) c.record = ITEM(cases, j);
        snprintf(ctx.id, sizeof(ctx.id), "proto/%s", c.name); ctx.error[0] = '\0';
        if (c.record && checks[i].check(&ctx, &c) == CHECKED) {
            ++passed; printf("PASS %s\n", ctx.id);
            /* Change actual inputs after schema validation: integrity alone
             * cannot make these comparisons pass. Restore the shared fixture. */
            diff_value *field = FIELD(FIELD(c.record, "input"), "path_id");
            field->as.number ^= 1;
            if (checks[i].check(&ctx, &c) == FAILED) ++rejected;
            field->as.number ^= 1;
        }
        else fprintf(stderr, "FAIL %s: %s\n", ctx.id, ctx.error);
    }
    printf("path: %zu checked, 0 skipped, %zu failed (wire reports, not host I/O)\n", passed, 3 - passed);
    diff_free(proto);
    printf("path negative checks: %zu/3 mismatches rejected\n", rejected);
    return passed == 3 && rejected == 3 ? 0 : 1;
}
