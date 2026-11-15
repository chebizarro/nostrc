#ifndef FIPS_DIFF_PROTO_MMP_H
#define FIPS_DIFF_PROTO_MMP_H
#include "proto_link.h"
result check_proto_sender_report(diff_context *ctx, const proto_case *c);
result check_proto_session_sender_report(diff_context *ctx, const proto_case *c);
result check_proto_receiver_report(diff_context *ctx, const proto_case *c);
result check_proto_session_receiver_report(diff_context *ctx, const proto_case *c);
result check_proto_path_mtu(diff_context *ctx, const proto_case *c);
#endif
