#include "internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void bound(fipsd_config *c, fipsd_config_log *log, const char *key,
                  uint64_t floor, uint64_t cap, const char *low, const char *high)
{
    struct fipsd_config_value *v = cfg_get(c->root,key);
    if (!v || v->kind != CFG_UINT) return;
    uint64_t used = v->u < floor ? floor : v->u > cap ? cap : v->u;
    if (v->u != used) {
        cfg_log(log,FIPSD_LOG_WARN,"%s key=%s configured=%llu used=%llu",
                v->u < floor ? low : high,key,(unsigned long long)v->u,(unsigned long long)used);
        cfg_set_uint(v,used);
    }
}
void cfg_floors(fipsd_config *c, fipsd_config_log *log)
{
    bound(c,log,"node.link_dead_timeout_secs",30,UINT64_MAX,
        "configured value is below the 30 s a link must stay silent before it is declared dead; running at 30 s",NULL);
    const char *warnkeys[]={"node.rate_limit.handshake_max_resends","node.rate_limit.handshake_timeout_secs"};
    const uint64_t warncaps[]={5,30};
    const char *why[]={
        "configured value is above the 5 msg1 resends allowed to a handshake an mDNS advert directs at a node not configured to dial; those handshakes resend at most 5 times",
        "configured value is above the 30 s a link rekey attempt may run; link rekey attempts end within 30 s"};
    for (size_t i=0;i<2;++i) {
        uint64_t n=fipsd_config_uint(fipsd_config_get(c,warnkeys[i]));
        if (n>warncaps[i]) cfg_log(log,FIPSD_LOG_WARN,"%s key=%s configured=%llu used=%llu",why[i],warnkeys[i],
            (unsigned long long)n,(unsigned long long)warncaps[i]);
    }
    bound(c,log,"node.cache.coord_ttl_secs",300,UINT64_MAX,
        "configured value is below the 300 s least lifetime of a coordinate cache entry; running at 300 s",NULL);
    const fipsd_config_value *test=fipsd_config_get(c,"node.cache.test_coord_ttl_secs");
    if (test && test->kind != CFG_NULL) {
#ifdef FIPSD_TEST_OVERRIDES
        const char *msg="test-only key honoured by this test-overrides build; coordinate cache entries expire at this lifetime, whatever the 300 s floor";
        uint64_t used=test->u;
#else
        const char *msg="test-only key, honoured only by a build with the test-overrides feature; ignored";
        uint64_t used=fipsd_config_uint(fipsd_config_get(c,"node.cache.coord_ttl_secs"));
#endif
        cfg_log(log,FIPSD_LOG_WARN,"%s key=node.cache.test_coord_ttl_secs configured=%llu used=%llu",msg,
            (unsigned long long)test->u,(unsigned long long)used);
    }
    bound(c,log,"node.bloom.update_debounce_ms",500,UINT64_MAX,
        "configured value is below the 500 ms send interval; running at 500 ms",NULL);
    struct fipsd_config_value *fpr=cfg_get(c->root,"node.bloom.max_inbound_fpr");
    if (!(fpr->f>=0.20 && fpr->f<1.0)) {
        cfg_log(log,FIPSD_LOG_WARN,"configured value is outside the [0.20, 1.0) range of an inbound filter cap; running at 0.20 key=node.bloom.max_inbound_fpr configured=%g used=0.2",fpr->f);
        fpr->f=0.20;
    }
    bound(c,log,"node.rendezvous.nostr.signal_ttl_secs",120,UINT64_MAX,
        "configured value is below the 120 s least lifetime of a traversal signal; running at 120 s",NULL);
    bound(c,log,"node.rendezvous.nostr.attempt_timeout_secs",10,UINT64_MAX,
        "configured value is below the 10 s least punch attempt timeout; running at 10 s",NULL);
    bound(c,log,"node.rendezvous.nostr.max_concurrent_incoming_offers",16,UINT64_MAX,
        "configured value is below the 16 traversal offers a node must handle at once; running at 16",NULL);
    bound(c,log,"node.rendezvous.nostr.max_concurrent_offers_per_npub",4,UINT64_MAX,
        "configured value is below the 4 traversal offers a node must handle at once from one sender; running at 4",NULL);
    struct fipsd_config_value *ladder=cfg_get(c->root,"node.lookup.attempt_timeouts_secs");
    if (!ladder->count) {
        struct fipsd_config_value *defaults=cfg_yaml("[1, 2, 4, 8]",strlen("[1, 2, 4, 8]"),log);
        if (defaults) {
            for (size_t i=0;i<defaults->count;++i) cfg_set_uint(defaults->items[i],UINT64_C(1)<<i);
            if (!cfg_put(cfg_get(c->root,"node.lookup"),"attempt_timeouts_secs",defaults)) {
                cfg_free(defaults); cfg_error(log,"out of memory");
            }
        }
        cfg_log(log,FIPSD_LOG_WARN,"configured lookup ladder is empty; running the default ladder key=node.lookup.attempt_timeouts_secs configured=[] used=[1, 2, 4, 8]");
    } else if (ladder->count>255) {
        cfg_log(log,FIPSD_LOG_WARN,"configured lookup ladder has more than 255 entries (configured and used are lengths); running its first 255 key=node.lookup.attempt_timeouts_secs configured=%zu used=255",ladder->count);
        for (size_t i=255;i<ladder->count;++i) cfg_free(ladder->items[i]);
        ladder->count=255;
    }
    const char *owd[]={"node.mmp.owd_window_size","node.session_mmp.owd_window_size"};
    for (size_t i=0;i<2;++i) bound(c,log,owd[i],2,4096,
        "configured value is below the 2 samples a slope needs; running at 2",
        "configured value is above the 4096-sample cap; running at 4096");
    bound(c,log,"node.log_max_size_mb",1,UINT64_MAX,"configured log size is below 1 MiB; running at 1 MiB",NULL);
    bound(c,log,"node.log_max_files",1,100,"configured log count is below 1; running at 1","configured log count is above 100; running at 100");
    bound(c,log,"dns.ttl",0,INT32_MAX,NULL,"dns.ttl is above 2147483647, the largest TTL resolvers honour; using 2147483647");
    bound(c,log,"gateway.dns.ttl",0,INT32_MAX,NULL,"gateway.dns.ttl is above 2147483647, the largest TTL resolvers honour; using 2147483647");
}
