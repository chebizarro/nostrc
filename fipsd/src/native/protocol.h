#ifndef FIPSD_NATIVE_PROTOCOL_H
#define FIPSD_NATIVE_PROTOCOL_H
/* Native datagram API command types and the pure decisions taken over them
 * (upstream src/native/protocol.rs). No I/O and no node state: the shell in
 * api.c reads a line, calls fipsd_native_parse and acts on the typed command.
 * The encoding is the control socket's: one JSON object per line, shaped
 * {"command": "...", "params": {...}}, answered by a serde Response
 * (status, data, message in struct order; nested maps in lexical key order).
 * Byte-for-byte compatible with the pinned upstream daemon, so upstream's Rust
 * client speaks to fipsd unmodified. */
#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FSP port tiers (docs/reference/native-api.md, Ports). */
#define FIPSD_NATIVE_PORT_PROTOCOL_MAX 255u
#define FIPSD_NATIVE_PORT_STANDARD_MAX 1023u
#define FIPSD_NATIVE_PORT_EPHEMERAL_MIN 49152u
/* Compiled-in bounds (native-api.md, Compiled-in bounds). */
#define FIPSD_NATIVE_MAX_COMMAND 8192u
#define FIPSD_NATIVE_MAX_MESSAGE 512u
#define FIPSD_NATIVE_MAX_INJECT_REPEAT 64u
#define FIPSD_NATIVE_MAX_DATAGRAM 65535u

typedef enum fipsd_native_command_kind {
    FIPSD_NATIVE_CMD_CONNECT, FIPSD_NATIVE_CMD_LISTEN,
    FIPSD_NATIVE_CMD_STATS, FIPSD_NATIVE_CMD_INJECT, FIPSD_NATIVE_CMD_ARRIVE
} fipsd_native_command_kind;
typedef struct fipsd_native_command {
    fipsd_native_command_kind kind;
    char *peer;          /* connect/arrive: the npub as the client wrote it (owned) */
    bool has_local;      /* connect/listen: a named local port; absent, null and 0 mean ephemeral */
    bool stream;         /* connect/listen: `"framing":"stream"` (C-only, nostrc-6ljk8; absent, null and
                          * "datagram" mean the platform's datagram pair) */
    uint16_t local, remote, src, dst;
    uint64_t flow;       /* stats/inject */
    uint8_t *data;       /* inject/arrive: decoded hex (owned) */
    size_t data_len;
    uint32_t repeat;     /* inject: 1..FIPSD_NATIVE_MAX_INJECT_REPEAT */
} fipsd_native_command;
void fipsd_native_command_clear(fipsd_native_command *c);
/* Why a client's command was refused before any node state was touched. */
typedef enum fipsd_native_command_error {
    FIPSD_NATIVE_CMD_OK = 0, FIPSD_NATIVE_CMD_UNKNOWN, FIPSD_NATIVE_CMD_NO_PARAMS,
    FIPSD_NATIVE_CMD_BAD_PARAMS, FIPSD_NATIVE_CMD_PORT_PROTOCOL, FIPSD_NATIVE_CMD_PORT_STANDARD,
    FIPSD_NATIVE_CMD_BAD_HEX, FIPSD_NATIVE_CMD_BAD_REPEAT
} fipsd_native_command_error;
/* Turn a parsed request object into a typed command. On refusal `message`
 * carries upstream's prose for the operator (clipped by the reply builder) and
 * `out` holds nothing to clear. */
fipsd_native_command_error fipsd_native_parse(const json_t *request, fipsd_native_command *out,
                                              char *message, size_t cap);
/* The port tiers: 0-255 protocol use, 256-1023 FIPS standard services. */
fipsd_native_command_error fipsd_native_check_bindable(uint16_t port, char *message, size_t cap);
/* The errno name a command refusal reports: EADDRNOTAVAIL for a reserved
 * tier, EINVAL for everything malformed. */
const char *fipsd_native_command_errno(fipsd_native_command_error error);
/* The client's name for a debug command, NULL for connect/listen. */
const char *fipsd_native_debug_name(fipsd_native_command_kind kind);

/* Replies. fipsd_native_ok consumes data; fipsd_native_deny clips the
 * message to FIPSD_NATIVE_MAX_MESSAGE serialised bytes. Both return owned
 * objects shaped {"status","data"[,"message"]}. */
json_t *fipsd_native_ok(json_t *data);
json_t *fipsd_native_deny(const char *errno_name, const char *message);
/* Cut `message` so it serialises to at most FIPSD_NATIVE_MAX_MESSAGE bytes
 * (serde_json escape widths), marking a cut with U+2026. Returns the length
 * written; `out` is NUL-terminated. A message that fits is copied as is. */
size_t fipsd_native_clip(const char *message, char *out, size_t cap);
/* The bytes serde_json writes for one UTF-8 sequence inside a string. */
size_t fipsd_native_json_width(const unsigned char *s, size_t len, size_t *advance);
/* One reply line in serde Response order with the trailing newline
 * (status, then data, then message, each only when present; nested maps
 * sorted). Owned; NULL on allocation failure. */
char *fipsd_native_encode_reply(const json_t *response);
/* One JSON object, compact with sorted keys and no newline: the arrival
 * message, whose SOCK_SEQPACKET boundary is its framing. */
char *fipsd_native_encode_object(const json_t *object);
/* Bound container nesting before Jansson runs on a small fiber stack. */
bool fipsd_native_bounded_depth(const char *line, size_t len);

/* The command reader's rule over a buffer of bytes read so far: a complete
 * line is everything before the first newline; the cap applies whether or not
 * the newline has arrived, so a well formed 9000-byte line ends the connection
 * exactly as a runaway one does. Returns true with `*line_len` and
 * `*consumed` (line plus newline) when a line is complete; false otherwise,
 * with `*too_large` set when the partial or complete line exceeds the cap. */
bool fipsd_native_line_take(const uint8_t *buf, size_t len, size_t *line_len, size_t *consumed, bool *too_large);
#endif
