#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/offline_gift_catalog.h"

static const OfflineGift *find(uint16_t id) {
    for (size_t i = 0; i < offline_gift_count(); i++) {
        const OfflineGift *gift = offline_gift_at(i);
        if (gift->id == id) return gift;
    }
    return NULL;
}

static void registry(void) {
    assert(offline_gift_count() == 19);
    assert(!offline_gift_at(offline_gift_count()));
    assert(!offline_gift_at(SIZE_MAX));
    unsigned kinds[5] = {0};
    for (size_t i = 0; i < offline_gift_count(); i++) {
        const OfflineGift *gift = offline_gift_at(i);
        assert(gift && gift->id && gift->name && gift->name[0]);
        assert(gift->description && gift->description[0]);
        assert(gift->kind >= OG_POKEMON && gift->kind <= OG_CLOTHING);
        kinds[gift->kind]++;
        for (size_t j = 0; j < i; j++) assert(offline_gift_at(j)->id != gift->id);
    }
    assert(kinds[OG_POKEMON] == 9 && kinds[OG_ITEMS] == 6);
    assert(kinds[OG_MISSION] == 3 && kinds[OG_CLOTHING] == 1);
}

static void payloads(void) {
    for (size_t i = 0; i < offline_gift_count(); i++) {
        const OfflineGift *gift = offline_gift_at(i);
        bool item_end = false, clothing_end = false;
        unsigned items = 0, clothing = 0;
        for (size_t j = 0; j < 6; j++) {
            const OfflineGiftItem *item = &gift->items[j];
            if (!item->id) { assert(!item->count); item_end = true; }
            else {
                assert(!item_end && item->id < 3000 && item->count > 0 && item->count <= 999);
                items++;
            }
            if (!gift->clothing[j]) clothing_end = true;
            else { assert(!clothing_end); clothing++; }
        }
        if (gift->kind == OG_POKEMON) {
            const OfflineGiftPokemon *pokemon = &gift->pokemon;
            assert(pokemon->species && pokemon->species < 905);
            assert(pokemon->level && pokemon->level <= 100);
            assert(pokemon->gender == 3 && pokemon->nature == 255);
            assert(pokemon->ability <= 2 && (pokemon->pid_type == 1 || pokemon->pid_type == 2));
            assert(strstr(gift->description, "Offline recreation"));
            assert(!items && !clothing && !gift->flag_key && !gift->flag_name);
        } else {
            assert(!gift->pokemon.species && !gift->random_spewpa_form);
            if (gift->kind == OG_CLOTHING) assert(clothing && !items);
            else assert(!clothing);
            if (gift->kind == OG_ITEMS) assert(items);
            if (gift->kind == OG_MISSION) assert(gift->flag_key && gift->flag_name && gift->flag_name[0]);
            else assert(!gift->flag_key && !gift->flag_name);
        }
    }
}

static void receipt_contract(void) {
    assert(offline_gift_at(0)->id == 2003 && offline_gift_at(1)->id == 2002 && offline_gift_at(2)->id == 2004);
    const OfflineGift *mewtwo = find(2003), *diancie = find(2002), *garchomp = find(2004);
    assert(mewtwo->flag_key == UINT64_C(0xBD7121CAF1FC28B3));
    assert(mewtwo->items[0].id == 662 && mewtwo->items[1].id == 663);
    assert(mewtwo->items[0].count == 1 && mewtwo->items[1].count == 1);
    assert(diancie->flag_key == UINT64_C(0xBD6728CAF1F3E013));
    assert(diancie->items[0].id == 764 && diancie->items[0].count == 1);
    assert(garchomp->flag_key == UINT64_C(0xFB1EFF0095F3BBFE));
    for (size_t i = 0; i < 6; i++) assert(!garchomp->items[i].id && !garchomp->clothing[i]);
    assert(find(1602)->clothing[0] == 3021111 && !find(1602)->clothing[1]);
    assert(find(1601)->pokemon.held_item == 657);
    assert(find(1607)->pokemon.alpha && find(1607)->pokemon.moves[0] == 76);
    assert(find(603)->pokemon.ability == 2);
    assert(find(1613)->pokemon.pid_type == 2 && find(1613)->pokemon.level == 100);
    assert(find(105)->pokemon.level == 5 && !find(105)->pokemon.moves[0]);
    assert(find(1615)->random_spewpa_form && find(1615)->pokemon.species == 665 && find(1615)->pokemon.level == 9);
    assert(find(1616)->pokemon.species == 666 && find(1616)->pokemon.form == 19 && find(1616)->pokemon.level == 12);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "registry")) registry();
    else if (!strcmp(argv[1], "payloads")) payloads();
    else { assert(!strcmp(argv[1], "receipt-contract")); receipt_contract(); }
    puts("PASS authored offline gift catalog scenario");
    return 0;
}
