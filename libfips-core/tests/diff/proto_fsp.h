#ifndef FIPS_DIFF_PROTO_FSP_H
#define FIPS_DIFF_PROTO_FSP_H
#include "proto_link.h"
result check_proto_fsp_setup(diff_context *ctx, const proto_case *c);
result check_proto_fsp_ack(diff_context *ctx, const proto_case *c);
result check_proto_fsp_msg3(diff_context *ctx, const proto_case *c);
result check_proto_fsp_encrypted(diff_context *ctx, const proto_case *c);
#endif
