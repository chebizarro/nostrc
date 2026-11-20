/* open(2) flags the Windows CRT lacks, for the few files fipsd opens itself
 * (configuration, logs). POSIX builds get <fcntl.h> unchanged. */
#ifndef FIPSD_COMPAT_POSIX_OPEN_H
#define FIPSD_COMPAT_POSIX_OPEN_H
#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC O_NOINHERIT /* the handle is not inherited by child processes */
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0 /* regular files never block */
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0 /* the CRT has no refuse-symlink flag; paths are operator-chosen */
#endif
#endif
#endif
