#include "auth_provider.h"
#include "auth_vault.h"
#include "nostr-event.h"
#include <stdlib.h>
#include <string.h>

typedef struct local_provider {
  nh_auth_provider base;
  char provider_id[NH_IDENTITY_UUID_CAP],account_id[NH_IDENTITY_UUID_CAP],pubkey[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint64_t generation;
  uint8_t *blob; size_t blob_len;
  char *challenge_json,expected_id[65];
} local_provider;
static void emit(local_provider*p,nh_auth_provider_event_type type,nh_auth_result result,const void*d,size_t n){nh_auth_provider_event e={type,result,d,n};if(p->base.emit)p->base.emit(p->base.emit_context,&e);}
static int prepare(nh_auth_provider*b,const nh_auth_provider_snapshot*s){
  local_provider*p=(local_provider*)b;if(!s||s->type!=NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY||!s->provider_id||!s->account_id||!s->pubkey_hex||strlen(s->provider_id)>=sizeof p->provider_id||strlen(s->account_id)>=sizeof p->account_id||strlen(s->pubkey_hex)!=64||!s->secret_blob||!s->secret_blob_len)return -1;
  uint8_t *copy=malloc(s->secret_blob_len);if(!copy)return -1;memcpy(copy,s->secret_blob,s->secret_blob_len);free(p->blob);p->blob=copy;p->blob_len=s->secret_blob_len;strcpy(p->provider_id,s->provider_id);strcpy(p->account_id,s->account_id);strcpy(p->pubkey,s->pubkey_hex);p->generation=s->key_generation;emit(p,NH_AUTH_PROVIDER_READY,NH_AUTH_RESULT_OK,NULL,0);return 0;
}
static int begin_proof(nh_auth_provider*b,const nh_auth_immutable_challenge*c){local_provider*p=(local_provider*)b;if(!c||!c->unsigned_event_json||!c->expected_event_id||c->unsigned_event_json_len>NH_AUTH_PROOF_MAX||strlen(c->expected_event_id)!=64)return -1;char*j=malloc(c->unsigned_event_json_len+1);if(!j)return -1;memcpy(j,c->unsigned_event_json,c->unsigned_event_json_len);j[c->unsigned_event_json_len]=0;free(p->challenge_json);p->challenge_json=j;strcpy(p->expected_id,c->expected_event_id);emit(p,NH_AUTH_PROVIDER_UNLOCK_REQUIRED,NH_AUTH_RESULT_INTERACTION_REQUIRED,NULL,0);return 0;}
static int submit(nh_auth_provider*b,const uint8_t*secret,size_t n){
  local_provider*p=(local_provider*)b;if(!p->blob||!p->challenge_json||!secret||n>NH_AUTH_SECRET_MAX)return -1;nostr_secure_buf pass=secure_alloc(n),key={0};if(!pass.ptr||!pass.locked){secure_free(&pass);emit(p,NH_AUTH_PROVIDER_FAILED,NH_AUTH_RESULT_INTERNAL_ERROR,NULL,0);return -1;}memcpy(pass.ptr,secret,n);
  nh_auth_vault_binding binding={p->provider_id,p->account_id,p->pubkey,p->generation};nh_auth_vault_rc vr=nh_auth_vault_open(p->blob,p->blob_len,pass.ptr,n,&binding,&key);secure_free(&pass);if(vr!=NH_AUTH_VAULT_OK){emit(p,NH_AUTH_PROVIDER_DENIED,NH_AUTH_RESULT_DENIED,NULL,0);return 0;}
  NostrEvent*e=nostr_event_new();char id[65];char*out=NULL;int ok=e&&nostr_event_deserialize_unsigned(e,p->challenge_json,NULL)==NOSTR_EVENT_VALIDATION_OK&&nostr_event_compute_id(e,id)==NOSTR_EVENT_VALIDATION_OK&&!strcmp(id,p->expected_id)&&e->pubkey&&!strcmp(e->pubkey,p->pubkey)&&nostr_event_sign_secure(e,&key)==0&&nostr_event_validate(e,NULL)==NOSTR_EVENT_VALIDATION_OK;
  secure_free(&key);if(ok)out=nostr_event_serialize_compact(e);nostr_event_free(e);if(!out){emit(p,NH_AUTH_PROVIDER_FAILED,NH_AUTH_RESULT_INVALID_PROOF,NULL,0);return 0;}emit(p,NH_AUTH_PROVIDER_SIGNED_EVENT,NH_AUTH_RESULT_OK,out,strlen(out));free(out);return 0;
}
static void cancel(nh_auth_provider*b){local_provider*p=(local_provider*)b;emit(p,NH_AUTH_PROVIDER_FAILED,NH_AUTH_RESULT_CANCELLED,NULL,0);}
static void destroy(nh_auth_provider*b){local_provider*p=(local_provider*)b;if(!p)return;if(p->blob){secure_wipe(p->blob,p->blob_len);free(p->blob);}if(p->challenge_json){secure_wipe(p->challenge_json,strlen(p->challenge_json));free(p->challenge_json);}secure_wipe(p,sizeof *p);free(p);}
static const nh_auth_provider_ops ops={prepare,begin_proof,submit,cancel,destroy};
nh_auth_provider *nh_auth_provider_local_new(nh_auth_provider_event_fn fn,void*ctx){local_provider*p=calloc(1,sizeof *p);if(!p)return NULL;p->base.ops=&ops;p->base.emit=fn;p->base.emit_context=ctx;return &p->base;}
