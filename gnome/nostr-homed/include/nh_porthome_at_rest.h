/* SPDX-License-Identifier: MIT */
#ifndef NH_PORTHOME_AT_REST_H
#define NH_PORTHOME_AT_REST_H

/* Internal, non-installed API. Evidence is a snapshot, not a security boundary
 * against root changing mounts or device-mapper tables. */
typedef enum {
    NH_AT_REST_UNKNOWN = 0,
    NH_AT_REST_UNENCRYPTED,
    NH_AT_REST_ENCRYPTED
} nh_at_rest;

/* Inspect a sysfs device directory; explicit root is for fixture tests only. */
nh_at_rest nh_porthome_at_rest_device(const char *device_dir);
nh_at_rest nh_porthome_at_rest_fd(int fd);
/* Missing targets use the nearest existing parent; other errors are unknown. */
nh_at_rest nh_porthome_at_rest_path(const char *path);
const char *nh_porthome_at_rest_name(nh_at_rest state);
/* NULL for encrypted; otherwise loud operator/user-facing text. */
const char *nh_porthome_at_rest_warning(nh_at_rest state);
int nh_porthome_at_rest_allowed(nh_at_rest state, int require_encrypted);
/* Merge the independent at_rest key into an existing status-file location. */
int nh_porthome_at_rest_write(const char *status_path, nh_at_rest state);
#endif
