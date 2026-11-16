/* fips-gateway suites (nostrc-iq1n9.3): one executable, a suite per module
 * named on the command line, so each is its own ctest. */
#include "test_gateway.h"

int main(int argc, char **argv)
{
    static const struct { const char *name; int (*run)(void); } suites[] = {
        {"pool", gw_suite_pool}, {"conntrack", gw_suite_conntrack}, {"state", gw_suite_state},
        {"nat", gw_suite_nat}, {"net", gw_suite_net}, {"dns", gw_suite_dns},
        {"control", gw_suite_control}, {"shutdown", gw_suite_shutdown},
    };
    if (argc != 2) { fprintf(stderr, "usage: %s <suite>\n", argv[0]); return 2; }
    for (size_t i = 0; i < sizeof suites / sizeof suites[0]; ++i) {
        if (strcmp(argv[1], suites[i].name)) continue;
        fprintf(stderr, "gateway %s\n", suites[i].name);
        int rc = suites[i].run();
        fprintf(stderr, rc == 77 ? "gateway %s: skipped\n" : "gateway %s: ok\n", suites[i].name);
        return rc;
    }
    fprintf(stderr, "unknown suite %s\n", argv[1]);
    return 2;
}
