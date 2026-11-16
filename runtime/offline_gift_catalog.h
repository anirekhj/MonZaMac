// Authored local reward descriptions and public numeric metadata; no event-card payloads.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OfflineGiftKind {
    OG_POKEMON = 1,
    OG_ITEMS = 2,
    OG_MISSION = 3,
    OG_CLOTHING = 4
} OfflineGiftKind;

typedef struct OfflineGiftItem {
    uint16_t id, count;
} OfflineGiftItem;

typedef struct OfflineGiftPokemon {
    uint16_t species, held_item, moves[4];
    uint8_t form, level, gender, nature, ability, pid_type;
    bool alpha;
} OfflineGiftPokemon;

typedef struct OfflineGift {
    uint16_t id;
    const char *name;
    const char *description;
    OfflineGiftKind kind;
    const char *flag_name;
    uint64_t flag_key;
    OfflineGiftItem items[6];
    uint32_t clothing[6];
    OfflineGiftPokemon pokemon;
    bool random_spewpa_form;
} OfflineGift;

size_t offline_gift_count(void);
const OfflineGift *offline_gift_at(size_t index);

#ifdef __cplusplus
}
#endif
