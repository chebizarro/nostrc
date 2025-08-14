#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GnostrIpcServer GnostrIpcServer;

// Start IPC server at endpoint string:
//  unix:/path/to/socket
//  tcp:host:port
//  npipe:\\.\pipe\name (Windows)
// If endpoint is NULL/empty, a platform default is chosen.
GnostrIpcServer* gnostr_ipc_server_start(const char *endpoint);
void gnostr_ipc_server_stop(GnostrIpcServer *srv);

#ifdef __cplusplus
}
#endif
