#include "fipsctl.h"
#include <signal.h>

int main(int argc, char **argv)
{
#ifdef SIGPIPE
    /* A daemon that closes early must surface as a read/write error, not a
     * signal, as it does for the Rust client. (Windows has no SIGPIPE.) */
    signal(SIGPIPE, SIG_IGN);
#endif
    return fipsctl_main(argc, argv);
}
