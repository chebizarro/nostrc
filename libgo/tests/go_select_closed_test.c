#include "select.h"
#include "channel.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    GoChannel *c = go_channel_create(1);
    if (!c) {
        fprintf(stderr, "failed to create channel\n");
        return 1;
    }
    // Close immediately; select on receive should return promptly
    go_channel_close(c);

    GoSelectCase cases[] = {
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = c, .value = NULL, .recv_buf = NULL },
    };
    int idx = go_select(cases, 1);
    if (idx != 0) {
        fprintf(stderr, "expected idx 0 for closed receive, got %d\n", idx);
        go_channel_free(c);
        return 2;
    }

    go_channel_free(c);
    return 0;
}
