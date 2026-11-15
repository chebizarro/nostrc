#ifndef FIPS_REKEY_RECORD_H
#define FIPS_REKEY_RECORD_H
#include <fips/proto/rekey/profile.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_REKEY_RECORD_BOUND 256u
typedef struct fips_rekey_digest {
    uint8_t bytes[32];
} fips_rekey_digest;
typedef struct fips_rekey_record {
    bool has_setup, has_origin, has_answer;
    fips_rekey_digest setup, origin;
    fips_rekey_wire answer;
    fips_rekey_digest seen[FIPS_REKEY_RECORD_BOUND];
    size_t head, count;
} fips_rekey_record;
typedef struct fips_rekey_record_entry {
    fips_node_addr peer;
    uint8_t epoch[8];
    fips_rekey_record record;
} fips_rekey_record_entry;
typedef struct fips_rekey_record_store {
    fips_rekey_record_entry *entries;
    size_t count, bound;
} fips_rekey_record_store;
typedef struct fips_rekey_held_answer {
    fips_rekey_digest msg1, ephemeral;
    fips_rekey_wire msg2;
    fips_rekey_route route;
    bool present;
} fips_rekey_held_answer;
FIPS_CORE_API void fips_rekey_record_new(fips_rekey_record *r);
FIPS_CORE_API void fips_rekey_record_setup(fips_rekey_record *r, fips_rekey_digest msg1,
                                           fips_rekey_digest ephemeral,
                                           const fips_rekey_wire *msg2);
FIPS_CORE_API void fips_rekey_record_push(fips_rekey_record *r, fips_rekey_digest ephemeral);
FIPS_CORE_API bool fips_rekey_record_holds(const fips_rekey_record *r, fips_rekey_digest ephemeral);
FIPS_CORE_API size_t fips_rekey_record_len(const fips_rekey_record *r);
/* Consumes older, preserves this record's setup and answer. No self-aliasing. */
FIPS_CORE_API void fips_rekey_record_absorb(fips_rekey_record *r, fips_rekey_record *older);
FIPS_CORE_API void fips_rekey_record_store_new(fips_rekey_record_store *s,
                                               fips_rekey_record_entry *storage, size_t bound);
FIPS_CORE_API void fips_rekey_record_store_file(fips_rekey_record_store *s, fips_node_addr peer,
                                                const uint8_t epoch[8], fips_rekey_record *record);
FIPS_CORE_API void fips_rekey_record_store_take(fips_rekey_record_store *s,
                                                const fips_node_addr *peer, const uint8_t epoch[8],
                                                fips_rekey_record *out);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_RECORD_H */
