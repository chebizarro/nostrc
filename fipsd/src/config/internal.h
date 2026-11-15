#ifndef FIPSD_CONFIG_INTERNAL_H
#define FIPSD_CONFIG_INTERNAL_H
#include "config.h"
#include <stdarg.h>
typedef enum cfg_kind { CFG_NULL, CFG_RAW, CFG_STRING, CFG_UINT, CFG_FLOAT,
    CFG_BOOL, CFG_MAP, CFG_LIST, CFG_INSTANCES } cfg_kind;
struct fipsd_config_value {
    cfg_kind kind;
    char *text;
    bool quoted, named_transport;
    uint64_t u;
    double f;
    struct fipsd_config_value **items;
    char **keys;
    size_t count;
};
typedef struct cfg_field {
    const char *name;
    cfg_kind kind;
    const char *value; /* default YAML, NULL = optional; "!" = required */
    uint64_t ceiling;
    const struct cfg_field *fields;
    const char *choices; /* | separated enum */
} cfg_field;
struct fipsd_config { struct fipsd_config_value *root; fipsd_config_core core; };
struct fipsd_config_log_entry { fipsd_log_level level; char *message; };
#define END {NULL, CFG_NULL, NULL, 0, NULL, NULL}
#define U(n,d) {n,CFG_UINT,d,UINT64_MAX,NULL,NULL}
#define U8(n,d) {n,CFG_UINT,d,UINT8_MAX,NULL,NULL}
#define U16(n,d) {n,CFG_UINT,d,UINT16_MAX,NULL,NULL}
#define U32(n,d) {n,CFG_UINT,d,UINT32_MAX,NULL,NULL}
#define Z(n,d) {n,CFG_UINT,d,SIZE_MAX,NULL,NULL}
#define F(n,d) {n,CFG_FLOAT,d,0,NULL,NULL}
#define B(n,d) {n,CFG_BOOL,d,0,NULL,NULL}
#define S(n,d) {n,CFG_STRING,d,0,NULL,NULL}
#define E(n,d,c) {n,CFG_STRING,d,0,NULL,c}
#define M(n,f) {n,CFG_MAP,"{}",0,f,NULL}
#define O(n,f) {n,CFG_MAP,NULL,0,f,NULL}
#define A(n,f) {n,CFG_LIST,"[]",0,f,NULL}
extern const cfg_field cfg_node[], cfg_transports[], cfg_peers[], cfg_gateway[];
extern const cfg_field cfg_root[];
void cfg_log(fipsd_config_log *log, fipsd_log_level level, const char *fmt, ...)
    __attribute__((format(printf,3,4)));
bool cfg_error(fipsd_config_log *log, const char *fmt, ...)
    __attribute__((format(printf,2,3)));
struct fipsd_config_value *cfg_new(cfg_kind kind);
void cfg_free(struct fipsd_config_value *v);
struct fipsd_config_value *cfg_clone(const struct fipsd_config_value *v);
bool cfg_put(struct fipsd_config_value *map, const char *key, struct fipsd_config_value *v);
struct fipsd_config_value *cfg_get(struct fipsd_config_value *root, const char *path);
bool cfg_normalize(struct fipsd_config_value *v, const cfg_field *fields,
                   const char *path, fipsd_config_log *log);
struct fipsd_config_value *cfg_yaml(const char *data, size_t len, fipsd_config_log *log);
void cfg_floors(fipsd_config *config, fipsd_config_log *log);
bool cfg_validate(fipsd_config *config, fipsd_config_log *log);
void cfg_deprecated(struct fipsd_config_value *root, fipsd_config_log *log);
void cfg_set_uint(struct fipsd_config_value *v, uint64_t n);
bool cfg_core(fipsd_config *config, fipsd_config_log *log);
bool cfg_socket_defaults(fipsd_config *config, fipsd_config_log *log);
#endif
