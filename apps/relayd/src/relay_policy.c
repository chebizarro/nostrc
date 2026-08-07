#include "relay_policy.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define HASH_EMPTY 0u
#define HASH_TOMBSTONE UINT32_MAX
#define ENTRY_NONE SIZE_MAX

typedef enum {
  ENTRY_RESERVED = 1,
  ENTRY_COMMITTED = 2
} ReplayEntryState;

typedef struct {
  unsigned char id[32];
  uint64_t hash;
  uint64_t expires_ms;
  size_t queue_prev;
  size_t queue_next;
  size_t free_next;
  ReplayEntryState state;
  int occupied;
} ReplayEntry;

struct RelayPolicy {
  ReplayEntry *entries;
  uint32_t *hash_slots;
  size_t capacity;
  size_t table_size;
  size_t size;
  size_t tombstones;
  size_t free_head;
  size_t expiry_head;
  size_t expiry_tail;
  uint64_t ttl_ms;
  int future_skew_seconds;
  int past_skew_seconds;
};

static size_t next_pow2(size_t value) {
  size_t out = 1;
  while (out < value && out <= SIZE_MAX / 2) out <<= 1;
  return out < value ? 0 : out;
}

static uint64_t id_hash(const unsigned char id[32]) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < 32; ++i) {
    hash ^= id[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static size_t find_hash_slot(const RelayPolicy *policy,
                             const unsigned char id[32], uint64_t hash,
                             int *found) {
  size_t mask = policy->table_size - 1;
  size_t first_tombstone = ENTRY_NONE;
  *found = 0;

  for (size_t probe = 0; probe < policy->table_size; ++probe) {
    size_t pos = ((size_t)hash + probe) & mask;
    uint32_t value = policy->hash_slots[pos];
    if (value == HASH_EMPTY) {
      return first_tombstone != ENTRY_NONE ? first_tombstone : pos;
    }
    if (value == HASH_TOMBSTONE) {
      if (first_tombstone == ENTRY_NONE) first_tombstone = pos;
      continue;
    }
    size_t entry_index = (size_t)value - 1;
    ReplayEntry *entry = &policy->entries[entry_index];
    if (entry->occupied && entry->hash == hash &&
        memcmp(entry->id, id, 32) == 0) {
      *found = 1;
      return pos;
    }
  }
  return first_tombstone;
}

static void queue_unlink(RelayPolicy *policy, size_t index) {
  ReplayEntry *entry = &policy->entries[index];
  if (entry->queue_prev != ENTRY_NONE)
    policy->entries[entry->queue_prev].queue_next = entry->queue_next;
  else
    policy->expiry_head = entry->queue_next;

  if (entry->queue_next != ENTRY_NONE)
    policy->entries[entry->queue_next].queue_prev = entry->queue_prev;
  else
    policy->expiry_tail = entry->queue_prev;

  entry->queue_prev = ENTRY_NONE;
  entry->queue_next = ENTRY_NONE;
}

static void entry_remove(RelayPolicy *policy, size_t index) {
  ReplayEntry *entry = &policy->entries[index];
  if (!entry->occupied) return;

  int found = 0;
  size_t slot = find_hash_slot(policy, entry->id, entry->hash, &found);
  if (found && slot != ENTRY_NONE) {
    policy->hash_slots[slot] = HASH_TOMBSTONE;
    policy->tombstones++;
  }

  if (entry->queue_prev != ENTRY_NONE || entry->queue_next != ENTRY_NONE ||
      policy->expiry_head == index)
    queue_unlink(policy, index);
  memset(entry->id, 0, sizeof(entry->id));
  entry->occupied = 0;
  entry->state = 0;
  entry->expires_ms = 0;
  entry->hash = 0;
  entry->free_next = policy->free_head;
  policy->free_head = index;
  if (policy->size > 0) policy->size--;
}

static int rebuild_hash(RelayPolicy *policy) {
  memset(policy->hash_slots, 0,
         policy->table_size * sizeof(*policy->hash_slots));
  policy->tombstones = 0;
  for (size_t i = 0; i < policy->capacity; ++i) {
    ReplayEntry *entry = &policy->entries[i];
    if (!entry->occupied) continue;
    int found = 0;
    size_t slot = find_hash_slot(policy, entry->id, entry->hash, &found);
    if (slot == ENTRY_NONE || found) return 0;
    policy->hash_slots[slot] = (uint32_t)(i + 1);
  }
  return 1;
}

static void expire_entries(RelayPolicy *policy, uint64_t now_ms) {
  while (policy->expiry_head != ENTRY_NONE) {
    size_t index = policy->expiry_head;
    if (policy->entries[index].expires_ms > now_ms) break;
    entry_remove(policy, index);
  }
}

RelayPolicy *relay_policy_create(size_t capacity, int replay_ttl_seconds,
                                 int future_skew_seconds,
                                 int past_skew_seconds) {
  if (capacity == 0 || capacity >= UINT32_MAX) return NULL;
  RelayPolicy *policy = calloc(1, sizeof(*policy));
  if (!policy) return NULL;

  size_t wanted = capacity > SIZE_MAX / 2 ? 0 : capacity * 2;
  policy->table_size = next_pow2(wanted);
  if (policy->table_size == 0) {
    free(policy);
    return NULL;
  }

  policy->entries = calloc(capacity, sizeof(*policy->entries));
  policy->hash_slots = calloc(policy->table_size, sizeof(*policy->hash_slots));
  if (!policy->entries || !policy->hash_slots) {
    free(policy->hash_slots);
    free(policy->entries);
    free(policy);
    return NULL;
  }

  policy->capacity = capacity;
  policy->free_head = 0;
  policy->expiry_head = ENTRY_NONE;
  policy->expiry_tail = ENTRY_NONE;
  for (size_t i = 0; i < capacity; ++i) {
    policy->entries[i].free_next = i + 1 < capacity ? i + 1 : ENTRY_NONE;
    policy->entries[i].queue_prev = ENTRY_NONE;
    policy->entries[i].queue_next = ENTRY_NONE;
  }
  policy->ttl_ms = replay_ttl_seconds > 0
                       ? (uint64_t)replay_ttl_seconds * 1000ull
                       : 0;
  policy->future_skew_seconds =
      future_skew_seconds > 0 ? future_skew_seconds : 0;
  policy->past_skew_seconds = past_skew_seconds > 0 ? past_skew_seconds : 0;
  return policy;
}

void relay_policy_destroy(RelayPolicy *policy) {
  if (!policy) return;
  free(policy->hash_slots);
  free(policy->entries);
  free(policy);
}

int relay_policy_get_replay_ttl(const RelayPolicy *policy) {
  return policy ? (int)(policy->ttl_ms / 1000ull) : 0;
}

void relay_policy_get_skew(const RelayPolicy *policy, int *future_seconds,
                           int *past_seconds) {
  if (future_seconds)
    *future_seconds = policy ? policy->future_skew_seconds : 0;
  if (past_seconds)
    *past_seconds = policy ? policy->past_skew_seconds : 0;
}

size_t relay_policy_size(const RelayPolicy *policy) {
  return policy ? policy->size : 0;
}

static int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int relay_policy_hex_to_id(const char id_hex[65], unsigned char id_out[32]) {
  if (!id_hex || !id_out || id_hex[64] != '\0') return 0;
  for (size_t i = 0; i < 32; ++i) {
    int hi = hex_value((unsigned char)id_hex[i * 2]);
    int lo = hex_value((unsigned char)id_hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return 0;
    id_out[i] = (unsigned char)((hi << 4) | lo);
  }
  return 1;
}

RelayReplayStatus relay_policy_reserve(RelayPolicy *policy,
                                       const unsigned char id[32],
                                       uint64_t now_ms) {
  if (!policy || !id) return RELAY_REPLAY_ERROR;
  if (policy->ttl_ms == 0) return RELAY_REPLAY_DISABLED;

  expire_entries(policy, now_ms);
  if (policy->tombstones > policy->table_size / 4 &&
      !rebuild_hash(policy))
    return RELAY_REPLAY_ERROR;
  uint64_t hash = id_hash(id);
  int found = 0;
  size_t slot = find_hash_slot(policy, id, hash, &found);
  if (found) {
    ReplayEntry *existing =
        &policy->entries[(size_t)policy->hash_slots[slot] - 1];
    return existing->state == ENTRY_RESERVED ? RELAY_REPLAY_IN_PROGRESS
                                             : RELAY_REPLAY_DUPLICATE;
  }

  /* Never weaken the advertised TTL by evicting an unexpired entry. */
  if (policy->size >= policy->capacity) return RELAY_REPLAY_ERROR;
  if (policy->free_head == ENTRY_NONE) return RELAY_REPLAY_ERROR;

  slot = find_hash_slot(policy, id, hash, &found);
  if (slot == ENTRY_NONE || found) return RELAY_REPLAY_ERROR;

  size_t index = policy->free_head;
  ReplayEntry *entry = &policy->entries[index];
  policy->free_head = entry->free_next;
  entry->free_next = ENTRY_NONE;
  memcpy(entry->id, id, 32);
  entry->hash = hash;
  entry->expires_ms = 0;
  entry->state = ENTRY_RESERVED;
  entry->occupied = 1;
  entry->queue_prev = ENTRY_NONE;
  entry->queue_next = ENTRY_NONE;
  policy->hash_slots[slot] = (uint32_t)(index + 1);
  policy->size++;
  return RELAY_REPLAY_RESERVED;
}

static ReplayEntry *find_entry(RelayPolicy *policy,
                               const unsigned char id[32]) {
  if (!policy || !id) return NULL;
  uint64_t hash = id_hash(id);
  int found = 0;
  size_t slot = find_hash_slot(policy, id, hash, &found);
  if (!found || slot == ENTRY_NONE) return NULL;
  return &policy->entries[(size_t)policy->hash_slots[slot] - 1];
}

int relay_policy_commit(RelayPolicy *policy, const unsigned char id[32],
                        uint64_t now_ms) {
  ReplayEntry *entry = find_entry(policy, id);
  if (!entry || entry->state != ENTRY_RESERVED) return 0;
  entry->state = ENTRY_COMMITTED;
  entry->expires_ms =
      now_ms > UINT64_MAX - policy->ttl_ms ? UINT64_MAX : now_ms + policy->ttl_ms;
  size_t index = (size_t)(entry - policy->entries);
  entry->queue_prev = policy->expiry_tail;
  entry->queue_next = ENTRY_NONE;
  if (policy->expiry_tail != ENTRY_NONE)
    policy->entries[policy->expiry_tail].queue_next = index;
  else
    policy->expiry_head = index;
  policy->expiry_tail = index;
  return 1;
}

int relay_policy_rollback(RelayPolicy *policy, const unsigned char id[32]) {
  ReplayEntry *entry = find_entry(policy, id);
  if (!entry || entry->state != ENTRY_RESERVED) return 0;
  size_t index = (size_t)(entry - policy->entries);
  entry_remove(policy, index);
  return 1;
}

int relay_policy_created_at_out_of_range(const RelayPolicy *policy,
                                         int64_t created_at, time_t now) {
  if (!policy || created_at <= 0) return 0;
  if (policy->future_skew_seconds > 0 &&
      created_at - (int64_t)now > policy->future_skew_seconds)
    return 1;
  if (policy->past_skew_seconds > 0 &&
      (int64_t)now - created_at > policy->past_skew_seconds)
    return 1;
  return 0;
}
