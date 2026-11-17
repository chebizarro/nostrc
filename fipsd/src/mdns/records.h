#ifndef FIPSD_MDNS_RECORDS_H
#define FIPSD_MDNS_RECORDS_H
/* Multicast DNS message codec (RFC 6762 over RFC 1035), the wire layer of the
 * LAN rendezvous port (nostrc-9nxwk.5; upstream src/mdns uses the mdns-sd
 * crate for this).
 *
 * The `.fips` responder's codec (dns/msg.h) screens unicast queries and
 * deliberately refuses compression pointers; mDNS responses use compression
 * everywhere, so this module decodes whole messages with pointer support and
 * encodes the five record types LAN rendezvous needs: PTR, SRV, TXT, A and
 * AAAA. It shares dns/msg.h's name bound, class and type constants. Names are
 * kept in uncompressed wire form and compared without regard to ASCII case
 * (RFC 1035 Section 2.3.3); the top bit of a record's class is the
 * cache-flush bit (RFC 6762 Section 10.2) and of a question's class the
 * unicast-response (QU) bit (Section 5.4).
 *
 * Pure: no I/O, no clock, no allocation. */
#include "dns/msg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_MDNS_PORT 5353u
/* Largest datagram read off the wire (RFC 6762 Section 17: 9000 bytes). */
#define FIPSD_MDNS_MAX_DATAGRAM 9000u
/* Packets this node builds stay within an Ethernet MTU without IP options. */
#define FIPSD_MDNS_MAX_PACKET 1440u
#define FIPSD_MDNS_TYPE_A 1u
#define FIPSD_MDNS_TYPE_PTR 12u
#define FIPSD_MDNS_TYPE_TXT 16u
#define FIPSD_MDNS_TYPE_AAAA FIPSD_DNS_TYPE_AAAA
#define FIPSD_MDNS_TYPE_SRV 33u
#define FIPSD_MDNS_TYPE_ANY FIPSD_DNS_TYPE_ANY
#define FIPSD_MDNS_CLASS_IN FIPSD_DNS_CLASS_IN
/* Record class top bit: cache flush. Question class top bit: QU. */
#define FIPSD_MDNS_CLASS_FLAG 0x8000u
/* Header flags: QR (a response) and AA (every mDNS response sets it). */
#define FIPSD_MDNS_FLAG_QR 0x8000u
#define FIPSD_MDNS_FLAG_AA FIPSD_DNS_AA
/* Canonical rdata bound: a TXT record of a FIPS advert is under 120 bytes;
 * longer records from other services are kept up to this length. */
#define FIPSD_MDNS_MAX_RDATA 1300u
#define FIPSD_MDNS_MAX_QUESTIONS 16u
#define FIPSD_MDNS_MAX_RECORDS 48u

/* A domain name in uncompressed wire form, root label included. */
typedef struct fipsd_mdns_name {
    uint8_t len;
    uint8_t wire[FIPSD_DNS_MAX_NAME];
} fipsd_mdns_name;
/* "a.b.local." or "a.b.local": labels of 1 to 63 bytes, no escapes. */
bool fipsd_mdns_name_from_text(const char *text, fipsd_mdns_name *out);
/* Labels joined by '.', with the trailing dot mdns-sd prints. Returns the
 * text length; 0 when it does not fit. Non-printable bytes print as '?'. */
size_t fipsd_mdns_name_to_text(const fipsd_mdns_name *name, char *out, size_t cap);
bool fipsd_mdns_name_equal(const fipsd_mdns_name *a, const fipsd_mdns_name *b);
/* One label followed by suffix's labels; false when it is not a label or the
 * result is longer than a name may be. */
bool fipsd_mdns_name_prepend(const char *label, const fipsd_mdns_name *suffix, fipsd_mdns_name *out);
/* Whether name is one label followed by exactly suffix; the label is copied
 * to label (63 bytes and a NUL) when it is. */
bool fipsd_mdns_name_child_of(const fipsd_mdns_name *name, const fipsd_mdns_name *suffix, char label[64]);

typedef struct fipsd_mdns_question {
    fipsd_mdns_name name;
    uint16_t qtype, qclass;  /* qclass without the QU bit */
    bool unicast;            /* the QU bit */
} fipsd_mdns_question;

/* A resource record with its rdata in canonical form: names inside PTR and
 * SRV rdata are decompressed, so two records compare by their bytes. */
typedef struct fipsd_mdns_rr {
    fipsd_mdns_name name;
    uint16_t rtype, rclass;  /* rclass without the cache-flush bit */
    bool cache_flush;
    uint32_t ttl;
    uint16_t rdlen;
    uint8_t rdata[FIPSD_MDNS_MAX_RDATA];
} fipsd_mdns_rr;

