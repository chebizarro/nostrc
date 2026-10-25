#include <blossom/types.h>
#include <stdlib.h>
void blossom_blob_descriptor_free(blossom_blob_descriptor_t *desc)
{
    if (!desc)
        return;
    free(desc->url);
    free(desc->mime_type);
    free(desc);
}
