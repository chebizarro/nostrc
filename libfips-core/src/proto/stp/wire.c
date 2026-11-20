#include <fips/proto/stp/wire.h>
#include <fips/proto/link.h>
#include "internal.h"
size_t fips_tree_announce_encoded_len(const fips_tree_announce *a) {
    return 100u + 32u * a->ancestry.len;
}
fips_status fips_tree_announce_encode(const fips_tree_announce *a, fips_writer *w,
                                      fips_proto_error *e) {
    if (!a->declaration.is_signed)
        return wire_error(e, FIPS_PROTO_InvalidSignature);
    if (writer_require(w, fips_tree_announce_encoded_len(a), e) != FIPS_OK)
        return FIPS_ERR_RANGE;
    fips_writer_write_u8(w, FIPS_LINK_TreeAnnounce, NULL);
    fips_writer_write_u8(w, FIPS_TREE_ANNOUNCE_VERSION_1, NULL);
    fips_writer_write_u64_le(w, a->declaration.sequence, NULL);
    fips_writer_write_u64_le(w, a->declaration.timestamp, NULL);
    fips_writer_write_bytes(w, a->declaration.parent_id.bytes, 16, NULL);
    fips_writer_write_u16_le(w, a->ancestry.len, NULL);
    for (size_t i = 0; i < a->ancestry.len; i++)
        fips_coord_entry_encode(&a->ancestry.entries[i], w, NULL);
    fips_writer_write_bytes(w, a->declaration.signature, 64, NULL);
    return FIPS_OK;
}
fips_status fips_tree_announce_decode(fips_reader *r, fips_tree_announce *out,
                                      fips_proto_error *e) {
    fips_reader q = *r;
    if (fips_reader_require(&q, 99, e) != FIPS_OK)
        return FIPS_ERR_RANGE;
    uint8_t version;
    uint16_t count;
    fips_parent_declaration d = {0};
    fips_reader_read_u8(&q, &version, NULL);
    if (version != 1) {
        if (e)
            *e = (fips_proto_error){.variant = FIPS_PROTO_UnsupportedVersion,
                                    .as.unsupported_version = version};
        return FIPS_ERR_RANGE;
    }
    fips_reader_read_u64_le(&q, &d.sequence, NULL);
    fips_reader_read_u64_le(&q, &d.timestamp, NULL);
    fips_reader_read_array(&q, d.parent_id.bytes, 16, NULL);
    fips_reader_read_u16_le(&q, &count, NULL);
    if (fips_reader_require(&q, (size_t)count * 32 + 64, e) != FIPS_OK)
        return FIPS_ERR_RANGE;
    if (!count) {
        if (e)
            *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed,
                                    .as.malformed = "ancestry must have at least one entry"};
        return FIPS_ERR_RANGE;
    }
    /* All fallible checks precede writes; avoid a 2 MiB temporary on the stack. */
    for (size_t i = 0; i < count; i++)
        fips_coord_entry_decode(&q, &out->ancestry.entries[i], NULL);
    d.node_addr = out->ancestry.entries[0].node_addr;
    d.is_signed = true;
    fips_reader_read_array(&q, d.signature, 64, NULL);
    out->ancestry.len = count;
    out->declaration = d;
    *r = q;
    return FIPS_OK;
}
fips_status fips_tree_announce_validate_semantics(const fips_tree_announce *a, fips_tree_error *e) {
    const fips_tree_coordinate *c = &a->ancestry;
    fips_tree_error err = {0};
    if (!c->len)
        err.variant = FIPS_TREE_EmptyCoordinate;
    else if (addr_cmp(c->entries[0].node_addr, a->declaration.node_addr)) {
        err.variant = FIPS_TREE_AncestryNodeMismatch;
        err.declared = a->declaration.node_addr;
        err.ancestry = c->entries[0].node_addr;
    } else if (fips_parent_declaration_is_root(&a->declaration) && c->len != 1)
        err.variant = FIPS_TREE_RootDeclarationMismatch;
    else if (!fips_parent_declaration_is_root(&a->declaration) && c->len < 2)
        err.variant = FIPS_TREE_AncestryTooShort;
    else if (!fips_parent_declaration_is_root(&a->declaration) &&
             addr_cmp(c->entries[1].node_addr, a->declaration.parent_id)) {
        err.variant = FIPS_TREE_AncestryParentMismatch;
        err.declared = a->declaration.parent_id;
        err.ancestry = c->entries[1].node_addr;
    } else {
        fips_node_addr minimum = c->entries[0].node_addr, root = *fips_tree_coordinate_root_id(c);
        for (size_t i = 1; i < c->len; i++)
            if (addr_cmp(c->entries[i].node_addr, minimum) < 0)
                minimum = c->entries[i].node_addr;
        if (!addr_cmp(root, minimum))
            return FIPS_OK;
        err.variant = FIPS_TREE_AncestryRootNotMinimum;
        err.declared = root;
        err.ancestry = minimum;
    }
    if (e)
        *e = err;
    return FIPS_ERR_RANGE;
}
