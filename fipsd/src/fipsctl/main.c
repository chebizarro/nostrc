#include "fipsctl.h"
#include <signal.h>

int main(int argc, char **argv)
{
    /* A daemon that closes early must surface as a read/write error, not a
     * signal, as it does for the Rust client. */
    signal(SIGPIPE, SIG_IGN);
    return fipsctl_main(argc, argv);
}
