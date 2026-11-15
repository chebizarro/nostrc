#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fipsd_config_free(void *ptr)
{
    fipsd_config *c=ptr;
    if (c) { cfg_free(c->root); free(c); }
}
static bool is_dir(const char *path)
{ struct stat st; return stat(path,&st)==0 && S_ISDIR(st.st_mode); }
static char *socket_path(const char *filename)
{
    char path[4096];
    const char *dir=NULL;
    if (is_dir("/run/fips")) dir="/run/fips";
#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (is_dir("/var/run/fips")) dir="/var/run/fips";
#ifdef __APPLE__
    else if (geteuid()==0 && is_dir("/var/run")) dir="/var/run/fips";
#endif
#endif
    if (dir) snprintf(path,sizeof(path),"%s/%s",dir,filename);
    else {
        const char *xdg=getenv("XDG_RUNTIME_DIR");
        if (xdg && is_dir(xdg)) snprintf(path,sizeof(path),"%s/fips/%s",xdg,filename);
        else snprintf(path,sizeof(path),"/tmp/fips-%s",filename);
    }
    return strdup(path);
}
bool cfg_socket_defaults(fipsd_config *c, fipsd_config_log *log)
{
    const char *keys[]={"node.control.socket_path","node.native_api.socket_path"};
    const char *markers[]={"__control_socket__","__api_socket__"};
    const char *files[]={"control.sock","api.sock"};
    for (size_t i=0;i<2;++i) {
        struct fipsd_config_value *v=cfg_get(c->root,keys[i]);
        if (v && v->text && !strcmp(v->text,markers[i])) {
            char *path=socket_path(files[i]);
            if (!path) return cfg_error(log,"cannot allocate default socket path");
            free(v->text); v->text=path;
            cfg_log(log,FIPSD_LOG_DEBUG,"applied default key=%s value=%s",keys[i],path);
        }
    }
    return true;
}
static bool transport_named(const fipsd_config_value *v)
{ return v && v->named_transport; }
static bool effective_udp(fipsd_config *c, fipsd_config_log *log)
{
    struct fipsd_config_value *udp=cfg_get(c->root,"transports.udp");
    if (!udp || udp->kind!=CFG_MAP) return true;
    bool named=transport_named(udp);
    for (size_t i=0;i<(named ? udp->count : 1);++i) {
        struct fipsd_config_value *v=named ? udp->items[i] : udp;
        if (!fipsd_config_bool(fipsd_config_member(v,"outbound_only"))) continue;
        struct fipsd_config_value *bind=cfg_get(v,"bind_addr");
        char *text=strdup("0.0.0.0:0");
        if (!text) return cfg_error(log,"cannot allocate outbound-only bind address");
        if (strcmp(bind->text,text) || fipsd_config_bool(fipsd_config_member(v,"advertise_on_nostr")) ||
            fipsd_config_bool(fipsd_config_member(v,"accept_connections")))
            cfg_log(log,FIPSD_LOG_DEBUG,"outbound_only overrides bind_addr, advertise_on_nostr and accept_connections key=transports.udp%s%s",
                    named ? "." : "",named ? udp->keys[i] : "");
        free(bind->text); bind->text=text;
        cfg_set_uint(cfg_get(v,"advertise_on_nostr"),0);
        cfg_get(v,"advertise_on_nostr")->kind=CFG_BOOL;
        cfg_set_uint(cfg_get(v,"accept_connections"),0);
        cfg_get(v,"accept_connections")->kind=CFG_BOOL;
    }
    return true;
}
static fipsd_config *finish(struct fipsd_config_value *raw, fipsd_config_log *log)
{
    if (!raw) return NULL;
    fipsd_config *c=calloc(1,sizeof(*c));
    if (!c) { cfg_free(raw); cfg_error(log,"out of memory"); return NULL; }
    c->root=raw;
    cfg_deprecated(raw,log);
    if (!cfg_normalize(raw,cfg_root,"",log)) goto fail;
    if (!cfg_socket_defaults(c,log) || !effective_udp(c,log)) goto fail;
    cfg_floors(c,log);
    if ((log && log->error[0]) || !cfg_validate(c,log) || !cfg_core(c,log)) goto fail;
    return c;
fail: fipsd_config_free(c); return NULL;
}
fipsd_config *fipsd_config_parse(const char *yaml, size_t len, fipsd_config_log *log)
{
    if (!log) {
        fipsd_config_log held={0};
        fipsd_config *c=fipsd_config_parse(yaml,len,&held);
        fipsd_config_log_clear(&held); return c;
    }
    return finish(cfg_yaml(yaml,len,log),log);
}

