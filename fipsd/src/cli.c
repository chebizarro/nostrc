#include "cli.h"

#include <fips/core.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include "FIPSD-version.h"
#include "fipsd_build_info.h"

#ifndef FIPSD_TARGET_TRIPLE
#define FIPSD_TARGET_TRIPLE "unknown"
#endif

enum { OPT_LOG_FILE = 256, OPT_VERSION_LONG, OPT_SERVICE, OPT_INSTALL_SERVICE, OPT_UNINSTALL_SERVICE };

static const struct option long_options[] = {
    {"config", required_argument, NULL, 'c'},
    {"log-file", required_argument, NULL, OPT_LOG_FILE},
    {"version", no_argument, NULL, OPT_VERSION_LONG},
    {"help", no_argument, NULL, 'h'},
#if defined(_WIN32)
    {"service", no_argument, NULL, OPT_SERVICE},
    {"install-service", no_argument, NULL, OPT_INSTALL_SERVICE},
    {"uninstall-service", no_argument, NULL, OPT_UNINSTALL_SERVICE},
#endif
    {NULL, 0, NULL, 0},
};

const char *fipsd_cli_short_version(void)
{
    /* `rev` is omitted when the build could not read a git revision. */
    return FIPSD_GIT_REV[0] ? FIPSD_VERSION " (rev " FIPSD_GIT_REV ")" : FIPSD_VERSION;
}

void fipsd_cli_print_version(FILE *out, bool long_form)
{
    fprintf(out, "fipsd %s\n", fipsd_cli_short_version());
    if (long_form)
        fprintf(out, "target: %s\nlibfips-core: %s\n", FIPSD_TARGET_TRIPLE, fips_core_version());
}

void fipsd_cli_print_usage(FILE *out)
{
    fputs("Usage: fipsd [OPTIONS]\n", out);
}

void fipsd_cli_print_help(FILE *out)
{
    fputs("FIPS mesh network daemon\n\n", out);
    fipsd_cli_print_usage(out);
    fputs("\nOptions:\n"
          "  -c, --config <FILE>    Use FILE as the configuration. Skips the default search\n"
          "                         paths.\n"
          "      --log-file <FILE>  Log to FILE instead of stderr, rolled by size at\n"
          "                         node.log_max_size_mb and kept to node.log_max_files\n"
          "                         rolled files. Overrides node.log_file, and is opened\n"
          "                         before the configuration loads, so a configuration\n"
          "                         error is logged there too.\n"
          "  -V                     Print the short version, <version> (rev <git-hash>).\n"
          "      --version          Print the long version: short version plus build\n"
          "                         target triple.\n"
          "  -h, --help             Print usage and exit.\n"
#if defined(_WIN32)
          "      --install-service  Install fipsd as the Windows service 'fipsd' running\n"
          "                         this executable (Administrator).\n"
          "      --uninstall-service\n"
          "                         Stop and remove the Windows service (Administrator).\n"
          "\n"
          "The daemon stays in the foreground, logging to stderr (or to --log-file),\n"
          "until Ctrl+C or Ctrl+Break, or the console closes. As the service it reads\n"
          "C:\\ProgramData\\fips\\fips.yaml and logs to C:\\ProgramData\\fips\\fips.log.\n"
#else
          "\n"
          "The daemon stays in the foreground, logging to stderr (or to --log-file),\n"
          "until it receives SIGINT or SIGTERM; SIGHUP requests a configuration reload.\n"
#endif
          "Exit codes: 0 clean shutdown, 1 start-up failure, 2 usage error.\n",
          out);
}

int fipsd_cli_exit_code(fipsd_cli_action action)
{
    switch (action) {
    case FIPSD_CLI_RUN:
    case FIPSD_CLI_HELP:
    case FIPSD_CLI_VERSION_SHORT:
    case FIPSD_CLI_VERSION_LONG:
        return FIPSD_EXIT_OK;
    case FIPSD_CLI_USAGE_ERROR:
        return FIPSD_EXIT_USAGE;
    default: /* the service commands exit with their own status */
        return FIPSD_EXIT_USAGE;
    }
}

static fipsd_cli_action usage_error(FILE *err, const char *what, const char *arg)
{
    if (err) {
        fprintf(err, "error: %s '%s'%s\n\n", what, arg,
                strcmp(what, "unexpected argument") == 0 ? " found" : " but none was supplied");
        fipsd_cli_print_usage(err);
        fputs("\nFor more information, try '--help'.\n", err);
    }
    return FIPSD_CLI_USAGE_ERROR;
}

static void reset_getopt(void)
{
#if defined(__GLIBC__)
    optind = 0; /* glibc re-initializes its scanner on 0 */
#else
    optind = 1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    optreset = 1;
#endif
#endif
    opterr = 0;
}

fipsd_cli_action fipsd_cli_parse(int argc, char **argv, fipsd_options *out, FILE *err)
{
    fipsd_options opts = {NULL, NULL, false};
    fipsd_cli_action action = FIPSD_CLI_RUN;
#if defined(_WIN32)
    fipsd_cli_action service_command = FIPSD_CLI_RUN; /* help and version win over it */
#endif
    reset_getopt();
    for (;;) {
        int longindex = -1;
        int c = getopt_long(argc, argv, ":c:Vh", long_options, &longindex);
        if (c == -1)
            break;
        switch (c) {
        case 'c':
            opts.config_path = optarg;
            break;
        case OPT_LOG_FILE:
            opts.log_file = optarg;
            break;
        case 'V':
            if (action == FIPSD_CLI_RUN)
                action = FIPSD_CLI_VERSION_SHORT;
            break;
        case OPT_VERSION_LONG:
            if (action == FIPSD_CLI_RUN)
                action = FIPSD_CLI_VERSION_LONG;
            break;
        case 'h':
            action = FIPSD_CLI_HELP; /* help wins, as with clap */
            break;
#if defined(_WIN32)
        case OPT_SERVICE:
            opts.service = true;
            break;
        case OPT_INSTALL_SERVICE:
            service_command = FIPSD_CLI_INSTALL_SERVICE; /* upstream checks install first */
            break;
        case OPT_UNINSTALL_SERVICE:
            if (service_command == FIPSD_CLI_RUN)
                service_command = FIPSD_CLI_UNINSTALL_SERVICE;
            break;
#endif
        case ':':
            return usage_error(err, "a value is required for",
                               optopt == 'c' ? "--config <FILE>" : "--log-file <FILE>");
        case '?':
        default:
            return usage_error(err, "unexpected argument", argv[optind - 1]);
        }
    }
    if (optind < argc)
        return usage_error(err, "unexpected argument", argv[optind]);
#if defined(_WIN32)
    if (action == FIPSD_CLI_RUN)
        action = service_command;
#endif
    if (out)
        *out = opts;
    return action;
}
