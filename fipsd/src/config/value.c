#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sodium.h>
#include <yaml.h>

struct fipsd_config_value *cfg_new(cfg_kind kind)
{
    struct fipsd_config_value *v = calloc(1, sizeof(*v));
    if (v) v->kind = kind;
    return v;
}
void cfg_free(struct fipsd_config_value *v)
{
    if (!v) return;
    if (v->text) { sodium_memzero(v->text, strlen(v->text)); free(v->text); }
    for (size_t i = 0; i < v->count; ++i) {
        cfg_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items); free(v->keys); free(v);
}
static bool append(struct fipsd_config_value *v, const char *key, struct fipsd_config_value *item)
{
    if (key) {
        char **keys = realloc(v->keys, (v->count + 1) * sizeof(*keys));
        if (!keys) return false;
        v->keys = keys;
    }
    struct fipsd_config_value **items = realloc(v->items, (v->count + 1) * sizeof(*items));
    if (!items) return false;
    v->items = items;
    char *copy = key ? strdup(key) : NULL;
    if (key && !copy) return false;
    if (key) v->keys[v->count] = copy;
    v->items[v->count++] = item;
    return true;
}
bool cfg_put(struct fipsd_config_value *v, const char *key, struct fipsd_config_value *item)
{
    for (size_t i = 0; i < v->count; ++i) {
        if (v->keys && !strcmp(v->keys[i], key)) {
            cfg_free(v->items[i]); v->items[i] = item; return true;
        }
    }
    return append(v, key, item);
}
struct fipsd_config_value *cfg_clone(const struct fipsd_config_value *v)
{
    if (!v) return NULL;
    struct fipsd_config_value *out = cfg_new(v->kind);
    if (!out) return NULL;
    out->u = v->u; out->f = v->f; out->quoted = v->quoted; out->named_transport = v->named_transport;
    if (v->text && !(out->text = strdup(v->text))) goto fail;
    for (size_t i = 0; i < v->count; ++i) {
        struct fipsd_config_value *item = cfg_clone(v->items[i]);
        if (!item || !append(out, v->keys ? v->keys[i] : NULL, item)) {
            cfg_free(item); goto fail;
        }
    }
    return out;
fail: cfg_free(out); return NULL;
}
const fipsd_config_value *fipsd_config_member(const fipsd_config_value *v, const char *key)
{
    if (!v || !key || !v->keys) return NULL;
    for (size_t i = 0; i < v->count; ++i)
        if (!strcmp(v->keys[i], key)) return v->items[i];
    return NULL;
}
const fipsd_config_value *fipsd_config_at(const fipsd_config_value *v, size_t i)
{ return v && i < v->count ? v->items[i] : NULL; }
size_t fipsd_config_count(const fipsd_config_value *v) { return v ? v->count : 0; }
const char *fipsd_config_key(const fipsd_config_value *v, size_t i)
{ return v && v->keys && i < v->count ? v->keys[i] : NULL; }
const char *fipsd_config_string(const fipsd_config_value *v)
{ return v && v->kind == CFG_STRING ? v->text : NULL; }
uint64_t fipsd_config_uint(const fipsd_config_value *v) { return v ? v->u : 0; }
double fipsd_config_number(const fipsd_config_value *v) { return v ? v->f : 0; }
bool fipsd_config_bool(const fipsd_config_value *v) { return v && v->u != 0; }
const fipsd_config_value *fipsd_config_get(const fipsd_config *c, const char *path)
{
    const fipsd_config_value *v = c ? c->root : NULL;
    while (v && path && *path) {
        const char *end = strchr(path, '.');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        if (n == 0 || n >= 256) return NULL;
        char key[256]; memcpy(key, path, n); key[n] = 0;
        if (v->kind == CFG_LIST) {
            char *stop; errno = 0;
            unsigned long i = strtoul(key, &stop, 10);
            v = !errno && !*stop && key[0] >= '0' && key[0] <= '9'
                ? fipsd_config_at(v, i) : NULL;
        } else v = fipsd_config_member(v, key);
        path = end ? end + 1 : NULL;
    }
    return v && v->kind != CFG_NULL ? v : NULL;
}
struct fipsd_config_value *cfg_get(struct fipsd_config_value *root, const char *path)
{
    fipsd_config c = {.root=root};
    return (struct fipsd_config_value *)fipsd_config_get(&c, path);
}
void cfg_set_uint(struct fipsd_config_value *v, uint64_t n)
{ v->u = n; v->kind = CFG_UINT; }

