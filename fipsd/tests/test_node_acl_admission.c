#include "handshake_fixture.h"
#include "test_tmpdir.h"
#include <stdio.h>

static void dial(handshake_fixture *f, void *arg)
{
    bool allowed = arg != NULL;
    hf_state before, after; hf_state_read(f, &before);
    bool ok = hs_dial(f->h, &f->remote, (fips_transport_id){1}, fipsd_loopback_transport_address(f->client), false);
    assert(ok == allowed);
    hf_state_read(f, &after);
    if (!allowed) {
        hf_unchanged(&before, &after);
        assert(!hs_dial(f->h, &f->remote, (fips_transport_id){1}, fipsd_loopback_transport_address(f->client), true));
        hf_state_read(f, &after); hf_unchanged(&before, &after);
        assert(hs_dial_configured(f->h));
        hf_state_read(f, &after); hf_unchanged(&before, &after);
    }
}
static void disconnect(handshake_fixture *f, void *arg)
{ (void)arg; assert(hs_disconnect(f->h, &f->remote.node_addr, FIPS_FMP_Restart, false)); }
void test_node_acl_admission(void)
{
    handshake_fixture f; hf_init(&f);
    const char *tmp = fipsd_test_tmpdir(NULL);
    char path[4096]; snprintf(path, sizeof path, "%s/admission.deny", tmp);
    hf_policy(&f, true, path);
    hf_call(&f, dial, NULL);
    hf_start(&f, 0xa1);
    hf_send(&f, f.client, f.msg1.bytes, f.msg1.len);
    hf_unchanged(&f.before, &f.after);
    assert(f.after.peers == 0 && f.after.indices == 0 && f.after.sent == 0);
    puts("A1 denied new peer + manual/configured dials: zero peers/indices/bindings/msg2");

    hf_policy(&f, false, path);
    hf_send(&f, f.client, f.msg1.bytes, f.msg1.len); hf_finish(&f); hf_heartbeat(&f);
    hf_policy(&f, true, path);
    /* Different epoch used to remove the live peer before the late ACL gate. */
    hf_start(&f, 0xb2); hf_send(&f, f.client, f.msg1.bytes, f.msg1.len);
    hf_unchanged(&f.before, &f.after); assert(f.after.peers == 1 && f.after.promoted == 1);
    /* Same epoch on another endpoint used to bypass the late gate entirely. */
    hf_start(&f, 0xa1); hf_send(&f, f.alternate, f.msg1.bytes, f.msg1.len);
    hf_unchanged(&f.before, &f.after);
    assert(fips_transport_addr_equal(&f.after.info.addr, fipsd_loopback_transport_address(f.client)));
    hf_heartbeat(&f); /* admission policy does not tear down existing traffic */
    puts("A1 denied restart + rebind: peer/index/endpoint/key/replay/send state unchanged; traffic survives");

    hf_call(&f, disconnect, NULL);
    hf_policy(&f, false, path); hf_call(&f, dial, &f);
    fipsd_transport_event *e = hf_wire(&f, 1);
    fips_fmp_ik_msg1 msg1 = {0}; fips_rekey_completed done = {0}; fips_rekey_wire msg2 = {0};
    assert(fips_fmp_rekey_read_msg1(&f.local, e->bytes, e->size, &msg1));
    assert(fips_fmp_rekey_profile.msg1_answer(&msg1, &f.local, (fips_rekey_index){100, true}, &done, &msg2));
    free(e);
    /* The authenticated reply was built under allow, but has not entered the
     * dispatcher. Publish deny, retaining the old immutable policy across it. */
    GoRefPtr old = go_swap_load(f.configs);
    hf_policy(&f, true, path);
    assert(fipsd_acl_check(fipsd_config_acl(old.ptr), &f.remote) != FIPSD_ACL_DENY_LIST);
    hf_send(&f, f.client, msg2.bytes, msg2.len);
    assert(f.before.peers == 0 && f.after.peers == 0 && f.after.promoted == f.before.promoted);
    assert(f.before.indices == 1 && f.after.indices == 0 && f.after.bindings == 0 && f.after.legs == 0);
    assert(f.after.sent == f.before.sent); /* denial cancels only its pending lease */
    go_refptr_release(&old); fips_rekey_completed_clear(&done); fips_fmp_rekey_msg1_clear(&msg1);
    puts("A1 reload during outbound IK: old policy retained, new deny wins at reply admission, pending lease reclaimed, no promotion/send");
    hf_destroy(&f);
}
