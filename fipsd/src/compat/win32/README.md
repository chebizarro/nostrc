# Windows stand-ins for POSIX socket headers

On the `fipsd_lib` include path for WIN32 builds only (see `fipsd/CMakeLists.txt`),
so transport and protocol sources keep their POSIX `#include` lists. Each header
maps to Winsock plus libgo's POSIX-shaped `struct msghdr`/`struct iovec`
(`libgo/net_compat.h`). Headers for facilities Windows lacks (`ifaddrs.h`,
`sys/un.h`, `grp.h`, `sys/wait.h`, ...) are deliberately absent: the code that
needs them is guarded or replaced by a `*_unsupported.c` stub.
