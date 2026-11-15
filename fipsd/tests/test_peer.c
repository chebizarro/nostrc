#include "peer_test.h"
int main(void)
{
    CHECK(peer_machine_tests() == 0);
    CHECK(peer_budget_tests() == 0);
    CHECK(peer_data_table_tests() == 0);
    CHECK(peer_link_tests() == 0);
    CHECK(peer_dispatcher_tests() == 0);
    puts("peer: lifecycle, retry/cooldown, restart/crossing, budgets, identities, table, rekey seam, concurrent snapshots passed");
    return 0;
}
