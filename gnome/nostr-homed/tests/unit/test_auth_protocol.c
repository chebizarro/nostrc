#include "nostr_auth_protocol.h"
#include <assert.h>
#include "../nh_test.h"
#include <stdio.h>
#include <string.h>

static const char rid[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
int main(void){
  char good[512];snprintf(good,sizeof good,"{\"version\":1,\"operation\":\"BeginLogin\",\"request_id\":\"%s\",\"transaction_id\":\"\",\"payload\":{}}",rid);
  nh_auth_message m;NH_CHECK(nh_auth_message_parse(good,strlen(good),&m)==0);NH_CHECK(m.operation==NH_AUTH_OP_BEGIN_LOGIN);nh_auth_message_clear(&m);
  const char *dup="{\"version\":1,\"version\":1,\"operation\":\"Cancel\",\"request_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"transaction_id\":\"\",\"payload\":{}}";
  NH_CHECK(nh_auth_message_parse(dup,strlen(dup),&m)<0);
  const char *unknown="{\"version\":2,\"operation\":\"Nope\",\"request_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"transaction_id\":\"\",\"payload\":{}}";
  NH_CHECK(nh_auth_message_parse(unknown,strlen(unknown),&m)<0);
  NH_CHECK(nh_auth_message_parse(good,NH_AUTH_PACKET_MAX+1,&m)<0);
  NH_CHECK(nh_auth_operation_allowed(NH_AUTH_ENDPOINT_AUTH,NH_AUTH_OP_BEGIN_LOGIN,0,0));
  NH_CHECK(!nh_auth_operation_allowed(NH_AUTH_ENDPOINT_AUTH,NH_AUTH_OP_BEGIN_LOGIN,1000,0));
  NH_CHECK(nh_auth_operation_allowed(NH_AUTH_ENDPOINT_USER,NH_AUTH_OP_BEGIN_SMB_PROOF,1000,0));
  NH_CHECK(!nh_auth_operation_allowed(NH_AUTH_ENDPOINT_USER,NH_AUTH_OP_BEGIN_LOGIN,1000,0));
  NH_CHECK(!nh_auth_operation_allowed(NH_AUTH_ENDPOINT_WORKER,NH_AUTH_OP_CANCEL,0,1));
  return 0;
}
