#include "diff/proto_fmp.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    diff_context ctx = {.dir = argc > 1 ? argv[1] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto");
    diff_value *proto = diff_load(&ctx, "proto");
    if (!proto) { fprintf(stderr, "%s\n", ctx.error); return 1; }
    const struct { const char *name; result (*check)(diff_context *, const proto_case *); } checks[] = {
        {"disconnect", check_proto_disconnect}, {"fmp-msg-1", check_proto_fmp_msg_1},
        {"fmp-msg-2", check_proto_fmp_msg_2}, {"fmp-encrypted", check_proto_fmp_encrypted}
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
            diff_value *field;
            if (i == 0) {
                field = FIELD(FIELD(c.record, "input"), "reason");
                char *saved = field->as.text;
                field->as.text = "Shutdown";
                if (checks[i].check(&ctx, &c) == FAILED) ++rejected;
                field->as.text = saved;
            } else {
                field = FIELD(c.record, i == 1 ? "sender_idx" : i == 2 ? "receiver_idx" : "counter");
                field->as.number ^= 1;
                if (checks[i].check(&ctx, &c) == FAILED) ++rejected;
                field->as.number ^= 1;
            }
        }
        else fprintf(stderr, "FAIL %s: %s\n", ctx.id, ctx.error);
    }
    printf("FMP: %zu checked, 0 skipped, %zu failed (wire envelopes, not Noise handshake)\n", passed, 4 - passed);
    diff_free(proto);
    printf("FMP negative checks: %zu/4 mismatches rejected\n", rejected);
    return passed == 4 && rejected == 4 ? 0 : 1;
}
