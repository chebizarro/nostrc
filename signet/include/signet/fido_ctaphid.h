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

/**
 * SignetFidoCtapHid:
 * Opaque virtual CTAP-HID device.
 *
 * Since: 1.0
 */
typedef struct SignetFidoCtapHid SignetFidoCtapHid;

/**
 * SIGNET_CTAPHID_REPORT_SIZE:
 *
 * CTAP-HID report size in bytes.
 *
 * Since: 1.0
 */
#define SIGNET_CTAPHID_REPORT_SIZE 64u
/**
 * SIGNET_CTAPHID_MAX_MSG_SIZE:
 *
 * Default maximum CTAP-HID message size in bytes.
 *
 * Since: 1.0
 */
#define SIGNET_CTAPHID_MAX_MSG_SIZE 4096u
/**
 * SIGNET_CTAPHID_BROADCAST_CID:
 *
 * CTAP-HID broadcast channel identifier.
 *
 * Since: 1.0
 */
#define SIGNET_CTAPHID_BROADCAST_CID 0xffffffffu

/**
 * SignetFidoCtapHidConfig:
 * @enabled: whether the service is enabled.
 * @fido: borrowed FIDO service dependency.
 * @agent_id: agent identifier.
 * @device_name: device name value.
 * @vendor_id: vendor id value.
 * @product_id: product id value.
 * @version: record version.
 * @aaguid: optional 16-byte AAGUID for CTAP2 getInfo.
 * @max_msg_size: 0 => SIGNET_CTAPHID_MAX_MSG_SIZE.
 *
 * Configuration for the virtual CTAP-HID device.
 *
 * Since: 1.0
 */
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

/**
 * signet_fido_ctaphid_new:
 * @cfg: (nullable): configuration to use
 *
 * Creates a virtual CTAP-HID device wrapper.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetFidoCtapHid *signet_fido_ctaphid_new(const SignetFidoCtapHidConfig *cfg);
/**
 * signet_fido_ctaphid_start:
 * @dev: (not nullable): a #SignetFidoCtapHid
 *
 * signet fido ctaphid start.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_ctaphid_start(SignetFidoCtapHid *dev);
/**
 * signet_fido_ctaphid_stop:
 * @dev: (nullable): a #SignetFidoCtapHid
 *
 * signet fido ctaphid stop.
 *
 * Since: 1.0
 */
void signet_fido_ctaphid_stop(SignetFidoCtapHid *dev);
/**
 * signet_fido_ctaphid_free:
 * @dev: (nullable): a #SignetFidoCtapHid
 *
 * signet fido ctaphid free.
 *
 * Since: 1.0
 */
void signet_fido_ctaphid_free(SignetFidoCtapHid *dev);

/* Test/diagnostic helper: feed one or more 64-byte CTAP-HID host frames and
 * collect authenticator response frames. The returned buffer is malloc-owned and
 * consists of N contiguous 64-byte reports. Returns 0 when at least one complete
 * command was processed, 1 when more continuation frames are needed, and -1 on
 * malformed input/allocation failure.
 */
/**
 * signet_ctaphid_test_process_frames:
 * @fido: (nullable): fido
 * @agent_id: (not nullable): agent identifier
 * @aaguid: (not nullable): aaguid
 * @frames: (not nullable): frames
 * @frames_len: length of @frames in bytes
 * @out_frames: (out) (transfer full) (not nullable) (array): return location for frames
 * @out_frames_len: (out) (not nullable): return location for frames len
 *
 * Test/diagnostic helper: feed one or more 64-byte CTAP-HID host frames and collect authenticator response frames. The returned buffer is malloc-owned and consists of N contiguous 64-byte reports. Returns 0 when at least one complete command was processed, 1 when more continuation frames are needed, and -1 on malformed input/allocation failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
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
