#ifndef NH_IDENTITY_INTERNAL_H
#define NH_IDENTITY_INTERNAL_H

#include "nostr_identity.h"
#include <sqlite3.h>

struct nh_identity_store {
  sqlite3 *db;
  nh_identity_config config;
  nh_identity_ownership_probe_fn ownership_probe;
  void *ownership_probe_context;
  int lock_fd;
  char lock_path[NH_IDENTITY_HOME_CAP];
  char error_detail[256];
};

struct nh_identity_reader {
  char path[NH_IDENTITY_HOME_CAP];
};

void nh_identity_set_error(nh_identity_store *store, const char *format, ...);
nh_identity_rc nh_identity_sqlite_result(nh_identity_store *store, int rc,
                                         const char *operation);
nh_identity_rc nh_identity_metadata_u64(nh_identity_store *store,
                                        const char *key, uint64_t *out);
nh_identity_rc nh_identity_bump_generation(nh_identity_store *store,
                                           uint64_t *out);
nh_identity_rc nh_identity_begin(nh_identity_store *store);
nh_identity_rc nh_identity_commit(nh_identity_store *store);
void nh_identity_rollback(nh_identity_store *store);
int nh_identity_status_to_text(nh_identity_status status, const char **out);
int nh_identity_origin_to_text(nh_identity_origin origin, const char **out);
int nh_identity_origin_from_text(const char *text, nh_identity_origin *out);
int nh_identity_provider_type_to_text(nh_identity_provider_type type,
                                      const char **out);
int nh_identity_provider_type_from_text(const char *text,
                                        nh_identity_provider_type *out);
int nh_identity_operation_type_to_text(nh_identity_operation_type type,
                                       const char **out);
int nh_identity_operation_type_from_text(const char *text,
                                         nh_identity_operation_type *out);
int nh_identity_phase_to_text(nh_identity_operation_phase phase,
                              const char **out);
int nh_identity_phase_from_text(const char *text,
                                nh_identity_operation_phase *out);
int nh_identity_outcome_to_text(nh_identity_operation_outcome outcome,
                                const char **out);
int nh_identity_outcome_from_text(const char *text,
                                  nh_identity_operation_outcome *out);
int nh_identity_home_mode_to_text(nh_identity_home_mode mode, const char **out);
int nh_identity_home_mode_from_text(const char *text,
                                    nh_identity_home_mode *out);

#endif
