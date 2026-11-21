#include <fips/proto/settle/settle.h>
#include <string.h>

static fips_settle_link *find(const fips_settle_links *s, uint64_t n)
{
    for (size_t i=0; i<s->capacity; ++i)
        if (s->slots[i].active && s->slots[i].number==n) return &s->slots[i];
    return NULL;
}
bool fips_settle_pair_equal(const fips_settle_pair *a, const fips_settle_pair *b)
{
    return a->transport.value==b->transport.value && fips_transport_addr_equal(&a->addr,&b->addr);
}
static bool valid(const fips_settle_pair *p)
{
    return p->addr.len<=FIPS_TRANSPORT_ADDR_CAPACITY;
}
static bool counts(fips_settle_proven p) { return p==FIPS_SETTLE_LIVE || p==FIPS_SETTLE_ACKED; }
static bool proven(const fips_settle_link *r, const fips_settle_pair *p)
{
    for (size_t i=0; i<r->path_count; ++i)
        if (counts(r->paths[i].state) && fips_settle_pair_equal(&r->paths[i].pair,p)) return true;
    return false;
}
static bool uses(const fips_settle_link *r, const fips_settle_pair *p)
{
    return (fips_settle_pair_equal(&r->setup,p) && (!r->path_count || !r->holder.peer)) || proven(r,p);
}
fips_status fips_settle_links_init(fips_settle_links *s, fips_settle_link *slots,
    size_t capacity, fips_settle_path *paths, size_t paths_per_link)
{
    if (capacity>SIZE_MAX/sizeof(*slots) || (paths_per_link && capacity>SIZE_MAX/paths_per_link) ||
        (capacity && paths_per_link>SIZE_MAX/sizeof(*paths)/capacity)) return FIPS_ERR_RANGE;
    if (capacity) memset(slots,0,capacity*sizeof(*slots));
    for (size_t i=0; i<capacity; ++i) slots[i].paths=paths_per_link ? paths+i*paths_per_link : NULL;
    *s=(fips_settle_links){.slots=slots,.capacity=capacity,.paths_per_link=paths_per_link};
    return FIPS_OK;
}
fips_status fips_settle_link_open(fips_settle_links *s, const fips_settle_pair *pair,
    fips_link_direction direction, fips_settle_holder holder, uint64_t *number)
{
    if (!valid(pair) || (direction!=FIPS_LINK_INBOUND && direction!=FIPS_LINK_OUTBOUND) || s->next==UINT64_MAX)
        return FIPS_ERR_RANGE;
    for (size_t i=0; i<s->capacity; ++i) {
        fips_settle_link *r=&s->slots[i];
        if (r->active) continue;
        fips_settle_path *paths=r->paths;
        *r=(fips_settle_link){.active=true,.number=s->next,.setup=*pair,
            .direction=direction,.holder=holder,.paths=paths};
        *number=s->next++;
        return FIPS_OK;
    }
    return FIPS_ERR_RANGE;
}
const fips_settle_link *fips_settle_link_get(const fips_settle_links *s, uint64_t n) { return find(s,n); }
void fips_settle_link_hold(fips_settle_links *s, uint64_t n, fips_settle_holder holder)
{
    fips_settle_link *r=find(s,n);
    if (r) r->holder=holder;
}
void fips_settle_link_capable(fips_settle_links *s, uint64_t n)
{
    fips_settle_link *r=find(s,n);
    if (r) r->capable=true;
}
fips_status fips_settle_link_path(fips_settle_links *s, uint64_t n,
    const fips_settle_pair *pair, fips_settle_proven state)
{
    if (!valid(pair) || (state!=FIPS_SETTLE_LIVE && state!=FIPS_SETTLE_ACKED && state!=FIPS_SETTLE_PROBING)) return FIPS_ERR_RANGE;
    fips_settle_link *r=find(s,n);
    if (!r) return FIPS_OK;
    for (size_t i=0; i<r->path_count; ++i) {
        if (fips_settle_pair_equal(&r->paths[i].pair,pair)) {
            r->paths[i].state=state;
            return FIPS_OK;
        }
    }
    if (r->path_count==s->paths_per_link) return FIPS_ERR_RANGE;
    r->paths[r->path_count++]=(fips_settle_path){.pair=*pair,.state=state};
    return FIPS_OK;
}
fips_status fips_settle_link_moved(fips_settle_links *s, uint64_t n,
    const fips_settle_pair *from, const fips_settle_pair *to)
{
    if (!valid(from) || !valid(to)) return FIPS_ERR_RANGE;
    fips_settle_link *r=find(s,n);
    if (!r) return FIPS_OK;
    for (size_t i=0; i<r->path_count; ++i) {
        if (fips_settle_pair_equal(&r->paths[i].pair,from)) {
            r->paths[i].pair=*to;
            return FIPS_OK;
        }
    }
    /* Missing from appends even when to already exists, as Rust does. */
    if (r->path_count==s->paths_per_link) return FIPS_ERR_RANGE;
    r->paths[r->path_count++]=(fips_settle_path){.pair=*to,.state=FIPS_SETTLE_LIVE};
    return FIPS_OK;
}
bool fips_settle_link_indexed(const fips_settle_links *s, uint64_t n, const fips_settle_pair *pair)
{
    const fips_settle_link *r=find(s,n);
    return r && ((!r->released && fips_settle_pair_equal(&r->setup,pair)) || proven(r,pair));
}
bool fips_settle_link_in_use(const fips_settle_links *s, const fips_settle_pair *pair, uint64_t except)
{
    for (size_t i=0; i<s->capacity; ++i) {
        const fips_settle_link *r=&s->slots[i];
        if (r->active && r->number!=except && uses(r,pair)) return true;
    }
    return false;
}
static const fips_settle_pair *address(const fips_settle_link *r, size_t i)
{
    return i ? &r->paths[i-1].pair : &r->setup;
}
static bool close_at(const fips_settle_links *s, const fips_settle_link *r, size_t i)
{
    const fips_settle_pair *p=address(r,i);
    for (size_t j=0; j<i; ++j) if (fips_settle_pair_equal(address(r,j),p)) return false;
    return !fips_settle_link_in_use(s,p,r->number);
}
fips_status fips_settle_link_retire(fips_settle_links *s, uint64_t n,
    fips_settle_pair *closes, size_t capacity, size_t *count)
{
    fips_settle_link *r=find(s,n);
    size_t needed=0;
    if (r) for (size_t i=0; i<=r->path_count; ++i) if (close_at(s,r,i)) ++needed;
    if (needed>capacity) return FIPS_ERR_RANGE;
    size_t len=0;
    if (r) {
        for (size_t i=0; i<=r->path_count; ++i) if (close_at(s,r,i)) closes[len++]=*address(r,i);
        fips_settle_path *paths=r->paths;
        *r=(fips_settle_link){.paths=paths};
    }
    *count=len;
    return FIPS_OK;
}
fips_status fips_settle_link_release(fips_settle_links *s, uint64_t n,
    const fips_settle_pair *pair, bool keep, fips_settle_pair *closes, size_t capacity, size_t *count)
{
    if (!valid(pair)) return FIPS_ERR_RANGE;
    fips_settle_link *r=find(s,n);
    if (!r) { *count=0; return FIPS_OK; }
    size_t left=0;
    for (size_t i=0; i<r->path_count; ++i) if (!fips_settle_pair_equal(&r->paths[i].pair,pair)) ++left;
    bool own=fips_settle_pair_equal(&r->setup,pair) && (!left || !r->holder.peer);
    bool close=!keep && !own && !fips_settle_link_in_use(s,pair,n);
    if (close && !capacity) return FIPS_ERR_RANGE;
    left=0;
    for (size_t i=0; i<r->path_count; ++i)
        if (!fips_settle_pair_equal(&r->paths[i].pair,pair)) r->paths[left++]=r->paths[i];
    r->path_count=left;
    if (fips_settle_pair_equal(&r->setup,pair)) r->released=true;
    if (close) closes[0]=*pair;
    *count=close ? 1 : 0;
    return FIPS_OK;
}
fips_settle_placement fips_settle_place(const fips_settle_links *s,
    const fips_settle_pair *pair, uint64_t held, fips_settle_expects expects, void *ctx)
{
    fips_settle_placement p={0};
    const fips_settle_link *named=NULL, *fallback=NULL, *on=find(s,held);
    bool has_path=false;
    for (size_t i=0; i<s->capacity; ++i) {
        const fips_settle_link *r=&s->slots[i];
        if (!r->active) continue;
        if (fips_settle_link_indexed(s,r->number,pair) && (!named || r->number>named->number)) named=r;
        /* Placement uses the first matching path, unlike the derived index. */
        for (size_t j=0; j<r->path_count; ++j) {
            if (!fips_settle_pair_equal(&r->paths[j].pair,pair)) continue;
            bool c=counts(r->paths[j].state);
            if (r->holder.peer && c && (!fallback || r->number<fallback->number)) fallback=r;
            if (r==on) { has_path=true; p.on_link=c; }
            break;
        }
    }
    p.established=named || fallback;
    if (named) {
        if (named->holder.peer) { p.has_expected=true; p.expected=named->holder.identity; }
        else if (expects) p.has_expected=expects(ctx,named->number,&p.expected);
        p.has_unanswered=!named->holder.peer && named->direction==FIPS_LINK_INBOUND;
        if (p.has_unanswered) p.unanswered=named->number;
    }
    if (!p.has_expected && fallback) { p.has_expected=true; p.expected=fallback->holder.identity; }
    if (!has_path) p.on_link=named && named->number==held && !named->capable;
    return p;
}
