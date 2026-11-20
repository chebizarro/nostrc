#ifndef FIPS_PROTO_PATH_RULES_H
#define FIPS_PROTO_PATH_RULES_H
#include <fips/proto/path/set.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_path_class { FIPS_PATH_Clear, FIPS_PATH_Tor, FIPS_PATH_Nym } fips_path_class;
typedef enum fips_path_kind { FIPS_PATH_Unfixed=-1, FIPS_PATH_Local, FIPS_PATH_Internet } fips_path_kind;
typedef enum fips_path_role { FIPS_PATH_Never, FIPS_PATH_Backup, FIPS_PATH_Normal, FIPS_PATH_Preferred } fips_path_role;
typedef enum fips_path_source { FIPS_PATH_Config, FIPS_PATH_Nostr, FIPS_PATH_Discovery, FIPS_PATH_Heard, FIPS_PATH_Request } fips_path_source;
typedef enum fips_path_rule { FIPS_PATH_RuleNone, FIPS_PATH_Class, FIPS_PATH_Kind, FIPS_PATH_Role, FIPS_PATH_Accept, FIPS_PATH_Dial } fips_path_rule;
typedef struct fips_path_scope { fips_path_class class_; fips_path_kind kind; } fips_path_scope;
/* All callbacks required; facts are synchronous, side-effect-free host knowledge,
 * never IO. The ctx is borrowed for each call; a bundle does not retain facts. */
typedef struct fips_path_facts {
    void *ctx;
    size_t (*mtu)(void *,fips_transport_id,const fips_transport_addr *);
    fips_path_medium (*medium)(void *,fips_transport_id,const fips_transport_addr *);
    bool (*oriented)(void *,fips_transport_id);
    fips_path_class (*class_)(void *,fips_transport_id);
    fips_path_kind (*kind)(void *,fips_transport_id);
    fips_path_role (*role)(void *,fips_transport_id);
    bool (*accepts)(void *,fips_transport_id);
    bool (*dials)(void *,fips_transport_id);
} fips_path_facts;
typedef struct fips_path_env { const fips_path_facts *facts; double cost; } fips_path_env;
FIPS_CORE_API fips_path_kind fips_path_rules_scope(const fips_transport_addr *a);
FIPS_CORE_API fips_path_kind fips_path_rules_kind(const fips_path_facts *f,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API fips_path_scope fips_path_rules_link(const fips_path_facts *f,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API fips_path_rule fips_path_rules_moves(const fips_path_facts *f,fips_path_scope link,bool mix,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API fips_path_rule fips_path_rules_probe(const fips_path_facts *f,fips_path_scope link,bool mix,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API fips_path_rule fips_path_rules_admit(const fips_path_facts *f,fips_path_scope link,bool mix,fips_transport_id t,const fips_transport_addr *a);
#ifdef __cplusplus
}
#endif
#endif
