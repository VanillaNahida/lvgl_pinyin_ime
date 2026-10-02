/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ime_hash.h"

#include "ime_alloc.h"

#include <string.h>

static uint32_t hash_key(const char *key)
{
    /* FNV-1a: small, dependency free, good enough for a few thousand words. */
    uint32_t h = 2166136261u;
    while (*key != '\0') {
        h ^= (uint8_t)*key++;
        h *= 16777619u;
    }
    return h;
}

bool ime_hash_init(ime_hash_t *map, size_t capacity)
{
    if (map == NULL || capacity == 0) {
        return false;
    }

    size_t cap = 8;
    while (cap < capacity) {
        size_t next = cap << 1;
        if (next <= cap) {
            return false; /* overflow */
        }
        cap = next;
    }

    map->slots = (ime_hash_slot_t *)ime_calloc(cap, sizeof(ime_hash_slot_t));
    if (map->slots == NULL) {
        return false;
    }
    map->capacity = cap;
    map->mask = cap - 1;
    map->count = 0;
    map->dead = 0;
    return true;
}

void ime_hash_deinit(ime_hash_t *map)
{
    if (map == NULL || map->slots == NULL) {
        return;
    }
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->slots[i].state == IME_HASH_SLOT_USED && map->slots[i].key != NULL) {
            ime_free(map->slots[i].key);
        }
    }
    ime_free(map->slots);
    memset(map, 0, sizeof(*map));
}

bool ime_hash_put(ime_hash_t *map, const char *key, uint32_t value)
{
    if (map == NULL || map->slots == NULL || key == NULL) {
        return false;
    }
    /* Keep a quarter of the slots free so probing stays short. */
    if ((map->count + map->dead + 1) * 4 > map->capacity * 3) {
        return false;
    }

    size_t index = hash_key(key) & map->mask;
    size_t reusable = map->capacity;

    for (size_t probe = 0; probe <= map->mask; probe++) {
        ime_hash_slot_t *slot = &map->slots[index];

        if (slot->state == IME_HASH_SLOT_EMPTY) {
            if (reusable == map->capacity) {
                reusable = index;
            }
            break;
        }
        if (slot->state == IME_HASH_SLOT_DEAD) {
            if (reusable == map->capacity) {
                reusable = index;
            }
        } else if (strcmp(slot->key, key) == 0) {
            slot->value = value;
            return true;
        }

        index = (index + 1) & map->mask;
    }

    if (reusable == map->capacity) {
        return false;
    }

    ime_hash_slot_t *slot = &map->slots[reusable];
    if (slot->state == IME_HASH_SLOT_DEAD) {
        map->dead--;
    } else {
        char *copy = (char *)ime_malloc(strlen(key) + 1);
        if (copy == NULL) {
            return false;
        }
        strcpy(copy, key);
        slot->key = copy;
    }
    slot->value = value;
    slot->state = IME_HASH_SLOT_USED;
    map->count++;
    return true;
}

bool ime_hash_get(const ime_hash_t *map, const char *key, uint32_t *out_value)
{
    if (map == NULL || map->slots == NULL || key == NULL) {
        return false;
    }

    size_t index = hash_key(key) & map->mask;
    for (size_t probe = 0; probe <= map->mask; probe++) {
        const ime_hash_slot_t *slot = &map->slots[index];

        if (slot->state == IME_HASH_SLOT_EMPTY) {
            return false;
        }
        if (slot->state == IME_HASH_SLOT_USED && strcmp(slot->key, key) == 0) {
            if (out_value != NULL) {
                *out_value = slot->value;
            }
            return true;
        }
        index = (index + 1) & map->mask;
    }
    return false;
}

bool ime_hash_remove(ime_hash_t *map, const char *key)
{
    if (map == NULL || map->slots == NULL || key == NULL) {
        return false;
    }

    size_t index = hash_key(key) & map->mask;
    for (size_t probe = 0; probe <= map->mask; probe++) {
        ime_hash_slot_t *slot = &map->slots[index];

        if (slot->state == IME_HASH_SLOT_EMPTY) {
            return false;
        }
        if (slot->state == IME_HASH_SLOT_USED && strcmp(slot->key, key) == 0) {
            ime_free(slot->key);
            slot->key = NULL;
            slot->state = IME_HASH_SLOT_DEAD;
            map->count--;
            map->dead++;
            return true;
        }
        index = (index + 1) & map->mask;
    }
    return false;
}

size_t ime_hash_count(const ime_hash_t *map)
{
    return (map != NULL) ? map->count : 0;
}

size_t ime_hash_capacity(const ime_hash_t *map)
{
    return (map != NULL) ? map->capacity : 0;
}
