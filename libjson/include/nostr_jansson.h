#ifndef NOSTR_JANSSON_H
#define NOSTR_JANSSON_H

#include "nostr-json-export.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr.h"

NOSTR_JSON_API extern NostrJsonInterface *jansson_impl;

#ifdef __cplusplus
}
#endif

#endif // NOSTR_JANSSON_H
