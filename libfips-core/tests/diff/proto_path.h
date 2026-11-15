#ifndef FIPS_DIFF_PROTO_PATH_H
#define FIPS_DIFF_PROTO_PATH_H
#include "proto_link.h"
result check_proto_path_probe(diff_context *ctx,const proto_case *c);
result check_proto_path_ack(diff_context *ctx,const proto_case *c);
result check_proto_path_close(diff_context *ctx,const proto_case *c);
#endif
