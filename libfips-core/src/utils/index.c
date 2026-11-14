#include <fips/utils/index.h>
#include <string.h>

fips_session_index fips_session_index_new(uint32_t value)
{
    return (fips_session_index){value};
}
uint32_t fips_session_index_as_u32(fips_session_index index) { return index.value; }
void fips_session_index_to_le_bytes(fips_session_index index, uint8_t out[4])
{
    for (size_t i = 0; i < 4; ++i) out[i] = (uint8_t)(index.value >> (i * 8));
}
fips_session_index fips_session_index_from_le_bytes(const uint8_t bytes[4])
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= (uint32_t)bytes[i] << (i * 8);
    return fips_session_index_new(value);
}
fips_status fips_session_index_format(fips_session_index index, char *out, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    if (capacity < 9) return FIPS_ERR_RANGE;
    for (size_t i = 0; i < 8; ++i) out[i] = hex[(index.value >> ((7 - i) * 4)) & 15];
    out[8] = '\0';
    return FIPS_OK;
}
void fips_index_allocator_init(fips_index_allocator *a, uint32_t *storage,
    size_t capacity, size_t max_attempts, fips_entropy entropy)
{
    *a = (fips_index_allocator){storage, 0, capacity, max_attempts, entropy};
}
static size_t lower(const fips_index_allocator *a, uint32_t value)
{
    size_t lo = 0, hi = a->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->indices[mid] < value) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
bool fips_index_allocator_is_allocated(const fips_index_allocator *a, fips_session_index index)
{
    size_t pos = lower(a, index.value);
    return pos < a->count && a->indices[pos] == index.value;
}
fips_index_status fips_index_allocator_reserve(fips_index_allocator *a, fips_session_index index)
{
    size_t pos = lower(a, index.value);
    if (pos < a->count && a->indices[pos] == index.value) return FIPS_INDEX_ALREADY_IN_USE;
    if (a->count == a->capacity) return FIPS_INDEX_CAPACITY;
    memmove(a->indices + pos + 1, a->indices + pos, (a->count - pos) * sizeof(*a->indices));
    a->indices[pos] = index.value;
    ++a->count;
    return FIPS_INDEX_OK;
}
fips_index_status fips_index_allocator_allocate(fips_index_allocator *a, fips_session_index *out)
{
    if (a->count == a->capacity) return FIPS_INDEX_CAPACITY;
    for (size_t i = 0; i < a->max_attempts; ++i) {
        uint8_t bytes[4];
        a->entropy.fill(a->entropy.ctx, bytes, sizeof(bytes));
        fips_session_index index = fips_session_index_from_le_bytes(bytes);
        if (fips_index_allocator_reserve(a, index) == FIPS_INDEX_OK) {
            *out = index;
            return FIPS_INDEX_OK;
        }
    }
    return FIPS_INDEX_EXHAUSTED;
}
fips_index_status fips_index_allocator_free(fips_index_allocator *a, fips_session_index index)
{
    size_t pos = lower(a, index.value);
    if (pos == a->count || a->indices[pos] != index.value) return FIPS_INDEX_NOT_FOUND;
    memmove(a->indices + pos, a->indices + pos + 1, (a->count - pos - 1) * sizeof(*a->indices));
    --a->count;
    return FIPS_INDEX_OK;
}
size_t fips_index_allocator_count(const fips_index_allocator *a) { return a->count; }
bool fips_index_allocator_is_empty(const fips_index_allocator *a) { return a->count == 0; }
void fips_index_allocator_clear(fips_index_allocator *a) { a->count = 0; }
