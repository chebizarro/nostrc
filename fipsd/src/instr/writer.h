#ifndef FIPSD_INSTR_WRITER_H
#define FIPSD_INSTR_WRITER_H
#ifdef FIPSD_ENABLE_PROFILING
#include <stdio.h>
#include <stdint.h>
#include <jansson.h>
#define FIPSD_PROFILE_INTERVAL 10u
#define FIPSD_PROFILE_CAP (32u * 1024u * 1024u)
/* Pure formatting and draining seams used by golden tests, no thread required. */
int fipsd_instr_write_header(FILE *file, const char *node, uint64_t period, uint64_t unix_secs);
int fipsd_instr_flush(FILE *file, uint64_t unix_secs);
/* NULL means not a profiler command; otherwise owns the response envelope. */
json_t *fipsd_instr_command(const char *name, const json_t *params, const char *node, uint64_t period);
void fipsd_instr_shutdown(void);
#define FIPSD_INSTR_SHUTDOWN() fipsd_instr_shutdown()
#else
#define FIPSD_INSTR_SHUTDOWN()
#endif
#endif
