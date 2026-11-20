#ifndef FIPSD_DNS_MSG_H
#define FIPSD_DNS_MSG_H
/* DNS message screening and reply building for the `.fips` responder, the
 * port of upstream src/dnsmsg.rs (nostrc-9nxwk.3).
 *
 * Every datagram the responder receives runs through fipsd_dns_screen_datagram
 * before it is answered, and every reply is built with fipsd_dns_reply. The
 * screen decides whether a datagram is a query worth answering at all; the
 * reply builder writes a fresh header, the query's one question byte for
 * byte, and only the records the responder produced, so nothing an untrusted
 * sender put in its datagram is ever sent back beyond the question itself.
 *
 * The module is pure: it does no I/O, reads no clock and knows nothing of
 * `.fips`. The responder's receive loop logs and sends what it returns. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length of the fixed DNS message header. */
#define FIPSD_DNS_HEADER_LEN 12u
/* Receive buffer size of the responder. Bounds how much of a datagram it
 * reads; a datagram of this length or more may have been cut short by the
 * read (Linux truncates silently), so the screen drops it rather than judge a
 * prefix. An attacker gains nothing from a larger buffer: the reply to any
 * datagram is bounded by its question, not by its length. */
#define FIPSD_DNS_MAX_DATAGRAM 4096u
#define FIPSD_DNS_TYPE_SOA 6u
#define FIPSD_DNS_TYPE_AAAA 28u
#define FIPSD_DNS_TYPE_ANY 255u
#define FIPSD_DNS_CLASS_IN 1u
#define FIPSD_DNS_CLASS_ANY 255u
/* Header flags a caller of fipsd_dns_reply may ask for: AA and RA. */
#define FIPSD_DNS_AA 0x0400u
#define FIPSD_DNS_RA 0x0080u
/* Longest name on the wire, counting length bytes and the root (RFC 1035
 * Section 2.3.4). */
#define FIPSD_DNS_MAX_NAME 255u
/* Largest TTL a resolver honours (RFC 2181 Section 8). */
#define FIPSD_DNS_MAX_TTL 0x7FFFFFFFu
/* A question is at most 259 bytes, an AAAA record 28 and an SOA with two
 * whole 255-byte names 530, so every reply this module builds fits. */
#define FIPSD_DNS_MAX_RDATA 532u
#define FIPSD_DNS_MAX_REPLY 1024u

#define FIPSD_DNS_RCODE_NOERROR 0u
#define FIPSD_DNS_RCODE_FORMERR 1u
#define FIPSD_DNS_RCODE_SERVFAIL 2u
#define FIPSD_DNS_RCODE_NXDOMAIN 3u
#define FIPSD_DNS_RCODE_NOTIMP 4u
#define FIPSD_DNS_RCODE_REFUSED 5u

/* A configured TTL, clamped to FIPSD_DNS_MAX_TTL. */
uint32_t fipsd_dns_clamp_ttl(uint32_t ttl);
/* The code's mnemonic (RFC 6895 Section 2.3) for codes 0 to 10, written into
 * buf as RCODE<n> for any higher code; buf needs 8 bytes. */
const char *fipsd_dns_rcode_name(uint8_t rcode, char *buf, size_t cap);
/* The response code in a message's header, from the header's fourth byte. */
uint8_t fipsd_dns_rcode_from_header(uint8_t header_byte3);

/* Why the screen dropped a datagram. */
typedef enum fipsd_dns_drop_reason {
    FIPSD_DNS_DROP_SHORT,            /* shorter than a header */
    FIPSD_DNS_DROP_FILLS_BUFFER,     /* filled the receive buffer, so may be truncated */
    FIPSD_DNS_DROP_RESPONSE,         /* QR is set: a response, never answered */
    FIPSD_DNS_DROP_SERVICE_PORT,     /* sent from a port no resolver sends from */
    FIPSD_DNS_DROP_NOT_QUERY_SHAPED  /* its counts are not a query's */
} fipsd_dns_drop_reason;
const char *fipsd_dns_drop_reason_str(fipsd_dns_drop_reason reason);

/* A query that passed the screen. `question` borrows the datagram. */
typedef struct fipsd_dns_query {
    uint16_t id;
    uint16_t flags;                 /* a reply copies RD and CD from it */
    const uint8_t *question;        /* QNAME through QCLASS, exactly as received */
    size_t question_len;
    /* The question name in text form, identical to simple-dns's Name
     * display: labels joined by '.', no trailing dot, invalid UTF-8 bytes
     * shown as U+FFFD. A dot inside a label is not escaped, so this is for
     * logs and text lookups only; which zone the name is in is read from
     * its labels (fipsd_dns_query_next_label). */
    char name[4 * FIPSD_DNS_MAX_NAME];
    uint16_t qtype;
} fipsd_dns_query;
/* Iterate the question name's labels, each as its bytes on the wire: start
 * with *pos == 0; false once the root is reached. */
