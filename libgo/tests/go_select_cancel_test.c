#include "select.h"
#include "channel.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // Create a cancelable context and then cancel it; select should wake
    CancelContextResult ctxr = go_context_with_cancel(go_context_background());
    if (!ctxr.context || !ctxr.cancel) {
        fprintf(stderr, "failed to create cancelable context\n");
        return 1;
    }

    GoSelectCase cases[] = {
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ctxr.context->done, .value = NULL, .recv_buf = NULL },
    };

    // Cancel the context and expect select to return promptly
    ctxr.cancel(ctxr.context);

    int idx = go_select(cases, 1);
    if (idx != 0) {
        fprintf(stderr, "expected idx 0 for canceled context, got %d\n", idx);
        return 2;
    }
    return 0;
}
