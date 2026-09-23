/* nip05_client.c — parent side of nostr-homed-nip05.
 *
 * The parent (root, inside the broker) fork/execs the unprivileged
 * helper after setresgid/setresuid to the drop_user, captures the
 * helper's stdout on a pipe (bounded), and parses the compact JSON
 * summary into nh_nip05_result. The helper's exit code is mapped
 * back to nh_nip05_rc so the caller can distinguish failure classes
 * without parsing text.
 *
 * The wire format between us and the child is deliberately tiny:
 *   {"pubkey":"<64-hex>","relays":["wss://...", ...]}
 * plus an "error" key on failure paths. Any parse failure or
 * unexpected shape becomes NH_NIP05_ERR_INTERNAL; the exit code is
 * still authoritative for classification. */
#define _GNU_SOURCE
#include "nostr_nip05.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <jansson.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Bounded stdout capture. The child emits a compact JSON summary
 * (~a few hundred bytes even with 4 relay hints). 8 KiB is generous
 * head-room without letting a hostile helper keep filling the pipe. */
#define NH_NIP05_STDOUT_CAP (8u * 1024u)

static char *const helper_envp[] = {
    (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    NULL,
};

/* Map the helper's exit code to nh_nip05_rc. The enum members are
 * already numbered to match, so this is really just a "known code
 * or NH_NIP05_ERR_INTERNAL" gate. */
static nh_nip05_rc rc_from_exit(int ec) {
    switch (ec) {
        case NH_NIP05_OK:
        case NH_NIP05_ERR_ARG:
        case NH_NIP05_ERR_SSRF:
        case NH_NIP05_ERR_TRANSPORT:
        case NH_NIP05_ERR_CONTENT:
        case NH_NIP05_ERR_JSON:
        case NH_NIP05_ERR_NAME:
        case NH_NIP05_ERR_PRIV:
        case NH_NIP05_ERR_INTERNAL:
            return (nh_nip05_rc)ec;
        default:
            return NH_NIP05_ERR_INTERNAL;
    }
}

/* Drain up to cap bytes from fd into buf; returns bytes read. */
static size_t drain(int fd, char *buf, size_t cap) {
    size_t off = 0;
    for (;;) {
        if (off >= cap) break;
        ssize_t n = read(fd, buf + off, cap - off);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)n;
    }
    return off;
}

/* Parse the JSON payload emitted by the helper. On success the fields
 * of @result_out are populated; on failure @result_out is zeroed. */
static int parse_helper_stdout(const char *body, size_t len,
                               nh_nip05_result *result_out) {
    if (!body || len == 0 || !result_out) return -1;
    memset(result_out, 0, sizeof *result_out);
    json_error_t je;
    json_t *root = json_loadb(body, len, 0, &je);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        return -1;
    }
    json_t *pk = json_object_get(root, "pubkey");
    if (!pk || !json_is_string(pk)) { json_decref(root); return -1; }
    const char *pks = json_string_value(pk);
    if (!pks || strlen(pks) != 64) { json_decref(root); return -1; }
    memcpy(result_out->pubkey_hex, pks, 64);
    result_out->pubkey_hex[64] = '\0';
    json_t *relays = json_object_get(root, "relays");
    if (relays && json_is_array(relays)) {
        size_t idx;
        json_t *v;
        json_array_foreach(relays, idx, v) {
            if (result_out->relays_count >= NH_NIP05_RELAY_HINTS_MAX) break;
            if (!json_is_string(v)) continue;
            const char *s = json_string_value(v);
            if (!s) continue;
            size_t sl = strlen(s);
            if (sl == 0 || sl > NH_NIP05_RELAY_URL_MAX) continue;
            memcpy(result_out->relays[result_out->relays_count], s, sl + 1);
            result_out->relays_count++;
        }
    }
    json_decref(root);
    return 0;
}

nh_nip05_rc nh_nip05_client_resolve(const char *helper_path,
                                    const char *drop_user,
                                    const nh_nip05_address *addr,
                                    nh_nip05_result *result_out) {
    if (!addr || !result_out) return NH_NIP05_ERR_ARG;
    memset(result_out, 0, sizeof *result_out);
    /* Belt-and-braces: re-validate here so no matter which caller
     * grew a shortcut, we never fork/exec the child with a bogus
     * argv. */
    if (!nh_nip05_is_valid(addr->address)) return NH_NIP05_ERR_ARG;

    const char *helper = helper_path && *helper_path
                             ? helper_path
                             : "nostr-homed-nip05";
    const char *duser = drop_user && *drop_user ? drop_user : "nobody";

    struct passwd *pw = getpwnam(duser);
    if (!pw) return NH_NIP05_ERR_ARG;
    if (pw->pw_uid == 0) return NH_NIP05_ERR_PRIV;

    int pipefd[2];
    if (pipe(pipefd) != 0) return NH_NIP05_ERR_INTERNAL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return NH_NIP05_ERR_INTERNAL;
    }
    if (pid == 0) {
        /* Child: dup stdout to the write end, drop privileges, exec. */
        close(pipefd[0]);
        if (dup2(pipefd[1], 1) < 0) _exit(NH_NIP05_ERR_INTERNAL);
        close(pipefd[1]);
        /* Silence stderr into /dev/null so a chatty helper cannot
         * flood the broker's journal from the greeter. */
        int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 2);
            close(devnull);
        }
        if (setgroups(1, &pw->pw_gid) != 0) _exit(NH_NIP05_ERR_PRIV);
        if (setgid(pw->pw_gid) != 0) _exit(NH_NIP05_ERR_PRIV);
        if (setuid(pw->pw_uid) != 0) _exit(NH_NIP05_ERR_PRIV);
        if (getuid() == 0 || geteuid() == 0) _exit(NH_NIP05_ERR_PRIV);

        char *args[] = {
            (char *)helper,
            (char *)addr->address,
            NULL,
        };
        if (helper[0] == '/') execve(helper, args, helper_envp);
        else                  execvpe(helper, args, helper_envp);
        _exit(NH_NIP05_ERR_INTERNAL);
    }

    close(pipefd[1]);
    char buf[NH_NIP05_STDOUT_CAP];
    size_t got = drain(pipefd[0], buf, sizeof buf);
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return NH_NIP05_ERR_INTERNAL;
    }
    if (!WIFEXITED(status)) return NH_NIP05_ERR_INTERNAL;
    nh_nip05_rc rc = rc_from_exit(WEXITSTATUS(status));
    if (rc != NH_NIP05_OK) return rc;
    if (parse_helper_stdout(buf, got, result_out) != 0)
        return NH_NIP05_ERR_INTERNAL;
    return NH_NIP05_OK;
}

/* NB: the in-memory (address -> pubkey) cache lives in a separate
 * translation unit (nip05_cache.c) so it can be linked into unit
 * tests without pulling in this file's fork/exec dependencies. */
