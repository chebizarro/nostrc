#define _GNU_SOURCE
#include "nss_nostr.h"
#include "nostr_identity.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NH_NSS_CONFIG_PATH
#define NH_NSS_CONFIG_PATH "/etc/nss_nostr.conf"
#endif

/* No environment overrides: this code also runs inside privileged processes.
 * Check every path component without following links; nobody except root may
 * replace the immutable snapshot or redirect a configured lookup. */
static int trusted_file(const char *path) {
  char copy[NH_IDENTITY_HOME_CAP], *save = NULL, *part;
  int fd, next;
  struct stat st;
  if (!path || path[0] != '/' || strlen(path) >= sizeof copy) return -1;
  strcpy(copy, path);
  fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  for (part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
    if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
    next = openat(fd, part, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(fd); fd = next;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 0022) ||
        (save && *save ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
      close(fd); return -1;
    }
  }
  return fd;
}

static nh_identity_reader_result open_reader(nh_identity_reader **reader) {
  char path[NH_IDENTITY_HOME_CAP] = NH_IDENTITY_DEFAULT_PROJECTION_PATH;
  char line[NH_IDENTITY_HOME_CAP + 32];
  int fd = trusted_file(NH_NSS_CONFIG_PATH), found = 0;
  FILE *config;
  /* Missing or untrusted configuration fails closed; the shipped sample must
   * be explicitly installed. Do not fall back to the legacy writable cache. */
  if (fd < 0) return NH_IDENTITY_READER_UNAVAILABLE;
  config = fdopen(fd, "r");
  if (!config) { close(fd); return NH_IDENTITY_READER_UNAVAILABLE; }
  while (fgets(line, sizeof line, config)) {
    size_t length = strlen(line);
    if (length && line[length - 1] == '\n') line[--length] = 0;
    else if (!feof(config)) goto invalid;
    if (!length || line[0] == '#') continue;
    if (found || strncmp(line, "projection_path=", 16) ||
        !line[16] || strlen(line + 16) >= sizeof path) goto invalid;
    strcpy(path, line + 16); found = 1;
  }
  if (ferror(config) || !found) goto invalid;
  fclose(config);
  fd = trusted_file(path);
  if (fd < 0) return NH_IDENTITY_READER_UNAVAILABLE;
  close(fd);
  return nh_identity_reader_open(path, reader);
invalid:
  fclose(config);
  return NH_IDENTITY_READER_UNAVAILABLE;
}