static struct fipsd_config_value *from_yaml(yaml_document_t *doc, yaml_node_t *node,
    unsigned depth, size_t *budget, fipsd_config_log *log)
{
    if (!node || depth > 64 || *budget == 0) {
        cfg_error(log, "YAML nesting or alias expansion limit exceeded"); return NULL;
    }
    --*budget;
    if (node->tag && strncmp((const char *)node->tag,"tag:yaml.org,2002:",18)) {
        cfg_error(log,"unsupported YAML tag"); return NULL;
    }
    struct fipsd_config_value *v = cfg_new(node->type == YAML_MAPPING_NODE ? CFG_MAP :
        node->type == YAML_SEQUENCE_NODE ? CFG_LIST : CFG_RAW);
    if (!v) return NULL;
    if (node->type == YAML_SCALAR_NODE) {
        v->quoted = node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE;
        size_t n = node->data.scalar.length;
        if (memchr(node->data.scalar.value, 0, n)) {
            cfg_error(log, "YAML scalar contains a NUL byte"); goto fail;
        }
        v->text = strdup((const char *)node->data.scalar.value);
        if (!v->text) goto fail;
        if (node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE &&
            (!n || !strcmp(v->text,"~") || !strcasecmp(v->text,"null"))) v->kind = CFG_NULL;
    } else if (node->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *i = node->data.sequence.items.start; i < node->data.sequence.items.top; ++i) {
            struct fipsd_config_value *item = from_yaml(doc, yaml_document_get_node(doc,*i),depth+1,budget,log);
            if (!item || !append(v, NULL, item)) { cfg_free(item); goto fail; }
        }
    } else if (node->type == YAML_MAPPING_NODE) {
        for (yaml_node_pair_t *p = node->data.mapping.pairs.start; p < node->data.mapping.pairs.top; ++p) {
            yaml_node_t *k = yaml_document_get_node(doc,p->key);
            if (!k || k->type != YAML_SCALAR_NODE || k->data.scalar.length > 255 ||
                memchr(k->data.scalar.value,0,k->data.scalar.length)) {
                cfg_error(log,"YAML mapping keys must be strings of at most 255 bytes"); goto fail;
            }
            const char *key = (const char *)k->data.scalar.value;
            if (fipsd_config_member(v,key)) {
                cfg_error(log,"duplicate field `%s`",key); goto fail;
            }
            struct fipsd_config_value *item = from_yaml(doc,yaml_document_get_node(doc,p->value),depth+1,budget,log);
            if (!item || !append(v,key,item)) { cfg_free(item); goto fail; }
        }
    } else { cfg_error(log,"unsupported YAML node"); goto fail; }
    return v;
fail: cfg_free(v); return NULL;
}
struct fipsd_config_value *cfg_yaml(const char *data, size_t len, fipsd_config_log *log)
{
    if (len > 8u*1024u*1024u) { cfg_error(log,"config exceeds 8 MiB limit"); return NULL; }
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) { cfg_error(log,"cannot allocate YAML parser"); return NULL; }
    yaml_parser_set_input_string(&parser,(const unsigned char *)data,len);
    yaml_document_t doc;
    struct fipsd_config_value *v = NULL;
    if (!yaml_parser_load(&parser,&doc)) {
        cfg_error(log,"YAML at line %zu column %zu: %s",parser.problem_mark.line+1,
                  parser.problem_mark.column+1,parser.problem ? parser.problem : "parse error");
    } else {
        size_t budget = 100000;
        yaml_node_t *root = yaml_document_get_root_node(&doc);
        v = root ? from_yaml(&doc,root,0,&budget,log) : cfg_new(CFG_MAP);
        /* libyaml holds plaintext scalars, including identity keys. */
        for (yaml_node_t *n = doc.nodes.start; n < doc.nodes.top; ++n)
            if (n->type == YAML_SCALAR_NODE) sodium_memzero(n->data.scalar.value,n->data.scalar.length);
        yaml_document_delete(&doc);
        if (v) {
            if (!yaml_parser_load(&parser,&doc)) {
                cfg_error(log,"YAML trailing document: %s",parser.problem ? parser.problem : "parse error");
                cfg_free(v); v = NULL;
            } else {
                if (yaml_document_get_root_node(&doc) || !parser.stream_end_produced) {
                    cfg_error(log,"deserializing from YAML containing more than one document is not supported");
                    cfg_free(v); v = NULL;
                }
                for (yaml_node_t *n = doc.nodes.start; n < doc.nodes.top; ++n)
                    if (n->type == YAML_SCALAR_NODE) sodium_memzero(n->data.scalar.value,n->data.scalar.length);
                yaml_document_delete(&doc);
            }
        }
    }
    yaml_parser_delete(&parser);
    if (!v) cfg_error(log,"cannot load YAML document");
    return v;
}