static struct fipsd_config_value *read_file(const char *path, fipsd_config_log *log)
{
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) { cfg_error(log,"failed to read config file %s: %s",path,strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>8*1024*1024) {
        close(fd); cfg_error(log,"failed to read config file %s: expected regular file of at most 8 MiB",path); return NULL;
    }
    size_t cap=(size_t)st.st_size+1, len=0;
    char *data=malloc(cap);
    if (!data) { close(fd); cfg_error(log,"out of memory"); return NULL; }
    bool ok=true;
    while (len<cap) {
        ssize_t n=read(fd,data+len,cap-len);
        if (n<0 && errno==EINTR) continue;
        if (n<0) { cfg_error(log,"failed to read config file %s: %s",path,strerror(errno)); ok=false; break; }
        if (!n) break;
        len+=(size_t)n;
    }
    close(fd);
    if (len==cap) { cfg_error(log,"config file %s grew while reading; retry reload",path); ok=false; }
    struct fipsd_config_value *v=ok ? cfg_yaml(data,len,log) : NULL;
    sodium_memzero(data,cap); free(data);
    if (!v && log && log->error[0] && !strstr(log->error,"failed to read")) {
        char error[2048]; snprintf(error,sizeof(error),"%s",log->error);
        snprintf(log->error,sizeof(log->error),"failed to parse config file %s: %.1200s",path,error);
    }
    return v;
}
static bool nothing(const fipsd_config *c)
{
    const fipsd_config_value *identity=fipsd_config_get(c,"node.identity.nsec");
    const fipsd_config_value *gateway=fipsd_config_get(c,"gateway");
    return (!identity || identity->kind==CFG_NULL) &&
        !fipsd_config_bool(fipsd_config_get(c,"node.identity.persistent")) &&
        !fipsd_config_count(fipsd_config_get(c,"transports")) &&
        !fipsd_config_count(fipsd_config_get(c,"peers")) && (!gateway || gateway->kind==CFG_NULL);
}
static fipsd_config *check_loaded(fipsd_config *c, const char *path, fipsd_config_log *log)
{
    if (c && nothing(c)) {
        cfg_error(log,"config file %s configures nothing (no identity, transports, peers or gateway section); refusing to start on built-in defaults (ephemeral identity, no transports, no peers)",path);
        fipsd_config_free(c); return NULL;
    }
    return c;
}
void fipsd_config_search_paths_free(char **paths, size_t count)
{
    for (size_t i=0;i<count;++i) free(paths[i]);
    free(paths);
}
char **fipsd_config_search_paths(size_t *count)
{
    char user[4096],legacy[4096],cwd[4096];
    const char *paths[6]; size_t n=0;
    *count=0;
    paths[n++]="/etc/fips/fips.yaml";
#if defined(__APPLE__) || defined(__FreeBSD__)
    paths[n++]="/usr/local/etc/fips/fips.yaml";
#endif
    const char *home=getenv("HOME");
#ifdef __APPLE__
    if (home) {
        if (snprintf(user,sizeof(user),"%s/Library/Application Support/fips/fips.yaml",home)>=(int)sizeof(user)) return NULL;
        paths[n++]=user;
    }
#else
    const char *xdg=getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        if (snprintf(user,sizeof(user),"%s/fips/fips.yaml",xdg)>=(int)sizeof(user)) return NULL;
        paths[n++]=user;
    } else if (home) {
        if (snprintf(user,sizeof(user),"%s/.config/fips/fips.yaml",home)>=(int)sizeof(user)) return NULL;
        paths[n++]=user;
    }
