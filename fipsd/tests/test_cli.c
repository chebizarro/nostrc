#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FIPSD-version.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

/* Parses argv, capturing diagnostics into buf. */
static fipsd_cli_action parse(fipsd_options *opts, char *buf, size_t cap, int argc, char **argv)
{
    FILE *err = tmpfile();
    if (!err)
        abort();
    memset(opts, 0xff, sizeof(*opts)); /* detect "left untouched" */
    fipsd_cli_action action = fipsd_cli_parse(argc, argv, opts, err);
    rewind(err);
    size_t n = fread(buf, 1, cap - 1, err);
    buf[n] = '\0';
    fclose(err);
    return action;
}

/* argv is NULL-terminated like main()'s: getopt_long reads argv[argc]. */
#define PARSE(opts, buf, ...)                                                             \
    parse(&(opts), buf, sizeof(buf),                                                      \
          (int)(sizeof((char *[]){__VA_ARGS__, NULL}) / sizeof(char *)) - 1,              \
          (char *[]){__VA_ARGS__, NULL})

static int capture(void (*fn)(FILE *, bool), bool arg, char *buf, size_t cap)
{
    FILE *out = tmpfile();
    if (!out)
        return -1;
    fn(out, arg);
    rewind(out);
    size_t n = fread(buf, 1, cap - 1, out);
    buf[n] = '\0';
    fclose(out);
    return 0;
}

