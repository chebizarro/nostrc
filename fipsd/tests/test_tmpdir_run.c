/* Give non-C tests the same temp-path checks and exit cleanup as C tests. */
#define _POSIX_C_SOURCE 200809L
#include "test_tmpdir.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 3) {
        fputs("usage: fipsd_test_tmpdir_run <socket-suffix> <command> [args...]\n", stderr);
        return 2;
    }
    const char *dir = fipsd_test_tmpdir(argv[1]);
    if (setenv("FIPSD_TEST_TMPDIR", dir, 1) != 0) {
        perror("fipsd test: set temp directory");
        return 1;
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[2], NULL, NULL, argv + 2, environ);
    if (rc != 0) {
        fprintf(stderr, "fipsd test: spawn '%s': %s\n", argv[2], strerror(rc));
        return 1;
    }
    int status;
    do {
        rc = waitpid(pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        perror("fipsd test: wait for child");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