#endif
    if (home) {
        if (snprintf(legacy,sizeof(legacy),"%s/.fips.yaml",home)>=(int)sizeof(legacy)) return NULL;
        paths[n++]=legacy;
    }
    if (!getcwd(cwd,sizeof(cwd))) return NULL;
    size_t used=strlen(cwd);
    if (used+11>sizeof(cwd)) return NULL;
    memcpy(cwd+used,"/fips.yaml",11); paths[n++]=cwd;
    char **out=calloc(n,sizeof(*out));
    if (!out) return NULL;
    for (size_t i=0;i<n;++i) if (!(out[i]=strdup(paths[i]))) {
        fipsd_config_search_paths_free(out,n); return NULL;
    }
    *count=n; return out;
}
fipsd_config *fipsd_config_load(const char *path, fipsd_config_log *log)
{
    if (!log) {
        fipsd_config_log held={0};
        fipsd_config *c=fipsd_config_load(path,&held);
        fipsd_config_log_clear(&held); return c;
    }
    if (!path) {
        size_t count;
        char **paths=fipsd_config_search_paths(&count);
        if (!paths) { cfg_error(log,"cannot resolve configuration search paths"); return NULL; }
        fipsd_config *c=fipsd_config_load_paths((const char *const *)paths,count,log);
        fipsd_config_search_paths_free(paths,count);
        return c;
    }
    return check_loaded(finish(read_file(path,log),log),path,log);
}
static bool overlay(struct fipsd_config_value *to, const struct fipsd_config_value *from,
                    const char *path, fipsd_config_log *log)
{
    if (from->kind!=CFG_MAP) return cfg_error(log,"%s: expected struct",path);
    for (size_t i=0;i<from->count;++i) {
        const char *key=from->keys[i];
        const struct fipsd_config_value *v=from->items[i];
        struct fipsd_config_value *prior=(struct fipsd_config_value *)fipsd_config_member(to,key);
        char sub[4096]; snprintf(sub,sizeof(sub),"%s%s%s",path,*path ? "." : "",key);
        if (v->kind==CFG_NULL && ((prior && prior->kind==CFG_MAP) ||
            !strncmp(sub,"node.identity.",14) || !strcmp(sub,"node.leaf_only") ||
            !strncmp(sub,"tun.",4) || !strncmp(sub,"dns.",4))) continue;
        /* Root transport types replace whole; empty lists do not erase peers. */
        if (!strcmp(sub,"peers") && v->kind==CFG_LIST && !v->count) continue;
        if (!strcmp(sub,"node.leaf_only") && v->text && !strcmp(v->text,"false") && prior) continue;
        if (prior && prior->kind==CFG_MAP && v->kind==CFG_MAP &&
            strcmp(sub,"gateway") && strncmp(sub,"transports.",11)) {
            if (!overlay(prior,v,sub,log)) return false;
        } else {
            struct fipsd_config_value *copy=cfg_clone(v);
            if (!copy || !cfg_put(to,key,copy)) { cfg_free(copy); return cfg_error(log,"out of memory"); }
        }
    }
    return true;
}
/* Keep exactly the explicit recognized keys, not defaults introduced by
 * per-layer type checking. Unknown keys were logged by that check. */
static void recognized(struct fipsd_config_value *raw, const fipsd_config_value *typed)
{
    if (!raw || !typed) return;
    if (raw->kind==CFG_LIST && typed->kind==CFG_LIST) {
        for (size_t i=0;i<raw->count && i<typed->count;++i) recognized(raw->items[i],typed->items[i]);
        return;
    }
    if (raw->kind!=CFG_MAP || typed->kind!=CFG_MAP) return;
    for (size_t i=0;i<raw->count;) {
        const fipsd_config_value *match=fipsd_config_member(typed,raw->keys[i]);
        if (!match) {
            cfg_free(raw->items[i]); free(raw->keys[i]); --raw->count;
            memmove(raw->items+i,raw->items+i+1,(raw->count-i)*sizeof(*raw->items));
            memmove(raw->keys+i,raw->keys+i+1,(raw->count-i)*sizeof(*raw->keys));
            continue;
        }
        recognized(raw->items[i],match); ++i;
    }
}
fipsd_config *fipsd_config_load_paths(const char *const *paths, size_t count, fipsd_config_log *log)
{
    if (!log) {
        fipsd_config_log held={0};
        fipsd_config *c=fipsd_config_load_paths(paths,count,&held);
        fipsd_config_log_clear(&held); return c;
    }
    struct fipsd_config_value *raw=cfg_new(CFG_MAP);
    if (!raw) { cfg_error(log,"out of memory"); return NULL; }
    const char *last=NULL;
    for (size_t i=0;i<count;++i) {
        struct stat st;
        if (stat(paths[i],&st)!=0 && errno==ENOENT) continue;
        struct fipsd_config_value *file=read_file(paths[i],log);
        if (!file) { cfg_free(raw); return NULL; }
        cfg_deprecated(file,log);
        /* Check each layer's types before overlay, without applying floors or
         * cross-field validation to a deliberately partial layer. */
        struct fipsd_config_value *typed=cfg_clone(file);
        fipsd_config_log layer={0};
        bool ok=typed && cfg_normalize(typed,cfg_root,"",&layer);
        for (size_t j=0;j<layer.count;++j)
            if (layer.entries[j].level<=FIPSD_LOG_WARN)
                cfg_log(log,layer.entries[j].level,"%s",layer.entries[j].message);
        if (!ok) cfg_error(log,"failed to parse config file %s: %s",paths[i],layer.error);
        if (ok) recognized(file,typed);
        fipsd_config_log_clear(&layer);
        cfg_free(typed);
        if (ok) ok=overlay(raw,file,"",log);
        cfg_free(file);
        if (!ok) { cfg_free(raw); return NULL; }
        last=paths[i];
        cfg_log(log,FIPSD_LOG_INFO,"Loaded configuration from %s",paths[i]);
    }
    fipsd_config *c=finish(raw,log);
    return last ? check_loaded(c,last,log) : c;
}

