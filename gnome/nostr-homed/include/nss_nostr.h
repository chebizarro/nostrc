#ifndef NSS_NOSTR_H
#define NSS_NOSTR_H

#include <pwd.h>
#include <grp.h>
#include <nss.h>

enum nss_status _nss_nostr_getpwnam_r(const char *name, struct passwd *pwd,
                                      char *buffer, size_t buflen, int *errnop);

enum nss_status _nss_nostr_getpwuid_r(uid_t uid, struct passwd *pwd,
                                      char *buffer, size_t buflen, int *errnop);

enum nss_status _nss_nostr_getgrnam_r(const char *name, struct group *grp,
                                      char *buffer, size_t buflen, int *errnop);

enum nss_status _nss_nostr_getgrgid_r(gid_t gid, struct group *grp,
                                      char *buffer, size_t buflen, int *errnop);

#endif /* NSS_NOSTR_H */
