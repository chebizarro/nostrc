#ifndef FIPS_PROTO_SETTLE_MODEL_HISTORY_H
#define FIPS_PROTO_SETTLE_MODEL_HISTORY_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Abstract history only: none of these IDs or addresses are wire values. */
typedef struct fips_settle_model_addr { uint8_t transport; uint16_t at; } fips_settle_model_addr;
typedef enum fips_settle_model_binding { FIPS_MODEL_CONNECTIONLESS, FIPS_MODEL_CONNECTED } fips_settle_model_binding;
typedef enum fips_settle_model_event_kind {
    FIPS_MODEL_ADVANCE, FIPS_MODEL_DIAL, FIPS_MODEL_MSG1, FIPS_MODEL_MSG2_SENT,
    FIPS_MODEL_MSG2, FIPS_MODEL_TIMEOUT, FIPS_MODEL_FRAME, FIPS_MODEL_SWITCH,
    FIPS_MODEL_FOLLOW, FIPS_MODEL_REKEY_START, FIPS_MODEL_REKEY_MSG2,
    FIPS_MODEL_REKEY_CUTOVER, FIPS_MODEL_CLOSE, FIPS_MODEL_LOST, FIPS_MODEL_RESTART
} fips_settle_model_event_kind;
typedef struct fips_settle_model_event {
    fips_settle_model_event_kind kind;
    uint64_t ms;
    uint32_t hs, bytes, epoch, link;
    fips_settle_model_addr from, to;
    fips_settle_model_binding binding;
    bool sent, message;
    uint8_t paths;
} fips_settle_model_event;
#ifdef FIPS_TEST_HOOKS
FIPS_CORE_API fips_settle_model_event fips_settle_model_frame(uint32_t hs, fips_settle_model_addr from);
#endif
#ifdef __cplusplus
}
#endif
#endif
