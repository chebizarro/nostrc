#include <fips/utils/hash.h>
#include <string.h>

static uint64_t le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static uint64_t rotl(uint64_t x, unsigned n) { return (x << n) | (x >> (64 - n)); }
static void round13(uint64_t v[4])
{
    v[0] += v[1]; v[1] = rotl(v[1], 13); v[1] ^= v[0]; v[0] = rotl(v[0], 32);
    v[2] += v[3]; v[3] = rotl(v[3], 16); v[3] ^= v[2];
    v[0] += v[3]; v[3] = rotl(v[3], 21); v[3] ^= v[0];
    v[2] += v[1]; v[1] = rotl(v[1], 17); v[1] ^= v[2]; v[2] = rotl(v[2], 32);
}
static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n-- != 0) *b++ = 0;
}
void fips_keyed_hasher_init(fips_keyed_hasher *h, fips_entropy entropy)
{
    uint8_t key[16];
    entropy.fill(entropy.ctx, key, sizeof(key));
    h->k0 = le64(key); h->k1 = le64(key + 8);
    wipe(key, sizeof(key));
}
void fips_keyed_hasher_clear(fips_keyed_hasher *h) { wipe(h, sizeof(*h)); }
uint64_t fips_siphash13(const fips_keyed_hasher *h, const uint8_t *bytes, size_t len)
{
    uint64_t v[4] = {UINT64_C(0x736f6d6570736575) ^ h->k0,
        UINT64_C(0x646f72616e646f6d) ^ h->k1,
        UINT64_C(0x6c7967656e657261) ^ h->k0,
        UINT64_C(0x7465646279746573) ^ h->k1};
    size_t pos = 0;
    while (len - pos >= 8) {
        uint64_t word = le64(bytes + pos);
        v[3] ^= word; round13(v); v[0] ^= word;
        pos += 8;
    }
    uint64_t tail = (uint64_t)(len & 255) << 56;
    for (size_t i = 0; i < len - pos; ++i) tail |= (uint64_t)bytes[pos + i] << (8 * i);
    v[3] ^= tail; round13(v); v[0] ^= tail;
    v[2] ^= 255;
    round13(v); round13(v); round13(v);
    uint64_t result = v[0] ^ v[1] ^ v[2] ^ v[3];
    wipe(v, sizeof(v));
    return result;
}
static bool valid_capacity(size_t n)
{
    return n >= 4 && (n & (n - 1)) == 0 && n <= SIZE_MAX / sizeof(fips_hash_slot);
}
static size_t limit(size_t n) { return n < 8 ? n - 1 : n - n / 8; }
fips_status fips_hash_map_init(fips_hash_map *m, fips_hash_slot *storage,
    size_t capacity, fips_entropy entropy)
{
    if (!valid_capacity(capacity)) return FIPS_ERR_RANGE;
    memset(storage, 0, capacity * sizeof(*storage));
    m->slots = storage; m->capacity = capacity; m->count = 0;
    fips_keyed_hasher_init(&m->hasher, entropy);
    return FIPS_OK;
}
size_t fips_hash_map_limit(const fips_hash_map *m) { return limit(m->capacity); }
size_t fips_hash_map_count(const fips_hash_map *m) { return m->count; }
/* At most capacity probes, even when every vacant slot is a tombstone. */
static size_t find(const fips_hash_map *m, const uint8_t *key, size_t len,
    uint64_t hash, bool *found)
{
    size_t first = SIZE_MAX, pos = (size_t)hash & (m->capacity - 1);
    *found = false;
    for (size_t i = 0; i < m->capacity; ++i) {
        const fips_hash_slot *s = &m->slots[pos];
        if (s->state == 0) return first == SIZE_MAX ? pos : first;
        if (s->state == 2 && first == SIZE_MAX) first = pos;
        if (s->state == 1 && s->hash == hash && s->key_len == len &&
            (len == 0 || memcmp(s->key, key, len) == 0)) {
            *found = true; return pos;
        }
        pos = (pos + 1) & (m->capacity - 1);
    }
    return first;
}
fips_status fips_hash_map_insert(fips_hash_map *m, const uint8_t *key, size_t len, void *value)
{
    uint64_t hash = fips_siphash13(&m->hasher, key, len);
    bool found;
    size_t pos = find(m, key, len, hash, &found);
    if (found) { m->slots[pos].value = value; return FIPS_OK; }
    if (m->count == limit(m->capacity) || pos == SIZE_MAX) return FIPS_ERR_RANGE;
    m->slots[pos] = (fips_hash_slot){key, len, value, hash, 1};
    ++m->count;
    return FIPS_OK;
}
bool fips_hash_map_get(const fips_hash_map *m, const uint8_t *key, size_t len, void **out)
{
    uint64_t hash = fips_siphash13(&m->hasher, key, len);
    bool found;
    size_t pos = find(m, key, len, hash, &found);
    if (found) *out = m->slots[pos].value;
    return found;
}
bool fips_hash_map_remove(fips_hash_map *m, const uint8_t *key, size_t len, void **out)
{
    uint64_t hash = fips_siphash13(&m->hasher, key, len);
    bool found;
    size_t pos = find(m, key, len, hash, &found);
    if (!found) return false;
    *out = m->slots[pos].value;
    m->slots[pos] = (fips_hash_slot){.state = 2};
    --m->count;
    return true;
}
void fips_hash_map_clear(fips_hash_map *m)
{
    memset(m->slots, 0, m->capacity * sizeof(*m->slots));
    m->count = 0;
}
fips_status fips_hash_map_rehash(fips_hash_map *m, fips_hash_slot *storage, size_t capacity)
{
    if (!valid_capacity(capacity) || m->count > limit(capacity)) return FIPS_ERR_RANGE;
    fips_hash_map next = {storage, capacity, 0, m->hasher};
    memset(storage, 0, capacity * sizeof(*storage));
    for (size_t i = 0; i < m->capacity; ++i) {
        if (m->slots[i].state != 1) continue;
        bool found;
        fips_hash_slot s = m->slots[i];
        size_t pos = find(&next, s.key, s.key_len, s.hash, &found);
        next.slots[pos] = s;
        ++next.count;
    }
    fips_hash_map_clear(m);
    *m = next;
    fips_keyed_hasher_clear(&next.hasher);
    return FIPS_OK;
}
void fips_hash_map_deinit(fips_hash_map *m)
{
    fips_hash_map_clear(m);
    fips_keyed_hasher_clear(&m->hasher);
    m->slots = NULL; m->capacity = 0;
}
