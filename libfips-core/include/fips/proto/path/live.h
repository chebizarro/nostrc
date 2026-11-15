#ifndef FIPS_PROTO_PATH_LIVE_H
#define FIPS_PROTO_PATH_LIVE_H
#include <fips/proto/path/params.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All value fields are read-only to callers. ops==NULL is an absent instant;
 * it is never passed to a host clock operation. Fixed storage; no allocation. */
typedef struct fips_path_sent { uint32_t probe_id; fips_instant at, due; size_t len; } fips_path_sent;
typedef enum fips_path_answer_kind { FIPS_PATH_AnswerNone, FIPS_PATH_InTime, FIPS_PATH_Late } fips_path_answer_kind;
typedef struct fips_path_answer { fips_path_answer_kind kind; fips_path_sent sent; } fips_path_answer;
typedef struct fips_path_sample { fips_instant at; fips_duration rtt; } fips_path_sample;
typedef struct fips_path_span { fips_instant from,to; fips_duration rtt; } fips_path_span;
typedef enum fips_path_connection { FIPS_PATH_Up, FIPS_PATH_Down, FIPS_PATH_Pending } fips_path_connection;
typedef struct fips_path_liveness {
    fips_path_sent ring[FIPS_PATH_RING], late[4];
    bool has_next_id; uint32_t next_id;
    fips_instant next_at;
    bool urgent; uint32_t misses;
    bool has_answered; uint32_t answered;
    bool has_rtt; fips_duration rtt;
    fips_path_sample rtts[32]; fips_path_span spans[8];
    double etx; bool has_mtu; size_t mtu;
    bool peer; uint32_t frames,bytes;
    fips_instant heard_at,data_at,live_at;
    bool carried; uint32_t fails; fips_instant trip;
    fips_path_connection connect;
    fips_instant quiet;
    struct { fips_instant free,until; fips_duration gap,used; bool owed; } stop;
    bool has_epoch; uint32_t epoch;
    struct { fips_instant at; uint32_t probe_id; uint16_t len,lost; } seek;
    bool fallen,has_last; size_t last;
    bool has_since; uint32_t since;
} fips_path_liveness;
FIPS_CORE_API void fips_path_liveness_new(fips_path_liveness *l);
FIPS_CORE_API size_t fips_path_liveness_outstanding(const fips_path_liveness *l);
FIPS_CORE_API size_t fips_path_liveness_samples(const fips_path_liveness *l);
FIPS_CORE_API bool fips_path_liveness_min_rtt(const fips_path_liveness *l,fips_duration *out);
FIPS_CORE_API bool fips_path_liveness_max_rtt(const fips_path_liveness *l,fips_duration *out);
FIPS_CORE_API bool fips_path_liveness_base_rtt(const fips_path_liveness *l,fips_duration *out);
FIPS_CORE_API fips_instant fips_path_liveness_earliest_due(const fips_path_liveness *l);
/* Mutators mirror upstream module-private operations, exposed for sans-IO
 * composition and deterministic tests. Caller expires overdue probes BEFORE
 * answer, then expires probes at the instant AFTER answer. */
FIPS_CORE_API void fips_path_liveness_halt(fips_path_liveness *l,fips_instant at,bool active);
FIPS_CORE_API void fips_path_liveness_spend(fips_path_liveness *l,fips_instant at);
FIPS_CORE_API void fips_path_liveness_rebase(fips_path_liveness *l,fips_instant at,bool ok);
FIPS_CORE_API void fips_path_liveness_resume(fips_path_liveness *l);
FIPS_CORE_API fips_duration fips_path_liveness_timeout(const fips_path_liveness *l,fips_duration floor,fips_duration cap);
FIPS_CORE_API uint32_t fips_path_liveness_take_id(fips_path_liveness *l,fips_entropy entropy);
FIPS_CORE_API bool fips_path_liveness_send(fips_path_liveness *l,fips_instant at,fips_instant due,size_t len,fips_entropy entropy,uint32_t *id);
FIPS_CORE_API void fips_path_liveness_lapse(fips_path_liveness *l,fips_path_sent sent);
FIPS_CORE_API fips_path_answer fips_path_liveness_answer(fips_path_liveness *l,uint32_t id);
FIPS_CORE_API bool fips_path_liveness_expire(fips_path_liveness *l,fips_instant now,bool overdue,fips_duration cap,bool *counted);
FIPS_CORE_API void fips_path_liveness_fresh(fips_path_liveness *l);
FIPS_CORE_API void fips_path_liveness_thaw(fips_path_liveness *l);
FIPS_CORE_API size_t fips_path_liveness_size(const fips_path_liveness *l,size_t full);
FIPS_CORE_API void fips_path_liveness_clear(fips_path_liveness *l);
FIPS_CORE_API void fips_path_liveness_sample(fips_path_liveness *l,fips_instant now,fips_duration rtt,fips_duration window,fips_duration horizon);
FIPS_CORE_API void fips_path_liveness_fold(fips_path_liveness *l,bool delivered);
FIPS_CORE_API bool fips_path_liveness_prove(fips_path_liveness *l,size_t mtu);
FIPS_CORE_API bool fips_path_liveness_seek_size(const fips_path_liveness *l,size_t full,fips_instant now,size_t *out);
FIPS_CORE_API uint32_t fips_path_liveness_seek_send(fips_path_liveness *l,fips_instant at,size_t len,fips_entropy e);
FIPS_CORE_API bool fips_path_liveness_seek_answer(fips_path_liveness *l,uint32_t id,fips_duration gap,size_t *out);
FIPS_CORE_API bool fips_path_liveness_seek_proved(fips_path_liveness *l,size_t sent,size_t acked,fips_instant now);
FIPS_CORE_API void fips_path_liveness_seek_judge(fips_path_liveness *l,fips_instant now,fips_duration timeout,fips_duration gap);
FIPS_CORE_API void fips_path_liveness_heard(fips_path_liveness *l,size_t len,fips_instant now);
FIPS_CORE_API bool fips_path_liveness_settle(fips_path_liveness *l,bool ok);
#ifdef __cplusplus
}
#endif
#endif
