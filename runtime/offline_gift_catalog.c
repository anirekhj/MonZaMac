#include "offline_gift_catalog.h"

// Public facts: PKHeX species/item/move enums and the FlagsEditorEXPlugin flag-name map.
// Distribution descriptions: official Pokemon announcements and Project Pokemon's textual catalog.
// https://github.com/kwsch/PKHeX/tree/6b21d83543db3e7fb7149000e8b13a2753c2f014
// https://github.com/fattard/FlagsEditorEXPlugin/tree/710f9a9b93f5c697fbfcc47eb4af4f65af0419ff
// https://legends.pokemon.com/en-us/news
// https://projectpokemon.org/home/files/category/229-legends-z-a/
// Unknown original event attributes use explicit local recreation choices, not event-card data.
static const OfflineGift gifts[] = {
    {
        .id = 2003, .name = "Project M", .kind = OG_MISSION,
        .description = "Mewtwonite X and Y, plus the Project M mission unlock. After completing the main story, visit Lysandre Labs B3F.",
        .flag_name = "flag_Fushigina_Okurimono_sub_0120", .flag_key = UINT64_C(0xBD7121CAF1FC28B3),
        .items = {{662, 1}, {663, 1}},
    },
    {
        .id = 2002, .name = "Shine Bright like a Gemstone", .kind = OG_MISSION,
        .description = "Diancite, plus the Diancie mission unlock. After completing the main story, speak to Mimi at the Looker Bureau.",
        .flag_name = "flag_Fushigina_Okurimono_sub_0119", .flag_key = UINT64_C(0xBD6728CAF1F3E013),
        .items = {{764, 1}},
    },
    {
        .id = 2004, .name = "Special distortion detected!", .kind = OG_MISSION,
        .description = "Unlock the Mega Garchomp Z distortion. Encounter requires Mega Dimension DLC and a completed hyperspace adventure. Win to earn its stone.",
        .flag_name = "flag_Fushigina_Okurimono_dim_MgabuZ", .flag_key = UINT64_C(0xFB1EFF0095F3BBFE),
    },
    {
        .id = 103, .name = "Nintendo Newsletter Item Set", .kind = OG_ITEMS,
        .description = "Local archive: 5 Max Revives, 10 Full Restores and 10 Ultra Balls. Originally a newsletter serial-code reward.",
        .items = {{29, 5}, {23, 10}, {2, 10}},
    },
    {
        .id = 104, .name = "Mega Shard Gift", .kind = OG_ITEMS,
        .description = "Local archive: 100 Mega Shards.",
        .items = {{2618, 100}},
    },
    {
        .id = 1603, .name = "Early Purchase Poke Ball Set", .kind = OG_ITEMS,
        .description = "Local archive: 100 Poke Balls. Originally an early-purchase serial-code reward.",
        .items = {{4, 100}},
    },
    {
        .id = 1604, .name = "Luxurious Poke Ball Set", .kind = OG_ITEMS,
        .description = "Local archive: 3 each of Fast, Level, Lure and Heavy Balls. Originally an early-purchase serial-code reward.",
        .items = {{492, 3}, {493, 3}, {494, 3}, {495, 3}},
    },
    {
        .id = 1605, .name = "Adventure Set", .kind = OG_ITEMS,
        .description = "Local archive: 20 Super Potions, 5 Revives, 3 Full Heals and 1 Rare Candy. Originally a purchase-bonus serial-code reward.",
        .items = {{26, 20}, {28, 5}, {27, 3}, {50, 1}},
    },
    {
        .id = 1612, .name = "Ultra Ball Set", .kind = OG_ITEMS,
        .description = "Local archive: 100 Ultra Balls. Originally an all-in-one bundle serial-code reward.",
        .items = {{2, 100}},
    },
    {
        .id = 101, .name = "Birthday Audino", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 5 Audino inspired by the first birthday distribution.",
        .pokemon = {.species = 531, .moves = {45, 273, 574, 219}, .level = 5, .gender = 3, .nature = 255, .pid_type = 1},
    },
    {
        .id = 102, .name = "Pokemon Center Slowpoke", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 5 Slowpoke inspired by the Kagawa opening reward.",
        .pokemon = {.species = 79, .moves = {347, 45, 352, 219}, .level = 5, .gender = 3, .nature = 255, .pid_type = 1},
    },
    {
        .id = 105, .name = "Birthday Audino (second distribution)", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 5 Audino representing a separate birthday reward. Unknown original attributes use local defaults.",
        .pokemon = {.species = 531, .level = 5, .gender = 3, .nature = 255, .pid_type = 1},
    },
    {
        .id = 603, .name = "Worlds26 Greninja", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 50 Greninja with Protean. Originally an attendee serial-code reward.",
        .pokemon = {.species = 658, .moves = {594, 56, 399, 58}, .level = 50, .gender = 3, .nature = 255, .ability = 2, .pid_type = 1},
    },
    {
        .id = 1601, .name = "Ralts and Gardevoirite", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 6 Ralts and Gardevoirite, respecting unique-stone ownership.",
        .pokemon = {.species = 280, .held_item = 657, .moves = {574, 45, 93, 129}, .level = 6, .gender = 3, .nature = 255, .pid_type = 1},
    },
    {
        .id = 1607, .name = "Alpha Charizard", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 36 alpha Charizard with Solar Beam. Originally a password reward.",
        .pokemon = {.species = 6, .moves = {76, 53, 337, 403}, .level = 36, .gender = 3, .nature = 255, .pid_type = 1, .alpha = true},
    },
    {
        .id = 1613, .name = "Shiny Xerneas", .kind = OG_POKEMON,
        .description = "Offline recreation: a shiny Lv. 100 Xerneas with local default moves.",
        .pokemon = {.species = 716, .level = 100, .gender = 3, .nature = 255, .pid_type = 2},
    },
    {
        .id = 1615, .name = "Spewpa (random pattern)", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 9 Spewpa with one of 16 evolution patterns. One gift, with a random pattern; unknown original attributes use local defaults.",
        .pokemon = {.species = 665, .level = 9, .gender = 3, .nature = 255, .pid_type = 1},
        .random_spewpa_form = true,
    },
    {
        .id = 1616, .name = "Poke Ball Pattern Vivillon", .kind = OG_POKEMON,
        .description = "Offline recreation: a Lv. 12 Vivillon with the Poke Ball pattern and local default moves.",
        .pokemon = {.species = 666, .form = 19, .level = 12, .gender = 3, .nature = 255, .pid_type = 1},
    },
    {
        .id = 1602, .name = "Trench Coat and Pants Set", .kind = OG_CLOTHING,
        .description = "Local archive: the Beige Trench Coat and Pants Set, a combined outfit. Originally a purchase-bonus serial-code reward; current outfit is preserved.",
        .clothing = {3021111},
    },
};

size_t offline_gift_count(void) { return sizeof(gifts) / sizeof(gifts[0]); }

const OfflineGift *offline_gift_at(size_t index) {
    return index < offline_gift_count() ? &gifts[index] : NULL;
}
