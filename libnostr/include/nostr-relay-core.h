#ifndef NOSTR_RELAY_CORE_H
#define NOSTR_RELAY_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Basic wire helpers for OK/CLOSED/EOSE formatting */

/* Build an OK message: ["OK", <id>, <ok>, <reason>] -> JSON string */
char *nostr_ok_build_json(const char *event_id_hex, int ok, const char *reason);

/* Build a CLOSED message: ["CLOSED", <sub>, <reason>] */
char *nostr_closed_build_json(const char *sub_id, const char *reason);

/* Build an EOSE message: ["EOSE", <sub>] */
char *nostr_eose_build_json(const char *sub_id);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_RELAY_CORE_H */
