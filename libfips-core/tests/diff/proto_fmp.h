#ifndef FIPS_DIFF_PROTO_FMP_H
#define FIPS_DIFF_PROTO_FMP_H
#include "proto_link.h"
result check_proto_disconnect(diff_context *ctx, const proto_case *c);
result check_proto_fmp_msg_1(diff_context *ctx, const proto_case *c);
result check_proto_fmp_msg_2(diff_context *ctx, const proto_case *c);
result check_proto_fmp_encrypted(diff_context *ctx, const proto_case *c);
#endif
