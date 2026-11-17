/* Windows counterpart of test_tmpdir.c: each call creates %TEMP%\fd-XXXXXX
 * and an atexit cleanup removes every directory with its contents. There is
 * no Unix socket path limit to check (no AF_UNIX endpoints on Windows). */
#include "test_tmpdir.h"

#include <direct.h>
#include <errno.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct test_dir {
    char path[MAX_PATH];
    struct test_dir *next;
} test_dir;
static test_dir *dirs;

static void remove_tree(const char *path)
{
    char pattern[MAX_PATH + 3];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE h = FindFirstFileA(pattern, &entry);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[MAX_PATH * 2];
            snprintf(child, sizeof child, "%s\\%s", path, entry.cFileName);
            /* Reparse points are removed, never followed. */
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree(child);
            else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                (void)RemoveDirectoryA(child);
            else if (!DeleteFileA(child))
                fprintf(stderr, "fipsd test: cleanup '%s': error %lu\n", child, GetLastError());
        } while (FindNextFileA(h, &entry));
        FindClose(h);
    }
    if (!RemoveDirectoryA(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
        fprintf(stderr, "fipsd test: cleanup '%s': error %lu\n", path, GetLastError());
}

static void cleanup(void)
{
    while (dirs) {
        test_dir *dir = dirs;
        dirs = dir->next;
        remove_tree(dir->path);
        free(dir);
    }
}

const char *fipsd_test_tmpdir(const char *socket_suffix)
{
    (void)socket_suffix;
    char root[MAX_PATH];
    DWORD n = GetTempPathA(sizeof root, root);
    if (!n || n >= sizeof root) {
        fputs("fipsd test: no temporary directory (GetTempPath)\n", stderr);
        exit(EXIT_FAILURE);
    }
    test_dir *dir = calloc(1, sizeof(*dir));
    if (!dir) {
        perror("fipsd test: allocate temp directory");
        exit(EXIT_FAILURE);
    }
    if (!dirs && atexit(cleanup) != 0) {
        free(dir);
        fputs("fipsd test: cannot register temp directory cleanup\n", stderr);
        exit(EXIT_FAILURE);
    }
    for (unsigned attempt = 0;; ++attempt) {
        snprintf(dir->path, sizeof dir->path, "%sfd-XXXXXX", root); /* root ends in '\' */
        if (_mktemp_s(dir->path, strlen(dir->path) + 1) != 0 || attempt == 64) break;
        if (_mkdir(dir->path) == 0) {
            dir->next = dirs;
            dirs = dir;
            return dir->path;
        }
        if (errno != EEXIST) break;
    }
    fprintf(stderr, "fipsd test: cannot create '%s': %s\n", dir->path, strerror(errno));
    exit(EXIT_FAILURE);
}
