#ifndef FIPSD_NODE_DIAG_H
#define FIPSD_NODE_DIAG_H
#include <fips/node/rate_limit.h>
#include <stddef.h>
#include <stdint.h>
typedef struct fipsd_diag_budget { fips_token_bucket bucket; uint64_t withheld; } fipsd_diag_budget;
void fipsd_diag_init(fipsd_diag_budget *, fips_instant);
bool fipsd_diag_admit(fipsd_diag_budget *, fips_instant, uint64_t *withheld);
/* Lowercase, unseparated hex; caller supplies 2*n+1 bytes. */
void fipsd_diag_hex(const uint8_t *, size_t, char *);
#endif
