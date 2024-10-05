#include "nip94.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static char *strdup_safe(const char *src) {
    if (src == NULL) {
        return NULL;
    }
    size_t len = strlen(src) + 1;
    char *dest = malloc(len);
    if (dest) {
        memcpy(dest, src, len);
    }
    return dest;
}

FileMetadata parse_file_metadata(const char *event_content) {
    FileMetadata fm = {0};

    const char *tag_start = event_content;
    const char *tag_end = NULL;
    while ((tag_start = strstr(tag_start, "\"tag\":[")) != NULL) {
        tag_start += 7; // skip past '"tag":['
        tag_end = strchr(tag_start, ']');
        if (tag_end == NULL) {
            break;
        }

        char *tag_content = strndup(tag_start, tag_end - tag_start);
        if (tag_content == NULL) {
            continue;
        }

        char *tag = strtok(tag_content, ",");
        if (tag == NULL) {
            free(tag_content);
            continue;
        }
        tag = strtok(NULL, "\"");
        char *value = strtok(NULL, "\"");
        if (value == NULL) {
            free(tag_content);
            continue;
        }

        if (strcmp(tag, "url") == 0) {
            fm.URL = strdup_safe(value);
        } else if (strcmp(tag, "x") == 0) {
            fm.X = strdup_safe(value);
        } else if (strcmp(tag, "ox") == 0) {
            fm.OX = strdup_safe(value);
        } else if (strcmp(tag, "size") == 0) {
            fm.Size = strdup_safe(value);
        } else if (strcmp(tag, "dim") == 0) {
            fm.Dim = strdup_safe(value);
        } else if (strcmp(tag, "magnet") == 0) {
            fm.Magnet = strdup_safe(value);
        } else if (strcmp(tag, "i") == 0) {
            fm.TorrentInfoHash = strdup_safe(value);
        } else if (strcmp(tag, "blurhash") == 0) {
            fm.Blurhash = strdup_safe(value);
        } else if (strcmp(tag, "thumb") == 0) {
            fm.Thumb = strdup_safe(value);
        } else if (strcmp(tag, "summary") == 0) {
            fm.Summary = strdup_safe(value);
        }

        free(tag_content);
    }

    return fm;
}

bool is_video(const FileMetadata *fm) {
    return fm->M != NULL && strncmp(fm->M, "video", 5) == 0;
}

bool is_image(const FileMetadata *fm) {
    return fm->M != NULL && strncmp(fm->M, "image", 5) == 0;
}

char *display_image(const FileMetadata *fm) {
    if (fm->Image != NULL) {
        return strdup_safe(fm->Image);
    } else if (is_image(fm)) {
        return strdup_safe(fm->URL);
    } else {
        return NULL;
    }
}

void free_file_metadata(FileMetadata *fm) {
    if (fm->Magnet) free(fm->Magnet);
    if (fm->Dim) free(fm->Dim);
    if (fm->Size) free(fm->Size);
    if (fm->Summary) free(fm->Summary);
    if (fm->Image) free(fm->Image);
    if (fm->URL) free(fm->URL);
    if (fm->M) free(fm->M);
    if (fm->X) free(fm->X);
    if (fm->OX) free(fm->OX);
    if (fm->TorrentInfoHash) free(fm->TorrentInfoHash);
    if (fm->Blurhash) free(fm->Blurhash);
    if (fm->Thumb) free(fm->Thumb);
}
