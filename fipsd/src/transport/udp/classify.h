#ifndef FIPSD_UDP_CLASSIFY_H
#define FIPSD_UDP_CLASSIFY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <fips/transport/types.h>
#define FIPSD_UDP_CLASSIFIERS 8u
typedef enum fipsd_udp_class { FIPSD_UDP_FMP, FIPSD_UDP_DISCARD } fipsd_udp_class;
typedef fipsd_udp_class (*fipsd_udp_classifier)(void *, const uint8_t *, size_t);
/* Receive metadata is borrowed only during the callback. Callbacks must not
 * block the UDP drain; queue bounded copies for the owning subsystem. */
typedef fipsd_udp_class (*fipsd_udp_datagram_classifier)(void *, const uint8_t *, size_t, const fips_transport_addr *);
typedef struct fipsd_udp_classifier_entry {
    const char *subsystem;
    fipsd_udp_classifier classify;
    void *user;
    fipsd_udp_datagram_classifier datagram;
} fipsd_udp_classifier_entry;
typedef struct fipsd_udp_classifiers {
    fipsd_udp_classifier_entry entries[FIPSD_UDP_CLASSIFIERS];
    size_t count;
} fipsd_udp_classifiers;
/* Registration before start only; names/users borrowed through runtime join.
 * Any subsystem may consume a datagram. Reserved punch prefixes are always
 * discarded before FMP, including when no rendezvous subsystem is running. */
bool fipsd_udp_classifier_add(fipsd_udp_classifiers *, const char *, fipsd_udp_classifier, void *);
fipsd_udp_class fipsd_udp_classify(const fipsd_udp_classifiers *, const uint8_t *, size_t);
bool fipsd_udp_datagram_classifier_add(fipsd_udp_classifiers *, const char *, fipsd_udp_datagram_classifier, void *);
fipsd_udp_class fipsd_udp_classify_from(const fipsd_udp_classifiers *, const uint8_t *, size_t, const fips_transport_addr *);
#endif
