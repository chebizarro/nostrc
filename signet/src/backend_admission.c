/* SPDX-License-Identifier: MIT
 * Backend admission v1: strict private protocol + one transactional durable
 * authority. There are no timers, leases, secret payloads or takeover workers.
 */
#include "signet/backend_admission.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

struct SignetBackendAdmission {
  sqlite3 *db; /* borrowed; the owning SignetStore must outlive this service */
  char *id;
  char *operator;
  gint healthy; /* atomic: a failed commit permanently closes this ingress */
  GHashTable *budgets; /* separate bounded requester domain; never evict */
};
typedef struct { gint64 window; unsigned count; } Budget;
static bool requester_admit(SignetBackendAdmission *a,const char *sender) {
  Budget *b=g_hash_table_lookup(a->budgets,sender);
  gint64 now=g_get_monotonic_time();
  if(!b) {
    if(g_hash_table_size(a->budgets)>=1024)return false;
    b=g_new0(Budget,1);b->window=now;
    g_hash_table_insert(a->budgets,g_strdup(sender),b);
  }
  if(now-b->window>=60*G_USEC_PER_SEC){b->window=now;b->count=0;}
  if(b->count>=120)return false;
  b->count++;return true;
}

static const char *str(JsonObject *o, const char *k) {
  JsonNode *n = o ? json_object_get_member(o, k) : NULL;
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
      ? json_node_get_string(n) : NULL;
}
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a,b); }
static bool hex(const char *s, size_t n) {
  if (!s || strlen(s) != n) return false;
  for (size_t i=0;i<n;i++) if (!(s[i]>='0'&&s[i]<='9') && !(s[i]>='a'&&s[i]<='f')) return false;
  return true;
}
static bool uuid(const char *s) {
  if (!s || strlen(s)!=36) return false;
  for (int i=0;i<36;i++) {
    if (i==8||i==13||i==18||i==23) { if(s[i]!='-') return false; }
    else if (!(s[i]>='0'&&s[i]<='9') && !(s[i]>='a'&&s[i]<='f')) return false;
  }
  return true;
}
static bool digest(const char *s) { return s && g_str_has_prefix(s,"sha256:") && hex(s+7,64); }
static bool decimal(const char *s) {
  if (!s || !s[0] || strlen(s)>19 || (s[0]=='0'&&s[1])) return false;
  for (const char *p=s;*p;p++) if (*p<'0'||*p>'9') return false;
  return strlen(s)<19 || strcmp(s,"9223372036854775807")<=0;
}
static bool increment(JsonObject *o,const char *k) {
  const char *v=str(o,k);
  if (!decimal(v) || eq(v,"9223372036854775807")) return false;
  char s[24]; g_snprintf(s,sizeof s,"%" G_GINT64_FORMAT,g_ascii_strtoll(v,NULL,10)+1);
  json_object_set_string_member(o,k,s); return true;
}
static char *encode(JsonObject *o) {
  JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_set_object(n,o);
  char *s=json_to_string(n,FALSE); json_node_free(n); return s;
}

typedef struct { GHashTable *members; bool duplicate; } ParseCheck;
static void member_seen(JsonParser *p,JsonObject *o,const char *name,void *data) {
  (void)p; ParseCheck *c=data;
  char *key=g_strdup_printf("%p/%s",(void*)o,name);
  if (g_hash_table_contains(c->members,key)) c->duplicate=true;
  g_hash_table_add(c->members,key);
}
static JsonObject *parse(const char *s) {
  /* All v1 strings are closed ASCII tokens. Reject escaped NUL before the
   * C-string JSON API could truncate a token or a member name. */
  if (!s || strlen(s)>65536 || strstr(s,"\\u0000")) return NULL;
  ParseCheck c={g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL),false};
  JsonParser *p=json_parser_new();
  g_signal_connect(p,"object-member",G_CALLBACK(member_seen),&c);
  bool ok=json_parser_load_from_data(p,s,-1,NULL);
  JsonNode *n=ok?json_parser_get_root(p):NULL;
  JsonObject *o=(!c.duplicate&&n&&JSON_NODE_HOLDS_OBJECT(n))?json_object_ref(json_node_get_object(n)):NULL;
  g_hash_table_unref(c.members); g_object_unref(p); return o;
}
static bool fields(JsonObject *o,const char *names) {
  char **a=g_strsplit(names,",",-1); size_t count=g_strv_length(a);
  bool ok=json_object_get_size(o)==count;
  for (size_t i=0;i<count;i++) if (!json_object_has_member(o,a[i])) ok=false;
  g_strfreev(a); return ok;
}
/* SQL helper: -1 error, 0 no row/DONE, 1 first-column row. All values bound. */
static int sql(sqlite3 *db,const char *query,const char **values,int n,char **out) {
  if(out) *out=NULL;
  sqlite3_stmt *s=NULL; int rc=sqlite3_prepare_v2(db,query,-1,&s,NULL);
  if(rc!=SQLITE_OK) return -1;
  for(int i=0;i<n;i++) if(sqlite3_bind_text(s,i+1,values[i],-1,SQLITE_TRANSIENT)!=SQLITE_OK) {sqlite3_finalize(s);return -1;}
  rc=sqlite3_step(s);
  if(rc==SQLITE_ROW&&out) {
    const unsigned char *v=sqlite3_column_text(s,0);
    if(v) *out=g_strdup((const char*)v);
  }
  int result=rc==SQLITE_ROW?1:rc==SQLITE_DONE?0:-1;
  if(sqlite3_finalize(s)!=SQLITE_OK) result=-1;
  return result;
}
/* Caller holds the connection mutex (and, for authorization, its transaction).
 * Preserve absence vs SQL failure: the legacy boolean store API cannot express
 * an unknown role and must not authorize a boot on that ambiguity. */
