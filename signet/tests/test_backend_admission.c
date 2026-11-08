/* SPDX-License-Identifier: MIT */
#include "signet/backend_admission.h"
#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/revocation.h"
#include "signet/signet_config.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip17/nip17.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <sqlite3.h>
#include <time.h>
#include <unistd.h>
#include "test_check.h"

#define MASTER "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define AUTH "11111111-1111-1111-1111-111111111111"
#define BACKEND "22222222-2222-2222-2222-222222222222"
#define OP "44444444-4444-4444-4444-444444444444"
#define HASH "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SNAP "\"profile_hash\":\"" HASH "\",\"generation\":\"0\",\"namespace_digest\":\"" HASH "\""

static char *self_path;

typedef struct {
  char *path,*operator_sk,*operator_pk,*boot_sk,*boot_pk,*clone_sk,*clone_pk,*bunker_sk,*bunker_pk;
  SignetKeyStore *keys;
  SignetStore *store;
  SignetBackendAdmission *a;
  SignetMgmtHandler *mgmt;
  char *session,*epoch,*revision;
} Fixture;
static void pair(char **sk,char **pk) {*sk=nostr_key_generate_private();CHECK(*sk);*pk=nostr_key_get_public(*sk);CHECK(*pk);}
static JsonObject *object(const char *s) {
  JsonParser *p=json_parser_new();CHECK(json_parser_load_from_data(p,s,-1,NULL));
  JsonObject *o=json_object_ref(json_node_get_object(json_parser_get_root(p)));g_object_unref(p);return o;
}
static char *encoded(JsonObject *o) {JsonNode *n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,o);char *s=json_to_string(n,FALSE);json_node_free(n);return s;}
static const char *sget(JsonObject *o,const char *k) {return json_object_get_string_member(o,k);}
static void expect_store_mode(void) {
  if(!signet_store_sqlcipher_available())g_test_expect_message(NULL,G_LOG_LEVEL_WARNING,"*NOT SQLCipher-encrypted at rest*");
}
static SignetKeyStore *open_keys(SignetKeyStoreConfig *cfg) {
  expect_store_mode();SignetKeyStore *keys=signet_key_store_new(NULL,cfg);g_test_assert_expected_messages();return keys;
}
static SignetStore *open_store(SignetStoreConfig *cfg) {
  expect_store_mode();SignetStore *store=signet_store_open(cfg);g_test_assert_expected_messages();return store;
}
static void setup(Fixture *f) {
  memset(f,0,sizeof *f);pair(&f->operator_sk,&f->operator_pk);pair(&f->boot_sk,&f->boot_pk);pair(&f->clone_sk,&f->clone_pk);pair(&f->bunker_sk,&f->bunker_pk);
  char t[]="/tmp/signet-backend-XXXXXX";int fd=g_mkstemp(t);CHECK(fd>=0);close(fd);unlink(t);f->path=g_strdup(t);
  SignetKeyStoreConfig k={.db_path=f->path,.master_key=MASTER};f->keys=open_keys(&k);CHECK(f->keys);
  f->store=signet_key_store_get_store(f->keys);CHECK(f->store);
  f->a=signet_backend_admission_new(f->store,AUTH,f->operator_pk);CHECK(f->a);
  SignetMgmtHandlerConfig m={.bunker_secret_key_hex=f->bunker_sk,.bunker_pubkey_hex=f->bunker_pk,.backend_authority_id=AUTH,.backend_operator_pubkey=f->operator_pk};
  f->mgmt=signet_mgmt_handler_new(f->keys,NULL,NULL,NULL,&m);CHECK(f->mgmt);
}
static void teardown(Fixture *f) {
  signet_mgmt_handler_free(f->mgmt);signet_backend_admission_free(f->a);signet_key_store_free(f->keys);
  unlink(f->path);char *wal=g_strconcat(f->path,"-wal",NULL),*shm=g_strconcat(f->path,"-shm",NULL);unlink(wal);unlink(shm);g_free(wal);g_free(shm);g_free(f->path);
  char *secrets[]={f->operator_sk,f->boot_sk,f->clone_sk,f->bunker_sk};for(size_t i=0;i<G_N_ELEMENTS(secrets);i++){sodium_memzero(secrets[i],strlen(secrets[i]));free(secrets[i]);}
  free(f->operator_pk);free(f->boot_pk);free(f->clone_pk);free(f->bunker_pk);g_free(f->session);g_free(f->epoch);g_free(f->revision);
}
static char *request(const char *method,const char *extras) {
  char *nonce=g_uuid_string_random();
  char *json=g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"backend/%s\",\"params\":{\"protocol_version\":1,\"authority_id\":\"" AUTH "\",\"backend_uuid\":\"" BACKEND "\",\"correlation_nonce\":\"%s\",%s}}",nonce,method,nonce,extras);g_free(nonce);return json;
}
static JsonObject *call(Fixture *f,const char *sender,const char *method,const char *extras,const char *error) {
  char *req=request(method,extras),*reply=signet_backend_admission_dispatch(f->a,sender,req,false);CHECK(reply);
  JsonObject *o=object(reply);g_free(req);g_free(reply);
  if(error) {CHECK(json_object_has_member(o,"error"));JsonObject *e=json_object_get_object_member(o,"error");g_assert_cmpstr(sget(e,"data"),==,error);json_object_unref(o);return NULL;}
  if(json_object_has_member(o,"error")){g_printerr("unexpected admission error: %s\n",sget(json_object_get_object_member(o,"error"),"data"));CHECK(false);}
  JsonObject *r=json_object_ref(json_object_get_object_member(o,"result"));
  CHECK(json_object_get_size(r)==15);g_free(f->revision);f->revision=g_strdup(sget(r,"revision"));
  JsonNode *sid=json_object_get_member(r,"session_id");
  if(!JSON_NODE_HOLDS_NULL(sid)){g_free(f->session);g_free(f->epoch);f->session=g_strdup(sget(r,"session_id"));f->epoch=g_strdup(sget(r,"epoch"));}
  json_object_unref(o);return r;
}
static char *command(const char *revision,const char *extras) {
  char *id=g_uuid_string_random();char *out=g_strdup_printf("\"command_id\":\"%s\",\"expected_revision\":\"%s\"%s%s",id,revision,extras&&extras[0]?",":"",extras?extras:"");g_free(id);return out;
}
static void enroll(Fixture *f) {char *e=command("0",SNAP);JsonObject *r=call(f,f->operator_pk,"enroll",e,NULL);json_object_unref(r);g_free(e);}
static char *activate_args(Fixture *f,const char *key) {char *extra=g_strdup_printf("\"boot_pubkey\":\"%s\"," SNAP,key);char *e=command(f->revision,extra);g_free(extra);return e;}
static void activate(Fixture *f) {char *e=activate_args(f,f->boot_pk);JsonObject *r=call(f,f->operator_pk,"activate",e,NULL);g_assert_cmpstr(sget(r,"phase"),==,"STARTING");json_object_unref(r);g_free(e);}
static char *bound(Fixture *f,const char *extras) {return g_strdup_printf("\"epoch\":\"%s\",\"session_id\":\"%s\",%s",f->epoch,f->session,extras);}
static JsonObject *begin(Fixture *f,const char *class,const char *id,const char *error) {
  char *x=g_strdup_printf("\"op_id\":\"%s\",\"class\":\"%s\",\"argument_digest\":\"" HASH "\"," SNAP,id,class),*e=bound(f,x);JsonObject *r=call(f,f->boot_pk,"begin",e,error);g_free(e);g_free(x);return r;
}
static void finish_id(Fixture *f,const char *id,const char *outcome,const char *error) {
  char *x=g_strdup_printf("\"op_id\":\"%s\",\"argument_digest\":\"" HASH "\",\"outcome\":\"%s\",\"proof_digest\":\"" HASH "\",\"generation\":\"0\",\"namespace_digest\":\"" HASH "\"",id,outcome),*e=bound(f,x);
  JsonObject *r=call(f,f->boot_pk,"finish",e,error);if(r)json_object_unref(r);g_free(x);g_free(e);
}
static void finish(Fixture *f,const char *outcome) {finish_id(f,OP,outcome,NULL);}
static void startup(Fixture *f) {enroll(f);activate(f);JsonObject *r=begin(f,"STARTUP",OP,NULL);CHECK(json_object_get_boolean_member(r,"created"));json_object_unref(r);finish(f,"STARTUP_CONFIRMED");}
static int64_t count(Fixture *f,const char *query) {sqlite3_stmt *s=NULL;CHECK(sqlite3_prepare_v2(signet_store_get_db(f->store),query,-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);int64_t n=sqlite3_column_int64(s,0);sqlite3_finalize(s);return n;}

static void test_lifecycle(void) {
  Fixture f;setup(&f);enroll(&f);
  call(&f,f.boot_pk,"open",SNAP,"not_active");activate(&f);
  JsonObject *r=call(&f,f.boot_pk,"open",SNAP,NULL);CHECK(!json_object_get_boolean_member(r,"created"));json_object_unref(r);
  call(&f,f.clone_pk,"open",SNAP,"not_active");
  begin(&f,"READ",OP,"not_active");r=begin(&f,"STARTUP",OP,NULL);CHECK(json_object_get_boolean_member(r,"created"));json_object_unref(r);
  r=begin(&f,"STARTUP",OP,NULL);CHECK(!json_object_get_boolean_member(r,"created"));json_object_unref(r);
  char *changed=bound(&f,"\"op_id\":\"" OP "\",\"class\":\"STARTUP\",\"argument_digest\":\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"," SNAP);
  call(&f,f.boot_pk,"begin",changed,"conflict");g_free(changed);
  char *seal=command(f.revision,"\"predecessor_proof_digest\":\"" HASH "\"");call(&f,f.operator_pk,"seal",seal,"busy");g_free(seal);
  finish(&f,"STARTUP_CONFIRMED");finish(&f,"STARTUP_CONFIRMED");
  r=begin(&f,"STARTUP",OP,NULL);CHECK(!json_object_get_boolean_member(r,"created"));json_object_unref(r);
  char *drain=command(f.revision,"");r=call(&f,f.boot_pk,"drain",drain,NULL);json_object_unref(r);g_free(drain);
  begin(&f,"READ","55555555-5555-5555-5555-555555555555","not_active");
  seal=command(f.revision,"\"predecessor_proof_digest\":\"" HASH "\"");r=call(&f,f.operator_pk,"seal",seal,NULL);g_assert_cmpstr(sget(r,"phase"),==,"SEALED");json_object_unref(r);g_free(seal);
  char *e=activate_args(&f,f.boot_pk);call(&f,f.operator_pk,"activate",e,"conflict");g_free(e);
  e=activate_args(&f,f.clone_pk);r=call(&f,f.operator_pk,"activate",e,NULL);g_assert_cmpstr(sget(r,"epoch"),==,"2");json_object_unref(r);g_free(e);
  call(&f,f.boot_pk,"open",SNAP,"not_active");r=call(&f,f.clone_pk,"open",SNAP,NULL);json_object_unref(r);
  teardown(&f);
}
static void test_uncertainty_reopen(void) {
  Fixture f;setup(&f);enroll(&f);activate(&f);JsonObject *r=begin(&f,"STARTUP",OP,NULL);json_object_unref(r);
  char *args=bound(&f,"\"op_id\":\"" OP "\",\"argument_digest\":\"" HASH "\"");
  r=call(&f,f.boot_pk,"uncertain",args,NULL);g_assert_cmpstr(sget(r,"phase"),==,"QUARANTINED");json_object_unref(r);g_free(args);
  signet_mgmt_handler_free(f.mgmt);f.mgmt=NULL;signet_backend_admission_free(f.a);signet_key_store_free(f.keys);
  char *child_args[]={self_path,"--probe-restart",f.path,f.operator_pk,f.boot_pk,f.session,f.epoch,NULL};
  int status=0;char *child_out=NULL,*child_err=NULL;CHECK(g_spawn_sync(NULL,child_args,NULL,G_SPAWN_DEFAULT,NULL,NULL,&child_out,&child_err,&status,NULL));
  if(!g_spawn_check_wait_status(status,NULL))g_printerr("restart child failed: %s\n",child_err?child_err:"");CHECK(g_spawn_check_wait_status(status,NULL));CHECK(strstr(child_out,"durable uncertainty observed"));g_free(child_out);g_free(child_err);
  SignetKeyStoreConfig k={.db_path=f.path,.master_key=MASTER};f.keys=open_keys(&k);CHECK(f.keys);f.store=signet_key_store_get_store(f.keys);f.a=signet_backend_admission_new(f.store,AUTH,f.operator_pk);CHECK(f.a);
  args=bound(&f,"\"op_id\":\"" OP "\"");r=call(&f,f.boot_pk,"status",args,NULL);g_assert_cmpstr(sget(json_object_get_object_member(r,"op"),"state"),==,"UNCERTAIN");json_object_unref(r);g_free(args);
  char *seal=command(f.revision,"\"predecessor_proof_digest\":\"" HASH "\"");call(&f,f.operator_pk,"seal",seal,"busy");g_free(seal);
  finish(&f,"NEVER_STARTED");begin(&f,"STARTUP","55555555-5555-5555-5555-555555555555","not_active");
  seal=command(f.revision,"\"predecessor_proof_digest\":\"" HASH "\"");r=call(&f,f.operator_pk,"seal",seal,NULL);json_object_unref(r);g_free(seal);
  CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops WHERE state='TERMINAL'")==1);teardown(&f);
}
static void test_command_identity(void) {
  Fixture f;setup(&f);char *args=command("0",SNAP);JsonObject *r=call(&f,f.operator_pk,"enroll",args,NULL);json_object_unref(r);
  r=call(&f,f.operator_pk,"enroll",args,NULL);CHECK(!json_object_get_boolean_member(r,"created"));json_object_unref(r);CHECK(count(&f,"SELECT count(*) FROM backend_admission_commands")==1);
  char *req=request("enroll",args);JsonObject *o=object(req),*p=json_object_get_object_member(o,"params");json_object_set_string_member(p,"generation","1");char *bad=encoded(o),*reply=signet_backend_admission_dispatch(f.a,f.operator_pk,bad,false);JsonObject *err=object(reply);g_assert_cmpstr(sget(json_object_get_object_member(err,"error"),"data"),==,"conflict");json_object_unref(err);json_object_unref(o);g_free(bad);g_free(reply);g_free(req);g_free(args);
  activate(&f);CHECK(signet_store_grant_provisioner(f.store,f.boot_pk,f.operator_pk,time(NULL))<0);
  CHECK(signet_store_grant_provisioner(f.store,f.clone_pk,f.operator_pk,time(NULL))==0);
  char *seal=command(f.revision,"\"predecessor_proof_digest\":\"" HASH "\"");r=call(&f,f.operator_pk,"seal",seal,NULL);json_object_unref(r);g_free(seal);
  args=activate_args(&f,f.clone_pk);call(&f,f.operator_pk,"activate",args,"unauthorized");g_free(args);teardown(&f);
}
static void test_strict(void) {
  Fixture f;setup(&f);char *args=command("0",SNAP),*req=request("enroll",args);
  const char *edits[]={",\"extra\":1",",\"generation\":\"0\"",",\"generation\":\"00\"",",\"protocol_version\":1"};
  for(size_t i=0;i<G_N_ELEMENTS(edits);i++) {
    size_t n=strlen(req);char *bad=g_strdup_printf("%.*s%s}}",(int)n-2,req,edits[i]);char *reply=signet_backend_admission_dispatch(f.a,f.operator_pk,bad,false);JsonObject *o=object(reply);CHECK(json_object_has_member(o,"error"));json_object_unref(o);g_free(reply);g_free(bad);
  }
  JsonObject *rpc=object(req),*params=json_object_get_object_member(rpc,"params");
  for(int variant=0;variant<3;variant++) {
    if(variant==0)json_object_set_double_member(params,"protocol_version",1.0);
    if(variant==1){json_object_set_int_member(params,"protocol_version",1);json_object_set_int_member(params,"generation",0);}
    if(variant==2){json_object_set_string_member(params,"generation","0");json_object_set_string_member(rpc,"id","sensitive-marker");}
    char *bad=encoded(rpc),*answer=signet_backend_admission_dispatch(f.a,f.operator_pk,bad,false);JsonObject *error=object(answer);CHECK(json_object_has_member(error,"error"));CHECK(!strstr(answer,"sensitive-marker"));json_object_unref(error);g_free(answer);g_free(bad);
  }
  json_object_unref(rpc);
  char *nul=request("enroll","\"command_id\":\"44444444-4444-4444-4444-444444444444\",\"expected_revision\":\"0\\u0000evil\"," SNAP),*answer=signet_backend_admission_dispatch(f.a,f.operator_pk,nul,false);JsonObject *error=object(answer);CHECK(json_object_has_member(error,"error"));json_object_unref(error);g_free(answer);g_free(nul);
  CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==0);
  char *reply=signet_backend_admission_dispatch(NULL,f.operator_pk,req,false);JsonObject *o=object(reply);g_assert_cmpstr(sget(json_object_get_object_member(o,"error"),"data"),==,"unavailable");json_object_unref(o);g_free(reply);
  reply=signet_backend_admission_dispatch(f.a,f.operator_pk,req,true);o=object(reply);g_assert_cmpstr(sget(json_object_get_object_member(o,"error"),"data"),==,"unauthorized");json_object_unref(o);g_free(reply);g_free(req);g_free(args);teardown(&f);
}
static void gift_at(Fixture *f,const char *sk,const char *req,int64_t at,int64_t live_since) {
  char *pk=nostr_key_get_public(sk);CHECK(pk);
  NostrEvent *rumor=nostr_nip17_create_rumor(pk,f->bunker_pk,req,at);CHECK(rumor);free(pk);
  NostrEvent *seal=nostr_nip17_create_seal(rumor,sk,f->bunker_pk);CHECK(seal);
  NostrEvent *event=nostr_nip17_create_gift_wrap(seal,f->bunker_pk);CHECK(event);char *json=nostr_event_serialize_compact(event);CHECK(json);
  SignetRelayEventView view={.kind=1059,.event_json=json,.event_id_hex=nostr_event_get_id(event),.live_since=live_since};
  (void)signet_mgmt_handler_handle_relay_event(f->mgmt,&view,time(NULL));free(json);nostr_event_free(event);nostr_event_free(seal);nostr_event_free(rumor);
}
static void gift(Fixture *f,const char *sk,const char *req) {gift_at(f,sk,req,time(NULL),time(NULL)-1);}
static void test_authenticated_ingress(void) {
  Fixture f;setup(&f);char *args=command("0",SNAP),*req=request("enroll",args);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt,f.operator_pk,req,"",time(NULL),time(NULL))<0);
  CHECK(signet_mgmt_handler_handle_direct_intent(f.mgmt,f.operator_pk,req,"",time(NULL),time(NULL))<0);
  CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==0);
  gift_at(&f,f.operator_sk,req,time(NULL),0);CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==0);
  gift_at(&f,f.operator_sk,req,time(NULL)-1000,time(NULL)-1001);CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==0);
  gift_at(&f,f.operator_sk,req,time(NULL)+1000,time(NULL)-1);CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==0);
  gift(&f,f.boot_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==0);
  gift(&f,f.operator_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==1);
  gift(&f,f.operator_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_commands")==1);
  g_free(req);g_free(args);
  f.revision=g_strdup("1");args=activate_args(&f,f.boot_pk);req=request("activate",args);gift(&f,f.clone_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_boots")==0);
  gift(&f,f.operator_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_boots")==1);
  JsonObject *r=call(&f,f.boot_pk,"open",SNAP,NULL);json_object_unref(r);g_free(args);g_free(req);
  args=bound(&f,"\"op_id\":\"" OP "\",\"class\":\"STARTUP\",\"argument_digest\":\"" HASH "\"," SNAP);req=request("begin",args);
  gift(&f,f.clone_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops")==0);
  SignetDenyList *deny=signet_deny_list_new(f.store);CHECK(deny);CHECK(signet_deny_list_add(deny,f.boot_pk,NULL,"fixture",time(NULL))==0);signet_mgmt_handler_set_deny_list(f.mgmt,deny);
  gift(&f,f.boot_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops")==0);
  signet_mgmt_handler_set_deny_list(f.mgmt,NULL);signet_deny_list_free(deny);
  gift(&f,f.boot_sk,req);gift(&f,f.boot_sk,req);CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops")==1);
  g_free(req);g_free(args);teardown(&f);
}
static void test_inconsistent_history(void) {
  Fixture f;setup(&f);startup(&f);
  CHECK(!signet_backend_admission_new(f.store,"99999999-9999-9999-9999-999999999999",f.operator_pk));
  CHECK(!signet_backend_admission_new(f.store,AUTH,f.clone_pk));
  CHECK(sqlite3_exec(signet_store_get_db(f.store),"DELETE FROM backend_admission_ops",NULL,NULL,NULL)==SQLITE_OK);
  CHECK(signet_backend_admission_validate_store(f.store)<0);
  signet_mgmt_handler_free(f.mgmt);f.mgmt=NULL;signet_backend_admission_free(f.a);f.a=NULL;signet_key_store_free(f.keys);f.keys=NULL;
  SignetStoreConfig cfg={.db_path=f.path,.master_key=MASTER};expect_store_mode();g_test_expect_message(NULL,G_LOG_LEVEL_CRITICAL,"*inconsistent backend admission history*");CHECK(!signet_store_open(&cfg));g_test_assert_expected_messages();teardown(&f);
}
static void test_quota_completion(void) {
  Fixture f;setup(&f);enroll(&f);
  const char *fill="WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<1023) INSERT INTO backend_admission_backends SELECT printf('66666666-6666-6666-6666-%012x',x),json_set(record,'$.backend_uuid',printf('66666666-6666-6666-6666-%012x',x)) FROM n,backend_admission_backends WHERE backend='" BACKEND "'";
  CHECK(sqlite3_exec(signet_store_get_db(f.store),fill,NULL,NULL,NULL)==SQLITE_OK);CHECK(signet_backend_admission_validate_store(f.store)==0);
  char *args=command("0",SNAP),*req=request("enroll",args);JsonObject *rpc=object(req),*p=json_object_get_object_member(rpc,"params");json_object_set_string_member(p,"backend_uuid","77777777-7777-7777-7777-777777777777");char *json=encoded(rpc),*reply=signet_backend_admission_dispatch(f.a,f.operator_pk,json,false);JsonObject *o=object(reply);g_assert_cmpstr(sget(json_object_get_object_member(o,"error"),"data"),==,"quota");json_object_unref(o);json_object_unref(rpc);g_free(args);g_free(req);g_free(json);g_free(reply);
  activate(&f);o=begin(&f,"STARTUP",OP,NULL);json_object_unref(o);finish(&f,"STARTUP_CONFIRMED");CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends")==1024);CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops WHERE state='TERMINAL'")==1);teardown(&f);
}
static void test_operation_classes(void) {
  Fixture f;setup(&f);startup(&f);
  const char *read="55555555-5555-5555-5555-555555555555",*infer="66666666-6666-6666-6666-666666666666",*config="77777777-7777-7777-7777-777777777777";
  JsonObject *r=begin(&f,"READ",read,NULL);json_object_unref(r);begin(&f,"CONFIG",config,"busy");finish_id(&f,read,"PROVIDER_TERMINAL","conflict");CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops WHERE state='ADMITTED'")==1);finish_id(&f,read,"READ_DONE",NULL);
  r=begin(&f,"INFER",infer,NULL);json_object_unref(r);finish_id(&f,infer,"PROVIDER_TERMINAL",NULL);
  r=begin(&f,"CONFIG",config,NULL);json_object_unref(r);
  char *x=g_strdup_printf("\"op_id\":\"%s\",\"argument_digest\":\"" HASH "\",\"outcome\":\"CONFIG_CONFIRMED\",\"proof_digest\":\"" HASH "\",\"generation\":\"1\",\"namespace_digest\":\"" HASH "\"",config),*args=bound(&f,x);r=call(&f,f.boot_pk,"finish",args,NULL);g_assert_cmpstr(sget(r,"generation"),==,"1");json_object_unref(r);g_free(x);g_free(args);
  r=begin(&f,"CONFIG",config,NULL);CHECK(!json_object_get_boolean_member(r,"created"));json_object_unref(r);
  x=g_strdup_printf("\"op_id\":\"%s\",\"class\":\"CONFIG\",\"argument_digest\":\"" HASH "\",\"profile_hash\":\"" HASH "\",\"generation\":\"1\",\"namespace_digest\":\"" HASH "\"",config);args=bound(&f,x);call(&f,f.boot_pk,"begin",args,"conflict");g_free(x);g_free(args);
  r=begin(&f,"READ",read,NULL);CHECK(!json_object_get_boolean_member(r,"created"));json_object_unref(r);
  char *cmd=command(f.revision,"");args=g_strdup_printf("%s,\"epoch\":\"%s\",\"session_id\":\"%s\"",cmd,f.epoch,f.session);r=call(&f,f.boot_pk,"detach",args,NULL);g_assert_cmpstr(sget(r,"phase"),==,"DRAINING");json_object_unref(r);g_free(cmd);g_free(args);
  call(&f,f.boot_pk,"open","\"profile_hash\":\"" HASH "\",\"generation\":\"1\",\"namespace_digest\":\"" HASH "\"","not_active");CHECK(signet_backend_admission_validate_store(f.store)==0);teardown(&f);
}

static void test_cleanup_capacity(void) {
  Fixture f;setup(&f);enroll(&f);activate(&f);JsonObject *r=begin(&f,"STARTUP",OP,NULL);json_object_unref(r);
  char *status=bound(&f,"\"op_id\":null");
  for(int i=0;i<120;i++) {
    char *req=request("status",status),*reply=signet_backend_admission_dispatch(f.a,f.boot_pk,req,false);JsonObject *o=object(reply);json_object_unref(o);g_free(req);g_free(reply);
  }
  call(&f,f.boot_pk,"status",status,"quota");g_free(status);
  char *uncertain=bound(&f,"\"op_id\":\"" OP "\",\"argument_digest\":\"" HASH "\"");r=call(&f,f.boot_pk,"uncertain",uncertain,NULL);json_object_unref(r);g_free(uncertain);
  /* Fill the unreserved command lane with retained, well-formed synthetic
   * history. Activation has already reserved three distinct shutdown slots. */
  const char *fill="WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<99995) INSERT INTO backend_admission_commands SELECT sender,printf('88888888-8888-8888-8888-%012x',x),backend,json_set(semantic,'$.command_id',printf('88888888-8888-8888-8888-%012x',x)) FROM n,(SELECT sender,backend,semantic FROM backend_admission_commands WHERE command=(SELECT command FROM backend_admission_commands LIMIT 1))";
  CHECK(sqlite3_exec(signet_store_get_db(f.store),fill,NULL,NULL,NULL)==SQLITE_OK);CHECK(count(&f,"SELECT commands+reserved FROM backend_admission_meta")==100000);
  char *refused=command("0",SNAP);call(&f,f.operator_pk,"enroll",refused,"quota");g_free(refused);
  finish(&f,"NEVER_STARTED");
  char *cmd=command(f.revision,"");r=call(&f,f.boot_pk,"drain",cmd,NULL);json_object_unref(r);g_free(cmd);
  cmd=command(f.revision,"");call(&f,f.boot_pk,"drain",cmd,"quota");g_free(cmd);
  cmd=command(f.revision,"");char *args=g_strdup_printf("%s,\"epoch\":\"%s\",\"session_id\":\"%s\"",cmd,f.epoch,f.session);r=call(&f,f.boot_pk,"detach",args,NULL);json_object_unref(r);g_free(cmd);g_free(args);
  cmd=command(f.revision,"\"predecessor_proof_digest\":\"" HASH "\"");r=call(&f,f.operator_pk,"seal",cmd,NULL);json_object_unref(r);g_free(cmd);
  CHECK(count(&f,"SELECT count(*) FROM backend_admission_commands")==100000);CHECK(count(&f,"SELECT reserved FROM backend_admission_meta")==0);CHECK(signet_backend_admission_validate_store(f.store)==0);teardown(&f);
}

typedef struct {Fixture *f;SignetBackendAdmission *a;char *args;GMutex *lock;GCond *cond;int *ready;bool *go;bool success;} Race;
static gpointer race_activate(gpointer data) {
  Race *r=data;g_mutex_lock(r->lock);(*r->ready)++;g_cond_broadcast(r->cond);while(!*r->go)g_cond_wait(r->cond,r->lock);g_mutex_unlock(r->lock);
  char *req=request("activate",r->args),*reply=signet_backend_admission_dispatch(r->a,r->f->operator_pk,req,false);JsonObject *o=object(reply);r->success=json_object_has_member(o,"result");json_object_unref(o);g_free(reply);g_free(req);return NULL;
}
static void test_concurrent_activation(void) {
  Fixture f;setup(&f);enroll(&f);SignetStoreConfig cfg={.db_path=f.path,.master_key=MASTER};SignetStore *other=open_store(&cfg);CHECK(other);SignetBackendAdmission *a=signet_backend_admission_new(other,AUTH,f.operator_pk);CHECK(a);
  GMutex lock;GCond cond;g_mutex_init(&lock);g_cond_init(&cond);int ready=0;bool go=false;
  Race r[2]={{&f,f.a,activate_args(&f,f.boot_pk),&lock,&cond,&ready,&go,false},{&f,a,activate_args(&f,f.clone_pk),&lock,&cond,&ready,&go,false}};
  GThread *threads[]={g_thread_new("activate-1",race_activate,&r[0]),g_thread_new("activate-2",race_activate,&r[1])};
  g_mutex_lock(&lock);while(ready<2)g_cond_wait(&cond,&lock);go=true;g_cond_broadcast(&cond);g_mutex_unlock(&lock);g_thread_join(threads[0]);g_thread_join(threads[1]);
  CHECK((r[0].success?1:0)+(r[1].success?1:0)==1);CHECK(count(&f,"SELECT count(*) FROM backend_admission_boots")==1);
  g_free(r[0].args);g_free(r[1].args);g_cond_clear(&cond);g_mutex_clear(&lock);signet_backend_admission_free(a);signet_store_close(other);teardown(&f);
}
static int deny_commit(void *data,int action,const char *first,const char *second,const char *db,const char *trigger) {
  (void)second;(void)db;(void)trigger;
  bool rollback=GPOINTER_TO_INT(data)!=0;
  if(action==SQLITE_TRANSACTION&&first&&(!strcmp(first,"COMMIT")||(rollback&&!strcmp(first,"ROLLBACK"))))return SQLITE_DENY;
  return SQLITE_OK;
}
static void test_commit_failure(void) {
  for(int deny_rollback=0;deny_rollback<2;deny_rollback++) {
    Fixture f;setup(&f);enroll(&f);activate(&f);sqlite3 *db=signet_store_get_db(f.store);
    CHECK(sqlite3_set_authorizer(db,deny_commit,GINT_TO_POINTER(deny_rollback))==SQLITE_OK);
    begin(&f,"STARTUP",OP,"unavailable");
    CHECK(sqlite3_set_authorizer(db,NULL,NULL)==SQLITE_OK);
    char *args=bound(&f,"\"op_id\":null");call(&f,f.boot_pk,"status",args,"unavailable");g_free(args);
    if(deny_rollback)CHECK(sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops")==0);teardown(&f);
  }
}
typedef struct {
  sqlite3 *db;
  bool inside_transaction;
  unsigned pre_reads;
  unsigned tx_reads;
  unsigned denied;
} RoleLookupDenial;
static int deny_role_lookup(void *data,int action,const char *table,const char *column,const char *db_name,const char *trigger) {
  (void)column;(void)db_name;(void)trigger;
  RoleLookupDenial *fault=data;
  if(action!=SQLITE_READ||!table||strcmp(table,"provisioners"))return SQLITE_OK;
  bool in_tx=sqlite3_get_autocommit(fault->db)==0;
  if(in_tx)fault->tx_reads++;else fault->pre_reads++;
  if(in_tx!=fault->inside_transaction)return SQLITE_OK;
  fault->denied++;return SQLITE_DENY;
}
static void test_role_lookup_sender(void) {
  Fixture f;setup(&f);enroll(&f);activate(&f);sqlite3 *db=signet_store_get_db(f.store);
  /* The exact same boot sender and table query must fail closed at both
   * boundaries, without changing the holder between fault phases. */
  for(int inside=0;inside<2;inside++) {
    RoleLookupDenial fault={.db=db,.inside_transaction=inside!=0};
    int changes=sqlite3_total_changes(db);
    CHECK(sqlite3_set_authorizer(db,deny_role_lookup,&fault)==SQLITE_OK);
    begin(&f,"STARTUP",OP,"unavailable");
    char *cmd=command(f.revision,"");call(&f,f.boot_pk,"drain",cmd,"unavailable");g_free(cmd);
    CHECK(sqlite3_set_authorizer(db,NULL,NULL)==SQLITE_OK);
    CHECK(fault.denied>=2);CHECK(fault.pre_reads>=2);
    if(inside)CHECK(fault.tx_reads>=2);else CHECK(fault.tx_reads==0);
    CHECK(sqlite3_get_autocommit(db)==1);CHECK(sqlite3_total_changes(db)==changes);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops")==0);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_commands")==2);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends WHERE json_extract(record,'$.phase')='STARTING' AND json_extract(record,'$.revision')='2'")==1);
  }
  JsonObject *r=begin(&f,"STARTUP",OP,NULL);CHECK(json_object_get_boolean_member(r,"created"));json_object_unref(r);finish(&f,"NEVER_STARTED");
  CHECK(signet_backend_admission_validate_store(f.store)==0);teardown(&f);
}
static void test_role_lookup_activation(void) {
  for(int provisioner=0;provisioner<2;provisioner++) {
    Fixture f;setup(&f);enroll(&f);sqlite3 *db=signet_store_get_db(f.store);
    if(provisioner)CHECK(signet_store_grant_provisioner(f.store,f.boot_pk,f.operator_pk,time(NULL))==0);
    RoleLookupDenial fault={.db=db,.inside_transaction=true};
    int changes=sqlite3_total_changes(db);char *args=activate_args(&f,f.boot_pk);
    CHECK(sqlite3_set_authorizer(db,deny_role_lookup,&fault)==SQLITE_OK);
    call(&f,f.operator_pk,"activate",args,"unavailable");
    CHECK(sqlite3_set_authorizer(db,NULL,NULL)==SQLITE_OK);
    CHECK(fault.denied==1);CHECK(fault.tx_reads==1);CHECK(fault.pre_reads==0);
    CHECK(sqlite3_get_autocommit(db)==1);CHECK(sqlite3_total_changes(db)==changes);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_boots")==0);CHECK(count(&f,"SELECT count(*) FROM backend_admission_ops")==0);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_commands")==1);CHECK(count(&f,"SELECT reserved FROM backend_admission_meta")==0);
    CHECK(count(&f,"SELECT count(*) FROM backend_admission_backends WHERE json_extract(record,'$.phase')='ENROLLED' AND json_extract(record,'$.revision')='1' AND json_extract(record,'$.epoch')='0'")==1);
    if(provisioner)call(&f,f.operator_pk,"activate",args,"unauthorized");
    else {JsonObject *r=call(&f,f.operator_pk,"activate",args,NULL);json_object_unref(r);}
    g_free(args);CHECK(signet_backend_admission_validate_store(f.store)==0);teardown(&f);
  }
}
static void test_partial_namespace(void) {
  Fixture f;setup(&f);
  CHECK(sqlite3_exec(signet_store_get_db(f.store),"DROP TABLE backend_admission_commands",NULL,NULL,NULL)==SQLITE_OK);
  CHECK(signet_backend_admission_validate_store(f.store)<0);CHECK(!signet_backend_admission_new(f.store,AUTH,f.operator_pk));
  CHECK(count(&f,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name='backend_admission_commands'")==0);teardown(&f);
}

static void test_config_pins(void) {
  char temp[]="/tmp/signet-admission-config-XXXXXX";int fd=g_mkstemp(temp);CHECK(fd>=0);close(fd);
  const char *operator="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  for(int variant=0;variant<3;variant++) {
    char *content=g_strdup_printf("[backend_admission]\nauthority_id=%s\noperator_pubkey=%s\n",AUTH,variant?"invalid":operator);CHECK(g_file_set_contents(temp,content,-1,NULL));g_free(content);
    char *args[]={self_path,"--probe-config",temp,variant==1?"invalid":"valid",NULL};
    char *env_base[]={"PATH=/usr/bin:/bin:/opt/homebrew/bin","HOME=/tmp",NULL};
    char *env_override[]={"PATH=/usr/bin:/bin:/opt/homebrew/bin","HOME=/tmp","SIGNET_BACKEND_AUTHORITY_ID=" AUTH,"SIGNET_BACKEND_OPERATOR_PUBKEY=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",NULL};
    int status=0;char *err=NULL;CHECK(g_spawn_sync(NULL,args,variant==2?env_override:env_base,G_SPAWN_DEFAULT,NULL,NULL,NULL,&err,&status,NULL));
    if(!g_spawn_check_wait_status(status,NULL))g_printerr("config child failed: %s\n",err?err:"");CHECK(g_spawn_check_wait_status(status,NULL));g_free(err);
  }
  unlink(temp);
}
static int probe_config(char **argv) {
  SignetConfig cfg;int rc=signet_config_load(argv[2],&cfg);
  if(!strcmp(argv[3],"invalid")){CHECK(rc<0);return 0;}
  CHECK(rc==0);g_assert_cmpstr(cfg.backend_authority_id,==,AUTH);g_assert_cmpstr(cfg.backend_operator_pubkey,==,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");signet_config_clear(&cfg);return 0;
}

static int probe_restart(char **argv) {
  SignetStoreConfig cfg={.db_path=argv[2],.master_key=MASTER};SignetStore *store=signet_store_open(&cfg);CHECK(store);
  SignetBackendAdmission *a=signet_backend_admission_new(store,AUTH,argv[3]);CHECK(a);
  char *extras=g_strdup_printf("\"epoch\":\"%s\",\"session_id\":\"%s\",\"op_id\":\"" OP "\"",argv[6],argv[5]),*req=request("status",extras),*reply=signet_backend_admission_dispatch(a,argv[4],req,false);JsonObject *o=object(reply);CHECK(json_object_has_member(o,"result"));JsonObject *r=json_object_get_object_member(o,"result");g_assert_cmpstr(sget(json_object_get_object_member(r,"op"),"state"),==,"UNCERTAIN");CHECK(!json_object_get_boolean_member(r,"created"));
  json_object_unref(o);g_free(extras);g_free(req);g_free(reply);signet_backend_admission_free(a);signet_store_close(store);g_print("durable uncertainty observed\n");return 0;
}
int main(int argc,char **argv) {
  CHECK(sodium_init()>=0);
  if(argc==7&&!strcmp(argv[1],"--probe-restart"))return probe_restart(argv);
  if(argc==4&&!strcmp(argv[1],"--probe-config"))return probe_config(argv);
  self_path=g_canonicalize_filename(argv[0],NULL);g_test_init(&argc,&argv,NULL);
  g_test_add_func("/backend/lifecycle",test_lifecycle);g_test_add_func("/backend/uncertainty-reopen",test_uncertainty_reopen);g_test_add_func("/backend/command-identity-roles",test_command_identity);g_test_add_func("/backend/strict",test_strict);g_test_add_func("/backend/authenticated-ingress",test_authenticated_ingress);g_test_add_func("/backend/inconsistent-history",test_inconsistent_history);g_test_add_func("/backend/concurrent-activation",test_concurrent_activation);
  g_test_add_func("/backend/quota-completion",test_quota_completion);g_test_add_func("/backend/operation-classes",test_operation_classes);
  g_test_add_func("/backend/cleanup-capacity",test_cleanup_capacity);
  g_test_add_func("/backend/commit-failure",test_commit_failure);g_test_add_func("/backend/partial-namespace",test_partial_namespace);
  g_test_add_func("/backend/config-pins",test_config_pins);
  g_test_add_func("/backend/role-lookup-sender",test_role_lookup_sender);g_test_add_func("/backend/role-lookup-activation",test_role_lookup_activation);
  int result=g_test_run();g_free(self_path);return result;
}
