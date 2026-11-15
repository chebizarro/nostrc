#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool normalize_value(struct fipsd_config_value *v, const cfg_field *f,
                             const char *path, fipsd_config_log *log);
static bool enum_ok(const char *choices, const char *text)
{
    for (const char *c = choices; c && *c;) {
        const char *end = strchr(c,'|');
        size_t n = end ? (size_t)(end-c) : strlen(c);
        if (strlen(text) == n && !memcmp(c,text,n)) return true;
        c = end ? end+1 : NULL;
    }
    return false;
}
static const cfg_field *field(const cfg_field *fields, const char *name)
{
    for (const cfg_field *f = fields; f && f->name; ++f)
        if (!strcmp(f->name,name)) return f;
    return NULL;
}
bool cfg_normalize(struct fipsd_config_value *v, const cfg_field *fields,
                   const char *path, fipsd_config_log *log)
{
    if (v->kind == CFG_NULL) v->kind = CFG_MAP;
    if (v->kind != CFG_MAP) return cfg_error(log,"%s: invalid type, expected struct",path);
    /* Unknown keys are explicitly ignored, never accidentally retained as live
     * settings. Do not print their values: an unknown key may hold a secret. */
    for (size_t i = 0; i < v->count;) {
        if (field(fields,v->keys[i])) { ++i; continue; }
        cfg_log(log,FIPSD_LOG_WARN,"unknown configuration key `%s%s%s`; ignored",path,
                *path ? "." : "",v->keys[i]);
        cfg_free(v->items[i]); free(v->keys[i]);
        --v->count;
        memmove(v->items+i,v->items+i+1,(v->count-i)*sizeof(*v->items));
        memmove(v->keys+i,v->keys+i+1,(v->count-i)*sizeof(*v->keys));
    }
    for (const cfg_field *f = fields; f->name; ++f) {
        char key[4096];
        if (snprintf(key,sizeof(key),"%s%s%s",path,*path ? "." : "",f->name) >= (int)sizeof(key))
            return cfg_error(log,"configuration path too long");
        struct fipsd_config_value *item = (struct fipsd_config_value *)fipsd_config_member(v,f->name);
        if (!item || item->kind == CFG_NULL) {
            if (f->value && !strcmp(f->value,"!")) return cfg_error(log,"%s: missing field `%s`",path,f->name);
            if (!f->value) {
                if (item) item->kind = CFG_NULL;
                continue;
            }
            item = cfg_yaml(f->value,strlen(f->value),log);
            if (!item) return false;
            if (!cfg_put(v,f->name,item)) { cfg_free(item); return cfg_error(log,"out of memory"); }
            if (f->kind != CFG_MAP && !strstr(f->value,"__control_socket__") && !strstr(f->value,"__api_socket__"))
                cfg_log(log,FIPSD_LOG_DEBUG,"applied default key=%s value=%s",key,f->value);
        }
        if (!normalize_value(item,f,key,log)) return false;
    }
    return !log || !log->error[0];
}
static bool normalize_value(struct fipsd_config_value *v, const cfg_field *f,
                             const char *path, fipsd_config_log *log)
{
    if (f->kind == CFG_MAP) return cfg_normalize(v,f->fields,path,log);
    if (f->kind == CFG_LIST) {
        if (v->kind != CFG_LIST) return cfg_error(log,"%s: invalid type, expected a sequence",path);
        for (size_t i = 0; i < v->count; ++i) {
            char key[4096];
            snprintf(key,sizeof(key),"%s[%zu]",path,i);
            if (!f->fields) continue;
            bool ok = f->fields[0].name && !*f->fields[0].name
                ? normalize_value(v->items[i],f->fields,key,log)
                : cfg_normalize(v->items[i],f->fields,key,log);
            if (!ok) return false;
        }
        return true;
    }
    if (f->kind == CFG_INSTANCES) {
        if (v->kind != CFG_MAP) return cfg_error(log,"%s: expected a transport or named transports",path);
        bool single = v->count == 0;
        for (size_t i = 0; i < v->count; ++i) {
            const cfg_field *known=field(f->fields,v->keys[i]);
            if (v->items[i]->kind != CFG_MAP || (known && known->kind==CFG_MAP)) single = true;
        }
        v->named_transport=!single;
        if (single) return cfg_normalize(v,f->fields,path,log);
        for (size_t i = 0; i < v->count; ++i) {
            char key[4096]; snprintf(key,sizeof(key),"%s.%s",path,v->keys[i]);
            if (!cfg_normalize(v->items[i],f->fields,key,log)) return false;
        }
        return true;
    }
    if (v->kind != CFG_RAW && v->kind != f->kind)
        return cfg_error(log,"%s: invalid type, expected a scalar",path);
    if (v->quoted && f->kind!=CFG_STRING)
        return cfg_error(log,"%s: invalid type, expected %s",path,
            f->kind==CFG_BOOL ? "a boolean" : f->kind==CFG_UINT ? "an unsigned integer" : "f64");
    const char *s = v->text;
    if (!s) return cfg_error(log,"%s: invalid null scalar",path);
    if (f->kind == CFG_STRING) {
        if (f->choices && !enum_ok(f->choices,s))
            return cfg_error(log,"%s: unknown variant; expected one of %s",path,f->choices);
    } else if (f->kind == CFG_BOOL) {
        if (!strcasecmp(s,"true")) v->u = 1;
        else if (!strcasecmp(s,"false")) v->u = 0;
        else return cfg_error(log,"%s: invalid type, expected a boolean",path);
    } else if (f->kind == CFG_UINT) {
        char *end;
        int base = s[0]=='0' && s[1]=='x' ? 16 : s[0]=='0' && s[1]=='o' ? 8 : 10;
        const char *start = base == 8 ? s+2 : s;
        errno = 0;
        unsigned long long n = strtoull(start,&end,base);
        if (errno || *end || end==start || *s=='-' || *s==' ' || n > f->ceiling)
            return cfg_error(log,"%s: invalid value, expected unsigned integer in 0..%llu",path,
                             (unsigned long long)f->ceiling);
        v->u = (uint64_t)n;
    } else if (f->kind == CFG_FLOAT) {
        char *end; errno = 0;
        if (!strcasecmp(s,".nan")) v->f = NAN;
        else if (!strcasecmp(s,".inf")) v->f = INFINITY;
        else if (!strcasecmp(s,"-.inf")) v->f = -INFINITY;
        else {
            v->f = strtod(s,&end);
            if (errno || *end || end==s) return cfg_error(log,"%s: invalid type, expected f64",path);
        }
    }
    v->kind = f->kind;
    return true;
}

