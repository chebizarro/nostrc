#include "auth_transaction.h"

static nh_identity_rc lookup_name(void *context, const char *name,
                                  nh_identity_account *out) {
  return nh_identity_store_lookup_by_name(context, name, out);
}
static nh_identity_rc lookup_uid(void *context, uint32_t uid,
                                 nh_identity_account *out) {
  return nh_identity_store_lookup_by_uid(context, uid, out);
}
static nh_identity_rc recheck_account(void *context, const char *account_id,
                                      uint64_t key_generation,
                                      uint64_t authority_generation,
                                      nh_identity_status *status,
                                      nh_identity_account *out) {
  return nh_identity_store_recheck(context, account_id, key_generation,
                                   authority_generation, status, out);
}
static const nh_auth_authority_ops store_ops = {lookup_name, lookup_uid,
                                                recheck_account};

nh_auth_authority nh_auth_authority_from_store(nh_identity_store *store) {
  nh_auth_authority authority = {&store_ops, store};
  return authority;
}
