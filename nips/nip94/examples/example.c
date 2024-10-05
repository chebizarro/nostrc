#include <stdio.h>
#include "nip94.h"

int main() {
    const char *event_content = 
        "{\"tags\":["
        "[\"url\",\"http://example.com/video.mp4\"],"
        "[\"m\",\"video/mp4\"],"
        "[\"size\",\"12345\"],"
        "[\"dim\",\"1920x1080\"],"
        "[\"magnet\",\"magnet:?xt=urn:btih:...\"],"
        "[\"i\",\"infohash\"],"
        "[\"blurhash\",\"LEHV6nWB2yk8pyo0adR*.7kCMdnj\"],"
        "[\"thumb\",\"http://example.com/thumb.jpg\"],"
        "[\"summary\",\"Example summary\"]"
        "]}";

    FileMetadata fm = parse_file_metadata(event_content);
    
    printf("URL: %s\n", fm.URL);
    printf("Media Type: %s\n", fm.M);
    printf("Is Video: %d\n", is_video(&fm));
    printf("Display Image: %s\n", display_image(&fm));

    free_file_metadata(&fm);
    return 0;
}
