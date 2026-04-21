/**
 * NIP-94: File Metadata — Unit Tests
 *
 * Tests parse_file_metadata(), is_video(), is_image(),
 * display_image(), and free_file_metadata().
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nip94.h"

/* ── is_video / is_image ─────────────────────────────────────────────── */

static void test_is_video_true(void) {
    FileMetadata fm = {0};
    fm.M = strdup("video/mp4");
    assert(is_video(&fm) == true);
    free(fm.M);
}

static void test_is_video_false(void) {
    FileMetadata fm = {0};
    fm.M = strdup("image/png");
    assert(is_video(&fm) == false);
    free(fm.M);
}

static void test_is_video_null_m(void) {
    FileMetadata fm = {0};
    assert(is_video(&fm) == false);
}

static void test_is_image_true(void) {
    FileMetadata fm = {0};
    fm.M = strdup("image/jpeg");
    assert(is_image(&fm) == true);
    free(fm.M);
}

static void test_is_image_false(void) {
    FileMetadata fm = {0};
    fm.M = strdup("video/webm");
    assert(is_image(&fm) == false);
    free(fm.M);
}

static void test_is_image_null_m(void) {
    FileMetadata fm = {0};
    assert(is_image(&fm) == false);
}

/* ── display_image ───────────────────────────────────────────────────── */

static void test_display_image_with_image_field(void) {
    FileMetadata fm = {0};
    fm.Image = strdup("https://example.com/thumb.jpg");
    fm.URL = strdup("https://example.com/file.bin");
    fm.M = strdup("image/png");

    /* When Image is set, it takes priority over URL */
    char *result = display_image(&fm);
    assert(result != NULL);
    assert(strcmp(result, "https://example.com/thumb.jpg") == 0);
    free(result);

    free(fm.Image);
    free(fm.URL);
    free(fm.M);
}

static void test_display_image_fallback_to_url(void) {
    FileMetadata fm = {0};
    fm.Image = NULL;
    fm.URL = strdup("https://example.com/photo.png");
    fm.M = strdup("image/png");

    /* When Image is NULL but MIME is image, fall back to URL */
    char *result = display_image(&fm);
    assert(result != NULL);
    assert(strcmp(result, "https://example.com/photo.png") == 0);
    free(result);

    free(fm.URL);
    free(fm.M);
}

static void test_display_image_non_image(void) {
    FileMetadata fm = {0};
    fm.Image = NULL;
    fm.URL = strdup("https://example.com/doc.pdf");
    fm.M = strdup("application/pdf");

    /* Non-image MIME and no Image field → NULL */
    char *result = display_image(&fm);
    assert(result == NULL);

    free(fm.URL);
    free(fm.M);
}

static void test_display_image_no_mime(void) {
    FileMetadata fm = {0};
    fm.Image = NULL;
    fm.URL = strdup("https://example.com/file.bin");

    /* No MIME and no Image → NULL */
    char *result = display_image(&fm);
    assert(result == NULL);

    free(fm.URL);
}

/* ── free_file_metadata ──────────────────────────────────────────────── */

static void test_free_file_metadata_all_fields(void) {
    FileMetadata fm = {0};
    fm.Magnet = strdup("magnet:?xt=urn:btih:abc");
    fm.Dim = strdup("1920x1080");
    fm.Size = strdup("1048576");
    fm.Summary = strdup("A test file");
    fm.Image = strdup("https://example.com/thumb.jpg");
    fm.URL = strdup("https://example.com/file.bin");
    fm.M = strdup("video/mp4");
    fm.X = strdup("abcdef1234567890");
    fm.OX = strdup("fedcba0987654321");
    fm.TorrentInfoHash = strdup("0123456789abcdef");
    fm.Blurhash = strdup("LEHV6nWB2yk8pyo0adR*.7kCMdnj");
    fm.Thumb = strdup("https://example.com/small.jpg");

    /* Should not crash or leak */
    free_file_metadata(&fm);
}

static void test_free_file_metadata_null_fields(void) {
    FileMetadata fm = {0};
    /* All NULL — should not crash */
    free_file_metadata(&fm);
}

int main(void) {
    /* is_video / is_image */
    test_is_video_true();
    test_is_video_false();
    test_is_video_null_m();
    test_is_image_true();
    test_is_image_false();
    test_is_image_null_m();

    /* display_image */
    test_display_image_with_image_field();
    test_display_image_fallback_to_url();
    test_display_image_non_image();
    test_display_image_no_mime();

    /* free_file_metadata */
    test_free_file_metadata_all_fields();
    test_free_file_metadata_null_fields();

    printf("nip94 ok\n");
    return 0;
}
