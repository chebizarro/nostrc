/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include <glib/gi18n-lib.h>
#include <string.h>

/* Names and icons are cosmetic desktop metadata, not proof of identity. */
char *grotto_panels_app_display_info(const char *principal, const char *label, GIcon **icon) {
  const char *p = principal ? principal : "";
  g_autofree char *id = NULL;
  g_autofree char *exe = NULL;
  if (g_str_has_prefix(p, "app:") || g_str_has_prefix(p, "flatpak:") || g_str_has_prefix(p, "snap:")) {
    const char *start = strchr(p, ':') + 1;
    id = g_strndup(start, strcspn(start, ";"));
  }
  const char *start = g_str_has_prefix(p, "exe:") ? p + 4 : strstr(p, ";exe:");
  if (start) {
    if (*start == ';') start += 5;
    exe = g_strndup(start, strcspn(start, ";"));
  }
  g_autofree char *base = exe && *exe ? g_path_get_basename(exe) : NULL;
  g_autofree char *desktop_id = id && !g_str_has_suffix(id, ".desktop") ? g_strconcat(id, ".desktop", NULL) : g_strdup(id);
  GList *all = g_app_info_get_all();
  GAppInfo *match = NULL;
  /* Prefer a desktop ID over an executable shared by several launchers. */
  for (GList *l = all; l && !match; l = l->next)
    if (desktop_id && !g_strcmp0(g_app_info_get_id(l->data), desktop_id)) match = l->data;
  for (GList *l = all; l && !match && base; l = l->next) {
    const char *command = g_app_info_get_executable(l->data);
    if (command && (!strcmp(command, exe) || (!g_path_is_absolute(command) && !strcmp(command, base)))) match = l->data;
  }
  /* Preserve a user-assigned name, but don't mistake a principal echoed by the
   * daemon for a custom label. Render all returned text without markup. */
  const char *name = label && *label && strcmp(label, p) ? label :
    match ? g_app_info_get_display_name(match) : base ? base : id ? id : *p ? p : _("App");
  char *result = g_strdup(name);
  if (icon) {
    GIcon *desktop_icon = match ? g_app_info_get_icon(match) : NULL;
    *icon = desktop_icon ? g_object_ref(desktop_icon) : g_themed_icon_new("application-x-executable-symbolic");
  }
  g_list_free_full(all, g_object_unref);
  return result;
}