static enum nss_status status(nh_identity_reader_result result, int *errnop) {
  switch (result) {
    case NH_IDENTITY_READER_FOUND: return NSS_STATUS_SUCCESS;
    case NH_IDENTITY_READER_NOT_FOUND: return NSS_STATUS_NOTFOUND;
    case NH_IDENTITY_READER_TOO_SMALL:
      if (errnop) *errnop = ERANGE;
      return NSS_STATUS_TRYAGAIN;
    default:
      if (errnop) *errnop = EIO;
      return NSS_STATUS_UNAVAIL;
  }
}
static enum nss_status invalid(int *errnop) {
  if (errnop) *errnop = EINVAL;
  return NSS_STATUS_UNAVAIL;
}
static enum nss_status passwd_lookup(const char *name, uid_t uid, int by_name,
    struct passwd *pwd, char *buffer, size_t buflen, int *errnop) {
  nh_identity_reader *reader = NULL;
  nh_identity_passwd_record record;
  nh_identity_reader_result result;
  size_t required;
  if (!pwd || !buffer || (by_name && !name)) return invalid(errnop);
  result = open_reader(&reader);
  if (result == NH_IDENTITY_READER_FOUND)
    result = by_name ? nh_identity_reader_getpwnam(reader, name, &record, buffer, buflen, &required)
                     : nh_identity_reader_getpwuid(reader, uid, &record, buffer, buflen, &required);
  nh_identity_reader_close(reader);
  if (result == NH_IDENTITY_READER_FOUND) {
    memset(pwd, 0, sizeof *pwd);
    pwd->pw_name = (char *)record.name; pwd->pw_passwd = (char *)record.passwd;
    pwd->pw_gecos = (char *)record.gecos; pwd->pw_dir = (char *)record.home;
    pwd->pw_shell = (char *)record.shell; pwd->pw_uid = record.uid; pwd->pw_gid = record.gid;
  }
  return status(result, errnop);
}
enum nss_status _nss_nostr_getpwnam_r(const char *name, struct passwd *pwd,
    char *buffer, size_t buflen, int *errnop) {
  return passwd_lookup(name, 0, 1, pwd, buffer, buflen, errnop);
}
enum nss_status _nss_nostr_getpwuid_r(uid_t uid, struct passwd *pwd,
    char *buffer, size_t buflen, int *errnop) {
  return passwd_lookup(NULL, uid, 0, pwd, buffer, buflen, errnop);
}
static enum nss_status group_lookup(const char *name, gid_t gid, int by_name,
    struct group *grp, char *buffer, size_t buflen, int *errnop) {
  nh_identity_reader *reader = NULL;
  nh_identity_group_record record;
  nh_identity_reader_result result;
  size_t required, offset, padding;
  if (!grp || !buffer || (by_name && !name)) return invalid(errnop);
  result = open_reader(&reader);
  if (result == NH_IDENTITY_READER_FOUND)
    result = by_name ? nh_identity_reader_getgrnam(reader, name, &record, buffer, buflen, &required)
                     : nh_identity_reader_getgrgid(reader, gid, &record, buffer, buflen, &required);
  nh_identity_reader_close(reader);
  if (result != NH_IDENTITY_READER_FOUND) return status(result, errnop);
  padding = (sizeof(char *) - ((uintptr_t)(buffer + required) % sizeof(char *))) % sizeof(char *);
  if (required > buflen || padding > buflen - required ||
      sizeof(char *) > buflen - required - padding)
    return status(NH_IDENTITY_READER_TOO_SMALL, errnop);
  offset = required + padding;
  memset(grp, 0, sizeof *grp);
  grp->gr_name = (char *)record.name; grp->gr_passwd = (char *)record.passwd;
  grp->gr_gid = record.gid; grp->gr_mem = (char **)(buffer + offset);
  grp->gr_mem[0] = NULL;
  return NSS_STATUS_SUCCESS;
}
enum nss_status _nss_nostr_getgrnam_r(const char *name, struct group *grp,
    char *buffer, size_t buflen, int *errnop) {
  return group_lookup(name, 0, 1, grp, buffer, buflen, errnop);
}
enum nss_status _nss_nostr_getgrgid_r(gid_t gid, struct group *grp,
    char *buffer, size_t buflen, int *errnop) {
  return group_lookup(NULL, gid, 0, grp, buffer, buflen, errnop);
}
enum nss_status _nss_nostr_initgroups_dyn(const char *user, gid_t group,
    long *start, long *size, gid_t **groupsp, long limit, int *errnop) {
  char buffer[NH_IDENTITY_READER_BUF_MAX];
  struct passwd pwd;
  enum nss_status result;
  long next;
  gid_t *grown;
  if (!user || !start || !size || !groupsp || *start < 0 || *size < *start ||
      (*size && !*groupsp)) return invalid(errnop);
  result = _nss_nostr_getpwnam_r(user, &pwd, buffer, sizeof buffer, errnop);
  if (result != NSS_STATUS_SUCCESS) return result;
  if (pwd.pw_gid == group) return NSS_STATUS_SUCCESS;
  for (long i = 0; i < *start; i++)
    if ((*groupsp)[i] == pwd.pw_gid) return NSS_STATUS_SUCCESS;
  if (limit > 0 && *start >= limit) return NSS_STATUS_SUCCESS;
  if (*start == *size) {
    if (*size >= LONG_MAX / 2 || (uintmax_t)*size >= SIZE_MAX / sizeof(gid_t) / 2) {
      if (errnop) *errnop = ERANGE;
      return NSS_STATUS_TRYAGAIN;
    }
    next = *size ? *size * 2 : 8;
    if (limit > 0 && next > limit) next = limit;
    grown = realloc(*groupsp, (size_t)next * sizeof(gid_t));
    if (!grown) { if (errnop) *errnop = ENOMEM; return NSS_STATUS_TRYAGAIN; }
    *groupsp = grown; *size = next;
  }
  (*groupsp)[(*start)++] = pwd.pw_gid;
  return NSS_STATUS_SUCCESS;
}