static int quote(FILE *out, const char *s)
{
    if (fputc('"',out)==EOF) return -1;
    for (const unsigned char *p=(const unsigned char *)s;*p;++p) {
        if (*p=='"' || *p=='\\') { if (fputc('\\',out)==EOF || fputc(*p,out)==EOF) return -1; }
        else if (*p<32) { if (fprintf(out,"\\u%04x",*p)<0) return -1; }
        else if (fputc(*p,out)==EOF) return -1;
    }
    return fputc('"',out)==EOF ? -1 : 0;
}
typedef struct dump_pair { const char *key; const fipsd_config_value *value; } dump_pair;
static int pair_cmp(const void *a, const void *b)
{ return strcmp(((const dump_pair *)a)->key,((const dump_pair *)b)->key); }
static int dump(const fipsd_config_value *v, const char *path, FILE *out)
{
    if (v->kind==CFG_NULL) return 0;
    if (v->kind==CFG_MAP && v->count) {
        dump_pair *pairs=malloc(v->count*sizeof(*pairs));
        if (!pairs) return -1;
        for (size_t i=0;i<v->count;++i) pairs[i]=(dump_pair){v->keys[i],v->items[i]};
        qsort(pairs,v->count,sizeof(*pairs),pair_cmp);
        int rc=0;
        for (size_t i=0;i<v->count && !rc;++i) {
            char key[4096];
            snprintf(key,sizeof(key),"%s%s%s",path,*path ? "." : "",pairs[i].key);
            rc=dump(pairs[i].value,key,out);
        }
        free(pairs); return rc;
    }
    if (v->kind==CFG_LIST && v->count) {
        for (size_t i=0;i<v->count;++i) {
            char key[4096]; snprintf(key,sizeof(key),"%s.%zu",path,i);
            if (dump(v->items[i],key,out)) return -1;
        }
        return 0;
    }
    if (fprintf(out,"%s=",path)<0) return -1;
    switch (v->kind) {
    case CFG_STRING:
        if (quote(out,!strcmp(path,"node.identity.nsec") ? "<redacted>" : v->text)) return -1;
        break;
    case CFG_UINT: if (fprintf(out,"%" PRIu64,v->u)<0) return -1; break;
    case CFG_BOOL: if (fputs(v->u ? "true" : "false",out)==EOF) return -1; break;
    case CFG_FLOAT: if (fprintf(out,"%.17g",v->f)<0) return -1; break;
    case CFG_MAP: if (fputs("{}",out)==EOF) return -1; break;
    case CFG_LIST: if (fputs("[]",out)==EOF) return -1; break;
    default: return -1;
    }
    return fputc('\n',out)==EOF ? -1 : 0;
}
int fipsd_config_dump(const fipsd_config *c, FILE *out) { return dump(c->root,"",out); }
uint64_t fipsd_config_log_max_bytes(const fipsd_config *c)
{
    uint64_t mb=fipsd_config_uint(fipsd_config_get(c,"node.log_max_size_mb"));
    return mb>UINT64_MAX/(1024*1024) ? UINT64_MAX : mb*1024*1024;
}
