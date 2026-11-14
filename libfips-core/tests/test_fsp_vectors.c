#include "diff/proto_fsp.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    diff_context ctx = {.dir = argc > 1 ? argv[1] : FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto");
    diff_value *proto = diff_load(&ctx, "proto");
    if (!proto) { fprintf(stderr, "%s\n", ctx.error); return 1; }
    const struct { const char *name; result (*check)(diff_context *, const proto_case *); } checks[] = {
        {"fsp-setup", check_proto_fsp_setup}, {"fsp-ack", check_proto_fsp_ack},
        {"fsp-msg3", check_proto_fsp_msg3}, {"fsp-encrypted", check_proto_fsp_encrypted}
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
            if (i < 3) {
                diff_bytes bytes = BYTES(c.record, "bytes");
                bytes.data[bytes.len - 1] ^= 1;
                if (checks[i].check(&ctx, &c) == FAILED) ++rejected;
                bytes.data[bytes.len - 1] ^= 1;
            } else {
                field = FIELD(c.record, "counter");
                field->as.number ^= 1;
                if (checks[i].check(&ctx, &c) == FAILED) ++rejected;
                field->as.number ^= 1;
            }
        }
        else fprintf(stderr, "FAIL %s: %s\n", ctx.id, ctx.error);
    }
    printf("FSP: %zu checked, 0 skipped, %zu failed (wire envelopes, not Noise handshake)\n", passed, 4 - passed);
    diff_free(proto);
    printf("FSP negative checks: %zu/4 mismatches rejected\n", rejected);
    return passed == 4 && rejected == 4 ? 0 : 1;
}
