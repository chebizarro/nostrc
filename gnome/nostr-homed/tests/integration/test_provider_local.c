#include "auth_challenge.h"
#include "auth_provider.h"
#include "auth_vault.h"
#include "nostr-keys.h"
#include <assert.h>
#include "../nh_test.h"
#include <stdlib.h>
#include <string.h>
typedef struct capture { nh_auth_provider_event_type type; char *json; } capture;
static void on_event(void *ctx,const nh_auth_provider_event*e){capture*c=ctx;c->type=e->type;if(e->type==NH_AUTH_PROVIDER_SIGNED_EVENT){c->json=malloc(e->data_len+1);memcpy(c->json,e->data,e->data_len);c->json[e->data_len]=0;}}
int main(void){
  uint8_t sk[32]={0};sk[31]=1;const uint8_t pass[]="correct horse battery";char *pk=nostr_key_get_public("0000000000000000000000000000000000000000000000000000000000000001");NH_CHECK(pk);
  nh_identity_account a={0};strcpy(a.account_id,"11111111-1111-1111-1111-111111111111");strcpy(a.username,"n_test");strcpy(a.pubkey_hex,pk);a.uid=200001;a.status=NH_IDENTITY_STATUS_ACTIVE;a.key_generation=2;a.authority_generation=9;
  nh_auth_vault_binding bind={"22222222-2222-2222-2222-222222222222",a.account_id,a.pubkey_hex,a.key_generation};uint8_t *blob=NULL;size_t blob_len=0;NH_CHECK(nh_auth_vault_seal(sk,pass,sizeof pass-1,&bind,&blob,&blob_len)==NH_AUTH_VAULT_OK);
  uint8_t nonce[32]={2};nh_auth_challenge_input in={NH_AUTH_PURPOSE_LINUX_LOGIN,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","33333333-3333-3333-3333-333333333333","44444444-4444-4444-4444-444444444444","gdm-password","seat0","",&a,1700000000,1700000120,512000,nonce};nh_auth_challenge ch;NH_CHECK(nh_auth_challenge_build(&in,&ch)==0);char *unsigned_json=nostr_event_serialize_compact(ch.event);NH_CHECK(unsigned_json);
  capture cap={0};nh_auth_provider*p=nh_auth_provider_local_new(on_event,&cap);NH_CHECK(p);nh_auth_provider_snapshot snap={NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY,bind.provider_id,bind.account_id,bind.pubkey_hex,bind.key_generation,512000,"{}",blob,blob_len};NH_CHECK(p->ops->prepare(p,&snap)==0);nh_auth_immutable_challenge proof={unsigned_json,ch.expected_id,strlen(unsigned_json),512000};NH_CHECK(p->ops->begin_proof(p,&proof)==0);NH_CHECK(p->ops->submit_unlock(p,pass,sizeof pass-1)==0);NH_CHECK(cap.type==NH_AUTH_PROVIDER_SIGNED_EVENT&&cap.json);NH_CHECK(nh_auth_challenge_verify(&ch,cap.json,500000,&a)==NH_AUTH_PROOF_OK);
  p->ops->destroy(p);free(cap.json);free(unsigned_json);free(blob);free(pk);nh_auth_challenge_clear(&ch);return 0;
}