static int provisioner_role(sqlite3 *db,const char *pubkey) {
  const char *values[]={pubkey};
  return sql(db,"SELECT 1 FROM provisioners WHERE pubkey_hex=? COLLATE NOCASE LIMIT 1",values,1,NULL);
}
static bool exec(sqlite3 *db,const char *q) {return sqlite3_exec(db,q,NULL,NULL,NULL)==SQLITE_OK;}
static bool capacity(sqlite3 *db,const char *q) {
  char *s=NULL; int rc=sql(db,q,NULL,0,&s); bool ok=rc==1&&eq(s,"1");g_free(s);return ok;
}
static bool backend_valid(JsonObject *b) {
  if(!b||!fields(b,"backend_uuid,revision,epoch,session_id,boot_pubkey,phase,profile_hash,generation,namespace_digest,detached,cleanup_mask"))return false;
  const char *phase=str(b,"phase"),*session=str(b,"session_id"),*key=str(b,"boot_pubkey");
  bool enrolled=eq(phase,"ENROLLED");
  JsonNode *d=json_object_get_member(b,"detached");
  bool types=uuid(str(b,"backend_uuid"))&&decimal(str(b,"revision"))&&!eq(str(b,"revision"),"0")&&decimal(str(b,"epoch"))&&digest(str(b,"profile_hash"))&&decimal(str(b,"generation"))&&digest(str(b,"namespace_digest"))&&d&&json_node_get_value_type(d)==G_TYPE_BOOLEAN;
  if(!types||!decimal(str(b,"cleanup_mask"))||strlen(str(b,"cleanup_mask"))!=1||str(b,"cleanup_mask")[0]>'7')return false;
  if(eq(phase,"SEALED")&&!eq(str(b,"cleanup_mask"),"7"))return false;
  if(!eq(phase,"SEALED")&&str(b,"cleanup_mask")[0]>'3')return false;
  if((enrolled||eq(phase,"STARTING")||eq(phase,"ACTIVE"))&&!eq(str(b,"cleanup_mask"),"0"))return false;
  if(json_node_get_boolean(d)&&(eq(phase,"STARTING")||eq(phase,"ACTIVE")))return false;
  if(enrolled) return eq(str(b,"epoch"),"0")&&JSON_NODE_HOLDS_NULL(json_object_get_member(b,"session_id"))&&JSON_NODE_HOLDS_NULL(json_object_get_member(b,"boot_pubkey"))&&!json_node_get_boolean(d);
  return !eq(str(b,"epoch"),"0")&&uuid(session)&&hex(key,64)&&(eq(phase,"STARTING")||eq(phase,"ACTIVE")||eq(phase,"DRAINING")||eq(phase,"QUARANTINED")||eq(phase,"SEALED"));
}
static bool class_valid(const char *c) {return eq(c,"STARTUP")||eq(c,"READ")||eq(c,"CONFIG")||eq(c,"INFER");}
static bool op_valid(JsonObject *o) {
  if(!o||!fields(o,"op_id,class,argument_digest,state,outcome,proof_digest,epoch,session_id,boot_pubkey,generation,namespace_digest,begin_generation,begin_namespace_digest"))return false;
  if(!uuid(str(o,"op_id"))||!class_valid(str(o,"class"))||!digest(str(o,"argument_digest"))||!decimal(str(o,"epoch"))||eq(str(o,"epoch"),"0")||!uuid(str(o,"session_id"))||!hex(str(o,"boot_pubkey"),64)||!decimal(str(o,"generation"))||!digest(str(o,"namespace_digest")))return false;
  if(!decimal(str(o,"begin_generation"))||!digest(str(o,"begin_namespace_digest")))return false;
  const char *state=str(o,"state");
  if(!eq(str(o,"outcome"),"CONFIG_CONFIRMED")&&(!eq(str(o,"generation"),str(o,"begin_generation"))||!eq(str(o,"namespace_digest"),str(o,"begin_namespace_digest"))))return false;
  if(eq(state,"TERMINAL")) {
    const char *c=str(o,"class"),*v=str(o,"outcome");
    return digest(str(o,"proof_digest"))&&(eq(v,"NEVER_STARTED")||(eq(c,"READ")&&eq(v,"READ_DONE"))||(eq(c,"CONFIG")&&(eq(v,"CONFIG_NO_EFFECT")||eq(v,"CONFIG_CONFIRMED")))||(eq(c,"STARTUP")&&eq(v,"STARTUP_CONFIRMED"))||(eq(c,"INFER")&&eq(v,"PROVIDER_TERMINAL")));
  }
  return (eq(state,"ADMITTED")||eq(state,"UNCERTAIN"))&&JSON_NODE_HOLDS_NULL(json_object_get_member(o,"outcome"))&&JSON_NODE_HOLDS_NULL(json_object_get_member(o,"proof_digest"));
}
static bool save_backend(SignetBackendAdmission *a,JsonObject *b) {
  char *record=encode(b);const char *v[]={record,str(b,"backend_uuid")};
  bool ok=sql(a->db,"UPDATE backend_admission_backends SET record=? WHERE backend=?",v,2,NULL)==0;
  g_free(record);return ok;
}
static bool save_op(SignetBackendAdmission *a,const char *backend,JsonObject *o) {
  char *record=encode(o);const char *v[]={record,str(o,"state"),backend,str(o,"op_id")};
  bool ok=sql(a->db,"UPDATE backend_admission_ops SET record=?,state=? WHERE backend=? AND op=?",v,4,NULL)==0;
  g_free(record);return ok;
}
static const char *schema=
 "CREATE TABLE backend_admission_meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),version INTEGER NOT NULL CHECK(version=1),authority TEXT NOT NULL,operator TEXT NOT NULL,backends INTEGER NOT NULL DEFAULT 0,boots INTEGER NOT NULL DEFAULT 0,ops INTEGER NOT NULL DEFAULT 0,commands INTEGER NOT NULL DEFAULT 0,reserved INTEGER NOT NULL DEFAULT 0);"
 "CREATE TABLE backend_admission_backends(backend TEXT PRIMARY KEY NOT NULL,record TEXT NOT NULL);"
 "CREATE TABLE backend_admission_boots(pubkey TEXT PRIMARY KEY NOT NULL,backend TEXT NOT NULL REFERENCES backend_admission_backends(backend),epoch TEXT NOT NULL,session TEXT UNIQUE NOT NULL,UNIQUE(backend,epoch));"
 "CREATE TABLE backend_admission_ops(backend TEXT NOT NULL REFERENCES backend_admission_backends(backend),op TEXT NOT NULL,epoch TEXT NOT NULL,session TEXT NOT NULL REFERENCES backend_admission_boots(session),state TEXT NOT NULL CHECK(state IN ('ADMITTED','UNCERTAIN','TERMINAL')),record TEXT NOT NULL,PRIMARY KEY(backend,op));"
 "CREATE UNIQUE INDEX backend_admission_one_outstanding ON backend_admission_ops(backend) WHERE state!='TERMINAL';"
 "CREATE TABLE backend_admission_commands(sender TEXT NOT NULL,command TEXT NOT NULL,backend TEXT NOT NULL REFERENCES backend_admission_backends(backend),semantic TEXT NOT NULL,PRIMARY KEY(sender,command));"
 "CREATE TRIGGER backend_admission_count_backends AFTER INSERT ON backend_admission_backends BEGIN UPDATE backend_admission_meta SET backends=backends+1; END;"
 "CREATE TRIGGER backend_admission_count_boots AFTER INSERT ON backend_admission_boots BEGIN UPDATE backend_admission_meta SET boots=boots+1; END;"
 "CREATE TRIGGER backend_admission_count_ops AFTER INSERT ON backend_admission_ops BEGIN UPDATE backend_admission_meta SET ops=ops+1; END;"
 "CREATE TRIGGER backend_admission_count_commands AFTER INSERT ON backend_admission_commands BEGIN UPDATE backend_admission_meta SET commands=commands+1; END;"
 "CREATE TRIGGER backend_admission_provisioner_insert BEFORE INSERT ON provisioners WHEN EXISTS(SELECT 1 FROM backend_admission_boots WHERE pubkey=lower(NEW.pubkey_hex)) BEGIN SELECT RAISE(ABORT,'backend boot is not a provisioner'); END;"
 "CREATE TRIGGER backend_admission_provisioner_update BEFORE UPDATE OF pubkey_hex ON provisioners WHEN EXISTS(SELECT 1 FROM backend_admission_boots WHERE pubkey=lower(NEW.pubkey_hex)) BEGIN SELECT RAISE(ABORT,'backend boot is not a provisioner'); END;";

