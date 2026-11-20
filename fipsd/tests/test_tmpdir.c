#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "test_tmpdir.h"

#include <errno.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct test_dir {
    char *path;
    bool created;
    struct test_dir *next;
} test_dir;
static test_dir *dirs;

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)type;
    (void)ftw;
    return remove(path);
}

static void cleanup(void)
{
    while (dirs) {
        test_dir *dir = dirs;
        dirs = dir->next;
        /* FTW_PHYS never follows symlinks created by permission/refusal tests. */
        if (dir->created && nftw(dir->path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT)
            fprintf(stderr, "fipsd test: cleanup '%s': %s\n", dir->path, strerror(errno));
        free(dir->path);
        free(dir);
    }
}

const char *fipsd_test_tmpdir(const char *socket_suffix)
{
    const char *root = getenv("TMPDIR");
    if (!root || !*root)
        root = "/tmp";
    test_dir *dir = calloc(1, sizeof(*dir));
    if (!dir) {
        perror("fipsd test: allocate temp directory");
        exit(EXIT_FAILURE);
    }
    size_t size = strlen(root) + sizeof("/fd-XXXXXX");
    dir->path = malloc(size);
    if (!dir->path) {
        free(dir);
        perror("fipsd test: allocate temp path");
        exit(EXIT_FAILURE);
    }
    snprintf(dir->path, size, "%s/fd-XXXXXX", root);
    if (!dirs && atexit(cleanup) != 0) {
        free(dir->path);
        free(dir);
        fputs("fipsd test: cannot register temp directory cleanup\n", stderr);
        exit(EXIT_FAILURE);
    }
    dir->next = dirs;
    dirs = dir;
    if (socket_suffix) {
        size_t length = strlen(dir->path) + strlen(socket_suffix);
        size_t limit = sizeof(((struct sockaddr_un *)0)->sun_path);
        if (length >= limit) {
            fprintf(stderr, "fipsd test: Unix socket path '%s%s' is %zu bytes; "
                    "maximum is %zu bytes (sun_path[%zu] includes NUL); "
                    "choose a shorter TMPDIR\n",
                    dir->path, socket_suffix, length, limit - 1, limit);
            /* The template has the same length as the eventual mkdtemp path. */
            exit(EXIT_FAILURE);
        }
    }
    if (!mkdtemp(dir->path)) {
        fprintf(stderr, "fipsd test: mkdtemp '%s': %s\n", dir->path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    dir->created = true;
    return dir->path;
}