int main(void)
{
    fipsd_options o;
    char err[1024];

    CHECK(PARSE(o, err, "fipsd") == FIPSD_CLI_RUN);
    CHECK(o.config_path == NULL && o.log_file == NULL && err[0] == '\0');

    CHECK(PARSE(o, err, "fipsd", "-c", "/etc/fips/fips.yaml", "--log-file", "/var/log/fips.log")
          == FIPSD_CLI_RUN);
    CHECK(strcmp(o.config_path, "/etc/fips/fips.yaml") == 0);
    CHECK(strcmp(o.log_file, "/var/log/fips.log") == 0);

    CHECK(PARSE(o, err, "fipsd", "--config=/x.yaml", "--log-file=/y.log") == FIPSD_CLI_RUN);
    CHECK(strcmp(o.config_path, "/x.yaml") == 0 && strcmp(o.log_file, "/y.log") == 0);
    CHECK(PARSE(o, err, "fipsd", "-c/z.yaml") == FIPSD_CLI_RUN);
    CHECK(strcmp(o.config_path, "/z.yaml") == 0 && o.log_file == NULL);
    /* Last occurrence wins. */
    CHECK(PARSE(o, err, "fipsd", "-c", "a", "-c", "b") == FIPSD_CLI_RUN);
    CHECK(strcmp(o.config_path, "b") == 0);

    CHECK(PARSE(o, err, "fipsd", "-V") == FIPSD_CLI_VERSION_SHORT);
    CHECK(PARSE(o, err, "fipsd", "--version") == FIPSD_CLI_VERSION_LONG);
    CHECK(PARSE(o, err, "fipsd", "-h") == FIPSD_CLI_HELP);
    CHECK(PARSE(o, err, "fipsd", "--help") == FIPSD_CLI_HELP);
    CHECK(PARSE(o, err, "fipsd", "-V", "--help") == FIPSD_CLI_HELP);
    CHECK(PARSE(o, err, "fipsd", "--help", "-V") == FIPSD_CLI_HELP);
    CHECK(PARSE(o, err, "fipsd", "-c", "x", "-V") == FIPSD_CLI_VERSION_SHORT);

    CHECK(PARSE(o, err, "fipsd", "--bogus") == FIPSD_CLI_USAGE_ERROR);
    CHECK(strstr(err, "error: unexpected argument '--bogus' found\n") == err);
    CHECK(strstr(err, "Usage: fipsd [OPTIONS]\n") != NULL);
    CHECK(strstr(err, "For more information, try '--help'.") != NULL);
    CHECK(o.config_path == (const char *)-1); /* untouched on error */

    CHECK(PARSE(o, err, "fipsd", "-x") == FIPSD_CLI_USAGE_ERROR);
    CHECK(strstr(err, "unexpected argument '-x' found") != NULL);
    CHECK(PARSE(o, err, "fipsd", "stray") == FIPSD_CLI_USAGE_ERROR);
    CHECK(strstr(err, "unexpected argument 'stray' found") != NULL);
    CHECK(PARSE(o, err, "fipsd", "-c") == FIPSD_CLI_USAGE_ERROR);
    CHECK(strstr(err, "a value is required for '--config <FILE>' but none was supplied") != NULL);
    CHECK(PARSE(o, err, "fipsd", "--log-file") == FIPSD_CLI_USAGE_ERROR);
    CHECK(strstr(err, "a value is required for '--log-file <FILE>' but none was supplied") != NULL);
    CHECK(PARSE(o, err, "fipsd", "--install-service") == FIPSD_CLI_USAGE_ERROR);
    /* A NULL error stream is accepted. */
    CHECK(fipsd_cli_parse(2, (char *[]){"fipsd", "--bogus", NULL}, &o, NULL) == FIPSD_CLI_USAGE_ERROR);

    CHECK(fipsd_cli_exit_code(FIPSD_CLI_RUN) == 0);
    CHECK(fipsd_cli_exit_code(FIPSD_CLI_HELP) == 0);
    CHECK(fipsd_cli_exit_code(FIPSD_CLI_VERSION_SHORT) == 0);
    CHECK(fipsd_cli_exit_code(FIPSD_CLI_VERSION_LONG) == 0);
    CHECK(fipsd_cli_exit_code(FIPSD_CLI_USAGE_ERROR) == 2);
    CHECK(FIPSD_EXIT_OK == 0 && FIPSD_EXIT_FAILURE == 1 && FIPSD_EXIT_USAGE == 2);

    /* Version text: "<version>" or "<version> (rev <hex>[-dirty])". */
    const char *sv = fipsd_cli_short_version();
    CHECK(strncmp(sv, FIPSD_VERSION, strlen(FIPSD_VERSION)) == 0);
    const char *rest = sv + strlen(FIPSD_VERSION);
    if (*rest) {
        CHECK(strncmp(rest, " (rev ", 6) == 0);
        const char *p = rest + 6;
        size_t hex = strspn(p, "0123456789abcdef");
        CHECK(hex >= 4);
        p += hex;
        if (strncmp(p, "-dirty", 6) == 0)
            p += 6;
        CHECK(strcmp(p, ")") == 0);
    }
    char out[2048];
    CHECK(capture(fipsd_cli_print_version, false, out, sizeof(out)) == 0);
    CHECK(strncmp(out, "fipsd ", 6) == 0);
    CHECK(strncmp(out + 6, sv, strlen(sv)) == 0 && strcmp(out + 6 + strlen(sv), "\n") == 0);
    CHECK(capture(fipsd_cli_print_version, true, out, sizeof(out)) == 0);
    CHECK(strncmp(out, "fipsd ", 6) == 0);
    CHECK(strstr(out, "\ntarget: ") != NULL && strstr(out, "\nlibfips-core: ") != NULL);
    CHECK(out[strlen(out) - 1] == '\n');

    FILE *help = tmpfile();
    CHECK(help != NULL);
    fipsd_cli_print_help(help);
    rewind(help);
    size_t n = fread(out, 1, sizeof(out) - 1, help);
    out[n] = '\0';
    fclose(help);
    CHECK(strstr(out, "Usage: fipsd [OPTIONS]\n") != NULL);
    CHECK(strstr(out, "-c, --config <FILE>") != NULL);
    CHECK(strstr(out, "--log-file <FILE>") != NULL);
    CHECK(strstr(out, "-h, --help") != NULL);
    CHECK(strstr(out, "--version") != NULL && strstr(out, "  -V ") != NULL);
    return 0;
}
