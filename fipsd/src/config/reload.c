#include "internal.h"
#include "node/acl.h"
#include "select.h"
#include <blocking_executor.h>
#include <go.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool equal(const fipsd_config_value *a, const fipsd_config_value *b)
{
    if (!a || a->kind==CFG_NULL) return !b || b->kind==CFG_NULL;
    if (!b || a->kind!=b->kind || a->count!=b->count || a->named_transport!=b->named_transport) return false;
    if (a->kind==CFG_MAP) {
        for (size_t i=0;i<a->count;++i) {
            const fipsd_config_value *match=fipsd_config_member(b,a->keys[i]);
            if (!match || !equal(a->items[i],match)) return false;
        }
    } else if (a->kind==CFG_LIST) {
        for (size_t i=0;i<a->count;++i) if (!equal(a->items[i],b->items[i])) return false;
    } else if (a->kind==CFG_STRING) return !strcmp(a->text,b->text);
    else if (a->kind==CFG_FLOAT) return a->f==b->f || (isnan(a->f) && isnan(b->f));
    else return a->u==b->u;
    return true;
}
static bool reloadable(const char *path)
{
    return !strcmp(path,"node.log_level") || !strcmp(path,"node.log_max_size_mb") ||
           !strcmp(path,"node.log_max_files");
}
static bool changes(struct fipsd_config_value *out, const fipsd_config_value *old,
                    const fipsd_config_value *next, const char *path, fipsd_config_log *log)
{
    if (equal(old,next)) return true;
    if (old && next && old->kind==CFG_MAP && next->kind==CFG_MAP) {
        for (size_t i=0;i<old->count;++i) {
            char sub[4096]; snprintf(sub,sizeof(sub),"%s%s%s",path,*path ? "." : "",old->keys[i]);
            if (!changes(out,old->items[i],fipsd_config_member(next,old->keys[i]),sub,log)) return false;
        }
        for (size_t i=0;i<next->count;++i) if (!fipsd_config_member(old,next->keys[i])) {
            char sub[4096]; snprintf(sub,sizeof(sub),"%s%s%s",path,*path ? "." : "",next->keys[i]);
            if (!changes(out,NULL,next->items[i],sub,log)) return false;
        }
        return true;
    }
    if (!reloadable(path)) {
        cfg_log(log,FIPSD_LOG_WARN,"configuration change requires restart; keeping active value key=%s",path);
        return true;
    }
    const char *key=strrchr(path,'.');
    struct fipsd_config_value *copy=cfg_clone(next);
    if (!copy || !cfg_put(cfg_get(out,"node"),key+1,copy)) { cfg_free(copy); return cfg_error(log,"out of memory"); }
    cfg_log(log,FIPSD_LOG_INFO,"reloaded configuration key=%s",path);
    return true;
}
bool fipsd_config_reload(GoSwap *slot, const char *path, fipsd_config_log *log)
{
    if (!log) {
        fipsd_config_log held={0};
        bool ok=fipsd_config_reload(slot,path,&held);
        fipsd_config_log_clear(&held); return ok;
    }
    GoRefPtr ref=go_swap_load(slot);
    const fipsd_config *old=ref.ptr;
    if (!old) { go_refptr_release(&ref); return cfg_error(log,"configuration snapshot is empty"); }
    fipsd_config *next=fipsd_config_load(path,log);
    if (!next) { go_refptr_release(&ref); return false; }
    fipsd_config *out=calloc(1,sizeof(*out));
    bool ok=out!=NULL;
    if (out) { out->root=cfg_clone(old->root); out->core=old->core; ok=out->root!=NULL; }
    if (ok) ok=changes(out->root,old->root,next->root,"",log);
    if (ok && old->acl) { out->acl=fipsd_acl_load(out,NULL,old->acl); ok=out->acl!=NULL; }
    if (ok) ok=go_swap_store(slot,out);
    if (!ok) { fipsd_config_free(out); cfg_error(log,"cannot publish configuration snapshot"); }
    fipsd_config_free(next);
    go_refptr_release(&ref);
    return ok;
}
void fipsd_config_apply_logging(const fipsd_config *c, fipsd_log *logger, fipsd_log_file *file)
{
    fipsd_log_level level=FIPSD_LOG_INFO;
    const char *name=getenv("FIPSD_LOG_LEVEL");
    if (!name || !*name) name=fipsd_config_string(fipsd_config_get(c,"node.log_level"));
    if (name) (void)fipsd_log_level_parse(name,&level);
    fipsd_log_set_level(logger,level);
    if (file) fipsd_log_file_apply_limits(logger,file,fipsd_config_log_max_bytes(c),
        (unsigned)fipsd_config_uint(fipsd_config_get(c,"node.log_max_files")));
}
bool fipsd_config_enable_acl(fipsd_config *c, const fipsd_acl_paths *paths)
{
    if (!c) return false;
    fipsd_acl *a=fipsd_acl_load(c,paths,NULL);
    if (!a) return false;
    fipsd_acl_free(c->acl); c->acl=a; return true;
}
const fipsd_acl *fipsd_config_acl(const fipsd_config *c) { return c ? c->acl : NULL; }
/* Regular-file mtime maintenance mirrors upstream Reloadable, never relay
 * polling. It shares the single config writer with SIGHUP; no dispatcher I/O. */
