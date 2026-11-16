#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/offline_gift_card.h"

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t hash(const char *s) {
    uint64_t value = UINT64_C(0xcbf29ce484222645);
    while (*s) value = (value ^ (uint8_t)*s++) * UINT64_C(0x100000001b3);
    return value;
}
static void expected_header(uint8_t *p, uint16_t id, uint8_t kind) {
    p[8] = (uint8_t)id; p[9] = (uint8_t)(id >> 8); p[0xe] = 1; p[0x10] = 1; p[0x11] = kind;
}
static void items(void) {
    OfflineGift gift = {.id = 321, .kind = OG_ITEMS, .items = {{29, 5}, {2684, 999}}};
    uint8_t out[OFFLINE_GIFT_CARD_SIZE + 8], expected[OFFLINE_GIFT_CARD_SIZE] = {0};
    memset(out, 0xa5, sizeof out); expected_header(expected, 321, 2);
    expected[0x18] = 29; expected[0x1a] = 5;
    expected[0x1c] = 0x7c; expected[0x1d] = 0x0a; expected[0x1e] = 0xe7; expected[0x1f] = 3;
    assert(offline_gift_build_card(&gift, UINT32_MAX, out, sizeof out));
    assert(!memcmp(out, expected, sizeof expected));
    for (size_t i = sizeof expected; i < sizeof out; i++) assert(out[i] == 0xa5);
}
static void mission(void) {
    OfflineGift gift = {.id = 44, .kind = OG_MISSION, .flag_name = "authored_test_flag", .items = {{7, 2}}};
    gift.flag_key = hash(gift.flag_name);
    uint8_t out[OFFLINE_GIFT_CARD_SIZE], expected[OFFLINE_GIFT_CARD_SIZE] = {0};
    expected_header(expected, 44, 3); memcpy(expected + 0x28, gift.flag_name, strlen(gift.flag_name));
    expected[0x68] = 1; expected[0x6c] = 7; expected[0x6e] = 2;
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
    assert(!memcmp(out, expected, sizeof expected));
    char longest[64]; memset(longest, 'a', 63); longest[63] = 0;
    gift.flag_name = longest; gift.flag_key = hash(longest);
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
    assert(!memcmp(out + 0x28, longest, 64) && get32(out + 0x68) == 1);
    gift.items[0] = (OfflineGiftItem){0};
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
}
// Authored consumer oracle for the observed type-4 contract: -1 skips, zero attempts an invalid award.
static int clothing_awards(const uint8_t *card, uint32_t *awards) {
    int count = 0;
    for (unsigned i = 0; i < 6; i++) {
        uint32_t id = get32(card + 0x18 + 4 * i);
        if (id == UINT32_MAX) continue;
        if (!id) return -1;
        awards[count++] = id;
    }
    return count;
}
static void clothing(void) {
    OfflineGift gift = {.id = 55, .kind = OG_CLOTHING, .clothing = {0x12345678, 2, 3, 4, 5, 6}};
    uint8_t out[OFFLINE_GIFT_CARD_SIZE], expected[OFFLINE_GIFT_CARD_SIZE] = {0};
    expected_header(expected, 55, 4);
    expected[0x18] = 0x78; expected[0x19] = 0x56; expected[0x1a] = 0x34; expected[0x1b] = 0x12;
    for (unsigned i = 1; i < 6; i++) expected[0x18 + 4 * i] = (uint8_t)(i + 1);
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
    assert(!memcmp(out, expected, sizeof expected));
    // A single real clothing entry must leave five native -1 sentinels, never zero IDs.
    memset(gift.clothing + 1, 0, sizeof gift.clothing - sizeof gift.clothing[0]);
    memset(expected + 0x1c, 0xff, 5 * sizeof(uint32_t));
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
    assert(!memcmp(out, expected, sizeof expected));
    for (unsigned i = 1; i < 6; i++) assert(get32(out + 0x18 + 4 * i) == UINT32_MAX);
    for (unsigned length = 1; length <= 6; length++) {
        memset(gift.clothing, 0, sizeof gift.clothing);
        for (unsigned i = 0; i < length; i++) gift.clothing[i] = 100 + i;
        assert(offline_gift_build_card(&gift, 0, out, sizeof out));
        uint32_t awards[6] = {0};
        assert(clothing_awards(out, awards) == (int)length);
        for (unsigned i = 0; i < length; i++) assert(awards[i] == gift.clothing[i]);
        for (unsigned i = length; i < 6; i++) assert(get32(out + 0x18 + 4 * i) == UINT32_MAX);
        // Replacing either a populated slot or an unused sentinel with zero must fail the native contract.
        memset(out + 0x18 + 4 * (length < 6 ? length : 0), 0, 4);
        assert(clothing_awards(out, awards) == -1);
    }
}
static OfflineGift pokemon_fixture(void) {
    OfflineGift gift = {.id = 66, .kind = OG_POKEMON,
        .pokemon = {.species = 25, .held_item = 2684, .moves = {1, 255, 256, 920},
            .form = 0, .level = 37, .gender = 3, .nature = 255, .ability = 2, .pid_type = 2, .alpha = true}};
    return gift;
}
static void pokemon(void) {
    OfflineGift gift = pokemon_fixture();
    uint8_t out[OFFLINE_GIFT_CARD_SIZE], expected[OFFLINE_GIFT_CARD_SIZE] = {0};
    expected_header(expected, 66, 1);
    expected[0x25c] = 4; expected[0x25e] = 0x7c; expected[0x25f] = 0x0a;
    const uint8_t moves[] = {1, 0, 255, 0, 0, 1, 0x98, 3};
    memcpy(expected + 0x260, moves, sizeof moves); memcpy(expected + 0x268, moves, sizeof moves);
    expected[0x270] = 25; expected[0x273] = 3; expected[0x274] = 37;
    expected[0x276] = 255; expected[0x277] = 2; expected[0x278] = 2; expected[0x279] = 37;
    memset(expected + 0x27a, 255, 38); expected[0x2a0] = 2; expected[0x2ad] = 1; expected[0x2ae] = 1;
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
    assert(!memcmp(out, expected, sizeof expected));
    gift.pokemon = (OfflineGiftPokemon){.species = 666, .form = 19, .level = 100, .gender = 2, .nature = 24, .ability = 4};
    assert(offline_gift_build_card(&gift, 0, out, sizeof out));
    assert(out[0x272] == 19 && out[0x274] == 100 && !get16(out + 0x260) && !out[0x2ae]);
}
static void variations(void) {
    OfflineGift gift = pokemon_fixture(); gift.pokemon.species = 665; gift.random_spewpa_form = true;
    const uint8_t expected[] = {0, 1, 2, 3, 5, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
    uint8_t out[OFFLINE_GIFT_CARD_SIZE], first[OFFLINE_GIFT_CARD_SIZE];
    assert(offline_gift_build_card(&gift, 0, first, sizeof first));
    for (unsigned i = 0; i < 64; i++) {
        assert(offline_gift_build_card(&gift, i, out, sizeof out)); assert(out[0x272] == expected[i % 16]);
        out[0x272] = first[0x272]; assert(!memcmp(out, first, sizeof out));
    }
    assert(offline_gift_build_card(&gift, UINT32_MAX, out, sizeof out)); assert(out[0x272] == 18);
}
static void reject(OfflineGift gift) {
    uint8_t out[OFFLINE_GIFT_CARD_SIZE], before[OFFLINE_GIFT_CARD_SIZE];
    memset(out, 0xa5, sizeof out); memcpy(before, out, sizeof out);
    assert(!offline_gift_build_card(&gift, 0, out, sizeof out)); assert(!memcmp(before, out, sizeof out));
}
static void invalid(void) {
    OfflineGift item = {.id = 1, .kind = OG_ITEMS, .items = {{1, 1}}}, bad;
    uint8_t out[OFFLINE_GIFT_CARD_SIZE + 2]; memset(out, 0xa5, sizeof out);
    assert(!offline_gift_build_card(NULL, 0, out, sizeof out)); assert(!offline_gift_build_card(&item, 0, NULL, sizeof out));
    for (size_t size = 0; size < OFFLINE_GIFT_CARD_SIZE; size++) assert(!offline_gift_build_card(&item, 0, out + 1, size));
    for (size_t i = 0; i < sizeof out; i++) assert(out[i] == 0xa5);
    bad = item; bad.id = 0; reject(bad); bad = item; bad.kind = 0; reject(bad); bad.kind = 5; reject(bad);
    bad = item; bad.items[0].id = 2685; reject(bad); bad = item; bad.items[0].count = 1000; reject(bad);
    bad = item; bad.items[0].count = 0; reject(bad); bad = item; bad.items[0].id = 0; reject(bad);
    bad = item; bad.items[2] = (OfflineGiftItem){2, 1}; reject(bad);
    bad = item; bad.items[1] = bad.items[0]; reject(bad); bad = item; bad.clothing[0] = 1; reject(bad);
    bad = item; bad.flag_key = 1; reject(bad); bad = item; bad.random_spewpa_form = true; reject(bad);
    bad = item; bad.pokemon.level = 1; reject(bad);
    OfflineGift mon = pokemon_fixture();
    bad = mon; bad.pokemon.species = 0; reject(bad); bad = mon; bad.pokemon.species = 917; reject(bad);
    bad = mon; bad.pokemon.level = 0; reject(bad); bad.pokemon.level = 101; reject(bad);
    bad = mon; bad.pokemon.gender = 4; reject(bad); bad = mon; bad.pokemon.nature = 25; reject(bad);
    bad = mon; bad.pokemon.form = 20; reject(bad); bad = mon; bad.pokemon.ability = 5; reject(bad);
    bad = mon; bad.pokemon.pid_type = 3; reject(bad); bad = mon; bad.pokemon.held_item = 2685; reject(bad);
    bad = mon; bad.pokemon.moves[3] = 921; reject(bad); bad = mon; bad.random_spewpa_form = true; reject(bad);
    OfflineGift flag = {.id = 2, .kind = OG_MISSION, .flag_name = "authored_test_flag"}; flag.flag_key = hash(flag.flag_name);
    bad = flag; bad.flag_key++; reject(bad); bad = flag; bad.flag_name = NULL; reject(bad);
    bad = flag; bad.flag_name = ""; reject(bad); bad = flag; bad.flag_name = "bad flag"; bad.flag_key = hash(bad.flag_name); reject(bad);
    char unterminated[64]; memset(unterminated, 'a', sizeof unterminated); bad = flag; bad.flag_name = unterminated; reject(bad);
    OfflineGift clothes = {.id = 3, .kind = OG_CLOTHING}; reject(clothes);
    clothes.clothing[1] = 4; reject(clothes); clothes.clothing[0] = 4; reject(clothes);
    clothes.clothing[1] = UINT32_MAX; reject(clothes);
}
static void catalog(void) {
    uint8_t out[OFFLINE_GIFT_CARD_SIZE];
    for (size_t i = 0; i < offline_gift_count(); i++) {
        const OfflineGift *gift = offline_gift_at(i);
        assert(offline_gift_build_card(gift, 0, out, sizeof out));
        assert(get16(out + 8) == gift->id && out[0x11] == gift->kind);
        if (gift->kind == OG_MISSION) assert(!strcmp((const char *)out + 0x28, gift->flag_name) && get32(out + 0x68) == 1);
        if (gift->kind == OG_CLOTHING)
            for (unsigned j = 0; j < 6; j++) assert(get32(out + 0x18 + 4 * j) == (gift->clothing[j] ? gift->clothing[j] : UINT32_MAX));
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "items")) items(); else if (!strcmp(argv[1], "mission")) mission();
    else if (!strcmp(argv[1], "clothing")) clothing(); else if (!strcmp(argv[1], "pokemon")) pokemon();
    else if (!strcmp(argv[1], "variations")) variations(); else if (!strcmp(argv[1], "invalid")) invalid();
    else if (!strcmp(argv[1], "catalog")) catalog(); else assert(!"unknown scenario");
    puts("PASS authored offline gift card scenario");
    return 0;
}
