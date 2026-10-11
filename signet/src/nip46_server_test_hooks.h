/* SPDX-License-Identifier: MIT
 * Test-only seam; neither declaration nor implementation exists in normal builds. */
#ifndef SIGNET_NIP46_SERVER_TEST_HOOKS_H
#define SIGNET_NIP46_SERVER_TEST_HOOKS_H
#ifdef SIGNET_ENABLE_TEST_HOOKS
#include "signet/nip46_server.h"
void signet_nip46_server_set_after_binding_hook(SignetNip46Server *server,
                                                 void (*hook)(void *), void *data);
#endif
#endif
