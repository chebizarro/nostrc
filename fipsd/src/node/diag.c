#include "node/diag.h"
void fipsd_diag_init(fipsd_diag_budget *b, fips_instant now)
{
    *b = (fipsd_diag_budget){0};
    fips_token_bucket_init(&b->bucket, (fips_bucket_params){10, 1.0}, now);
}
bool fipsd_diag_admit(fipsd_diag_budget *b, fips_instant now, uint64_t *withheld)
{
    if (fips_token_bucket_acquire(&b->bucket, 1, now)) {
        *withheld = b->withheld; b->withheld = 0; return true;
    }
    if (b->withheld != UINT64_MAX) ++b->withheld;
    return false;
}
void fipsd_diag_hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15]; }
    out[2*n] = 0;
}