typedef struct fipsd_mdns_message {
    uint16_t id, flags;
    size_t qdcount, ancount, nscount, arcount;  /* entries kept per section */
    bool truncated;  /* a section held more than this struct keeps */
    fipsd_mdns_question questions[FIPSD_MDNS_MAX_QUESTIONS];
    /* answers, then authority, then additional records, in wire order */
    fipsd_mdns_rr records[FIPSD_MDNS_MAX_RECORDS];
} fipsd_mdns_message;
static inline bool fipsd_mdns_is_response(const fipsd_mdns_message *m) { return (m->flags & FIPSD_MDNS_FLAG_QR) != 0; }
static inline const fipsd_mdns_rr *fipsd_mdns_answers(const fipsd_mdns_message *m) { return m->records; }
static inline const fipsd_mdns_rr *fipsd_mdns_authority(const fipsd_mdns_message *m) { return m->records + m->ancount; }
static inline const fipsd_mdns_rr *fipsd_mdns_additional(const fipsd_mdns_message *m) { return m->records + m->ancount + m->nscount; }
static inline size_t fipsd_mdns_record_count(const fipsd_mdns_message *m) { return m->ancount + m->nscount + m->arcount; }

/* Decode a whole message. False when it is malformed: shorter than a header,
 * a name that loops, points forward, uses an extended label type or runs
 * past the datagram or past 255 bytes, an rdata that runs past the datagram
 * or (for PTR, SRV, A, AAAA) does not have its type's shape. Entries past the
 * struct's bounds are skipped and set `truncated`. */
bool fipsd_mdns_decode(const uint8_t *buf, size_t len, fipsd_mdns_message *out);

/* Record builders. The owner and target are copied. */
void fipsd_mdns_rr_ptr(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const fipsd_mdns_name *target, uint32_t ttl);
void fipsd_mdns_rr_srv(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, uint16_t port, const fipsd_mdns_name *target,
                       uint32_t ttl, bool flush);
void fipsd_mdns_rr_a(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const uint8_t addr[4], uint32_t ttl, bool flush);
void fipsd_mdns_rr_aaaa(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const uint8_t addr[16], uint32_t ttl, bool flush);
/* TXT from key=value strings (RFC 6763 Section 6). A NULL value writes the
 * bare key. False when an entry is longer than 255 bytes or the record would
 * exceed FIPSD_MDNS_MAX_RDATA. No entries: the single empty string. */
typedef struct fipsd_mdns_txt_entry { const char *key, *value; } fipsd_mdns_txt_entry;
bool fipsd_mdns_rr_txt(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const fipsd_mdns_txt_entry *entries, size_t n,
                       uint32_t ttl, bool flush);
/* Iterate TXT strings: start with *pos == 0; false at the end or on a string
 * that runs past the rdata. key/klen is the text before the first '=';
 * has_value says whether there was one. */
bool fipsd_mdns_txt_next(const uint8_t *rdata, size_t rdlen, size_t *pos, const uint8_t **key, size_t *klen,
                         const uint8_t **value, size_t *vlen, bool *has_value);
/* SRV fields from canonical rdata. */
bool fipsd_mdns_srv_fields(const fipsd_mdns_rr *rr, uint16_t *port, fipsd_mdns_name *target);
/* PTR target from canonical rdata. */
bool fipsd_mdns_ptr_target(const fipsd_mdns_rr *rr, fipsd_mdns_name *target);
/* Same name, type, class and rdata; TTL and cache-flush are not compared. */
bool fipsd_mdns_rr_same(const fipsd_mdns_rr *a, const fipsd_mdns_rr *b);
/* RFC 6762 Section 8.2 order of two records: class, then type, then rdata
 * bytes; <0, 0 or >0. */
int fipsd_mdns_rr_compare(const fipsd_mdns_rr *a, const fipsd_mdns_rr *b);

/* Message writer. Sections are written in order: questions, answers,
 * authority, additional. Names are written uncompressed. */
typedef enum { FIPSD_MDNS_ANSWER = 1, FIPSD_MDNS_AUTHORITY = 2, FIPSD_MDNS_ADDITIONAL = 3 } fipsd_mdns_section;
typedef struct fipsd_mdns_writer {
    uint8_t *buf;
    size_t cap, len;
    uint16_t counts[4];  /* qd, an, ns, ar */
    int section;         /* the last section written to: 0 questions .. 3 additional */
    bool overflow;
} fipsd_mdns_writer;
void fipsd_mdns_writer_init(fipsd_mdns_writer *w, uint8_t *buf, size_t cap, uint16_t id, uint16_t flags);
bool fipsd_mdns_put_question(fipsd_mdns_writer *w, const fipsd_mdns_name *name, uint16_t qtype, bool unicast);
/* ttl_cap > 0 caps the TTL written (legacy unicast replies); no_flush clears
 * the cache-flush bit (also for legacy unicast, RFC 6762 Section 6.7). */
bool fipsd_mdns_put_rr(fipsd_mdns_writer *w, fipsd_mdns_section section, const fipsd_mdns_rr *rr,
                       uint32_t ttl_cap, bool no_flush);
/* The message length with its counts patched in; 0 after an overflow or an
 * out-of-order section. */
size_t fipsd_mdns_writer_finish(fipsd_mdns_writer *w);
#endif
