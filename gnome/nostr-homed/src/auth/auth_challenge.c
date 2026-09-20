#include "auth_challenge.h"
#include "nostr_auth_protocol.h"
#include "nostr-tag.h"
#include <jansson.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

static const char *purpose_name(nh_auth_purpose p){switch(p){case NH_AUTH_PURPOSE_LINUX_LOGIN:return "linux-login";case NH_AUTH_PURPOSE_ENROLLMENT:return "enrollment";case NH_AUTH_PURPOSE_PROVIDER_ENROLLMENT:return "provider-enrollment";case NH_AUTH_PURPOSE_SMB_CREDENTIAL:return "smb-credential";default:return NULL;}}
static NostrTag *tag2(const char *key,const char *value){NostrTag*t=nostr_tag_new(NULL);if(!t)return NULL;nostr_tag_append(t,key);nostr_tag_append(t,value);return t;}
static void hex32(const uint8_t *in,char out[65]){static const char x[]="0123456789abcdef";for(size_t i=0;i<32;i++){out[2*i]=x[in[i]>>4];out[2*i+1]=x[in[i]&15];}out[64]=0;}
void nh_auth_challenge_clear(nh_auth_challenge*c){if(!c)return;nostr_event_free(c->event);memset(c,0,sizeof *c);}
int nh_auth_challenge_build(const nh_auth_challenge_input*i,nh_auth_challenge*out){
  if(!i||!out||!i->account||!purpose_name(i->purpose)||!i->transaction_id||!i->authority_id||!i->boot_id||!i->service||i->issued_at<=0||i->expires_at-i->issued_at!=NH_AUTH_CHALLENGE_LIFETIME_SEC||!i->deadline_monotonic_ms)return -1;
  memset(out,0,sizeof *out);uint8_t random[32];const uint8_t*n=i->nonce32;if(!n){if(RAND_bytes(random,sizeof random)!=1)return -1;n=random;}hex32(n,out->nonce_hex);
  json_t *content=json_object();if(!content)return -1;
  int bad=json_object_set_new(content,"protocol",json_string("org.nostr.auth/1"))||json_object_set_new(content,"purpose",json_string(purpose_name(i->purpose)))||json_object_set_new(content,"nonce",json_string(out->nonce_hex))||json_object_set_new(content,"transaction_id",json_string(i->transaction_id))||json_object_set_new(content,"authority_id",json_string(i->authority_id))||json_object_set_new(content,"boot_id",json_string(i->boot_id))||json_object_set_new(content,"account_id",json_string(i->account->account_id))||json_object_set_new(content,"username",json_string(i->account->username))||json_object_set_new(content,"uid",json_integer(i->account->uid))||json_object_set_new(content,"pubkey",json_string(i->account->pubkey_hex))||json_object_set_new(content,"key_generation",json_integer(i->account->key_generation))||json_object_set_new(content,"service",json_string(i->service))||json_object_set_new(content,"issued_at",json_integer(i->issued_at))||json_object_set_new(content,"expires_at",json_integer(i->expires_at))||json_object_set_new(content,"context",json_string(i->context_json?i->context_json:""))||json_object_set_new(content,"resource",json_string(i->resource_json?i->resource_json:""));
  char *text=bad?NULL:json_dumps(content,JSON_COMPACT);json_decref(content);if(!text)return -1;
  NostrEvent*e=nostr_event_new();if(!e){free(text);return -1;}nostr_event_set_pubkey(e,i->account->pubkey_hex);nostr_event_set_created_at(e,i->issued_at);nostr_event_set_kind(e,NH_AUTH_CHALLENGE_KIND);nostr_event_set_content(e,text);free(text);
  NostrTags*tags=nostr_tags_new(0);NostrTag*t1=tag2("a","org.nostr.auth/1"),*t2=tag2("purpose",purpose_name(i->purpose)),*t3=tag2("challenge",out->nonce_hex);if(!tags||!t1||!t2||!t3){nostr_tag_free(t1);nostr_tag_free(t2);nostr_tag_free(t3);nostr_tags_free(tags);nostr_event_free(e);return -1;}nostr_tags_append(tags,t1);nostr_tags_append(tags,t2);nostr_tags_append(tags,t3);nostr_event_set_tags(e,tags);
  if(nostr_event_compute_id(e,out->expected_id)!=NOSTR_EVENT_VALIDATION_OK){nostr_event_free(e);return -1;}out->event=e;memcpy(out->account_id,i->account->account_id,strlen(i->account->account_id)+1);out->key_generation=i->account->key_generation;out->authority_generation=i->account->authority_generation;out->deadline_monotonic_ms=i->deadline_monotonic_ms;return 0;
}
nh_auth_proof_rc nh_auth_challenge_verify(const nh_auth_challenge*c,const char*json,uint64_t now,const nh_identity_account*current){
  if(!c||!c->event||!json||!current||strlen(json)>NH_AUTH_PROOF_MAX)return NH_AUTH_PROOF_INVALID;if(now>c->deadline_monotonic_ms)return NH_AUTH_PROOF_EXPIRED;
  if(strcmp(current->account_id,c->account_id)||strcmp(current->pubkey_hex,c->event->pubkey)||current->key_generation!=c->key_generation||current->authority_generation!=c->authority_generation||current->status!=NH_IDENTITY_STATUS_ACTIVE)return NH_AUTH_PROOF_STALE_ACCOUNT;
  NostrEvent*p=nostr_event_new();if(!p)return NH_AUTH_PROOF_CRYPTO_ERROR;nh_auth_proof_rc rc=NH_AUTH_PROOF_INVALID;char id[65];
  if(nostr_event_deserialize_signed(p,json,NULL)!=NOSTR_EVENT_VALIDATION_OK)goto done;
  NostrEventValidationStatus validation=nostr_event_validate(p,id);if(validation!=NOSTR_EVENT_VALIDATION_OK){rc=(validation==NOSTR_EVENT_VALIDATION_CRYPTO_ERROR||validation==NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR)?NH_AUTH_PROOF_CRYPTO_ERROR:NH_AUTH_PROOF_INVALID;goto done;}
  char *a=nostr_tags_to_json(c->event->tags),*b=nostr_tags_to_json(p->tags);
  int same=a&&b&&!strcmp(a,b)&&!strcmp(p->pubkey,c->event->pubkey)&&p->created_at==c->event->created_at&&p->kind==c->event->kind&&!strcmp(p->content,c->event->content)&&!strcmp(id,c->expected_id)&&!strcmp(p->id,c->expected_id);
  free(a);free(b);if(same)rc=NH_AUTH_PROOF_OK;
done:nostr_event_free(p);return rc;
}
