#ifndef FIPS_TEST_SETTLE_TABLE_H
#define FIPS_TEST_SETTLE_TABLE_H
#include <fips/proto/settle/table.h>
#include <stdlib.h>
#include <string.h>
/* Host-side allocation only. The core receives two ordinary caller arenas. */
typedef struct fips_test_settle_table { fips_settle_table table; } fips_test_settle_table;
static inline void *fips_test_settle_array(size_t n,size_t size)
{ void *p=calloc(n ? n : 1,size); if (!p) abort(); return p; }
static inline fips_test_settle_table *fips_test_settle_table_create(fips_node_addr me,
    const fips_settle_params *p,size_t capacity,size_t paths,size_t outputs,size_t closes)
{
    fips_test_settle_table *f=fips_test_settle_array(1,sizeof(*f)); fips_settle_table_storage storage[2];
    for (unsigned i=0;i<2;++i) {
        storage[i]=(fips_settle_table_storage){
            .links=fips_test_settle_array(capacity,sizeof(fips_settle_link)),
            .paths=fips_test_settle_array(capacity*paths,sizeof(fips_settle_path)),
            .legs=fips_test_settle_array(capacity,sizeof(fips_settle_leg)),
            .meta=fips_test_settle_array(capacity,sizeof(fips_settle_meta)),
            .held=fips_test_settle_array(capacity,sizeof(fips_settle_held)),
            .memory=fips_test_settle_array(capacity,sizeof(fips_settle_memory)),
            .outputs=fips_test_settle_array(outputs,sizeof(fips_settle_output)),
            .closes=fips_test_settle_array(closes,sizeof(fips_settle_pair)),
            .capacity=capacity,.paths_per_link=paths,.gate_capacity=capacity,.output_capacity=outputs,.close_capacity=closes};
    }
    if (fips_settle_table_init(&f->table,me,p,storage)!=FIPS_OK) abort(); return f;
}
static inline void fips_test_settle_drain(fips_settle_table *t)
{
    fips_settle_outputs out=fips_settle_table_outputs(t);
    for (size_t i=0;i<out.count;++i) if (out.items[i].lease.live) {
        uint32_t index; if (!fips_rekey_index_lease_release(&out.items[i].lease,&index)) abort();
    }
}
static inline void fips_test_settle_table_destroy(fips_test_settle_table *f,fips_instant now)
{
    fips_test_settle_drain(&f->table);
    if (fips_settle_table_close(&f->table,now)!=FIPS_OK) abort();
    fips_test_settle_drain(&f->table);
    for (unsigned i=0;i<2;++i) {
        fips_settle_table_storage *s=&f->table.states[i].storage;
        free(s->links); free(s->paths); free(s->legs); free(s->meta); free(s->held);
        free(s->memory); free(s->outputs); free(s->closes);
    }
    free(f);
}
#endif
