#ifndef NIP94_H
#define NIP94_H

#include <nostr/nip94/export.h>

#include <stdbool.h>

typedef struct {
    char *Magnet;
    char *Dim;
    char *Size;
    char *Summary;
    char *Image;
    char *URL;
    char *M;
    char *X;
    char *OX;
    char *TorrentInfoHash;
    char *Blurhash;
    char *Thumb;
} FileMetadata;

NOSTR_NIP94_API FileMetadata parse_file_metadata(const char *event_content);
NOSTR_NIP94_API bool is_video(const FileMetadata *fm);
NOSTR_NIP94_API bool is_image(const FileMetadata *fm);
NOSTR_NIP94_API char *display_image(const FileMetadata *fm);
NOSTR_NIP94_API void free_file_metadata(FileMetadata *fm);

#endif // NIP94_H
