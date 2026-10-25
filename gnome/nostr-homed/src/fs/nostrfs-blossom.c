/* File/CAS adapter only. BUD-02 auth, HTTP and byte guards live in libblossom. */
#include "nostrfs-blossom.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int open_client(const char *base, const blossom_signer_t *signer, blossom_client_t **out) {
    blossom_client_opts_t opts = { .endpoint = base, .timeout_seconds = 120 };
    return blossom_client_new(&opts, signer, out) == BLOSSOM_OK ? 0 : -1;
}
int nostrfs_blossom_upload(const char *base, const char *path,
    const blossom_signer_t *signer, char **cid) {
    if (!cid) return -1;
    *cid = NULL;
    if (!path || !signer) return -1; /* No unsigned upload fallback. */
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size < 0 ||
        (uint64_t)st.st_size > BLOSSOM_DEFAULT_MAX_BLOB_BYTES) { fclose(f); return -1; }
    size_t n = (size_t)st.st_size;
    uint8_t *data = malloc(n ? n : 1);
    if (!data) { fclose(f); return -1; }
    bool read_ok = fread(data, 1, n, f) == n && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    blossom_client_t *c = NULL;
    blossom_blob_descriptor_t *desc = NULL;
    int rc = -1;
    if (read_ok && open_client(base, signer, &c) == 0 &&
        blossom_client_put(c, data, n, NULL, &desc) == BLOSSOM_OK && desc) {
        *cid = strdup(desc->sha256); rc = *cid ? 0 : -1;
    }
    blossom_blob_descriptor_free(desc); blossom_client_free(c); free(data);
    return rc;
}
int nostrfs_blossom_client_head(const char *base, const char *cid) {
    blossom_client_t *c = NULL;
    if (open_client(base, NULL, &c)) return -1;
    bool exists = false;
    blossom_error_t e = blossom_client_head(c, cid, &exists);
    blossom_client_free(c);
    return e == BLOSSOM_OK && exists ? 0 : -1;
}
static int parents(const char *path) {
    char *s = strdup(path);
    if (!s) return -1;
    for (char *p = s + 1; *p; p++) if (*p == '/') {
        *p = 0;
        if (mkdir(s, 0700) && errno != EEXIST) { free(s); return -1; }
        *p = '/';
    }
    free(s); return 0;
}
int nostrfs_blossom_fetch(const char *base, const char *cid, const char *path) {
    if (!path || parents(path)) return -1;
    blossom_client_t *c = NULL;
    if (open_client(base, NULL, &c)) return -1;
    uint8_t *data = NULL; size_t n = 0;
    blossom_error_t e = blossom_client_get_content(c, cid, &data, &n);
    blossom_client_free(c);
    if (e != BLOSSOM_OK) return -1;
    size_t len = strlen(path) + sizeof(".XXXXXX");
    char *tmp = malloc(len);
    if (!tmp) { free(data); return -1; }
    snprintf(tmp, len, "%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    int rc = -1;
    if (fd >= 0) {
        FILE *f = fdopen(fd, "wb");
        if (f) {
            bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fd) == 0;
            if (fclose(f)) ok = false;
            if (ok && rename(tmp, path) == 0) rc = 0;
        } else close(fd);
    }
    if (rc) unlink(tmp);
    free(tmp); free(data); return rc;
}
