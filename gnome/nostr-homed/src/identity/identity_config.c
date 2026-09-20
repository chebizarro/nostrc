#include "nostr_identity.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_value(char *out, size_t capacity, const char *value) {
  size_t len;
  if (!out || !value) return -1;
  len = strlen(value);
  if (len == 0 || len >= capacity) return -1;
  memcpy(out, value, len + 1);
  return 0;
}

static int parse_u32(const char *text, uint32_t *out) {
  char *end = NULL;
  unsigned long long value;
  if (!text || !*text || !out) return -1;
  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) return -1;
  *out = (uint32_t)value;
  return 0;
}

void nh_identity_config_defaults(nh_identity_config *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  (void)copy_value(out->authority_path, sizeof(out->authority_path),
                   NH_IDENTITY_DEFAULT_AUTHORITY_PATH);
  (void)copy_value(out->projection_path, sizeof(out->projection_path),
                   NH_IDENTITY_DEFAULT_PROJECTION_PATH);
  (void)copy_value(out->home_root, sizeof(out->home_root),
                   NH_IDENTITY_DEFAULT_HOME_ROOT);
  (void)copy_value(out->default_shell, sizeof(out->default_shell),
                   NH_IDENTITY_DEFAULT_SHELL);
  out->uid_min = NH_IDENTITY_DEFAULT_UID_MIN;
  out->uid_max = NH_IDENTITY_DEFAULT_UID_MAX;
  out->domain_default_min = 1000000u;
  out->domain_default_max = 1099999u;
  out->domain_rid_min = 1100000u;
  out->domain_rid_max = 1999999u;
  out->standalone_smb_min = 500000u;
  out->standalone_smb_max = 599999u;
}

static bool ranges_overlap(uint32_t amin, uint32_t amax,
                           uint32_t bmin, uint32_t bmax) {
  return amin <= bmax && bmin <= amax;
}

nh_identity_rc nh_identity_config_validate(const nh_identity_config *config) {
  const uint32_t mins[] = { config ? config->uid_min : 0,
    config ? config->domain_default_min : 0,
    config ? config->domain_rid_min : 0,
    config ? config->standalone_smb_min : 0 };
  const uint32_t maxs[] = { config ? config->uid_max : 0,
    config ? config->domain_default_max : 0,
    config ? config->domain_rid_max : 0,
    config ? config->standalone_smb_max : 0 };
  size_t i, j;
  if (!config || config->authority_path[0] != '/' ||
      config->projection_path[0] != '/' || config->home_root[0] != '/' ||
      config->default_shell[0] != '/') return NH_IDENTITY_INVALID;
  if (strchr(config->authority_path, '\n') ||
      strchr(config->projection_path, '\n') || strchr(config->home_root, '\n') ||
      strchr(config->default_shell, '\n') || strchr(config->default_shell, ':'))
    return NH_IDENTITY_INVALID;
  for (i = 0; i < 4; ++i) {
    if (mins[i] == 0 || mins[i] > maxs[i]) return NH_IDENTITY_INVALID;
    for (j = i + 1; j < 4; ++j)
      if (ranges_overlap(mins[i], maxs[i], mins[j], maxs[j]))
        return NH_IDENTITY_INVALID;
  }
  return NH_IDENTITY_OK;
}

nh_identity_rc nh_identity_config_load(const char *path,
                                       nh_identity_config *out) {
  enum { KEY_COUNT = 12 };
  const char *const keys[KEY_COUNT] = {"authority_path", "projection_path",
    "home_root", "default_shell", "uid_min", "uid_max",
    "domain_default_min", "domain_default_max", "domain_rid_min",
    "domain_rid_max", "standalone_smb_min", "standalone_smb_max"};
  bool seen[KEY_COUNT] = {false};
  FILE *file;
  char line[1024];
  unsigned long line_number = 0;
  if (!path || !out) return NH_IDENTITY_INVALID;
  file = fopen(path, "r");
  if (!file) return NH_IDENTITY_STORAGE_ERROR;
  nh_identity_config_defaults(out);
  while (fgets(line, sizeof(line), file)) {
    char *equals, *value, *newline;
    size_t key_index;
    line_number++;
    if (!strchr(line, '\n') && !feof(file)) { fclose(file); return NH_IDENTITY_INVALID; }
    newline = strpbrk(line, "\r\n");
    if (newline) *newline = '\0';
    if (line[0] == '\0' || line[0] == '#') continue;
    equals = strchr(line, '=');
    if (!equals || equals == line || strchr(equals + 1, '=')) {
      fclose(file); return NH_IDENTITY_INVALID;
    }
    *equals = '\0'; value = equals + 1;
    for (key_index = 0; key_index < KEY_COUNT; ++key_index)
      if (strcmp(line, keys[key_index]) == 0) break;
    if (key_index == KEY_COUNT || seen[key_index]) {
      fclose(file); return NH_IDENTITY_INVALID;
    }
    seen[key_index] = true;
    switch (key_index) {
      case 0: if (copy_value(out->authority_path, sizeof(out->authority_path), value)) goto invalid; break;
      case 1: if (copy_value(out->projection_path, sizeof(out->projection_path), value)) goto invalid; break;
      case 2: if (copy_value(out->home_root, sizeof(out->home_root), value)) goto invalid; break;
      case 3: if (copy_value(out->default_shell, sizeof(out->default_shell), value)) goto invalid; break;
      case 4: if (parse_u32(value, &out->uid_min)) goto invalid; break;
      case 5: if (parse_u32(value, &out->uid_max)) goto invalid; break;
      case 6: if (parse_u32(value, &out->domain_default_min)) goto invalid; break;
      case 7: if (parse_u32(value, &out->domain_default_max)) goto invalid; break;
      case 8: if (parse_u32(value, &out->domain_rid_min)) goto invalid; break;
      case 9: if (parse_u32(value, &out->domain_rid_max)) goto invalid; break;
      case 10: if (parse_u32(value, &out->standalone_smb_min)) goto invalid; break;
      case 11: if (parse_u32(value, &out->standalone_smb_max)) goto invalid; break;
      default: goto invalid;
    }
  }
  if (ferror(file)) { fclose(file); return NH_IDENTITY_STORAGE_ERROR; }
  fclose(file);
  return nh_identity_config_validate(out);
invalid:
  (void)line_number;
  fclose(file);
  return NH_IDENTITY_INVALID;
}
