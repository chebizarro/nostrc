#ifndef FIPSD_CLI_H
#define FIPSD_CLI_H

#include <stdbool.h>
#include <stdio.h>

/*
 * Command line per upstream docs/reference/cli-fips.md:
 *
 *   fipsd [-c FILE] [--log-file FILE]
 *   -c, --config FILE   configuration file (skips the default search paths)
 *       --log-file FILE log to FILE, rolled by size; opened before the
 *                       configuration loads
 *   -V                  short version: `fipsd <version> (rev <git-hash>)`
 *       --version       long version: short version plus target triple and
 *                       the linked libfips-core version
 *   -h, --help          usage
 *
 * Exit codes: 0 clean shutdown / help / version; 1 start-up failure;
 * 2 usage error (unknown flag, missing value, stray positional).
 * The Windows service flags are out of scope for this host.
 */

typedef enum fipsd_cli_action {
    FIPSD_CLI_RUN = 0,
    FIPSD_CLI_HELP,
    FIPSD_CLI_VERSION_SHORT,
    FIPSD_CLI_VERSION_LONG,
    FIPSD_CLI_USAGE_ERROR
} fipsd_cli_action;

/* Captured for the configuration loader (nostrc-9keoz.2); not parsed here.
 * Pointers alias argv and stay valid as long as it does. */
typedef struct fipsd_options {
    const char *config_path; /* NULL: default search paths */
    const char *log_file;    /* NULL: stderr */
} fipsd_options;

#define FIPSD_EXIT_OK 0
#define FIPSD_EXIT_FAILURE 1
#define FIPSD_EXIT_USAGE 2

/* Parses argv, which must be NULL-terminated as main()'s is (getopt_long
 * reads argv[argc]). Diagnostics for FIPSD_CLI_USAGE_ERROR go to `err`.
 * Resets getopt state, so it may be called repeatedly (tests). */
fipsd_cli_action fipsd_cli_parse(int argc, char **argv, fipsd_options *out, FILE *err);

void fipsd_cli_print_help(FILE *out);
void fipsd_cli_print_usage(FILE *out);
void fipsd_cli_print_version(FILE *out, bool long_form);

/* Short version text without a trailing newline, e.g. "0.1.0 (rev abc1234)". */
const char *fipsd_cli_short_version(void);

/* Exit status for a non-run action. */
int fipsd_cli_exit_code(fipsd_cli_action action);

#endif /* FIPSD_CLI_H */
