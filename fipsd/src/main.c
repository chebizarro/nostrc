#define _POSIX_C_SOURCE 200809L
#include <fips/core.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "cli.h"
#include "config/config.h"
#include "host_os.h"
#include "log.h"
#include "privilege.h"
#include "runtime.h"

#define MODULE "main"

/* Open --log-file before parsing so startup failures reach the requested sink. */
static fipsd_log *open_logger(const fipsd_options *opts, fipsd_log_file **out_file)
{
    fipsd_log_level level = FIPSD_LOG_INFO;
    const char *env_level = getenv("FIPSD_LOG_LEVEL");
    if (env_level && *env_level && !fipsd_log_level_parse(env_level, &level)) {
        fprintf(stderr, "fipsd: ignoring FIPSD_LOG_LEVEL=%s: expected error, warn, info, debug or trace\n",
                env_level);
        level = FIPSD_LOG_INFO;
    }
    fipsd_log *log;
    *out_file = NULL;
    if (opts->log_file) {
        char err[512];
        fipsd_log_file *file = fipsd_log_file_open(opts->log_file, FIPSD_LOG_DEFAULT_MAX_BYTES,
                                                   FIPSD_LOG_DEFAULT_MAX_FILES, err, sizeof(err));
        if (!file) {
            /* Fatal: a daemon that silently ran without the log it was told
             * to keep would present as exactly the missing-logs problem the
             * file exists to solve. */
            fprintf(stderr, "fipsd: %s\n", err);
            return NULL;
        }
        fipsd_log_sink sink = fipsd_log_file_sink(file);
        log = fipsd_log_create(level, &sink);
        if (!log) {
            fipsd_log_file_close(file);
            fputs("fipsd: cannot allocate the logger\n", stderr);
            return NULL;
        }
        *out_file = file;
        fipsd_log_set_default(log);
        /* Logged first: an operator looking for output that is no longer on
         * stderr needs the path and the limits that govern it. */
        FIPSD_INFO(MODULE, "logging to file %s (max %" PRIu64 " bytes, %u rolled files kept)",
                   fipsd_log_file_path(file), fipsd_log_file_max_bytes(file),
                   fipsd_log_file_max_files(file));
    } else {
        log = fipsd_log_create(level, NULL);
        if (!log) {
            fputs("fipsd: cannot allocate the logger\n", stderr);
            return NULL;
        }
        fipsd_log_set_default(log);
    }
    return log;
}

int main(int argc, char **argv)
{
    fipsd_options opts;
    fipsd_cli_action action = fipsd_cli_parse(argc, argv, &opts, stderr);
    switch (action) {
    case FIPSD_CLI_HELP:
        fipsd_cli_print_help(stdout);
        return fipsd_cli_exit_code(action);
    case FIPSD_CLI_VERSION_SHORT:
    case FIPSD_CLI_VERSION_LONG:
        fipsd_cli_print_version(stdout, action == FIPSD_CLI_VERSION_LONG);
        return fipsd_cli_exit_code(action);
    case FIPSD_CLI_USAGE_ERROR:
        return fipsd_cli_exit_code(action);
    case FIPSD_CLI_RUN:
        break;
    }

    /* The log file is opened before anything else so that every later
     * start-up error, including a configuration error, is logged there. */
    fipsd_log_file *log_file = NULL;
    fipsd_log *log = open_logger(&opts, &log_file);
    if (!log)
        return FIPSD_EXIT_FAILURE;
    FIPSD_INFO(MODULE, "fipsd %s starting (libfips-core %s)", fipsd_cli_short_version(),
               fips_core_version());

    mode_t previous_umask = fipsd_privilege_set_umask();
    FIPSD_DEBUG(MODULE, "umask %04o (was %04o)", (unsigned)FIPSD_UMASK, (unsigned)previous_umask);

    fipsd_config_log held = {0};
    fipsd_config *config = fipsd_config_load(opts.config_path, &held);
    if (!config) {
        fipsd_config_log_replay(&held, log);
        FIPSD_ERROR(MODULE, "Failed to load configuration%s%s: %s",
                    opts.config_path ? " from " : "", opts.config_path ? opts.config_path : "", held.error);
        fipsd_config_log_clear(&held);
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
        return FIPSD_EXIT_FAILURE;
    }
    const char *configured_file = fipsd_config_string(fipsd_config_get(config, "node.log_file"));
    if (!opts.log_file && configured_file) {
        fipsd_options configured_opts = opts;
        configured_opts.log_file = configured_file;
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
        log = open_logger(&configured_opts, &log_file);
        if (!log) {
            fipsd_config_free(config);
            fipsd_config_log_clear(&held);
            return FIPSD_EXIT_FAILURE;
        }
    }
    fipsd_config_apply_logging(config, log, log_file);
    fipsd_config_log_replay(&held, log);
    fipsd_config_log_clear(&held);
    FIPSD_INFO(MODULE, "configuration loaded");
    GoSwap *snapshots = go_swap_create(config, fipsd_config_free);
    if (!snapshots) {
        fipsd_config_free(config);
        FIPSD_ERROR(MODULE, "cannot allocate configuration snapshot");
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
        return FIPSD_EXIT_FAILURE;
    }
    fipsd_config_service service;
    fipsd_config_service_init(&service, snapshots, opts.config_path, log, log_file);

    /* Host adapters for the core (time, entropy) are linked and ready for the
     * node subsystem; nothing reads them in the skeleton. */
    fips_clock clock = fipsd_host_os_clock();
    fips_entropy entropy = fipsd_host_os_entropy();
    (void)clock;
    (void)entropy;

    int code = FIPSD_EXIT_OK;
    fipsd_runtime_config cfg;
    fipsd_runtime_config_default(&cfg);
    fipsd_runtime *rt = fipsd_runtime_create(&cfg);
    if (!rt) {
        FIPSD_ERROR(MODULE, "cannot allocate the runtime");
        code = FIPSD_EXIT_FAILURE;
    } else if (fipsd_runtime_register(rt, &service.subsystem) != 0 ||
               fipsd_runtime_start(rt) != 0) {
        FIPSD_ERROR(MODULE, "start-up failed");
        code = FIPSD_EXIT_FAILURE;
    } else {
        FIPSD_INFO(MODULE, "fipsd running");
        int status = fipsd_runtime_run(rt);
        if (status != FIPSD_RUN_OK) {
            FIPSD_ERROR(MODULE, "shutdown did not complete within the grace period");
            code = FIPSD_EXIT_FAILURE;
        }
    }
    fipsd_runtime_destroy(rt);
    go_swap_free(snapshots);
    FIPSD_INFO(MODULE, "fipsd shutdown complete (exit %d)", code);
    fipsd_log_set_default(NULL);
    fipsd_log_destroy(log);
    return code;
}
