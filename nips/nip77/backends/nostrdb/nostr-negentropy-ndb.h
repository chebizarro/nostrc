#ifndef NOSTR_NIP77_NDB_H
#define NOSTR_NIP77_NDB_H
#include "../../include/nostr/nip77/negentropy.h"

/* Create a NostrNegDataSource backed by nostrdb (opaque ctx inside). */
int nostr_ndb_make_datasource(const char *db_path, NostrNegDataSource *out);

#endif
