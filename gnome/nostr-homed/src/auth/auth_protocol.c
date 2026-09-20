#include "nostr_auth_protocol.h"

#include <errno.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct op_name { nh_auth_operation op; const char *name; } op_name;
static const op_name operations[] = {
  {NH_AUTH_OP_BEGIN_LOGIN,"BeginLogin"},{NH_AUTH_OP_SELECT_PROVIDER,"SelectProvider"},
  {NH_AUTH_OP_SUBMIT_UNLOCK,"SubmitUnlock"},{NH_AUTH_OP_WAIT_RESULT,"WaitResult"},
  {NH_AUTH_OP_CANCEL,"Cancel"},{NH_AUTH_OP_CHECK_ACCOUNT,"CheckAccount"},
  {NH_AUTH_OP_OPEN_LOCAL_SESSION,"OpenLocalSession"},{NH_AUTH_OP_CLOSE_LOCAL_SESSION,"CloseLocalSession"},
  {NH_AUTH_OP_BEGIN_SMB_PROOF,"BeginSmbProof"},{NH_AUTH_OP_ADMIN,"Admin"}
};

const char *nh_auth_operation_name(nh_auth_operation op) {
  for (size_t i=0;i<sizeof operations/sizeof operations[0];i++) if (operations[i].op==op) return operations[i].name;
  return NULL;
}
static nh_auth_operation parse_op(const char *s) {
  if (!s) return 0;
  for (size_t i=0;i<sizeof operations/sizeof operations[0];i++) if (!strcmp(operations[i].name,s)) return operations[i].op;
  return 0;
}
const char *nh_auth_result_name(nh_auth_result r) {
  static const char *names[]={"ok","unknown_account","disabled","not_ready","denied","invalid_proof","expired","cancelled","rate_limited","provider_unavailable","network_unavailable","interaction_required","storage_error","protocol_error","internal_error"};
  return (unsigned)r < sizeof names/sizeof names[0] ? names[r] : NULL;
}
static int lower_hex(const char *s, size_t n) {
  if (!s || strlen(s)!=n) return 0;
  for (size_t i=0;i<n;i++) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return 0;
  return 1;
}
void nh_auth_message_clear(nh_auth_message *m) { if (!m) return; free(m->payload_json); memset(m,0,sizeof *m); }

int nh_auth_message_parse(const void *packet,size_t len,nh_auth_message *out) {
  if (!packet || !out || !len || len>NH_AUTH_PACKET_MAX || memchr(packet,'\0',len)) return -1;
  memset(out,0,sizeof *out); json_error_t e;
  json_t *root=json_loadb(packet,len,JSON_REJECT_DUPLICATES,&e);
  if (!root || !json_is_object(root) || json_object_size(root)!=5) { if(root)json_decref(root); return -1; }
  json_t *v=json_object_get(root,"version"),*o=json_object_get(root,"operation"),*r=json_object_get(root,"request_id"),*t=json_object_get(root,"transaction_id"),*p=json_object_get(root,"payload");
  if (!json_is_integer(v)||json_integer_value(v)!=NH_AUTH_PROTOCOL_VERSION||!json_is_string(o)||!json_is_string(r)||!json_is_string(t)||!json_is_object(p)) { json_decref(root); return -1; }
  nh_auth_operation op=parse_op(json_string_value(o));
  const char *rid=json_string_value(r),*tid=json_string_value(t);
  if (!op || !lower_hex(rid,NH_AUTH_REQUEST_ID_HEX_LEN) || (tid[0] && !lower_hex(tid,NH_AUTH_TRANSACTION_ID_HEX_LEN))) { json_decref(root); return -1; }
  char *payload=json_dumps(p,JSON_COMPACT|JSON_ENCODE_ANY);
  if (!payload) { json_decref(root); return -1; }
  out->operation=op; memcpy(out->request_id,rid,sizeof out->request_id); memcpy(out->transaction_id,tid,strlen(tid)+1); out->payload_json=payload;
  json_decref(root); return 0;
}

int nh_auth_operation_allowed(nh_auth_endpoint ep,nh_auth_operation op,uid_t uid,int owns) {
  if (ep==NH_AUTH_ENDPOINT_WORKER) return 0;
  if (ep==NH_AUTH_ENDPOINT_AUTH) {
    if (uid!=0) return 0;
    if (op==NH_AUTH_OP_BEGIN_SMB_PROOF) return 0;
    if (op==NH_AUTH_OP_SELECT_PROVIDER||op==NH_AUTH_OP_SUBMIT_UNLOCK||op==NH_AUTH_OP_WAIT_RESULT||op==NH_AUTH_OP_CANCEL) return owns;
    return 1;
  }
  if (ep==NH_AUTH_ENDPOINT_USER) {
    if (op==NH_AUTH_OP_BEGIN_SMB_PROOF) return 1;
    return owns && (op==NH_AUTH_OP_SELECT_PROVIDER||op==NH_AUTH_OP_SUBMIT_UNLOCK||op==NH_AUTH_OP_WAIT_RESULT||op==NH_AUTH_OP_CANCEL);
  }
  return 0;
}

int nh_auth_recv_packet(int fd,unsigned char **out,size_t *out_len) {
  if (!out||!out_len) return -1; *out=NULL; *out_len=0;
  unsigned char *buf=malloc(NH_AUTH_PACKET_MAX); if(!buf) return -1;
  unsigned char control[CMSG_SPACE(sizeof(int))]; struct iovec iov={buf,NH_AUTH_PACKET_MAX};
  struct msghdr msg={0}; msg.msg_iov=&iov; msg.msg_iovlen=1; msg.msg_control=control; msg.msg_controllen=sizeof control;
  int flags=MSG_TRUNC;
#ifdef MSG_CMSG_CLOEXEC
  flags|=MSG_CMSG_CLOEXEC;
#endif
  ssize_t n=recvmsg(fd,&msg,flags);
  if(n<=0 || (size_t)n>NH_AUTH_PACKET_MAX || (msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || msg.msg_controllen!=0) { int saved=errno; free(buf); errno=saved; return -1; }
  *out=buf; *out_len=(size_t)n; return 0;
}
