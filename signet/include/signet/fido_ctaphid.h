/* SPDX-License-Identifier: MIT */
/*
 * fido_ctaphid.h - Linux /dev/uhid virtual CTAP-HID device for Signet.
 *
 * The CTAP-HID framing and CTAP2 adapter are compiled on every platform for
 * unit coverage. The actual /dev/uhid lifecycle is Linux-only; non-Linux builds
 * return a clean unsupported/no-op result from start().
 */
#ifndef SIGNET_FIDO_CTAPHID_H
#define SIGNET_FIDO_CTAPHID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "signet/fido.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SignetFidoCtapHid SignetFidoCtapHid;

#define SIGNET_CTAPHID_REPORT_SIZE 64u
#define SIGNET_CTAPHID_MAX_MSG_SIZE 4096u
#define SIGNET_CTAPHID_BROADCAST_CID 0xffffffffu

typedef struct {
  bool enabled;
  SignetFidoService *fido;
  const char *agent_id;
  const char *device_name;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t version;
  const uint8_t *aaguid; /* optional 16-byte AAGUID for CTAP2 getInfo */
  size_t max_msg_size;  /* 0 => SIGNET_CTAPHID_MAX_MSG_SIZE */
} SignetFidoCtapHidConfig;

SignetFidoCtapHid *signet_fido_ctaphid_new(const SignetFidoCtapHidConfig *cfg);
int signet_fido_ctaphid_start(SignetFidoCtapHid *dev);
void signet_fido_ctaphid_stop(SignetFidoCtapHid *dev);
void signet_fido_ctaphid_free(SignetFidoCtapHid *dev);

/* Test/diagnostic helper: feed one or more 64-byte CTAP-HID host frames and
 * collect authenticator response frames. The returned buffer is malloc-owned and
 * consists of N contiguous 64-byte reports. Returns 0 when at least one complete
 * command was processed, 1 when more continuation frames are needed, and -1 on
 * malformed input/allocation failure.
 */
int signet_ctaphid_test_process_frames(SignetFidoService *fido,
                                       const char *agent_id,
                                       const uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN],
                                       const uint8_t *frames,
                                       size_t frames_len,
                                       uint8_t **out_frames,
                                       size_t *out_frames_len);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_CTAPHID_H */
