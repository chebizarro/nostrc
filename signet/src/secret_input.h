#ifndef SIGNET_SECRET_INPUT_H
#define SIGNET_SECRET_INPUT_H

/* 0 = present, 1 = absent, -1 = invalid. Free *out with sodium_free().
 * Diagnostics identify only the input name, never secret contents. */
int signet_secret_input_resolve(const char *value_env, const char *file_env,
                                char **out);
int signet_secret_input_file(const char *path, char **out);

#endif
