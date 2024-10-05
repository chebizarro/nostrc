#ifndef NIP94_H
#define NIP94_H

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

FileMetadata parse_file_metadata(const char *event_content);
bool is_video(const FileMetadata *fm);
bool is_image(const FileMetadata *fm);
char *display_image(const FileMetadata *fm);
void free_file_metadata(FileMetadata *fm);

#endif // NIP94_H
