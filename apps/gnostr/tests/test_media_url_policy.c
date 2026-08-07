#include <glib.h>
#include "../src/util/utils.h"

static void
assert_unsafe(const char *url, GnostrMediaPolicyError code)
{
  g_autoptr(GError) error = NULL;
  g_assert_false(gnostr_media_url_is_safe(url, &error));
  g_assert_error(error, GNOSTR_MEDIA_POLICY_ERROR, code);
}

static void
test_accepts_public_http_urls(void)
{
  g_assert_true(gnostr_media_url_is_safe(
      "https://93.184.216.34/image.png", NULL));
  g_assert_true(gnostr_media_url_is_safe(
      "http://8.8.8.8/avatar.png", NULL));
}

static void
test_rejects_invalid_and_credentials(void)
{
  assert_unsafe("file:///etc/passwd", GNOSTR_MEDIA_POLICY_ERROR_INVALID_URL);
  assert_unsafe("https://user:secret@93.184.216.34/image.png",
                GNOSTR_MEDIA_POLICY_ERROR_CREDENTIALS);
  assert_unsafe("https://93.184.216.34:8080/image.png",
                GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_PORT);
  assert_unsafe("https:///missing-host",
                GNOSTR_MEDIA_POLICY_ERROR_INVALID_URL);
}

static void
test_rejects_local_private_and_reserved_ipv4(void)
{
  const char *urls[] = {
    "http://127.0.0.1/",
    "http://10.0.0.1/",
    "http://172.16.0.1/",
    "http://192.168.1.1/",
    "http://169.254.169.254/latest/meta-data/",
    "http://100.64.0.1/",
    "http://192.0.2.1/",
    "http://192.88.99.1/",
    "http://198.51.100.1/",
    "http://203.0.113.1/",
    "http://224.0.0.1/",
    NULL
  };
  for (guint i = 0; urls[i]; i++)
    assert_unsafe(urls[i], GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS);
}

static void
test_rejects_local_and_reserved_ipv6(void)
{
  const char *urls[] = {
    "http://[::1]/",
    "http://[::]/",
    "http://[fe80::1]/",
    "http://[fc00::1]/",
    "http://[2001::1]/",
    "http://[2001:db8::1]/",
    "http://[2002::1]/",
    "http://[3fff::1]/",
    "http://[100::1]/",
    "http://[::ffff:127.0.0.1]/",
    NULL
  };
  for (guint i = 0; urls[i]; i++)
    assert_unsafe(urls[i], GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS);
}

static void
test_rejects_localhost_names(void)
{
  assert_unsafe("http://localhost/image",
                GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS);
  assert_unsafe("http://tracker.localhost/image",
                GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS);
}

static void
test_redirect_policy(void)
{
  g_autofree char *resolved = NULL;
  g_assert_true(gnostr_media_redirect_is_safe(
      "https://93.184.216.34/start", "/image.png", &resolved, NULL));
  g_assert_cmpstr(resolved, ==, "https://93.184.216.34/image.png");

  g_autoptr(GError) error = NULL;
  g_assert_false(gnostr_media_redirect_is_safe(
      "https://93.184.216.34/start",
      "https://8.8.8.8/image.png", NULL, &error));
  g_assert_error(error, GNOSTR_MEDIA_POLICY_ERROR,
                 GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_REDIRECT);

  g_clear_error(&error);
  g_assert_false(gnostr_media_redirect_is_safe(
      "https://93.184.216.34/start",
      "http://93.184.216.34/image.png", NULL, &error));
  g_assert_error(error, GNOSTR_MEDIA_POLICY_ERROR,
                 GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_REDIRECT);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/media-policy/public", test_accepts_public_http_urls);
  g_test_add_func("/media-policy/invalid-credentials",
                  test_rejects_invalid_and_credentials);
  g_test_add_func("/media-policy/private-ipv4",
                  test_rejects_local_private_and_reserved_ipv4);
  g_test_add_func("/media-policy/private-ipv6",
                  test_rejects_local_and_reserved_ipv6);
  g_test_add_func("/media-policy/localhost", test_rejects_localhost_names);
  g_test_add_func("/media-policy/redirect", test_redirect_policy);
  return g_test_run();
}
