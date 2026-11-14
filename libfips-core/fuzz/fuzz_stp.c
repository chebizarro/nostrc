#include <fips/proto/stp/mod.h>
#include <stdlib.h>
#include <string.h>
static fips_tree_announce announce, again;
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size)
        return 0;
    bool tree = (data[0] & 1) != 0;
    data++;
    size--;
    fips_reader r;
    fips_reader_init(&r, data, size);
    fips_proto_error error;
    if (tree) {
        if (fips_tree_announce_decode(&r, &announce, &error) != FIPS_OK) {
            if (r.pos)
                abort();
            return 0;
        }
        (void)fips_tree_announce_validate_semantics(&announce, NULL);
        size_t n = fips_tree_announce_encoded_len(&announce);
        uint8_t *encoded = malloc(n);
        if (!encoded)
            return 0;
        fips_writer w;
        fips_writer_init(&w, encoded, n);
        if (fips_tree_announce_encode(&announce, &w, NULL) != FIPS_OK || w.pos != n ||
            n - 1 != r.pos || memcmp(encoded + 1, data, r.pos))
            abort();
        fips_reader q;
        fips_reader_init(&q, encoded + 1, n - 1);
        if (fips_tree_announce_decode(&q, &again, NULL) != FIPS_OK ||
            !fips_parent_declaration_equal(&announce.declaration, &again.declaration) ||
            !fips_tree_coordinate_equal(&announce.ancestry, &again.ancestry))
            abort();
        free(encoded);
    } else {
        fips_parent_declaration d;
        if (fips_parent_declaration_decode(&r, &d, &error) != FIPS_OK) {
            if (r.pos)
                abort();
            return 0;
        }
        uint8_t bytes[48];
        fips_writer w;
        fips_writer_init(&w, bytes, 48);
        if (fips_parent_declaration_signing_bytes(&d, &w, NULL) != FIPS_OK || w.pos != 48 ||
            r.pos != 48 || memcmp(bytes, data, 48))
            abort();
    }
    return 0;
}
