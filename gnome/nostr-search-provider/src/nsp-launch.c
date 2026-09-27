/* nsp-launch.c — see nsp-launch.h. */
#include "nsp-launch.h"

#include <gio/gio.h>
#include <string.h>

char *nsp_search_join_terms(const char *const *terms) {
  GString *s = g_string_new(NULL);
  for (guint i = 0; terms && terms[i]; i++) {
    if (!*terms[i]) continue;
    if (s->len) g_string_append_c(s, ' ');
    g_string_append(s, terms[i]);
  }
  return g_string_free(s, FALSE);
}

static gboolean option_valid(const char *arg) {
  if (!arg || arg[0] != '-' || !arg[1] || strchr(arg, '=')) return FALSE;
  for (const char *p = arg; *p; p++)
    if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') return FALSE;
  return TRUE;
}

gboolean nsp_search_entry_from_keyfile(GKeyFile *kf, char **out_search_arg,
                                       char **out_exec, gboolean *out_has_action) {
  *out_search_arg = NULL;
  *out_exec = NULL;
  if (out_has_action) *out_has_action = FALSE;
  const char *main_group = G_KEY_FILE_DESKTOP_GROUP;
  g_autofree char *arg = g_key_file_get_string(kf, main_group, NSP_SEARCH_ARG_KEY, NULL);
  if (arg) g_strstrip(arg);
  if (!option_valid(arg)) return FALSE;

  gboolean has_action = FALSE;
  g_auto(GStrv) actions = g_key_file_get_string_list(kf, main_group, "Actions", NULL, NULL);
  for (guint i = 0; actions && actions[i]; i++)
    if (g_strcmp0(actions[i], NSP_SEARCH_ACTION) == 0) has_action = TRUE;
  char *exec = NULL;
  if (has_action)
    exec = g_key_file_get_string(kf, "Desktop Action " NSP_SEARCH_ACTION, "Exec", NULL);
  if (!exec) {
    has_action = FALSE; /* declared but empty: not launchable as an action */
    exec = g_key_file_get_string(kf, main_group, "Exec", NULL);
  }
  if (!exec || !*g_strstrip(exec)) {
    g_free(exec);
    return FALSE;
  }
  *out_search_arg = g_steal_pointer(&arg);
  *out_exec = exec;
  if (out_has_action) *out_has_action = has_action;
  return TRUE;
}

/* One Exec argument after field-code processing: NULL = drop it. */
static gboolean expand_field_codes(const char *in, char **out, GError **error) {
  static const char *const drop[] = {"%f", "%F", "%u", "%U", "%d", "%D", "%n", "%N",
                                     "%i", "%c", "%k", "%v", "%m", "@@", "@@u", "@@f",
                                     NULL};
  *out = NULL;
  if (g_strv_contains(drop, in)) return TRUE;
  GString *s = g_string_new(NULL);
  for (const char *p = in; *p; p++) {
    if (*p != '%') {
      g_string_append_c(s, *p);
      continue;
    }
    if (p[1] == '%') {
      g_string_append_c(s, '%');
      p++;
      continue;
    }
    g_string_free(s, TRUE);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "unsupported field code inside Exec argument '%s'", in);
    return FALSE;
  }
  *out = g_string_free(s, FALSE);
  return TRUE;
}

char **nsp_search_argv(const char *exec, const char *search_arg,
                       const char *const *terms, GError **error) {
  if (!option_valid(search_arg)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "invalid " NSP_SEARCH_ARG_KEY " '%s'", search_arg ? search_arg : "");
    return NULL;
  }
  g_autofree char *joined = nsp_search_join_terms(terms);
  if (strlen(joined) > NSP_SEARCH_TERMS_MAX || !g_utf8_validate(joined, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "search terms are too long or not UTF-8");
    return NULL;
  }
  for (const char *p = joined; *p; p = g_utf8_next_char(p))
    if (g_unichar_iscntrl(g_utf8_get_char(p))) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "search terms contain control characters");
      return NULL;
    }

  g_auto(GStrv) parsed = NULL;
  if (!exec || !g_shell_parse_argv(exec, NULL, &parsed, error)) return NULL;

  gboolean long_opt = g_str_has_prefix(search_arg, "--");
  g_autofree char *prefix = g_strconcat(search_arg, "=", NULL);
  g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
  gboolean placed = FALSE;
  for (guint i = 0; parsed[i]; i++) {
    char *arg = NULL;
    if (!expand_field_codes(parsed[i], &arg, error)) return NULL;
    if (!arg) continue;
    gboolean is_opt = g_strcmp0(arg, search_arg) == 0;
    gboolean is_opt_eq = long_opt && g_str_has_prefix(arg, prefix);
    if ((is_opt || is_opt_eq) && !placed && argv->len > 0) {
      g_free(arg);
      if (is_opt && parsed[i + 1]) i++; /* drop the placeholder value */
      if (long_opt) {
        g_ptr_array_add(argv, g_strconcat(search_arg, "=", joined, NULL));
      } else {
        g_ptr_array_add(argv, g_strdup(search_arg));
        g_ptr_array_add(argv, g_strdup(joined));
      }
      placed = TRUE;
      continue;
    }
    g_ptr_array_add(argv, arg);
  }
  if (argv->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty Exec line");
    return NULL;
  }
  if (!placed) {
    if (long_opt) {
      g_ptr_array_add(argv, g_strconcat(search_arg, "=", joined, NULL));
    } else {
      g_ptr_array_add(argv, g_strdup(search_arg));
      g_ptr_array_add(argv, g_strdup(joined));
    }
  }
  g_ptr_array_add(argv, NULL);
  return (char **)g_ptr_array_free(g_steal_pointer(&argv), FALSE);
}
