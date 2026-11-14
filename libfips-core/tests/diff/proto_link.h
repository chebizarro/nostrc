#ifndef FIPS_DIFF_PROTO_LINK_H
#define FIPS_DIFF_PROTO_LINK_H

#include "diff.h"

/* Link-catalog, session-catalog, constants and session-datagram checks for the
 * proto family (nostrc-82g9n.6). The check vocabulary below duplicates the
 * static definitions cases.c still carries; when its per-name dispatch includes
 * this header, drop those copies so one definition remains. */
typedef enum result { CHECKED, FAILED, SKIPPED } result;
typedef struct proto_case {
    const char *name;
    diff_value *record, *common_inputs, *constants;
} proto_case;

/* Every function compares real C output/error payloads against the exporter
 * record and never returns CHECKED on fixture integrity alone. */
result check_proto_constants(diff_context *ctx, const diff_value *constants);
result check_proto_link_catalog(diff_context *ctx, const diff_value *catalog);
result check_proto_session_catalog(diff_context *ctx, const diff_value *catalog);
result check_proto_session_datagram(diff_context *ctx, const proto_case *c);

#endif
