#include "offline_gift_card.h"
#include <string.h>

// Field locations and sentinels: pinned public PKHeX WA9.cs; no event-card data or implementation is copied.
// https://github.com/kwsch/PKHeX/blob/6b21d83543db3e7fb7149000e8b13a2753c2f014/PKHeX.Core/MysteryGifts/WA9.cs
// Native type 3 is a named flag plus optional items, unlike PKHeX's historical BP label.
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static bool item_payload_valid(const OfflineGift *gift, bool required) {
    bool ended = false, any = false;
    for (unsigned i = 0; i < 6; i++) {
        OfflineGiftItem item = gift->items[i];
        if (!item.id) {
            if (item.count) return false;
            ended = true;
        } else {
            // Public Legal.cs: v2.0.2 / Mega Dimension max item ID 2684.
            if (ended || item.id > 2684 || !item.count || item.count > 999) return false;
            for (unsigned j = 0; j < i; j++) if (gift->items[j].id == item.id) return false;
            any = true;
        }
    }
    return !required || any;
}
static bool pokemon_empty(const OfflineGiftPokemon *p) {
    if (p->species || p->held_item || p->form || p->level || p->gender || p->nature || p->ability || p->pid_type || p->alpha)
        return false;
    for (unsigned i = 0; i < 4; i++) if (p->moves[i]) return false;
    return true;
}
static bool flag_valid(const OfflineGift *gift, size_t *length) {
    if (!gift->flag_name || !gift->flag_key) return false;
    // Pokemon flag names use the game's FNV-1a offset basis (PKHeX FnvHash.cs), not the standard basis.
    uint64_t hash = UINT64_C(0xcbf29ce484222645);
    size_t i;
    for (i = 0; i < 64 && gift->flag_name[i]; i++) {
        unsigned char c = (unsigned char)gift->flag_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        hash = (hash ^ c) * UINT64_C(0x100000001b3);
    }
    if (!i || i == 64 || hash != gift->flag_key) return false;
    *length = i;
    return true;
}

bool offline_gift_build_card(const OfflineGift *gift, uint32_t variation, uint8_t *out, size_t out_size) {
    if (!gift || !out || out_size < OFFLINE_GIFT_CARD_SIZE || !gift->id ||
        gift->kind < OG_POKEMON || gift->kind > OG_CLOTHING) return false;
    size_t flag_length = 0;
    if (gift->kind == OG_MISSION) {
        if (!flag_valid(gift, &flag_length)) return false;
    } else if (gift->flag_name || gift->flag_key) return false;
    if (!item_payload_valid(gift, gift->kind == OG_ITEMS)) return false;
    bool clothing_ended = false, has_clothing = false;
    for (unsigned i = 0; i < 6; i++) {
        if (gift->kind != OG_ITEMS && gift->kind != OG_MISSION && gift->items[i].id) return false;
        uint32_t id = gift->clothing[i];
        if (!id) clothing_ended = true;
        else {
            if (gift->kind != OG_CLOTHING || clothing_ended || id == UINT32_MAX) return false;
            for (unsigned j = 0; j < i; j++) if (gift->clothing[j] == id) return false;
            has_clothing = true;
        }
    }
    if (gift->kind == OG_CLOTHING && !has_clothing) return false;
    const OfflineGiftPokemon *pokemon = &gift->pokemon;
    if (gift->kind == OG_POKEMON) {
        // All catalog species precede Gen 9's remapped internal IDs (starting at national ID 917).
        // Newer species need an explicit conversion before this supported range is expanded.
        if (!pokemon->species || pokemon->species > 916 || !pokemon->level || pokemon->level > 100 ||
            pokemon->form > 19 || pokemon->gender > 3 || (pokemon->nature > 24 && pokemon->nature != 255) ||
            pokemon->ability > 4 || pokemon->pid_type > 2 || pokemon->held_item > 2684) return false;
        for (unsigned i = 0; i < 4; i++) if (pokemon->moves[i] > 920) return false;
        if (gift->random_spewpa_form && pokemon->species != 665) return false;
    } else if (!pokemon_empty(pokemon) || gift->random_spewpa_form) return false;

    // Build separately so failure or overlapping host input never leaves a partial card in the caller's buffer.
    uint8_t card[OFFLINE_GIFT_CARD_SIZE] = {0};
    put16(card + 8, gift->id);
    card[0xe] = 1;
    card[0x10] = 1;
    card[0x11] = (uint8_t)gift->kind;
    if (gift->kind == OG_ITEMS || gift->kind == OG_MISSION) {
        size_t items_at = gift->kind == OG_MISSION ? 0x6c : 0x18;
        if (gift->kind == OG_MISSION) {
            memcpy(card + 0x28, gift->flag_name, flag_length);
            put32(card + 0x68, 1);
        }
        for (unsigned i = 0; i < 6; i++) {
            put16(card + items_at + 4 * i, gift->items[i].id);
            put16(card + items_at + 4 * i + 2, gift->items[i].count);
        }
    } else if (gift->kind == OG_CLOTHING) {
        // The native clothing dispatcher skips -1 slots; zero is an attempted award and fails the grant.
        for (unsigned i = 0; i < 6; i++) put32(card + 0x18 + 4 * i, UINT32_MAX);
        for (unsigned i = 0; i < 6 && gift->clothing[i]; i++) put32(card + 0x18 + 4 * i, gift->clothing[i]);
    } else {
        put16(card + 0x25c, 4);  // Poke Ball
        put16(card + 0x25e, pokemon->held_item);
        for (unsigned i = 0; i < 4; i++) {
            put16(card + 0x260 + 2 * i, pokemon->moves[i]);
            put16(card + 0x268 + 2 * i, pokemon->moves[i]);
        }
        put16(card + 0x270, pokemon->species);
        card[0x272] = pokemon->form;
        if (gift->random_spewpa_form) {
            static const uint8_t forms[] = {0, 1, 2, 3, 5, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
            card[0x272] = forms[variation % (sizeof forms / sizeof *forms)];
        }
        card[0x273] = pokemon->gender;
        card[0x274] = card[0x279] = pokemon->level;
        card[0x276] = pokemon->nature;
        card[0x277] = pokemon->ability;
        card[0x278] = pokemon->pid_type;
        memset(card + 0x27a, 0xff, 0x26);  // no ribbons; random IVs
        card[0x2a0] = 2;  // current trainer
        put16(card + 0x2ac, 256);  // random scale
        card[0x2ae] = pokemon->alpha ? 1 : 0;
    }
    memcpy(out, card, sizeof card);
    return true;
}
