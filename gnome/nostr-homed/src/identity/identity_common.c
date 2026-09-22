#include "identity_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool nh_identity_username_is_valid(const char *username) {
  size_t i, len;
  if (!username) return false;
  len = strlen(username);
  if (len < 3 || len > NH_IDENTITY_USERNAME_MAX || username[0] != 'n' ||
      username[1] != '_')
    return false;
  for (i = 2; i < len; ++i) {
    unsigned char c = (unsigned char)username[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return true;
}

bool nh_identity_uuid_is_valid(const char *uuid) {
  size_t i;
  if (!uuid || strlen(uuid) != NH_IDENTITY_UUID_LEN) return false;
  for (i = 0; i < NH_IDENTITY_UUID_LEN; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (uuid[i] != '-') return false;
    } else if (!((uuid[i] >= '0' && uuid[i] <= '9') ||
                 (uuid[i] >= 'a' && uuid[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool nh_identity_pubkey_is_valid(const char *pubkey_hex) {
  size_t i;
  if (!pubkey_hex || strlen(pubkey_hex) != NH_IDENTITY_PUBKEY_HEX_LEN)
    return false;
  for (i = 0; i < NH_IDENTITY_PUBKEY_HEX_LEN; ++i) {
    char c = pubkey_hex[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

const char *nh_identity_rc_name(nh_identity_rc result) {
  switch (result) {
    case NH_IDENTITY_OK: return "ok";
    case NH_IDENTITY_NOT_FOUND: return "not_found";
    case NH_IDENTITY_INVALID: return "invalid";
    case NH_IDENTITY_CONFLICT: return "conflict";
    case NH_IDENTITY_OPERATION_MISMATCH: return "operation_mismatch";
    case NH_IDENTITY_BAD_STATE: return "bad_state";
    case NH_IDENTITY_STALE_GENERATION: return "stale_generation";
    case NH_IDENTITY_NOT_ACTIVE: return "not_active";
    case NH_IDENTITY_RANGE_EXHAUSTED: return "range_exhausted";
    case NH_IDENTITY_BUSY: return "busy";
    case NH_IDENTITY_NOT_INITIALIZED: return "not_initialized";
    case NH_IDENTITY_SCHEMA_UNSUPPORTED: return "schema_unsupported";
    case NH_IDENTITY_STORAGE_ERROR: return "storage_error";
    case NH_IDENTITY_OWNERSHIP_CHECK_FAILED: return "ownership_check_failed";
    case NH_IDENTITY_NO_MEMORY: return "no_memory";
    case NH_IDENTITY_UNSUPPORTED: return "unsupported";
    case NH_IDENTITY_PERMISSION_DENIED: return "permission_denied";
  }
  return "unknown";
}

const char *nh_identity_status_name(nh_identity_status status) {
  switch (status) {
    case NH_IDENTITY_STATUS_ENROLLING: return "enrolling";
    case NH_IDENTITY_STATUS_ACTIVE: return "active";
    case NH_IDENTITY_STATUS_DISABLED: return "disabled";
    case NH_IDENTITY_STATUS_REPAIR_REQUIRED: return "repair_required";
    case NH_IDENTITY_STATUS_RETIRED: return "retired";
  }
  return NULL;
}

nh_identity_rc nh_identity_status_parse(const char *text,
                                        nh_identity_status *out) {
  if (!text || !out) return NH_IDENTITY_INVALID;
  if (strcmp(text, "enrolling") == 0) *out = NH_IDENTITY_STATUS_ENROLLING;
  else if (strcmp(text, "active") == 0) *out = NH_IDENTITY_STATUS_ACTIVE;
  else if (strcmp(text, "disabled") == 0) *out = NH_IDENTITY_STATUS_DISABLED;
  else if (strcmp(text, "repair_required") == 0)
    *out = NH_IDENTITY_STATUS_REPAIR_REQUIRED;
  else if (strcmp(text, "retired") == 0) *out = NH_IDENTITY_STATUS_RETIRED;
  else return NH_IDENTITY_INVALID;
  return NH_IDENTITY_OK;
}

void nh_identity_set_error(nh_identity_store *store, const char *format, ...) {
  va_list ap;
  if (!store) return;
  va_start(ap, format);
  (void)vsnprintf(store->error_detail, sizeof(store->error_detail), format, ap);
  va_end(ap);
}

const char *nh_identity_store_error_detail(const nh_identity_store *store) {
  return store ? store->error_detail : "invalid store";
}

int nh_identity_status_to_text(nh_identity_status status, const char **out) {
  const char *text = nh_identity_status_name(status);
  if (!text || !out) return -1;
  *out = text;
  return 0;
}

int nh_identity_origin_to_text(nh_identity_origin origin, const char **out) {
  if (!out) return -1;
  if (origin == NH_IDENTITY_ORIGIN_ENROLLED) *out = "enrolled";
  else if (origin == NH_IDENTITY_ORIGIN_LEGACY_IMPORT) *out = "legacy_import";
  else return -1;
  return 0;
}

int nh_identity_origin_from_text(const char *text, nh_identity_origin *out) {
  if (!text || !out) return -1;
  if (strcmp(text, "enrolled") == 0) *out = NH_IDENTITY_ORIGIN_ENROLLED;
  else if (strcmp(text, "legacy_import") == 0)
    *out = NH_IDENTITY_ORIGIN_LEGACY_IMPORT;
  else return -1;
  return 0;
}

int nh_identity_provider_type_to_text(nh_identity_provider_type type,
                                      const char **out) {
  if (!out) return -1;
  if (type == NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY)
    *out = "local_encrypted_key";
  else if (type == NH_IDENTITY_PROVIDER_NIP46_BUNKER)
    *out = "nip46_bunker";
  else if (type == NH_IDENTITY_PROVIDER_NIP46_QR)
    *out = "nip46_qr";
  else return -1;
  return 0;
}

int nh_identity_provider_type_from_text(const char *text,
                                        nh_identity_provider_type *out) {
  if (!text || !out) return -1;
  if (strcmp(text, "local_encrypted_key") == 0)
    *out = NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY;
  else if (strcmp(text, "nip46_bunker") == 0)
    *out = NH_IDENTITY_PROVIDER_NIP46_BUNKER;
  else if (strcmp(text, "nip46_qr") == 0)
    *out = NH_IDENTITY_PROVIDER_NIP46_QR;
  else return -1;
  return 0;
}

int nh_identity_operation_type_to_text(nh_identity_operation_type type,
                                       const char **out) {
  static const char *const names[] = {NULL, "enroll", "import", "repair_home",
    "provider_stage", "provider_activate", "provider_discard", "set_status",
    "replace_identity", "provider_reseal"};
  if (!out || type < NH_IDENTITY_OPERATION_ENROLL ||
      type > NH_IDENTITY_OPERATION_PROVIDER_RESEAL) return -1;
  *out = names[(unsigned int)type];
  return 0;
}

int nh_identity_operation_type_from_text(const char *text,
                                         nh_identity_operation_type *out) {
  nh_identity_operation_type i;
  const char *name;
  if (!text || !out) return -1;
  for (i = NH_IDENTITY_OPERATION_ENROLL;
       i <= NH_IDENTITY_OPERATION_PROVIDER_RESEAL; i++) {
    if (nh_identity_operation_type_to_text(i, &name) == 0 &&
        strcmp(text, name) == 0) { *out = i; return 0; }
  }
  return -1;
}

int nh_identity_phase_to_text(nh_identity_operation_phase phase,
                              const char **out) {
  static const char *const names[] = {NULL, "reserved", "staged", "installed",
                                      "projected", "complete"};
  if (!out || phase < NH_IDENTITY_PHASE_RESERVED ||
      phase > NH_IDENTITY_PHASE_COMPLETE) return -1;
  *out = names[(unsigned int)phase];
  return 0;
}

int nh_identity_phase_from_text(const char *text,
                                nh_identity_operation_phase *out) {
  nh_identity_operation_phase i;
  const char *name;
  if (!text || !out) return -1;
  for (i = NH_IDENTITY_PHASE_RESERVED; i <= NH_IDENTITY_PHASE_COMPLETE; i++) {
    if (nh_identity_phase_to_text(i, &name) == 0 && strcmp(text, name) == 0) {
      *out = i; return 0;
    }
  }
  return -1;
}

int nh_identity_outcome_to_text(nh_identity_operation_outcome outcome,
                                const char **out) {
  static const char *const names[] = {NULL, "pending", "done",
                                      "repair_required", "abandoned"};
  if (!out || outcome < NH_IDENTITY_OUTCOME_PENDING ||
      outcome > NH_IDENTITY_OUTCOME_ABANDONED) return -1;
  *out = names[(unsigned int)outcome];
  return 0;
}

int nh_identity_outcome_from_text(const char *text,
                                  nh_identity_operation_outcome *out) {
  nh_identity_operation_outcome i;
  const char *name;
  if (!text || !out) return -1;
  for (i = NH_IDENTITY_OUTCOME_PENDING; i <= NH_IDENTITY_OUTCOME_ABANDONED; i++) {
    if (nh_identity_outcome_to_text(i, &name) == 0 && strcmp(text, name) == 0) {
      *out = i; return 0;
    }
  }
  return -1;
}

int nh_identity_home_mode_to_text(nh_identity_home_mode mode, const char **out) {
  if (!out) return -1;
  if (mode == NH_IDENTITY_HOME_CREATE) *out = "create";
  else if (mode == NH_IDENTITY_HOME_ADOPT_EXISTING) *out = "adopt_existing";
  else return -1;
  return 0;
}

int nh_identity_home_mode_from_text(const char *text,
                                    nh_identity_home_mode *out) {
  if (!text || !out) return -1;
  if (strcmp(text, "create") == 0) *out = NH_IDENTITY_HOME_CREATE;
  else if (strcmp(text, "adopt_existing") == 0)
    *out = NH_IDENTITY_HOME_ADOPT_EXISTING;
  else return -1;
  return 0;
}
