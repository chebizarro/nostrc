#define _GNU_SOURCE
#include "nss_nostr.h"
#include "nostr_cache.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static nh_cache g_cache;
static int g_inited = 0;

static void ensure_init(void){
  if (g_inited) return;
  if (nh_cache_open_configured(&g_cache, "/etc/nss_nostr.conf") == 0) g_inited = 1;
}

/**
 * Copy a string into the caller's buffer, advancing the cursor.
 * Returns NSS_STATUS_SUCCESS on success, NSS_STATUS_TRYAGAIN (with
 * *errnop = ERANGE) if the buffer is too small. Per NSS contract,
 * all strings MUST be copied into the caller-supplied buffer.
 */
static enum nss_status
nss_buf_copy(char **dst, const char *src, char **cur, char *end, int *errnop)
{
  if (src == NULL) src = "";
  size_t len = strlen(src) + 1; /* include NUL */
  if (*cur + len > end) {
    if (errnop) *errnop = ERANGE;
    return NSS_STATUS_TRYAGAIN;
  }
  memcpy(*cur, src, len);
  *dst = *cur;
  *cur += len;
  return NSS_STATUS_SUCCESS;
}

static enum nss_status fill_pwd(struct passwd *pwd, char *buffer, size_t buflen,
                                const char *name, uid_t uid, gid_t gid,
                                const char *home, int *errnop){
  if (!pwd) return NSS_STATUS_TRYAGAIN;
  memset(pwd, 0, sizeof(*pwd));

  char *cur = buffer;
  char *end = buffer + buflen;
  enum nss_status rc;

  if ((rc = nss_buf_copy(&pwd->pw_name, name ? name : "nostr", &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;
  if ((rc = nss_buf_copy(&pwd->pw_passwd, "x", &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;
  if ((rc = nss_buf_copy(&pwd->pw_gecos, "Nostr User", &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;
  if ((rc = nss_buf_copy(&pwd->pw_dir, home ? home : "/home/nostr", &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;
  if ((rc = nss_buf_copy(&pwd->pw_shell, "/bin/bash", &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;

  pwd->pw_uid = uid;
  pwd->pw_gid = gid ? gid : uid;
  return NSS_STATUS_SUCCESS;
}

static enum nss_status fill_grp(struct group *grp, char *buffer, size_t buflen,
                                const char *name, gid_t gid, int *errnop){
  if (!grp) return NSS_STATUS_TRYAGAIN;
  memset(grp, 0, sizeof(*grp));

  char *cur = buffer;
  char *end = buffer + buflen;
  enum nss_status rc;

  if ((rc = nss_buf_copy(&grp->gr_name, name, &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;
  if ((rc = nss_buf_copy(&grp->gr_passwd, "x", &cur, end, errnop)) != NSS_STATUS_SUCCESS)
    return rc;

  /* gr_mem: NULL-terminated array of member names (empty).
   * We need space for one char* (the NULL terminator). */
  size_t ptr_size = sizeof(char *);
  /* Align cursor to pointer boundary */
  uintptr_t align = (uintptr_t)cur % ptr_size;
  if (align != 0) cur += (ptr_size - align);
  if (cur + ptr_size > end) {
    if (errnop) *errnop = ERANGE;
    return NSS_STATUS_TRYAGAIN;
  }
  grp->gr_mem = (char **)cur;
  grp->gr_mem[0] = NULL;
  /* cur += ptr_size; -- not needed, we're done */

  grp->gr_gid = gid;
  return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_nostr_getpwnam_r(const char *name, struct passwd *pwd,
                                      char *buffer, size_t buflen, int *errnop){
  ensure_init();
  if (!g_inited) { if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  unsigned int uid=0,gid=0; char home[256];
  if (nh_cache_lookup_name(&g_cache, name, &uid, &gid, home, sizeof home) == 0) {
    return fill_pwd(pwd, buffer, buflen, name, (uid_t)uid, (gid_t)gid, home, errnop);
  }
  return NSS_STATUS_NOTFOUND;
}

enum nss_status _nss_nostr_getpwuid_r(uid_t uid, struct passwd *pwd,
                                      char *buffer, size_t buflen, int *errnop){
  ensure_init();
  if (!g_inited) { if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  char name[128]; unsigned int gid=0; char home[256];
  if (nh_cache_lookup_uid(&g_cache, (unsigned int)uid, name, sizeof name, &gid, home, sizeof home) == 0) {
    return fill_pwd(pwd, buffer, buflen, name, (uid_t)uid, (gid_t)gid, home, errnop);
  }
  return NSS_STATUS_NOTFOUND;
}

enum nss_status _nss_nostr_getgrnam_r(const char *name, struct group *grp,
                                      char *buffer, size_t buflen, int *errnop){
  if (!grp || !name) return NSS_STATUS_TRYAGAIN;
  ensure_init();
  if (!g_inited){ if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  unsigned int gid=0;
  if (nh_cache_group_lookup_name(&g_cache, name, &gid) != 0)
    return NSS_STATUS_NOTFOUND;
  return fill_grp(grp, buffer, buflen, name, (gid_t)gid, errnop);
}

enum nss_status _nss_nostr_getgrgid_r(gid_t gid, struct group *grp,
                                      char *buffer, size_t buflen, int *errnop){
  if (!grp) return NSS_STATUS_TRYAGAIN;
  ensure_init();
  if (!g_inited){ if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  char name[128]="";
  if (nh_cache_group_lookup_gid(&g_cache, (unsigned int)gid, name, sizeof name) != 0)
    return NSS_STATUS_NOTFOUND;
  return fill_grp(grp, buffer, buflen, name, gid, errnop);
}

/* Provide minimal initgroups: ensure primary group is present */
enum nss_status _nss_nostr_initgroups_dyn(const char *user, gid_t group,
    long int *start, long int *size, gid_t **groupsp, long int limit, int *errnop){
  (void)user; (void)limit; (void)errnop;
  if (!start || !size || !groupsp) return NSS_STATUS_TRYAGAIN;
  long n = *start;
  if (n >= *size){
    long newsize = (*size) ? (*size) * 2 : 8;
    gid_t *ng = realloc(*groupsp, (size_t)newsize * sizeof(gid_t));
    if (!ng) return NSS_STATUS_TRYAGAIN;
    *groupsp = ng; *size = newsize;
  }
  (*groupsp)[n++] = group; *start = n;
  return NSS_STATUS_SUCCESS;
}
