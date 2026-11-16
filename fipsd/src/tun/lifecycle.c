#include "tun/lifecycle.h"
#include "tun/internal.h"
#include "log.h"
#include <errno.h>
#include <string.h>
static int unwind(fipsd_tun *t)
{
    int error=0;
    if (t->routed) {
        if (t->platform->route(t,false)<0) error=errno;
        else t->routed=false;
    }
    if (t->raised) {
        if (t->platform->up(t,false)<0) { if (!error) error=errno; }
        else t->raised=false;
    }
    if (t->addressed) {
        if (t->platform->address(t,false)<0) { if (!error) error=errno; }
        else t->addressed=false;
    }
    if (t->fd>=0) {
        if (t->ops.close(t->ops.user,t->fd)<0 && !error) error=errno;
        t->fd=-1; /* close must not be retried against a reused descriptor */
    }
    if (t->destroy_pending) {
        if (t->platform->destroy(t)<0) { if (!error) error=errno; }
        else t->destroy_pending=false;
    }
    /* Close destroys Linux/macOS devices; explicit destroy does so on BSD.
     * Until then keep name/index and successful-stage ownership retryable. */
    if (!t->destroy_pending) t->routed=t->raised=t->addressed=false;
    return error;
}
int fipsd_tun_start(fipsd_tun *t,const fips_identity *id)
{
    if (!t || !id) { errno=EINVAL; return -1; }
    if (t->fd>=0) return 0;
    if (t->destroy_pending) {
        int error=unwind(t);
        if (error) { fipsd_tun_publish(t,FIPSD_TUN_FAILED,error); errno=error; return -1; }
    }
    t->address=fips_identity_address(id);
    for (unsigned attempt=1;attempt<=3;++attempt) {
        nsync_mu_lock(&t->status_mu); t->status.attempts=attempt; nsync_mu_unlock(&t->status_mu);
        t->index=0; t->name[0]=0;
        t->fd=t->platform->open(t);
        if (t->fd>=0) {
            t->destroy_pending=true;
            t->index=t->ops.ifindex(t->ops.user,t->name);
            if (!t->index) { errno=ENODEV; goto failed; }
            if (t->platform->address(t,true)<0) goto failed;
            t->addressed=true;
            if (t->platform->mtu(t)<0) goto failed;
            if (t->platform->up(t,true)<0) goto failed;
            t->raised=true;
            if (t->platform->route(t,true)<0) goto failed;
            /* route callback sets routed only for a newly installed route. */
            fipsd_tun_publish(t,FIPSD_TUN_UP,0); return 0;
        }
failed:;
        int error=errno;
        int cleanup=unwind(t);
        if (cleanup) { error=cleanup; fipsd_tun_publish(t,FIPSD_TUN_FAILED,error); errno=error; return -1; }
        if (attempt==3 || (error!=EBUSY && error!=ENOENT && error!=ENODEV)) {
            fipsd_tun_publish(t,FIPSD_TUN_FAILED,error); errno=error; return -1;
        }
    }
    return -1;
}
int fipsd_tun_stop(fipsd_tun *t)
{
    if (!t) return 0;
    int error=unwind(t);
    fipsd_tun_publish(t,FIPSD_TUN_STOPPED,error);
    if (error) errno=error;
    return error?-1:0;
}
static int service_start(fipsd_subsystem *self,fipsd_runtime *rt)
{
    (void)rt;
    fipsd_tun_service *s=self->user;
    GoRefPtr ref=fipsd_node_status_load(fipsd_node_status_slot(s->node));
    const fipsd_node_status *status=ref.ptr;
    bool ready=status && status->ready;
    go_refptr_release(&ref);
    if (!ready) { fipsd_tun_publish(s->device,FIPSD_TUN_WAITING,0); return 0; }
    if (fipsd_tun_start(s->device,fipsd_node_identity(fipsd_node_get_context(s->node)))<0)
        FIPSD_WARN("tun","setup failed: %s; continuing without TUN",strerror(errno));
    return 0;
}
static void service_stop(fipsd_subsystem *self,fipsd_runtime *rt)
{
    (void)rt; fipsd_tun_service *s=self->user;
    if (fipsd_tun_stop(s->device)<0) FIPSD_WARN("tun","teardown failed: %s",strerror(errno));
}
void fipsd_tun_service_init(fipsd_tun_service *s,fipsd_tun *t,fipsd_node *node)
{
    *s=(fipsd_tun_service){.device=t,.node=node};
    s->subsystem=(fipsd_subsystem){.name="tun",.start=service_start,.stop=service_stop,.user=s};
}
uint32_t fipsd_tun_transport_mtu(const fipsd_config *c)
{
    uint32_t mtu=UINT32_MAX;
    const fipsd_config_value *transports=fipsd_config_get(c,"transports");
    for (size_t i=0;i<fipsd_config_count(transports);++i) {
        const fipsd_config_value *type=fipsd_config_member(transports,fipsd_config_key(transports,i));
        const fipsd_config_value *value=fipsd_config_member(type,"mtu");
        if (value) { uint64_t n=fipsd_config_uint(value); if (n && n<mtu) mtu=(uint32_t)n; }
        else for (size_t j=0;j<fipsd_config_count(type);++j) {
            const fipsd_config_value *instance=fipsd_config_member(type,fipsd_config_key(type,j));
            uint64_t n=fipsd_config_uint(fipsd_config_member(instance,"mtu"));
            if (n && n<mtu) mtu=(uint32_t)n;
        }
    }
    return mtu==UINT32_MAX?1357:mtu;
}
