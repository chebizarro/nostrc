#ifndef FIPS_PROTO_SETTLE_PARITY_H
#define FIPS_PROTO_SETTLE_PARITY_H
#include <fips/proto/path/params.h>
#include <fips/proto/rekey/params.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Test-only, allocator/IO-free parity vocabulary and trace reader. Inputs and
 * outputs do not alias. Text is NUL-free ASCII (unlike Rust's Unicode whitespace), script
 * names are at most95 bytes; each trace holds8 settings,128 lines,64 core lines.
 * RANGE preserves result arrays/counts/values; scan diagnostics are bit masks
 * rather than allocated Rust error strings. No default-library symbols. */
#define FIPS_PARITY_NAME_MAX 96u
#define FIPS_PARITY_NODES_MAX 8u
#define FIPS_PARITY_LINES_MAX 128u
#define FIPS_PARITY_CORES_MAX 64u
typedef enum fips_parity_transport { FIPS_PARITY_UDP,FIPS_PARITY_TCP,FIPS_PARITY_BLE } fips_parity_transport;
typedef enum fips_parity_step_kind { FIPS_PARITY_DIAL,FIPS_PARITY_CONNECTED,FIPS_PARITY_HANDSHAKE,FIPS_PARITY_HEAL,FIPS_PARITY_ADVANCE,FIPS_PARITY_RESTART,FIPS_PARITY_REKEY } fips_parity_step_kind;
typedef struct fips_parity_step { fips_parity_step_kind kind; size_t from,to; fips_parity_transport transport; bool msg2; uint32_t nth; uint64_t millis; } fips_parity_step;
typedef struct fips_parity_script { const char *name; const bool *settings; size_t nodes,smaller; const fips_parity_transport *transports; size_t transport_count; const fips_parity_step *steps; size_t step_count; } fips_parity_script;
typedef enum fips_parity_at { FIPS_PARITY_HOME,FIPS_PARITY_SECOND,FIPS_PARITY_ACCEPTED } fips_parity_at;
typedef struct fips_parity_place { fips_parity_transport transport; fips_parity_at at; } fips_parity_place;
typedef struct fips_parity_mark { bool held,own,out,heard; fips_parity_place setup,active; } fips_parity_mark;
typedef enum fips_parity_field { FIPS_PARITY_HELD,FIPS_PARITY_SESSION,FIPS_PARITY_DIR,FIPS_PARITY_SETUP,FIPS_PARITY_ACTIVE,FIPS_PARITY_HEARD } fips_parity_field;
typedef struct fips_parity_value { fips_parity_field field; bool flag; fips_parity_place place; } fips_parity_value;
typedef enum fips_parity_cause { FIPS_PARITY_LATE_DIAL_SPLIT,FIPS_PARITY_AGED_PENDING } fips_parity_cause;
typedef struct fips_parity_line { size_t step,node; fips_parity_mark mark; } fips_parity_line;
typedef struct fips_parity_diff { size_t step,node; fips_parity_value values[6]; size_t count; fips_parity_cause cause; } fips_parity_diff;
typedef struct fips_parity_trace { char name[FIPS_PARITY_NAME_MAX]; bool settings[FIPS_PARITY_NODES_MAX]; size_t nodes; fips_parity_line lines[FIPS_PARITY_LINES_MAX]; size_t line_count; fips_parity_diff core[FIPS_PARITY_CORES_MAX]; size_t core_count; } fips_parity_trace;
typedef enum fips_parity_issue {
    FIPS_PARITY_SMALLER=1u<<0,FIPS_PARITY_TRANSPORT=1u<<1,FIPS_PARITY_SELF=1u<<2,FIPS_PARITY_NODE=1u<<3,
    FIPS_PARITY_DUPLICATE_BLOCK=1u<<4,FIPS_PARITY_NO_SCRIPT=1u<<5,FIPS_PARITY_NO_LINES=1u<<6,FIPS_PARITY_SETTINGS=1u<<7,
    FIPS_PARITY_LINE_COUNT=1u<<8,FIPS_PARITY_PAST_SCRIPT=1u<<9,FIPS_PARITY_CORE_NO_LINE=1u<<10,
    FIPS_PARITY_RESTATED=1u<<11,FIPS_PARITY_INCOMPLETE=1u<<12,FIPS_PARITY_INVALID=1u<<13
} fips_parity_issue;
#ifdef FIPS_TEST_HOOKS
FIPS_CORE_API const fips_parity_script *fips_settle_parity_scripts(size_t *count);
FIPS_CORE_API const char *const (*fips_settle_parity_unshared(size_t *count))[2];
FIPS_CORE_API fips_status fips_settle_parity_path(bool on,fips_path_params *out);
FIPS_CORE_API fips_status fips_settle_parity_rekey(bool on,fips_rekey_params *out);
FIPS_CORE_API fips_status fips_settle_parity_mark(const char *text,size_t length,fips_parity_mark *out);
FIPS_CORE_API fips_status fips_settle_parity_line(const char *text,size_t length,fips_parity_line *out);
FIPS_CORE_API fips_status fips_settle_parity_diff(const char *text,size_t length,fips_parity_diff *out);
FIPS_CORE_API fips_status fips_settle_parity_read(const char *text,size_t length,fips_parity_trace *out,size_t capacity,size_t *count);
FIPS_CORE_API fips_status fips_settle_parity_format_line(const fips_parity_line *line,char *out,size_t capacity,size_t *length);
FIPS_CORE_API fips_status fips_settle_parity_format_diff(const fips_parity_diff *diff,char *out,size_t capacity,size_t *length);
FIPS_CORE_API const fips_parity_trace *fips_settle_parity_expected(const fips_parity_trace *traces,size_t count,const char *name);
FIPS_CORE_API const fips_parity_mark *fips_settle_parity_at(const fips_parity_trace *trace,size_t step,size_t node);
FIPS_CORE_API fips_status fips_settle_parity_cored(const fips_parity_trace *trace,fips_parity_line *out,size_t capacity,size_t *count);
FIPS_CORE_API uint32_t fips_settle_parity_strays(const fips_parity_script *script);
FIPS_CORE_API uint32_t fips_settle_parity_unmatched(const fips_parity_script *scripts,size_t count,const fips_parity_trace *traces,size_t trace_count);
#endif
#ifdef __cplusplus
}
#endif
#endif
