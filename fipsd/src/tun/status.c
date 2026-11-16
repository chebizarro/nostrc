#include "tun/internal.h"
#include <fips/identity/address.h>
#include <stdlib.h>
#include <string.h>
void fipsd_tun_publish(fipsd_tun *t,fipsd_tun_state state,int error)
{
    nsync_mu_lock(&t->status_mu);
    t->status.state=state; t->status.error=error; t->status.mtu=t->mtu; t->status.index=t->index;
    memcpy(t->status.name,t->name,sizeof(t->name));
    if (t->address.bytes[0]==0xfd) (void)fips_ipv6_address_format(&t->address,t->status.address,sizeof(t->status.address));
    nsync_mu_unlock(&t->status_mu);
}
fipsd_tun_status fipsd_tun_status_read(fipsd_tun *t)
{
    if (!t) return (fipsd_tun_status){.state=FIPSD_TUN_DISABLED};
    nsync_mu_lock(&t->status_mu); fipsd_tun_status s=t->status; nsync_mu_unlock(&t->status_mu); return s;
}
json_t *fipsd_tun_status_rows(const fipsd_tun_status *s)
{
    static const char *const states[]={"disabled","configured","active","failed","disabled"};
    return json_pack("{s:s,s:s,s:i,s:I}","tun_state",states[s->state],
        "tun_name",s->state==FIPSD_TUN_UP?s->name:"-","effective_ipv6_mtu",(int)s->mtu,"host_drops",(json_int_t)0);
}
typedef struct combined {
    fipsd_control_snapshot snapshot;
    GoRefPtr base;
    json_t *rows;
} combined;
static void combined_free(void *arg)
{
    combined *c=arg; go_refptr_release(&c->base); json_decref(c->rows); free(c);
}
static GoRefPtr acquire(void *arg)
{
    fipsd_tun *t=arg; combined *c=calloc(1,sizeof(*c)); if (!c) return (GoRefPtr){0};
    if (t->base.acquire) c->base=t->base.acquire(t->base.user);
    if (c->base.ptr) c->snapshot=*(const fipsd_control_snapshot *)c->base.ptr;
    fipsd_tun_status s=fipsd_tun_status_read(t); c->rows=fipsd_tun_status_rows(&s);
    if (!c->rows) { combined_free(c); return (GoRefPtr){0}; }
    c->snapshot.tun_status=c->rows;
    return make_go_refptr(c,combined_free); /* snapshot is the first member */
}
static json_t *command(void *arg,const char *name,const json_t *params,const fipsd_control_request_context *ctx)
{
    fipsd_tun *t=arg;
    return t->base.command?t->base.command(t->base.user,name,params,ctx):NULL;
}
fipsd_control_provider fipsd_tun_provider(fipsd_tun *t,fipsd_control_provider base)
{
    t->base=base; return (fipsd_control_provider){.user=t,.acquire=acquire,.command=command};
}
