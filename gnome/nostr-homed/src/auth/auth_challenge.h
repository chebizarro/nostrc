#ifndef NH_AUTH_CHALLENGE_H
#define NH_AUTH_CHALLENGE_H
#include <stdint.h>
#include "nostr-event.h"
#include "nostr_identity.h"

typedef enum nh_auth_purpose { NH_AUTH_PURPOSE_LINUX_LOGIN=1, NH_AUTH_PURPOSE_ENROLLMENT, NH_AUTH_PURPOSE_PROVIDER_ENROLLMENT, NH_AUTH_PURPOSE_SMB_CREDENTIAL } nh_auth_purpose;
typedef enum nh_auth_proof_rc { NH_AUTH_PROOF_OK=0, NH_AUTH_PROOF_INVALID, NH_AUTH_PROOF_EXPIRED, NH_AUTH_PROOF_STALE_ACCOUNT, NH_AUTH_PROOF_CRYPTO_ERROR } nh_auth_proof_rc;
typedef struct nh_auth_challenge_input {
  nh_auth_purpose purpose; const char *transaction_id,*authority_id,*boot_id,*service,*context_json,*resource_json;
  const nh_identity_account *account; int64_t issued_at,expires_at; uint64_t deadline_monotonic_ms;
  const uint8_t *nonce32; /* NULL generates with RAND_bytes. */
} nh_auth_challenge_input;
typedef struct nh_auth_challenge { NostrEvent *event; char expected_id[65]; char nonce_hex[65]; char account_id[NH_IDENTITY_UUID_CAP]; uint64_t key_generation; uint64_t authority_generation; uint64_t deadline_monotonic_ms; } nh_auth_challenge;

int nh_auth_challenge_build(const nh_auth_challenge_input *input,nh_auth_challenge *out);
void nh_auth_challenge_clear(nh_auth_challenge *challenge);
nh_auth_proof_rc nh_auth_challenge_verify(const nh_auth_challenge *challenge,const char *signed_event_json,uint64_t monotonic_now_ms,const nh_identity_account *current);
#endif
