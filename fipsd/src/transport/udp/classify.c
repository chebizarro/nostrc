#include "transport/udp/classify.h"
#include <string.h>
bool fipsd_udp_classifier_add(fipsd_udp_classifiers *r, const char *name, fipsd_udp_classifier fn, void *user)
{
    if (!r || !name || !*name || !fn || r->count == FIPSD_UDP_CLASSIFIERS) return false;
    for (size_t i = 0; i < r->count; ++i) if (!strcmp(r->entries[i].subsystem, name)) return false;
    r->entries[r->count++] = (fipsd_udp_classifier_entry){.subsystem = name, .classify = fn, .user = user};
    return true;
}
fipsd_udp_class fipsd_udp_classify(const fipsd_udp_classifiers *r, const uint8_t *p, size_t n)
{ return fipsd_udp_classify_from(r, p, n, NULL); }
bool fipsd_udp_datagram_classifier_add(fipsd_udp_classifiers *r, const char *name, fipsd_udp_datagram_classifier fn, void *user)
{
    if (!r || !name || !*name || !fn || r->count == FIPSD_UDP_CLASSIFIERS) return false;
    for (size_t i = 0; i < r->count; ++i) if (!strcmp(r->entries[i].subsystem, name)) return false;
    r->entries[r->count++] = (fipsd_udp_classifier_entry){.subsystem = name, .user = user, .datagram = fn};
    return true;
}
fipsd_udp_class fipsd_udp_classify_from(const fipsd_udp_classifiers *r, const uint8_t *p, size_t n, const fips_transport_addr *from)
{
    if (r) for (size_t i = 0; i < r->count; ++i)
        if ((r->entries[i].datagram ? r->entries[i].datagram(r->entries[i].user, p, n, from) :
             r->entries[i].classify(r->entries[i].user, p, n)) == FIPSD_UDP_DISCARD) return FIPSD_UDP_DISCARD;
    if (n >= 4 && (!memcmp(p, "NPTC", 4) || !memcmp(p, "NPTA", 4))) return FIPSD_UDP_DISCARD;
    return FIPSD_UDP_FMP;
}
