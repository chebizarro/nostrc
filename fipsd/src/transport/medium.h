#ifndef FIPSD_MEDIUM_H
#define FIPSD_MEDIUM_H
#include <fips/proto/path/select.h>
/* Injectable ioctl has the same request/argument ABI as the native call. */
typedef int (*fipsd_ioctl_fn)(void *, int, unsigned long, void *);
fips_path_medium fipsd_medium_media_word(int);
fips_path_medium fipsd_medium_linux(unsigned arphrd, bool wireless, bool phy80211,
                                   const char *devtype, bool device);
fips_path_medium fipsd_medium_read(const char *);
fips_path_medium fipsd_medium_read_ioctl(const char *, int fd, fipsd_ioctl_fn, void *);
const char *fipsd_medium_name(fips_path_medium);
/* Ranking itself remains fips_path_select_choose: wired beats wireless only
 * inside the switch margin, never by fabricating a link score. */
#endif
