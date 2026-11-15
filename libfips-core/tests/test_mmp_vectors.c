#include "diff/proto_mmp.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    diff_context ctx = {.dir = argc > 1 ? argv[1] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto");
    diff_value *proto = diff_load(&ctx, "proto");
    if (!proto) { fprintf(stderr, "%s\n", ctx.error); return 1; }
    const struct { const char *name; result (*check)(diff_context *, const proto_case *); } checks[] = {
        {"sender-report", check_proto_sender_report}, {"session-sender-report", check_proto_session_sender_report},
        {"receiver-report", check_proto_receiver_report}, {"session-receiver-report", check_proto_session_receiver_report},
        {"path-mtu", check_proto_path_mtu}
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
            diff_value *field = FIELD(FIELD(c.record, "input"), i < 2 ? "interval_start_counter" : i < 4 ? "highest_counter" : "path_mtu");
            field->as.number ^= 1;
            if (checks[i].check(&ctx, &c) == FAILED) ++rejected;
            field->as.number ^= 1;
        }
        else fprintf(stderr, "FAIL %s: %s\n", ctx.id, ctx.error);
    }
    printf("MMP: %zu checked, 0 skipped, %zu failed (wire reports, not host I/O)\n", passed, 5 - passed);
    diff_free(proto);
    printf("MMP negative checks: %zu/5 mismatches rejected\n", rejected);
    return passed == 5 && rejected == 5 ? 0 : 1;
}
