/* Give non-C tests the same temp-path checks and exit cleanup as C tests.
 *
 * CTest's TIMEOUT kills only this process, not the command it runs. The
 * command inherits the read end of a lifeline pipe (FIPSD_TEST_LIFELINE_FD)
 * whose write end only this process holds, so it reads EOF as soon as we are
 * gone, however we die, and can stop its own children instead of outliving
 * the test while holding CTest's output pipes. */
#define _POSIX_C_SOURCE 200809L
#include "test_tmpdir.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
    int lifeline[2];
    char lifeline_fd[16];
    if (pipe(lifeline) != 0 || fcntl(lifeline[1], F_SETFD, FD_CLOEXEC) != 0) {
        perror("fipsd test: lifeline pipe");
        return 1;
    }
    (void)snprintf(lifeline_fd, sizeof lifeline_fd, "%d", lifeline[0]);
    if (setenv("FIPSD_TEST_LIFELINE_FD", lifeline_fd, 1) != 0) {
        perror("fipsd test: set lifeline descriptor");
        return 1;
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[2], NULL, NULL, argv + 2, environ);
    (void)close(lifeline[0]); /* the write end stays open until we exit */
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