static bool params_valid(JsonObject *p,const char *m);
static char *semantic(JsonObject *p,const char *method);

static bool validate_history(SignetBackendAdmission *a) {
  if(!capacity(a->db,"SELECT count(*)=1 AND min(backends)=(SELECT count(*) FROM backend_admission_backends) AND min(boots)=(SELECT count(*) FROM backend_admission_boots) AND min(ops)=(SELECT count(*) FROM backend_admission_ops) AND min(commands)=(SELECT count(*) FROM backend_admission_commands) AND min(backends)<=1024 AND min(boots)<=100000 AND min(ops)<=1000000 AND min(reserved)>=0 AND min(commands)+min(reserved)<=100000 FROM backend_admission_meta"))return false;
  if(!capacity(a->db,"SELECT count(*)=6 FROM sqlite_master WHERE type='trigger' AND name LIKE 'backend_admission_%'"))return false;
  if(!capacity(a->db,"SELECT count(*)=1 FROM sqlite_master WHERE type='index' AND name='backend_admission_one_outstanding'"))return false;
  if(!capacity(a->db,"SELECT count(*)=0 FROM (SELECT backend FROM backend_admission_ops WHERE state!='TERMINAL' GROUP BY backend HAVING count(*)>1)"))return false;
  sqlite3_stmt *s=NULL;int rc=sqlite3_prepare_v2(a->db,"SELECT backend,record FROM backend_admission_backends",-1,&s,NULL);
  if(rc!=SQLITE_OK)return false;
  bool ok=true;int reserved=0;
  while((rc=sqlite3_step(s))==SQLITE_ROW&&ok) {
    const char *id=(const char*)sqlite3_column_text(s,0),*record=(const char*)sqlite3_column_text(s,1);
    JsonObject *b=parse(record);ok=backend_valid(b)&&eq(id,str(b,"backend_uuid"));
    if(ok&&!eq(str(b,"phase"),"ENROLLED")&&!eq(str(b,"phase"),"SEALED")) {
      int mask=str(b,"cleanup_mask")[0]-'0';reserved+=3-((mask&1)?1:0)-((mask&2)?1:0);
    }
    if(ok&&!eq(str(b,"phase"),"ENROLLED")) {
      const char *v[]={str(b,"boot_pubkey"),id,str(b,"epoch"),str(b,"session_id")};char *found=NULL;
      ok=sql(a->db,"SELECT pubkey FROM backend_admission_boots WHERE pubkey=? AND backend=? AND epoch=? AND session=?",v,4,&found)==1&&eq(found,str(b,"boot_pubkey"));g_free(found);
    }
    if(ok) {
      char *boots=NULL;const char *v[]={id};
      ok=sql(a->db,"SELECT count(*) FROM backend_admission_boots WHERE backend=?",v,1,&boots)==1&&eq(boots,str(b,"epoch"));g_free(boots);
    }
    if(b)json_object_unref(b);
  }
  sqlite3_finalize(s);if(rc!=SQLITE_DONE||!ok)return false;
  char *reserved_text=NULL;
  if(sql(a->db,"SELECT reserved FROM backend_admission_meta",NULL,0,&reserved_text)!=1)return false;
  ok=decimal(reserved_text)&&g_ascii_strtoll(reserved_text,NULL,10)==reserved;g_free(reserved_text);if(!ok)return false;
  rc=sqlite3_prepare_v2(a->db,"SELECT o.backend,o.op,o.epoch,o.session,o.state,o.record,b.record FROM backend_admission_ops o JOIN backend_admission_backends b ON o.backend=b.backend",-1,&s,NULL);
  if(rc!=SQLITE_OK)return false;
  while((rc=sqlite3_step(s))==SQLITE_ROW&&ok) {
    JsonObject *o=parse((const char*)sqlite3_column_text(s,5)),*b=parse((const char*)sqlite3_column_text(s,6));
    ok=op_valid(o)&&backend_valid(b)&&eq((const char*)sqlite3_column_text(s,1),str(o,"op_id"))&&eq((const char*)sqlite3_column_text(s,2),str(o,"epoch"))&&eq((const char*)sqlite3_column_text(s,3),str(o,"session_id"))&&eq((const char*)sqlite3_column_text(s,4),str(o,"state"));
    if(ok&&!eq(str(o,"state"),"TERMINAL"))ok=eq(str(o,"epoch"),str(b,"epoch"))&&eq(str(o,"session_id"),str(b,"session_id"))&&eq(str(o,"boot_pubkey"),str(b,"boot_pubkey"))&&!eq(str(b,"phase"),"SEALED")&&!eq(str(b,"phase"),"ENROLLED")&&(!eq(str(o,"state"),"UNCERTAIN")||eq(str(b,"phase"),"QUARANTINED")||eq(str(b,"phase"),"DRAINING"));
    if(ok) {
      const char *v[]={(const char*)sqlite3_column_text(s,0),str(o,"boot_pubkey"),str(o,"epoch"),str(o,"session_id")};char *found=NULL;
      ok=sql(a->db,"SELECT pubkey FROM backend_admission_boots WHERE backend=? AND pubkey=? AND epoch=? AND session=?",v,4,&found)==1;g_free(found);
    }
    if(o)json_object_unref(o);if(b)json_object_unref(b);
  }
  sqlite3_finalize(s);if(rc!=SQLITE_DONE||!ok)return false;
  rc=sqlite3_prepare_v2(a->db,"SELECT k.pubkey,k.backend,k.epoch,k.session,b.record FROM backend_admission_boots k JOIN backend_admission_backends b ON b.backend=k.backend",-1,&s,NULL);
  if(rc!=SQLITE_OK)return false;
  while((rc=sqlite3_step(s))==SQLITE_ROW&&ok) {
    const char *key=(const char*)sqlite3_column_text(s,0),*id=(const char*)sqlite3_column_text(s,1),*epoch=(const char*)sqlite3_column_text(s,2),*session_id=(const char*)sqlite3_column_text(s,3);
    JsonObject *b=parse((const char*)sqlite3_column_text(s,4));
    ok=hex(key,64)&&!eq(key,a->operator)&&uuid(id)&&decimal(epoch)&&!eq(epoch,"0")&&uuid(session_id)&&backend_valid(b)&&g_ascii_strtoll(epoch,NULL,10)<=g_ascii_strtoll(str(b,"epoch"),NULL,10);
    if(b)json_object_unref(b);
  }
  sqlite3_finalize(s);if(rc!=SQLITE_DONE||!ok)return false;
  rc=sqlite3_prepare_v2(a->db,"SELECT sender,command,backend,semantic FROM backend_admission_commands",-1,&s,NULL);
  if(rc!=SQLITE_OK)return false;
  while((rc=sqlite3_step(s))==SQLITE_ROW&&ok) {
    const char *sender=(const char*)sqlite3_column_text(s,0),*command=(const char*)sqlite3_column_text(s,1),*backend=(const char*)sqlite3_column_text(s,2),*record=(const char*)sqlite3_column_text(s,3);
    JsonObject *p=parse(record);char *method=p?g_strdup(str(p,"method")):NULL,*canonical=NULL;
    ok=hex(sender,64)&&uuid(command)&&uuid(backend)&&p&&method;
    if(ok) {
      json_object_remove_member(p,"method");json_object_set_string_member(p,"correlation_nonce","00000000-0000-0000-0000-000000000000");
      ok=params_valid(p,method)&&eq(str(p,"authority_id"),a->id)&&eq(str(p,"command_id"),command)&&eq(str(p,"backend_uuid"),backend);
      canonical=semantic(p,method);ok=ok&&eq(canonical,record);
      if(ok&&!eq(sender,a->operator)) {
        char *found=NULL;const char *v[]={sender,backend};
        ok=(eq(method,"backend/drain")||eq(method,"backend/detach"))&&sql(a->db,"SELECT pubkey FROM backend_admission_boots WHERE pubkey=? AND backend=?",v,2,&found)==1;g_free(found);
      }
    }
    if(p)json_object_unref(p);g_free(method);g_free(canonical);
  }
  sqlite3_finalize(s);if(rc!=SQLITE_DONE||!ok)return false;
  char *bad=NULL;rc=sql(a->db,"PRAGMA foreign_key_check",NULL,0,&bad);g_free(bad);
  if(rc!=0)return false;
  rc=sql(a->db,"SELECT pubkey FROM backend_admission_boots WHERE pubkey IN (SELECT lower(pubkey_hex) FROM provisioners) LIMIT 1",NULL,0,&bad);g_free(bad);
  return rc==0;
}
SignetBackendAdmission *signet_backend_admission_new(SignetStore *store,const char *id,const char *operator) {
  if(!uuid(id)||!hex(operator,64))return NULL;
  sqlite3 *db=signet_store_get_db(store);if(!db)return NULL;
  SignetBackendAdmission *a=g_new0(SignetBackendAdmission,1);a->db=db;a->id=g_strdup(id);a->operator=g_strdup(operator);a->budgets=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
  sqlite3_mutex *m=sqlite3_db_mutex(db);sqlite3_mutex_enter(m);
  bool begun=exec(db,"BEGIN IMMEDIATE"),ok=begun;char *count=NULL;
  if(ok)ok=sql(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name LIKE 'backend_admission_%'",NULL,0,&count)==1;
  if(ok&&eq(count,"0")) {
    ok=exec(db,schema);
    const char *v[]={id,operator};if(ok)ok=sql(db,"INSERT INTO backend_admission_meta(singleton,version,authority,operator) VALUES(1,1,?,?)",v,2,NULL)==0;
  } else if(ok)ok=eq(count,"5");
  g_free(count);
  const char *v[]={id,operator};char *pin=NULL;
  if(ok)ok=sql(db,"SELECT authority FROM backend_admission_meta WHERE singleton=1 AND version=1 AND authority=? AND operator=?",v,2,&pin)==1&&eq(pin,id);
  g_free(pin);
  if(ok)ok=validate_history(a);
  if(ok)ok=exec(db,"COMMIT");
  if(!ok&&begun)exec(db,"ROLLBACK");
  sqlite3_mutex_leave(m);
  if(!ok){signet_backend_admission_free(a);return NULL;}
  g_atomic_int_set(&a->healthy,1);return a;
}
void signet_backend_admission_free(SignetBackendAdmission *a) {if(a){g_free(a->id);g_free(a->operator);g_hash_table_unref(a->budgets);g_free(a);}}

int signet_backend_admission_validate_store(SignetStore *store) {
  sqlite3 *db=signet_store_get_db(store);if(!db)return -1;
  char *count=NULL,*id=NULL,*operator=NULL;
  int rc=sql(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name LIKE 'backend_admission_%'",NULL,0,&count);
  bool absent=rc==1&&eq(count,"0");g_free(count);if(absent)return 0;
  if(rc!=1||sql(db,"SELECT authority FROM backend_admission_meta WHERE singleton=1",NULL,0,&id)!=1||sql(db,"SELECT operator FROM backend_admission_meta WHERE singleton=1",NULL,0,&operator)!=1){g_free(id);g_free(operator);return -1;}
  SignetBackendAdmission *a=signet_backend_admission_new(store,id,operator);
  g_free(id);g_free(operator);if(!a)return -1;
  signet_backend_admission_free(a);return 0;
}

bool signet_backend_admission_request(const char *json) {
  /* Classification only. Strict duplicate checks happen before any transition. */
  if(!json||strlen(json)>65536)return false;
  JsonParser *p=json_parser_new();bool result=false;
  if(json_parser_load_from_data(p,json,-1,NULL)) {
    JsonNode *n=json_parser_get_root(p);
    if(n&&JSON_NODE_HOLDS_OBJECT(n)){const char *m=str(json_node_get_object(n),"method");result=m&&g_str_has_prefix(m,"backend/");}
  }
  g_object_unref(p);return result;
}

static const char *extra(const char *m) {
  if(eq(m,"backend/enroll"))return "command_id,expected_revision,profile_hash,generation,namespace_digest";
  if(eq(m,"backend/activate"))return "command_id,expected_revision,boot_pubkey,profile_hash,generation,namespace_digest";
  if(eq(m,"backend/open"))return "profile_hash,generation,namespace_digest";
  if(eq(m,"backend/begin"))return "epoch,session_id,op_id,class,argument_digest,profile_hash,generation,namespace_digest";
  if(eq(m,"backend/status"))return "epoch,session_id,op_id";
  if(eq(m,"backend/finish"))return "epoch,session_id,op_id,argument_digest,outcome,proof_digest,generation,namespace_digest";
  if(eq(m,"backend/uncertain"))return "epoch,session_id,op_id,argument_digest";
  if(eq(m,"backend/drain"))return "command_id,expected_revision";
  if(eq(m,"backend/detach"))return "command_id,expected_revision,epoch,session_id";
  if(eq(m,"backend/seal"))return "command_id,expected_revision,predecessor_proof_digest";
  return NULL;
}
static bool params_valid(JsonObject *p,const char *m) {
  const char *e=extra(m);if(!e)return false;
  char *names=g_strconcat("protocol_version,authority_id,backend_uuid,correlation_nonce,",e,NULL);
  bool ok=fields(p,names);g_free(names);if(!ok)return false;
  JsonNode *v=json_object_get_member(p,"protocol_version");
  if(!v||json_node_get_value_type(v)!=G_TYPE_INT64||json_node_get_int(v)!=1)return false;
  GList *keys=json_object_get_members(p);
  for(GList *l=keys;l&&ok;l=l->next) {
    const char *k=l->data,*s=str(p,k);
    if(eq(k,"protocol_version"))continue;
    if(eq(k,"op_id")&&eq(m,"backend/status")&&JSON_NODE_HOLDS_NULL(json_object_get_member(p,k)))continue;
    if(eq(k,"authority_id")||eq(k,"backend_uuid")||eq(k,"correlation_nonce")||eq(k,"session_id")||eq(k,"command_id")||eq(k,"op_id"))ok=uuid(s);
    else if(eq(k,"epoch")||eq(k,"generation")||eq(k,"expected_revision"))ok=decimal(s);
    else if(eq(k,"boot_pubkey"))ok=hex(s,64);
    else if(eq(k,"class"))ok=class_valid(s);
    else if(eq(k,"outcome"))ok=s&&(eq(s,"NEVER_STARTED")||eq(s,"READ_DONE")||eq(s,"CONFIG_NO_EFFECT")||eq(s,"STARTUP_CONFIRMED")||eq(s,"CONFIG_CONFIRMED")||eq(s,"PROVIDER_TERMINAL"));
    else ok=digest(s);
  }
  g_list_free(keys);return ok;
}
static char *semantic(JsonObject *p,const char *method) {
  JsonObject *o=json_object_new();json_object_set_string_member(o,"method",method);
  GList *keys=g_list_sort(json_object_get_members(p),(GCompareFunc)strcmp);
  for(GList *l=keys;l;l=l->next)if(!eq(l->data,"correlation_nonce"))json_object_set_member(o,l->data,json_node_copy(json_object_get_member(p,l->data)));
  g_list_free(keys);char *s=encode(o);json_object_unref(o);return s;
}
static JsonObject *operation_result(JsonObject *o) {
  if(!o)return NULL;
  JsonObject *r=json_object_new();const char *keys[]={"op_id","class","argument_digest","state","outcome","proof_digest"};
  for(size_t i=0;i<G_N_ELEMENTS(keys);i++)json_object_set_member(r,keys[i],json_node_copy(json_object_get_member(o,keys[i])));
  return r;
}
static char *response(JsonObject *p,const char *method,JsonObject *b,JsonObject *op,bool created,const char *reason,const char *id) {
  JsonObject *rpc=json_object_new();json_object_set_string_member(rpc,"jsonrpc","2.0");
  if(id)json_object_set_string_member(rpc,"id",id);else json_object_set_null_member(rpc,"id");
  if(reason) {
    JsonObject *e=json_object_new();int code=eq(reason,"invalid_request")?-32602:eq(reason,"unauthorized")?-32002:eq(reason,"unavailable")?-32000:-32010;
    json_object_set_int_member(e,"code",code);json_object_set_string_member(e,"message","backend admission refused");json_object_set_string_member(e,"data",reason);json_object_set_object_member(rpc,"error",e);
  } else {
    JsonObject *r=json_object_new();json_object_set_int_member(r,"protocol_version",1);
    const char *common[]={"authority_id","backend_uuid","correlation_nonce"};
    for(size_t i=0;i<G_N_ELEMENTS(common);i++)json_object_set_string_member(r,common[i],str(p,common[i]));
    json_object_set_string_member(r,"method",method);
    const char *keys[]={"revision","epoch","session_id","phase","boot_pubkey","profile_hash","generation","namespace_digest"};
    for(size_t i=0;i<G_N_ELEMENTS(keys);i++)json_object_set_member(r,keys[i],json_node_copy(json_object_get_member(b,keys[i])));
    if(op)json_object_set_object_member(r,"op",operation_result(op));else json_object_set_null_member(r,"op");
    json_object_set_boolean_member(r,"created",created);json_object_set_object_member(rpc,"result",r);
  }
  char *s=encode(rpc);json_object_unref(rpc);return s;
}
static bool snapshot(JsonObject *p,JsonObject *b) {
  return eq(str(p,"profile_hash"),str(b,"profile_hash"))&&eq(str(p,"generation"),str(b,"generation"))&&eq(str(p,"namespace_digest"),str(b,"namespace_digest"));
}
static bool session(JsonObject *p,JsonObject *b) {return eq(str(p,"epoch"),str(b,"epoch"))&&eq(str(p,"session_id"),str(b,"session_id"));}
static void copy_fields(JsonObject *to,JsonObject *from,const char *names) {
  char **keys=g_strsplit(names,",",-1);
  for(int i=0;keys[i];i++)json_object_set_member(to,keys[i],json_node_copy(json_object_get_member(from,keys[i])));
  g_strfreev(keys);
}

char *signet_backend_admission_dispatch(SignetBackendAdmission *a,const char *sender,const char *json,bool denied) {
  JsonObject *rpc=parse(json),*p=NULL,*b=NULL,*op=NULL;const char *reason="invalid_request",*method=rpc?str(rpc,"method"):NULL,*id=rpc?str(rpc,"id"):NULL;
  if(!uuid(id))id=NULL; /* Never echo malformed, possibly sensitive input. */
  bool created=false,begun=false,changed=false;int cleanup_bit=0;char *sem=NULL,*record=NULL,*saved=NULL;
  sqlite3_mutex *mutex=NULL;
  if(!rpc||!fields(rpc,"jsonrpc,id,method,params")||!eq(str(rpc,"jsonrpc"),"2.0")||!uuid(id))goto done;
  JsonNode *pn=json_object_get_member(rpc,"params");if(!pn||!JSON_NODE_HOLDS_OBJECT(pn))goto done;
  p=json_node_get_object(pn);
  if(!params_valid(p,method)||!eq(id,str(p,"correlation_nonce")))goto done;
  reason="unavailable";if(!a||!g_atomic_int_get(&a->healthy))goto done;
  reason="authority_mismatch";if(!eq(a->id,str(p,"authority_id")))goto done;
  reason="unauthorized";if(denied||!hex(sender,64))goto done;
  bool operator=eq(sender,a->operator),command=json_object_has_member(p,"command_id");
  bool operator_only=eq(method,"backend/enroll")||eq(method,"backend/activate")||eq(method,"backend/seal");
  if(operator_only&&!operator)goto done;
  mutex=sqlite3_db_mutex(a->db);sqlite3_mutex_enter(mutex);
  reason="unavailable";if(!g_atomic_int_get(&a->healthy))goto done;
  /* This preflight does not replace the transactional role check. Both reads
   * are tri-state and mutex-held; the second is inside the write transaction. */
  if(!operator) {
    int role=provisioner_role(a->db,sender);
    if(role<0)goto done;
    if(role>0){reason="unauthorized";goto done;}
  }
  reason="unavailable";begun=exec(a->db,"BEGIN IMMEDIATE");if(!begun)goto done;
  const char *backend=str(p,"backend_uuid"),*lookup[]={backend};
  int rc=sql(a->db,"SELECT record FROM backend_admission_backends WHERE backend=?",lookup,1,&record);
  if(rc<0)goto done;
  if(record){b=parse(record);if(!backend_valid(b))goto done;}
  reason="unauthorized";
  bool holder=b&&eq(sender,str(b,"boot_pubkey"))&&!eq(str(b,"phase"),"SEALED")&&!eq(str(b,"phase"),"ENROLLED");
  if(!operator_only && !(eq(method,"backend/drain")?operator||holder:holder)) {
    if(eq(method,"backend/open"))reason="not_active";
    goto done;
  }
  if(!operator) {
    reason="unavailable";int role=provisioner_role(a->db,sender);
    if(role<0)goto done;
    reason="unauthorized";if(role>0)goto done;
  }
  if(json_object_has_member(p,"session_id")&&(!holder||!session(p,b)))goto done;
  bool cleanup=eq(method,"backend/finish")||eq(method,"backend/uncertain")||eq(method,"backend/drain")||eq(method,"backend/detach")||eq(method,"backend/seal");
  reason="quota";if(!cleanup&&!requester_admit(a,sender))goto done;
  /* Revalidate privilege conflicts in the same connection transaction. */
  reason="unavailable";
  if(command) {
    sem=semantic(p,method);const char *v[]={sender,str(p,"command_id")};
    rc=sql(a->db,"SELECT semantic FROM backend_admission_commands WHERE sender=? AND command=?",v,2,&saved);
    if(rc<0)goto done;
    if(saved){reason="conflict";if(!eq(saved,sem))goto done;reason=NULL;goto commit;}
    reason="quota";
    if(b&&!eq(str(b,"phase"),"ENROLLED")&&!eq(str(b,"phase"),"SEALED")) {
      int bit=eq(method,"backend/drain")?1:eq(method,"backend/detach")?2:eq(method,"backend/seal")?4:0;
      if(bit&&!((str(b,"cleanup_mask")[0]-'0')&bit))cleanup_bit=bit;
    }
    if(!cleanup_bit&&!capacity(a->db,eq(method,"backend/activate")?
        "SELECT commands+reserved+4<=100000 FROM backend_admission_meta":
        "SELECT commands+reserved<100000 FROM backend_admission_meta"))goto done;
    reason="conflict";
    if(b?!eq(str(p,"expected_revision"),str(b,"revision")):!eq(str(p,"expected_revision"),"0"))goto done;
  }
  if(eq(method,"backend/enroll")) {
    reason="conflict";if(b)goto done;
    reason="quota";if(!capacity(a->db,"SELECT count(*)<1024 FROM backend_admission_backends"))goto done;
    b=json_object_new();copy_fields(b,p,"backend_uuid,profile_hash,generation,namespace_digest");
    json_object_set_string_member(b,"revision","1");json_object_set_string_member(b,"epoch","0");json_object_set_string_member(b,"phase","ENROLLED");json_object_set_null_member(b,"session_id");json_object_set_null_member(b,"boot_pubkey");json_object_set_boolean_member(b,"detached",false);json_object_set_string_member(b,"cleanup_mask","0");
    char *data=encode(b);const char *v[]={backend,data};rc=sql(a->db,"INSERT INTO backend_admission_backends VALUES(?,?)",v,2,NULL);g_free(data);reason="unavailable";if(rc<0)goto done;
  } else {
    reason="not_active";if(!b)goto done;
    const char *phase=str(b,"phase");
    if(eq(method,"backend/activate")) {
      reason="conflict";if(!(eq(phase,"ENROLLED")||eq(phase,"SEALED"))||!snapshot(p,b))goto done;
      const char *key=str(p,"boot_pubkey");reason="unauthorized";
      if(eq(key,a->operator))goto done;
      reason="unavailable";int role=provisioner_role(a->db,key);
      if(role<0)goto done;
      reason="unauthorized";if(role>0)goto done;
      char *old=NULL;const char *v[]={key};rc=sql(a->db,"SELECT pubkey FROM backend_admission_boots WHERE pubkey=?",v,1,&old);g_free(old);
      reason="conflict";if(rc!=0)goto done;
      reason="quota";if(!capacity(a->db,"SELECT count(*)<100000 FROM backend_admission_boots"))goto done;
      if(!increment(b,"epoch"))goto done;
      char *sid=g_uuid_string_random();json_object_set_string_member(b,"session_id",sid);g_free(sid);
      json_object_set_string_member(b,"boot_pubkey",key);json_object_set_string_member(b,"phase","STARTING");json_object_set_boolean_member(b,"detached",false);json_object_set_string_member(b,"cleanup_mask","0");
      reason="unavailable";if(!exec(a->db,"UPDATE backend_admission_meta SET reserved=reserved+3"))goto done;
      const char *vals[]={key,backend,str(b,"epoch"),str(b,"session_id")};reason="unavailable";
      if(sql(a->db,"INSERT INTO backend_admission_boots VALUES(?,?,?,?)",vals,4,NULL)<0)goto done;
      changed=true;
    } else if(eq(method,"backend/open")) {
      reason="conflict";if(!snapshot(p,b))goto done;
      reason="not_active";if(json_object_get_boolean_member(b,"detached")||!(eq(phase,"STARTING")||eq(phase,"ACTIVE")))goto done;
    } else if(eq(method,"backend/drain")||eq(method,"backend/detach")||eq(method,"backend/seal")) {
      reason="not_active";if(eq(phase,"ENROLLED")||eq(phase,"SEALED"))goto done;
      if(eq(method,"backend/seal")) {
        char *out=NULL;rc=sql(a->db,"SELECT op FROM backend_admission_ops WHERE backend=? AND state!='TERMINAL'",lookup,1,&out);g_free(out);
        reason="busy";if(rc!=0)goto done;
        json_object_set_string_member(b,"phase","SEALED");json_object_set_boolean_member(b,"detached",true);
      } else {
        if(!eq(phase,"QUARANTINED"))json_object_set_string_member(b,"phase","DRAINING");
        if(eq(method,"backend/detach"))json_object_set_boolean_member(b,"detached",true);
      }
      changed=true;
    } else {
      const char *opid=str(p,"op_id");
      if(opid) {
        char *data=NULL;const char *v[]={backend,opid};rc=sql(a->db,"SELECT record FROM backend_admission_ops WHERE backend=? AND op=?",v,2,&data);
        reason="unavailable";if(rc<0)goto done;
        if(data){op=parse(data);g_free(data);if(!op_valid(op))goto done;}
      }
      if(eq(method,"backend/status")) {
        reason="unknown_operation";if(opid&&!op)goto done;
      } else if(eq(method,"backend/begin")) {
        reason="conflict";
        if(op) {
          if(!eq(str(p,"profile_hash"),str(b,"profile_hash"))||!session(op,b)||!eq(str(op,"boot_pubkey"),sender)||!eq(str(op,"class"),str(p,"class"))||!eq(str(op,"argument_digest"),str(p,"argument_digest"))||!eq(str(op,"begin_generation"),str(p,"generation"))||!eq(str(op,"begin_namespace_digest"),str(p,"namespace_digest")))goto done;
          /* Existing tombstone is observation only, never renewed permission. */
        } else {
          if(!snapshot(p,b))goto done;
          const char *c=str(p,"class");reason="not_active";
          if(json_object_get_boolean_member(b,"detached")||!(eq(phase,"STARTING")?eq(c,"STARTUP"):eq(phase,"ACTIVE")&&!eq(c,"STARTUP")))goto done;
          char *out=NULL;rc=sql(a->db,"SELECT op FROM backend_admission_ops WHERE backend=? AND state!='TERMINAL'",lookup,1,&out);g_free(out);
          reason="busy";if(rc!=0)goto done;
          reason="quota";if(!capacity(a->db,"SELECT count(*)<1000000 FROM backend_admission_ops"))goto done;
          op=json_object_new();copy_fields(op,p,"op_id,class,argument_digest,epoch,session_id,generation,namespace_digest");
          json_object_set_string_member(op,"begin_generation",str(p,"generation"));json_object_set_string_member(op,"begin_namespace_digest",str(p,"namespace_digest"));
          json_object_set_string_member(op,"boot_pubkey",sender);json_object_set_string_member(op,"state","ADMITTED");json_object_set_null_member(op,"outcome");json_object_set_null_member(op,"proof_digest");
          char *data=encode(op);const char *v[]={backend,opid,str(p,"epoch"),str(p,"session_id"),data};rc=sql(a->db,"INSERT INTO backend_admission_ops VALUES(?,?,?,?,'ADMITTED',?)",v,5,NULL);g_free(data);
          reason="unavailable";if(rc<0)goto done;created=true;changed=true;
        }
      } else {
        reason="unknown_operation";if(!op)goto done;
        reason="conflict";if(!session(op,b)||!eq(str(op,"boot_pubkey"),sender)||!eq(str(op,"argument_digest"),str(p,"argument_digest")))goto done;
        if(eq(method,"backend/uncertain")) {
          if(eq(str(op,"state"),"TERMINAL"))goto done;
          if(!eq(str(op,"state"),"UNCERTAIN")) {
            json_object_set_string_member(op,"state","UNCERTAIN");json_object_set_string_member(b,"phase","QUARANTINED");changed=true;
          }
        } else {
          const char *outcome=str(p,"outcome");
          if(eq(str(op,"state"),"TERMINAL")) {
            if(!eq(outcome,str(op,"outcome"))||!eq(str(p,"proof_digest"),str(op,"proof_digest"))||!eq(str(p,"generation"),str(op,"generation"))||!eq(str(p,"namespace_digest"),str(op,"namespace_digest")))goto done;
          } else {
            if(!eq(outcome,"CONFIG_CONFIRMED")&&(!eq(str(p,"generation"),str(b,"generation"))||!eq(str(p,"namespace_digest"),str(b,"namespace_digest"))))goto done;
            json_object_set_string_member(op,"state","TERMINAL");copy_fields(op,p,"outcome,proof_digest,generation,namespace_digest");
            if(!op_valid(op))goto done;
            if(eq(outcome,"CONFIG_CONFIRMED"))copy_fields(b,p,"generation,namespace_digest");
            if(eq(outcome,"STARTUP_CONFIRMED")&&eq(str(b,"phase"),"STARTING"))json_object_set_string_member(b,"phase","ACTIVE");
            changed=true;
          }
        }
        reason="unavailable";if(changed&&!save_op(a,backend,op))goto done;
      }
    }
    if(cleanup_bit) {
      int mask=(str(b,"cleanup_mask")[0]-'0')|cleanup_bit;
      int consumed=1;
      if(cleanup_bit==4){consumed+=3-((mask&1)?1:0)-((mask&2)?1:0)-1;mask=7;}
      char mask_text[2]={(char)('0'+mask),0};json_object_set_string_member(b,"cleanup_mask",mask_text);
      char *q=g_strdup_printf("UPDATE backend_admission_meta SET reserved=reserved-%d",consumed);
      bool ok=exec(a->db,q);g_free(q);reason="unavailable";if(!ok)goto done;
    }
    reason="quota";if(changed&&!increment(b,"revision"))goto done;
    reason="unavailable";if(changed&&!save_backend(a,b))goto done;
  }
  if(command) {
    const char *v[]={sender,str(p,"command_id"),backend,sem};reason="unavailable";
    if(sql(a->db,"INSERT INTO backend_admission_commands VALUES(?,?,?,?)",v,4,NULL)<0)goto done;
  }
  reason=NULL;
commit:
  if(!exec(a->db,"COMMIT")){reason="unavailable";g_atomic_int_set(&a->healthy,0);}else begun=false;
done:
  if(begun){if(!exec(a->db,"ROLLBACK")&&a)g_atomic_int_set(&a->healthy,0);}
  if(mutex)sqlite3_mutex_leave(mutex);
  char *out=response(p,method,b,op,created,reason,id);
  g_free(record);g_free(saved);g_free(sem);
  if(op)json_object_unref(op);if(b)json_object_unref(b);if(rpc)json_object_unref(rpc);
  return out;
}