static void remove_key(struct fipsd_config_value *map, const char *name)
{
    if (!map || map->kind != CFG_MAP) return;
    for (size_t i=0;i<map->count;++i) if (!strcmp(map->keys[i],name)) {
        free(map->keys[i]); cfg_free(map->items[i]); --map->count;
        memmove(map->keys+i,map->keys+i+1,(map->count-i)*sizeof(*map->keys));
        memmove(map->items+i,map->items+i+1,(map->count-i)*sizeof(*map->items)); return;
    }
}
static struct fipsd_config_value *ensure_map(struct fipsd_config_value *map, const char *name)
{
    struct fipsd_config_value *v = (struct fipsd_config_value *)fipsd_config_member(map,name);
    if (!v || v->kind != CFG_MAP) {
        v = cfg_new(CFG_MAP);
        if (!v) return NULL;
        if (!cfg_put(map,name,v)) { cfg_free(v); return NULL; }
    }
    return v;
}
static void ethernet_alias(struct fipsd_config_value *v, fipsd_config_log *log)
{
    struct fipsd_config_value *old=cfg_get(v,"discovery");
    if (!old) return;
    if (fipsd_config_member(v,"listen")) {
        cfg_error(log,"duplicate field `listen`");
        return;
    }
    struct fipsd_config_value *copy=cfg_clone(old);
    if (!copy || !cfg_put(v,"listen",copy)) { cfg_free(copy); cfg_error(log,"out of memory"); return; }
    remove_key(v,"discovery");
}
void cfg_deprecated(struct fipsd_config_value *root, fipsd_config_log *log)
{
    struct fipsd_config_value *ether=cfg_get(root,"transports.ethernet");
    if (ether && ether->kind==CFG_MAP) {
        if (cfg_get(ether,"interface")) ethernet_alias(ether,log);
        else for (size_t i=0;i<ether->count;++i)
            if (ether->items[i]->kind==CFG_MAP) ethernet_alias(ether->items[i],log);
    }
    struct fipsd_config_value *node = cfg_get(root,"node");
    struct fipsd_config_value *rekey = cfg_get(root,"node.rekey");
    struct fipsd_config_value *enabled = cfg_get(root,"node.rekey.enabled");
    if (enabled && enabled->kind != CFG_NULL) {
        const cfg_field spec=B("enabled",NULL);
        if (!normalize_value(enabled,&spec,"node.rekey.enabled",log)) return;
        bool yes = enabled->u!=0;
        cfg_log(log,FIPSD_LOG_WARN, "%s", yes ?
            "`node.rekey.enabled` is removed and ignored: rekey starts on the `node.rekey.after_secs` and `node.rekey.after_messages` thresholds. Delete the key from the config." :
            "`node.rekey.enabled: false` is removed and ignored: this node now starts rekeys of its own and answers its peers' rekeys. Delete the key from the config.");
        remove_key(rekey,"enabled");
    }
    struct fipsd_config_value *tick = cfg_get(root,"node.tick_interval_secs");
    if (tick && tick->kind != CFG_NULL) {
        const cfg_field spec=U("tick_interval_secs",NULL);
        if (!normalize_value(tick,&spec,"node.tick_interval_secs",log)) return;
        cfg_log(log,FIPSD_LOG_WARN,"`node.tick_interval_secs: %llu` is removed and ignored: the maintenance tick runs once a second. Delete the key from the config.",(unsigned long long)tick->u);
        remove_key(node,"tick_interval_secs");
    }
    struct fipsd_config_value *old = cfg_get(root,"node.discovery");
    if (old && old->kind != CFG_MAP) { cfg_error(log,"node.discovery: invalid type, expected struct"); return; }
    if (old && old->kind == CFG_MAP) {
        cfg_log(log,FIPSD_LOG_WARN,"`node.discovery.*` is deprecated and will be removed: mesh-lookup scalars moved to `node.lookup.*`, and peer-rendezvous keys moved to `node.rendezvous.nostr.*` / `node.rendezvous.lan.*`. Please migrate; a legacy `node.discovery` block still applies for now.");
        for (size_t i=0;i<old->count;++i) {
            const char *name=old->keys[i];
            struct fipsd_config_value *to=ensure_map(node,
                !strcmp(name,"nostr") || !strcmp(name,"lan") ? "rendezvous" : "lookup");
            struct fipsd_config_value *copy=cfg_clone(old->items[i]);
            if (!to || !copy || !cfg_put(to,name,copy)) { cfg_free(copy); cfg_error(log,"out of memory"); break; }
        }
        remove_key(node,"discovery");
    }
}
