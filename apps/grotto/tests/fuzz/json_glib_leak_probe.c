/* json_glib_leak_probe.c - JSON-GLib API-only leak and loaded-library probe.
 *
 * Uses no project code. Each input is a "multiple root statements" document:
 * JSON-GLib 1.8.x (Ubuntu 24.04's libjson-glib 1.8.0) leaks the nodes it
 * built for the extra root in json_parser_load_from_data(); upstream
 * GNOME/json-glib#74 (fixed by MR !82, first released in 1.10.0) removed the
 * leak. Run under a strict AddressSanitizer/LeakSanitizer build: exit status
 * is non-zero if the linked library leaks, so this is a leak-negative gate.
 *
 * Prints the file path of the libjson-glib actually mapped into this process
 * (from /proc/self/maps) so callers can verify the runtime-loaded library.
 *
 * Issue: nostrc-0cfsg.29
 */
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

static const char *const INPUTS[] = {
    "{}{}",
    "[][]",
    "[][]{}{}",
    "7{\"kind\":1,\"content\":\"tt\"}",
    "0{\"kind\":1,\"content\":\"test\"}",
    "{\"a\":[1,2,{\"b\":null}]}{\"c\":\"d\"}",
};

static void print_loaded_library(void)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[4096];
    char last[4096] = "";

    if (!maps) {
        fputs("loaded_library=unknown (no /proc/self/maps)\n", stderr);
        return;
    }
    while (fgets(line, sizeof line, maps)) {
        char *path = strchr(line, '/');
        if (path)
            path[strcspn(path, "\n")] = '\0';
        if (path && strstr(path, "libjson-glib-1.0.so") && strcmp(path, last) != 0) {
            printf("loaded_library=%s\n", path);
            snprintf(last, sizeof last, "%s", path);
        }
    }
    fclose(maps);
}

int main(void)
{
    for (size_t i = 0; i < sizeof INPUTS / sizeof INPUTS[0]; i++) {
        JsonParser *parser = json_parser_new();
        GError *error = NULL;
        gboolean ok = json_parser_load_from_data(parser, INPUTS[i], -1, &error);

        printf("input[%zu] load_from_data=%d\n", i, ok);
        if (error)
            g_error_free(error);
        g_object_unref(parser);
    }
    print_loaded_library();
    return 0;
}
