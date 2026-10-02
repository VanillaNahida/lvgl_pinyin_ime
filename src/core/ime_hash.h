/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Open addressing hash map from a NUL terminated key to a uint32 value.
 *
 * Used for the extension dictionary index (word -> record offset) and, from
 * stage S5, for the association bigram lookup and the user frequency overlay.
 * Linear probing with a power of two capacity keeps it branch-light and avoids
 * dynamic growth: the capacity is fixed at init().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One slot: empty, deleted or occupied. */
typedef enum {
    IME_HASH_SLOT_EMPTY = 0,
    IME_HASH_SLOT_DEAD,
    IME_HASH_SLOT_USED,
} ime_hash_slot_state_t;

typedef struct {
    char *key;   /**< owned copy for used slots, NULL otherwise */
    uint32_t value;
    uint8_t state;
} ime_hash_slot_t;

typedef struct {
    ime_hash_slot_t *slots;
    size_t capacity;   /**< power of two */
    size_t mask;       /**< capacity - 1 */
    size_t count;      /**< used slots */
    size_t dead;       /**< deleted slots, reused by later inserts */
} ime_hash_t;

/**
 * Allocate a map. @p capacity is rounded up to a power of two.
 *
 * @return false when out of memory
 */
bool ime_hash_init(ime_hash_t *map, size_t capacity);

/** Free the map and every key it owns. */
void ime_hash_deinit(ime_hash_t *map);

/** Insert or replace. Fails when the map is more than 3/4 full. */
bool ime_hash_put(ime_hash_t *map, const char *key, uint32_t value);

/** Look up @p key; @p out_value may be NULL. */
bool ime_hash_get(const ime_hash_t *map, const char *key, uint32_t *out_value);

/** Remove @p key, leaving a reusable slot behind. */
bool ime_hash_remove(ime_hash_t *map, const char *key);

/** Number of live entries. */
size_t ime_hash_count(const ime_hash_t *map);

/** Current capacity, for diagnostics and tests. */
size_t ime_hash_capacity(const ime_hash_t *map);

#ifdef __cplusplus
}
#endif
