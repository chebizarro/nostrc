/* Diagnostic probe has no wire codec. Fuzz bounded host-observation sequences,
 * action ordering, sticky ownership loss, finish idempotence and time extremes. */
#include <fips/proto/probe/mod.h>
#include <stdlib.h>
#include <string.h>
static fips_probe probe, saved;
static fips_probe_snapshot snapshot;
static fips_probe_path_facts path;
static fips_probe_next_hop_facts hop;
#define CHECK(x) do { if (!(x)) abort(); } while (0)
static uint64_t add(uint64_t a, uint64_t b) { return a > UINT64_MAX-b ? UINT64_MAX : a+b; }
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4) return 0;
    uint64_t tick = data[0] == 255 ? UINT64_MAX : (uint64_t)data[0]*100;
    uint64_t now = data[1] == 255 ? UINT64_MAX-10000 : 0;
    uint64_t ladder[] = {data[2],data[3]};
    fips_probe_budgets budgets = fips_probe_budgets_derive(tick,ladder,2);
    fips_probe_preflight pre = {(data[1]&1)!=0,(data[1]&2)!=0,(data[1]&4)!=0,(data[1]&8)!=0};
    fips_probe_new(&probe,now,budgets,pre);
    unsigned open = 0, teardown = 0, warmup = 0, finish = 0;
    bool lost = false;
    for (size_t pos = 4; pos + 8 <= size && pos < 260; pos += 8) {
        const uint8_t *b = data+pos;
        now = add(now,(uint64_t)b[0]*100);
        path.same_root = (b[2]&1)!=0;
        fips_probe_observation o = {.now_ms=now,.coords_cached=(b[1]&1)!=0,
            .lookup_pending=(b[1]&2)!=0,.has_lookup_outcome=(b[1]&4)!=0,
            .lookup_outcome=(fips_probe_lookup_outcome_kind)(b[2]%5),
            .has_lookup_fanout=(b[1]&8)!=0,.lookup_fanout=b[3],
            .path=(b[1]&16) ? &path : NULL,.next_hop=(b[1]&32) ? &hop : NULL,
            .session_present=(b[4]&1)!=0,.session_established=(b[4]&2)!=0,.session_is_ours=(b[4]&4)!=0,
            .target_is_direct_peer=(b[4]&8)!=0,.has_last_rtt_ms=(b[4]&16)!=0,.last_rtt_ms=b[5],
            .has_srtt_ms=(b[4]&32)!=0,.srtt_ms=b[5],.has_path_mtu=(b[4]&64)!=0,.path_mtu=b[6],
            .counters={b[5],b[6],b[7]&15,b[7]>>4}};
        if (b[3] == 255) { o.session_error="send error"; o.session_error_len=10; }
        fips_probe_actions a = {0};
        bool was_finished = fips_probe_is_finished(&probe);
        bool was_owned = fips_probe_owns_session(&probe);
        if (b[7] == 255) fips_probe_cancel(&probe,now,&a);
        else CHECK(fips_probe_step(&probe,&o,&a) == FIPS_OK);
        if (was_owned && !fips_probe_owns_session(&probe)) lost = true;
        if (lost) CHECK(!fips_probe_owns_session(&probe));
        CHECK(a.len <= FIPS_PROBE_ACTION_CAPACITY);
        if (was_finished) CHECK(a.len == 0);
        for (size_t i=0;i<a.len;++i) {
            CHECK(finish == 0);
            switch (a.items[i]) {
            case FIPS_PROBE_OpenSession: CHECK(++open == 1); break;
            case FIPS_PROBE_SendWarmup: CHECK(++warmup <= 2); break;
            case FIPS_PROBE_TeardownSession: CHECK(open == 1 && ++teardown == 1 && !lost); break;
            case FIPS_PROBE_Finish: ++finish; break;
            case FIPS_PROBE_InitiateLookup: break;
            }
        }
        fips_probe_snapshot_get(&probe,&snapshot);
        CHECK(snapshot.warmups_sent == warmup);
        CHECK(snapshot.torn_down == (teardown != 0));
    }
    /* Full state and output preservation on C's sole input-capacity refusal. */
    if (!fips_probe_is_finished(&probe)) {
        saved = probe;
        fips_probe_actions a = {0}, before = a;
        fips_probe_observation o = {.now_ms=now,.session_error="x",.session_error_len=FIPS_PROBE_DETAIL_CAPACITY};
        CHECK(fips_probe_step(&probe,&o,&a) == FIPS_ERR_RANGE);
        CHECK(!memcmp(&saved,&probe,sizeof(probe)) && !memcmp(&a,&before,sizeof(a)));
    }
    return 0;
}
