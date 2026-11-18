/* The Windows service's status sequence (src/service.c), on every host: the
 * fields each SetServiceStatus call carries, as the SCM documents them. */
#include "service.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static bool next(fipsd_service_status *s, uint32_t state, int code, fipsd_service_report *r)
{
    memset(r, 0xaa, sizeof *r);
    return fipsd_service_status_next(s, state, code, r);
}

int main(void)
{
    fipsd_service_status s;
    fipsd_service_report r;
    fipsd_service_status_init(&s, FIPSD_RUNTIME_DEFAULT_GRACE_MS);

    /* Nothing reported yet: only a known state. */
    CHECK(!next(&s, 0, 0, &r) && !next(&s, 7, 0, &r));

    /* START_PENDING: checkpoints 1, 2, ...; the start wait hint; no controls. */
    CHECK(next(&s, FIPSD_SVC_START_PENDING, 0, &r));
    CHECK(r.state == FIPSD_SVC_START_PENDING && r.checkpoint == 1 && r.wait_hint_ms == FIPSD_SVC_START_WAIT_MS);
    CHECK(r.controls_accepted == 0 && r.win32_exit_code == 0 && r.specific_exit_code == 0);
    CHECK(next(&s, FIPSD_SVC_START_PENDING, 0, &r) && r.checkpoint == 2);

    /* RUNNING: stop and system shutdown accepted; no checkpoint or hint; once. */
    CHECK(next(&s, FIPSD_SVC_RUNNING, 0, &r));
    CHECK(r.state == FIPSD_SVC_RUNNING && r.checkpoint == 0 && r.wait_hint_ms == 0);
    CHECK(r.controls_accepted == (FIPSD_SVC_ACCEPT_STOP | FIPSD_SVC_ACCEPT_SHUTDOWN));
    CHECK(!next(&s, FIPSD_SVC_RUNNING, 0, &r));
    CHECK(!next(&s, FIPSD_SVC_START_PENDING, 0, &r)); /* never backwards */

    /* STOP_PENDING: checkpoints restart at 1; hint is the grace plus margin. */
    CHECK(next(&s, FIPSD_SVC_STOP_PENDING, 0, &r));
    CHECK(r.checkpoint == 1 && r.controls_accepted == 0);
    CHECK(r.wait_hint_ms == FIPSD_RUNTIME_DEFAULT_GRACE_MS + FIPSD_SVC_STOP_MARGIN_MS);
    CHECK(next(&s, FIPSD_SVC_STOP_PENDING, 0, &r) && r.checkpoint == 2);
    CHECK(!next(&s, FIPSD_SVC_RUNNING, 0, &r)); /* a late "running" after a stop began */

    /* STOPPED with a clean exit, then nothing more. */
    CHECK(next(&s, FIPSD_SVC_STOPPED, 0, &r));
    CHECK(r.state == FIPSD_SVC_STOPPED && r.checkpoint == 0 && r.wait_hint_ms == 0 && r.controls_accepted == 0);
    CHECK(r.win32_exit_code == 0 && r.specific_exit_code == 0);
    CHECK(!next(&s, FIPSD_SVC_STOPPED, 0, &r) && !next(&s, FIPSD_SVC_STOP_PENDING, 0, &r));

    /* A failed start goes straight from START_PENDING to STOPPED with the
     * daemon's exit code as a service-specific error. */
    fipsd_service_status_init(&s, 2000);
    CHECK(next(&s, FIPSD_SVC_START_PENDING, 0, &r) && r.checkpoint == 1);
    CHECK(next(&s, FIPSD_SVC_STOPPED, 1, &r));
    CHECK(r.win32_exit_code == FIPSD_SVC_SPECIFIC_ERROR && r.specific_exit_code == 1);

    /* A daemon that ends on its own while running: STOP_PENDING, STOPPED. */
    fipsd_service_status_init(&s, 2000);
    CHECK(next(&s, FIPSD_SVC_START_PENDING, 0, &r) && next(&s, FIPSD_SVC_RUNNING, 0, &r));
    CHECK(next(&s, FIPSD_SVC_STOP_PENDING, 0, &r) && r.wait_hint_ms == 2000 + FIPSD_SVC_STOP_MARGIN_MS);
    CHECK(next(&s, FIPSD_SVC_STOPPED, 0, &r) && r.win32_exit_code == 0);
    puts("service status: start/stop pending checkpoints and hints, accepted controls, exit codes, one-way order");
    return 0;
}
