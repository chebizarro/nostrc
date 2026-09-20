#include "auth_challenge.h"
#include "nostr-keys.h"
#include <assert.h>
#include "../nh_test.h"
#include <stdlib.h>
#include <string.h>
int main(void){
  const char *sk="0000000000000000000000000000000000000000000000000000000000000001";char *pk=nostr_key_get_public(sk);NH_CHECK(pk);
  nh_identity_account a={0};strcpy(a.account_id,"11111111-1111-1111-1111-111111111111");strcpy(a.username,"n_test");strcpy(a.pubkey_hex,pk);a.uid=200001;a.status=NH_IDENTITY_STATUS_ACTIVE;a.key_generation=2;a.authority_generation=9;
  uint8_t nonce[32]={1};nh_auth_challenge_input in={NH_AUTH_PURPOSE_LINUX_LOGIN,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","22222222-2222-2222-2222-222222222222","33333333-3333-3333-3333-333333333333","gdm-password","seat0","",&a,1700000000,1700000120,512000,nonce};
  nh_auth_challenge c;NH_CHECK(nh_auth_challenge_build(&in,&c)==0);NH_CHECK(nostr_event_sign(c.event,sk)==0);char *json=nostr_event_serialize_compact(c.event);NH_CHECK(json);NH_CHECK(nh_auth_challenge_verify(&c,json,500000,&a)==NH_AUTH_PROOF_OK);
  a.key_generation++;NH_CHECK(nh_auth_challenge_verify(&c,json,500000,&a)==NH_AUTH_PROOF_STALE_ACCOUNT);a.key_generation--;NH_CHECK(nh_auth_challenge_verify(&c,json,512001,&a)==NH_AUTH_PROOF_EXPIRED);
  char *saved=c.event->content;c.event->content=strdup("{}");char *bad=nostr_event_serialize_compact(c.event);NH_CHECK(bad);NH_CHECK(nh_auth_challenge_verify(&c,bad,500000,&a)!=NH_AUTH_PROOF_OK);free(bad);free(c.event->content);c.event->content=saved;
  free(json);nh_auth_challenge_clear(&c);free(pk);return 0;
}