bool fipsd_dns_query_next_label(const fipsd_dns_query *q, size_t *pos, const uint8_t **label, size_t *len);
/* The question name as it appears on the wire (no pointers), with its length. */
const uint8_t *fipsd_dns_query_qname(const fipsd_dns_query *q, size_t *len);

typedef enum fipsd_dns_screen_kind {
    FIPSD_DNS_SCREEN_DROP,   /* send nothing */
    FIPSD_DNS_SCREEN_REPLY,  /* send this error reply, header only or with the question */
    FIPSD_DNS_SCREEN_QUERY   /* a well-formed query: answer it */
} fipsd_dns_screen_kind;
typedef struct fipsd_dns_screen {
    fipsd_dns_screen_kind kind;
    fipsd_dns_drop_reason reason;  /* DROP */
    uint8_t rcode;                 /* REPLY */
    size_t reply_len;              /* REPLY */
    uint8_t reply[FIPSD_DNS_MAX_REPLY];
    fipsd_dns_query query;         /* QUERY; borrows the datagram */
} fipsd_dns_screen;
/* Decide what to do with a datagram received from src_port. In order: a
 * datagram shorter than a header, or one that fills the receive buffer, is
 * dropped; a response is dropped; a datagram from a service port is dropped;
 * one whose counts are not a query's (more than one question, any answer or
 * authority record, more than one additional record) is dropped. Then an
 * opcode other than QUERY gets a header-only NOTIMP, a datagram with no
 * question a header-only FORMERR, a malformed question (a compression
 * pointer or extended label type, a label over 63 bytes, a name over 255
 * bytes, or one that runs past the datagram) a header-only FORMERR, and a
 * class other than IN or ANY a REFUSED carrying the question. Anything else
 * is a query. The Z bit is ignored, and the one additional record a query may
 * carry (normally an EDNS OPT) is neither parsed nor echoed. */
void fipsd_dns_screen_datagram(const uint8_t *datagram, size_t len, uint16_t src_port, fipsd_dns_screen *out);
/* The parse step alone, on a datagram whose header has passed the screen:
 * true and *out on success; false with the screen's error reply in *error
 * otherwise. */
bool fipsd_dns_parse_query(const uint8_t *datagram, size_t len, fipsd_dns_query *out, fipsd_dns_screen *error);

/* A resource record a responder adds to its reply. */
typedef struct fipsd_dns_record {
    uint16_t owner;  /* offset within the reply of the name the owner pointer names */
    uint16_t rtype;
    uint32_t ttl;
    size_t rdata_len;
    uint8_t rdata[FIPSD_DNS_MAX_RDATA];
} fipsd_dns_record;
/* An AAAA record for the question name. */
fipsd_dns_record fipsd_dns_aaaa(const uint8_t addr[16], uint32_t ttl);
/* An SOA record for a negative answer to `query` in `zone`, owned by the zone
 * apex (RFC 2308 Section 3): the point in the question name where its
 * remaining labels equal zone's, ignoring ASCII case; a question not in the
 * zone has its whole name as the owner. mname and rname are written as their
 * leading labels and a pointer to that apex when they end in zone and the
 * question is in it, otherwise whole. Refresh, retry, expire, minimum and the
 * record's TTL are all ttl. The names are constants with labels of at most
 * 63 bytes; a longer label is cut to 63. */
fipsd_dns_record fipsd_dns_soa(const fipsd_dns_query *query, const char *zone, const char *mname,
                               const char *rname, uint32_t serial, uint32_t ttl);
/* A one-question query: id, no flags, then the name, type and class. 0 when
 * it does not fit. */
size_t fipsd_dns_encode_query(uint16_t id, const uint8_t *qname_wire, size_t qname_len, uint16_t qtype,
                              uint16_t qclass, uint8_t *out, size_t cap);
/* Build a reply to query. The header carries the query's ID, QR set, opcode
 * QUERY, the query's RD and CD, the AA and RA bits the caller asks for in
 * flags (no others), and rcode. The query's one question follows byte for
 * byte, then answers and authority. No additional record is added, so TC is
 * never needed. Returns the length, 0 when it does not fit cap. */
size_t fipsd_dns_reply(const fipsd_dns_query *query, uint8_t rcode, uint16_t flags,
                       const fipsd_dns_record *answers, size_t n_answers,
                       const fipsd_dns_record *authority, size_t n_authority, uint8_t *out, size_t cap);
#endif