static void *resource_job(void *arg)
{
    fipsd_config_service *service=arg;
    GoRefPtr ref=go_swap_load(service->snapshots);
    const fipsd_config *old=ref.ptr;
    if (old && old->acl && fipsd_acl_changed(old->acl)) {
        fipsd_config *out=calloc(1,sizeof *out);
        if (out) {
            out->root=cfg_clone(old->root); out->core=old->core;
            out->acl=fipsd_acl_load(old,NULL,old->acl);
            if (!out->root || !out->acl || !go_swap_store(service->snapshots,out)) fipsd_config_free(out);
        }
    }
    go_refptr_release(&ref); return NULL;
}
static void *reload_job(void *arg)
{
    fipsd_config_service *service=arg;
    fipsd_config_log held={0};
    if (fipsd_config_reload(service->snapshots,service->path,&held)) {
        GoRefPtr ref=go_swap_load(service->snapshots);
        fipsd_config_apply_logging(ref.ptr,service->logger,service->log_file);
        go_refptr_release(&ref);
        fipsd_log_write(service->logger,FIPSD_LOG_INFO,"config","configuration reload complete");
    } else {
        fipsd_log_write(service->logger,FIPSD_LOG_ERROR,"config","configuration reload failed; keeping active snapshot: %s",held.error);
    }
    fipsd_config_log_replay(&held,service->logger);
    fipsd_config_log_clear(&held);
    return NULL;
}
static void reload_fiber(void *arg)
{
    fipsd_config_service *service=arg;
    GoContext *context=fipsd_runtime_context(service->runtime);
    while (!go_context_is_canceled(context)) {
        void *token=NULL;
        GoSelectCase cases[]={
            {.op=GO_SELECT_RECEIVE,.chan=go_context_done(context),.recv_buf=&token},
            {.op=GO_SELECT_RECEIVE,.chan=fipsd_runtime_reload_channel(service->runtime),.recv_buf=&token}
        };
        GoSelectResult selected=go_select_timeout(cases,2,1000);
        if (go_context_is_canceled(context) || selected.selected_case==0) break;
        (void)go_blocking_submit(selected.selected_case==1 ? reload_job : resource_job,service);
    }
}
static int start(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    fipsd_config_service *service=subsystem->user;
    service->runtime=rt;
    return fipsd_runtime_spawn(rt,reload_fiber,service);
}
void fipsd_config_service_init(fipsd_config_service *service, GoSwap *slot,
                               const char *path, fipsd_log *logger, fipsd_log_file *file)
{
    *service=(fipsd_config_service){.subsystem={"config",start,NULL,service},
        .snapshots=slot,.path=path,.logger=logger,.log_file=file};
}
