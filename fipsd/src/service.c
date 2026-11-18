#include "service.h"

#include <string.h>

/* Position in the one-way lifecycle; 0 is "nothing reported yet". */
static unsigned rank(uint32_t state)
{
    switch (state) {
    case FIPSD_SVC_START_PENDING: return 1;
    case FIPSD_SVC_RUNNING: return 2;
    case FIPSD_SVC_STOP_PENDING: return 3;
    case FIPSD_SVC_STOPPED: return 4;
    default: return 0;
    }
}

void fipsd_service_status_init(fipsd_service_status *s, uint32_t grace_ms)
{
    memset(s, 0, sizeof *s);
    s->stop_wait_ms = grace_ms + FIPSD_SVC_STOP_MARGIN_MS;
}

bool fipsd_service_status_next(fipsd_service_status *s, uint32_t state, int exit_code,
                               fipsd_service_report *out)
{
    unsigned next = rank(state), now = rank(s->state);
    bool pending = state == FIPSD_SVC_START_PENDING || state == FIPSD_SVC_STOP_PENDING;
    if (!next || next < now || now == rank(FIPSD_SVC_STOPPED) || (next == now && !pending))
        return false;
    memset(out, 0, sizeof *out);
    out->state = state;
    if (pending) {
        /* Checkpoints count up within one pending state and restart in the next. */
        s->checkpoint = next == now ? s->checkpoint + 1 : 1;
        out->checkpoint = s->checkpoint;
        out->wait_hint_ms = state == FIPSD_SVC_START_PENDING ? FIPSD_SVC_START_WAIT_MS : s->stop_wait_ms;
    } else {
        s->checkpoint = 0;
        if (state == FIPSD_SVC_RUNNING)
            out->controls_accepted = FIPSD_SVC_ACCEPT_STOP | FIPSD_SVC_ACCEPT_SHUTDOWN;
        else if (exit_code != 0) {
            out->win32_exit_code = FIPSD_SVC_SPECIFIC_ERROR;
            out->specific_exit_code = (uint32_t)exit_code;
        }
    }
    s->state = state;
    return true;
}
