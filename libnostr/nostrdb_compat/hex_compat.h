#ifndef NOSTRDB_HEX_COMPAT
#define NOSTRDB_HEX_COMPAT
#include "hex.h"
#ifndef hex_str_size
#define hex_str_size(len) ((size_t)((len) * 2 + 1))
#endif
/* Preserve original 3-arg function under a new name, then macro-dispatch. */
static inline int nostrdb_hex_encode3(const void *buf, size_t bufsize, char *dest) { return hex_encode(buf, bufsize, dest); }
#undef hex_encode
#define __NDB_HEX_DISPATCH(_1,_2,_3,_4,NAME,...) NAME
#define nostrdb_hex_encode4(_buf,_len,_dest,_dsize) nostrdb_hex_encode3(_buf,_len,_dest)
#define hex_encode(...) __NDB_HEX_DISPATCH(__VA_ARGS__, nostrdb_hex_encode4, nostrdb_hex_encode3)(__VA_ARGS__)
#endif
