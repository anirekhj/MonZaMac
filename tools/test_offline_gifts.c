// Exercise the actual production hook with authored guest memory and native-call doubles only.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/offline_gift_catalog.h"
static size_t fixture_count(void);
#define offline_gift_count fixture_count
#include "../runtime/hle/offline_gifts.c"
#undef offline_gift_count

uint64_t g_alias_base, g_alias_size;
uint8_t g_game_build[8];
int g_nmodules;
volatile int g_rt_unwind;
static struct {
    uint64_t capture;
    _Alignas(16) uint8_t page[0x200], model[0x200], manager[0x80], receipts[0x40], rewards[0x80];
    _Alignas(16) uint8_t item_manager[0x10], database[0x10];
    _Alignas(16) uint8_t clothing_context[0x20], clothing_component[0x20];
    _Alignas(64) uint8_t heap[256 * 1024];
} arena;
static struct Region { uint64_t address; void *bytes; size_t size; bool readable; } regions[20];
static size_t region_count, heap_used, count_override;
static uint64_t item_manager_pointer, database_pointer, missing_function;
static uint64_t clothing_component_pointer, clothing_owned_results[10];
static uint32_t clothing_ready;
static unsigned clothing_component_calls, clothing_queries[10];
static unsigned lookups, overrides, original_calls, native_calls, allocations, applies, confirmed;
static unsigned menu_calls, option_count, queries[65536], owned[3000], capacities[3000];
static bool receipts_by_id[65536], record_on_apply, gallery_closed;
static uint64_t query_high_bits, blocked_result;
static HostFn registered;
static char history_status[8][MAX_LOCAL_GIFTS][160];
static bool history_available[8][MAX_LOCAL_GIFTS];
enum Effect { NORMAL, CLAIMED_DURING_PICKER, UNWIND_DURING_PICKER, CONCURRENT_PICKER, BAD_INDEX, CLOTHING_OWNED_DURING_PICKER };
static struct Action { uint16_t gift_id; enum Effect effect; } actions[8];
static unsigned action_count;

static uint64_t address(const void *p) { return (uintptr_t)p; }
static void map(uint64_t at, void *bytes, size_t size) {
    assert(region_count < sizeof regions / sizeof *regions);
    regions[region_count++] = (struct Region){at, bytes, size, true};
}
static void unreadable(uint64_t at) {
    for (size_t i = 0; i < region_count; i++) {
        if (regions[i].address == at) { regions[i].readable = false; return; }
    }
    assert(!"unknown authored region");
}
static void pointer(void *at, const void *value) { uint64_t p = address(value); memcpy(at, &p, sizeof p); }
static size_t index_for(uint16_t id) {
    for (size_t i = 0; i < offline_gift_count(); i++) if (offline_gift_at(i)->id == id) return i;
    assert(!"unknown authored catalog ID"); return 0;
}
static const OfflineGift *gift_for(uint16_t id) { return offline_gift_at(index_for(id)); }
static size_t fixture_count(void) { return count_override == SIZE_MAX ? offline_gift_count() : count_override; }
static uint16_t word(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static Cpu cpu(void) {
    Cpu c;
    for (size_t i = 0; i < sizeof c; i++) ((uint8_t *)&c)[i] = (uint8_t)(i * 13 + 7);
    c.x[0] = address(&arena.capture);
    return c;
}
// No save work may be entered or required while the gift callback owns the native UI lock.
static bool save_function(uint64_t at) {
    return at == UINT64_C(0x70009632f8) || at == UINT64_C(0x7000962b88) || at == UINT64_C(0x700090a164);
}
static void original(Cpu *c) { original_calls++; c->x[0] = 0xfedc; }
GuestFn dispatch_lookup(uint64_t function) { assert(!save_function(function)); lookups++; return function == missing_function ? NULL : original; }
void hle_override(uint64_t at, HostFn fn, const char *name) {
    assert(at == UINT64_C(0x7002c6e690));  // the indirect invoke thunk, not its directly called page body
    assert(!strcmp(name, "offline_mystery_gifts")); assert(fn); overrides++; registered = fn;
}
bool kernel_read_guest(void *out, uint64_t at, size_t size) {
    assert(at != UINT64_C(0x7003ec32b8));  // no callback-time save-manager probing either
    if (!out || !size || !guest_contains(at, size)) return false;
    for (size_t i = 0; i < region_count; i++) {
        struct Region *r = &regions[i];
        if (r->readable && at >= r->address && at - r->address < r->size && size <= r->size - (at - r->address)) {
            memcpy(out, (uint8_t *)r->bytes + (at - r->address), size); return true;
        }
    }
    return false;
}
void *hle_alloc(uint64_t size) {
    assert(size && size <= sizeof arena.heap - heap_used);
    void *out = arena.heap + heap_used; heap_used += (size + 63) & ~UINT64_C(63);
    memset(out, 0, size); allocations++; return out;
}
uint64_t rt_ticks(void) { return 0x12345678; }
void devlog(const char *subject, const char *fmt, ...) { assert(!strcmp(subject, "gift")); (void)fmt; }
static void read_card(uint64_t at, uint8_t *card) {
    assert(kernel_read_guest(card, at, OFFLINE_GIFT_CARD_SIZE));
    assert(card[0xe] == 1 && card[0x10] == 1);
}
void guest_call(Cpu *c, uint64_t function) {
    native_calls++;
    uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], result = 0;
    assert(c->x[3] == 0);
    if (function != CLOTHING_OWNED) assert(a2 == 0);
    if (function == GIFT_ELIGIBLE || function == GIFT_APPLY) {
        assert(a0 == address(arena.manager));
        uint8_t card[OFFLINE_GIFT_CARD_SIZE]; read_card(a1, card);
        uint16_t id = word(card + 8); const OfflineGift *gift = gift_for(id);
        assert(card[0x11] == gift->kind);
        if (function == GIFT_ELIGIBLE) { queries[id]++; result = query_high_bits | (receipts_by_id[id] ? 0 : 1); }
        else {
            assert(!receipts_by_id[id]);
            if (gift->kind == OG_CLOTHING) {
                for (unsigned category = 0; category < 10; category++) assert(!(clothing_owned_results[category] & 1));
            }
            applies++;
            if (record_on_apply) {
                receipts_by_id[id] = true; confirmed++;
                if (gift->kind == OG_CLOTHING) clothing_owned_results[9] = 1;
                for (unsigned i = 0; i < 6 && gift->items[i].id; i++) {
                    unsigned item = gift->items[i].id;
                    unsigned next = owned[item] + gift->items[i].count;
                    owned[item] = next < capacities[item] ? next : capacities[item];
                }
            }
        }
    } else if (function == POKEMON_BLOCKED) {
        assert(a0 == 5 && !a1); result = blocked_result;
    } else if (function == ITEM_COUNT || function == ITEM_CAPACITY) {
        assert(a1 < 3000); assert(a0 == address(function == ITEM_COUNT ? arena.item_manager : arena.database));
        result = function == ITEM_COUNT ? owned[a1] : capacities[a1];
    } else if (function == CLOTHING_COMPONENT) {
        assert(a0 == CLOTHING_CONTEXT && !a1);
        clothing_component_calls++; result = clothing_component_pointer;
    } else if (function == CLOTHING_OWNED) {
        assert(a0 == address(arena.clothing_component) && a1 < 10 && a2 == 3021111);
        clothing_queries[a1]++; result = clothing_owned_results[a1];
    } else if (save_function(function)) {
        assert(!"native save calls from the gift callback can deadlock");
    } else assert(!"unexpected guest/native call, including network calls");
    memset(c, 0x3c, sizeof *c);  // native calls may clobber any caller state; the hook must restore every byte
    c->x[0] = result;
}
static void *parallel_picker(void *unused) {
    (void)unused; Cpu c = cpu(), before = c; registered(&c); assert(!memcmp(&c, &before, sizeof c)); return NULL;
}
int menu_mystery_gift_run(const MenuGiftOption *options, int count) {
    assert(registered && count == 19 && menu_calls < 8);
    unsigned visit = menu_calls++; option_count = (unsigned)count;
    for (int i = 0; i < count; i++) {
        const OfflineGift *gift = offline_gift_at((size_t)i);
        assert(!strcmp(options[i].name, gift->name) && !strcmp(options[i].description, gift->description));
        assert(options[i].status); history_available[visit][i] = options[i].available;
        snprintf(history_status[visit][i], sizeof history_status[visit][i], "%s", options[i].status);
    }
    struct Action action = visit < action_count ? actions[visit] : (struct Action){0};
    if (action.effect == CONCURRENT_PICKER) {
        pthread_t thread; assert(!pthread_create(&thread, NULL, parallel_picker, NULL)); assert(!pthread_join(thread, NULL));
    }
    if (action.effect == UNWIND_DURING_PICKER) g_rt_unwind = 1;
    if (action.effect == CLAIMED_DURING_PICKER) receipts_by_id[action.gift_id] = true;
    if (action.effect == CLOTHING_OWNED_DURING_PICKER) clothing_owned_results[4] = 1;
    if (action.effect == BAD_INDEX) { gallery_closed = true; return count + 9; }
    if (!action.gift_id) { gallery_closed = true; return -1; }
    gallery_closed = false;
    return (int)index_for(action.gift_id);
}
static void fresh(void) {
    unsetenv("LUMIOSE_TEST_MODULE"); offline_gifts_reset(); installed = false;
    memset(&arena, 0, sizeof arena); memset(queries, 0, sizeof queries); memset(owned, 0, sizeof owned);
    memset(receipts_by_id, 0, sizeof receipts_by_id); memset(history_status, 0, sizeof history_status);
    memset(history_available, 0, sizeof history_available); memset(actions, 0, sizeof actions);
    g_alias_base = address(&arena); g_alias_size = sizeof arena; g_nmodules = 2; g_rt_unwind = 0;
    memcpy(g_game_build, supported_build, sizeof g_game_build);
    region_count = heap_used = 0; count_override = SIZE_MAX; missing_function = 0;
    lookups = overrides = original_calls = native_calls = allocations = applies = confirmed = 0;
    menu_calls = option_count = action_count = 0; registered = NULL;
    blocked_result = query_high_bits = 0; record_on_apply = true; gallery_closed = false;
    clothing_ready = 1; clothing_component_pointer = address(arena.clothing_component); clothing_component_calls = 0;
    memset(clothing_queries, 0, sizeof clothing_queries); memset(clothing_owned_results, 0, sizeof clothing_owned_results);
    for (unsigned i = 0; i < 3000; i++) capacities[i] = 999;
    capacities[662] = capacities[663] = capacities[764] = 1;
    pointer(&arena.capture, arena.page); pointer(arena.page + 0x178, arena.model);
    pointer(arena.model + 0x198, arena.manager); pointer(arena.manager + 0x30, arena.receipts);
    pointer(arena.manager + 0x38, arena.rewards);
    map(address(&arena.capture), &arena.capture, sizeof arena.capture);
    map(address(arena.page), arena.page, sizeof arena.page); map(address(arena.model), arena.model, sizeof arena.model);
    map(address(arena.manager), arena.manager, sizeof arena.manager); map(address(arena.receipts), arena.receipts, sizeof arena.receipts);
    map(address(arena.rewards), arena.rewards, sizeof arena.rewards); map(address(arena.heap), arena.heap, sizeof arena.heap);
    map(address(arena.item_manager), arena.item_manager, sizeof arena.item_manager);
    map(address(arena.database), arena.database, sizeof arena.database);
    item_manager_pointer = address(arena.item_manager); database_pointer = address(arena.database);
    map(ITEM_MANAGER, &item_manager_pointer, 8); map(ITEM_DATABASE, &database_pointer, 8);
    map(CLOTHING_CONTEXT, arena.clothing_context, sizeof arena.clothing_context);
    map(CLOTHING_READY, &clothing_ready, sizeof clothing_ready);
    map(address(arena.clothing_component), arena.clothing_component, sizeof arena.clothing_component);
}
static void install(void) { offline_gifts_install(); assert(registered && overrides == 1); }
static void invoke(void) { Cpu c = cpu(), before = c; assert(registered); registered(&c); assert(!memcmp(&c, &before, sizeof c)); }
static void choose(uint16_t id, enum Effect effect) { assert(action_count < 8); actions[action_count++] = (struct Action){id, effect}; }
static void no_mutation(void) { assert(!applies && !confirmed && !original_calls); }
static void guards(void) {
    fresh(); g_game_build[0] ^= 1; offline_gifts_install(); assert(!registered && !overrides); Cpu c = cpu(); gift_picker(&c); assert(original_calls == 1 && c.x[0] == 0xfedc && !native_calls);
    fresh(); g_nmodules = 3; offline_gifts_install(); assert(!registered); c = cpu(); gift_picker(&c); assert(original_calls == 1 && !native_calls);
    fresh(); assert(!setenv("LUMIOSE_TEST_MODULE", "local-module", 1)); offline_gifts_install(); assert(!registered); c = cpu(); gift_picker(&c); assert(original_calls == 1 && !native_calls);
    const uint64_t required[] = {GIFT_APPLY, CLOTHING_COMPONENT, CLOTHING_OWNED};
    for (unsigned i = 0; i < sizeof required / sizeof *required; i++) {
        fresh(); missing_function = required[i]; offline_gifts_install(); assert(!registered && !overrides);
    }
    missing_function = 0; install(); unsigned prior = lookups; offline_gifts_install(); assert(lookups == prior && overrides == 1);
}
static void invalid_capture(void) {
    for (unsigned which = 0; which < 11; which++) {
        fresh(); install(); Cpu c = cpu();
        if (which == 0) c.x[0] = 0;
        else if (which == 1) arena.capture = 0;
        else if (which == 2) arena.capture = address(arena.page) + 1;
        else if (which == 3) arena.capture = MAIN_BASE + 0x100000;  // reserved, never mapped by the fixture
        else if (which == 4) c.x[0] = address(&arena.capture) + 1;  // crosses capture region boundary
        else if (which == 5) regions[1].size = 0x178 + 7;
        else if (which == 6) memset(arena.manager + 0x30, 0, 8);
        else if (which == 7) memset(arena.manager + 0x38, 0, 8);
        else {
            uint64_t hole = MAIN_BASE + 0x200000;
            void *slot = which == 8 ? arena.manager + 0x30 : which == 9 ? arena.manager + 0x38 : arena.model + 0x198;
            memcpy(slot, &hole, sizeof hole);
        }
        Cpu before = c; registered(&c); assert(!memcmp(&c, &before, sizeof c));
        no_mutation(); assert(!native_calls && !menu_calls && !allocations);
    }
    fresh(); install(); count_override = 0; invoke(); no_mutation(); assert(!menu_calls && !allocations);
    count_override = MAX_LOCAL_GIFTS + 1; invoke(); no_mutation(); assert(!menu_calls && !allocations);
}
static void claim_and_instruction(void) {
    fresh(); install(); query_high_bits = 0x100; choose(103, NORMAL); choose(0, NORMAL); invoke();
    size_t i = index_for(103); assert(option_count == 19 && menu_calls == 2);
    assert(history_available[0][i] && !strcmp(history_status[0][i], "Available"));
    assert(!history_available[1][i] && strstr(history_status[1][i], "Received") &&
           strstr(history_status[1][i], "Save your progress after leaving this menu"));
    assert(applies == 1 && confirmed == 1 && receipts_by_id[103] && queries[103] >= 3);
    assert(owned[29] == 5 && owned[23] == 10 && owned[2] == 10);
    assert(gallery_closed && !original_calls);
    choose(103, NORMAL); invoke(); assert(applies == 1);  // never a second grant
}
static void preflight(void) {
    fresh(); install(); owned[29] = capacities[29]; choose(103, NORMAL); invoke();
    assert(!history_available[0][index_for(103)] && strstr(history_status[0][index_for(103)], "Make room")); no_mutation();
    fresh(); install(); capacities[29] = 0; invoke(); assert(!history_available[0][index_for(103)]); no_mutation();
    fresh(); install(); blocked_result = 0x101; choose(101, NORMAL); invoke();
    assert(!history_available[0][index_for(101)] && strstr(history_status[0][index_for(101)], "storage")); no_mutation();
    fresh(); install(); item_manager_pointer = 0; choose(103, NORMAL); invoke();
    assert(!history_available[0][index_for(103)] && strstr(history_status[0][index_for(103)], "unavailable")); no_mutation();
    fresh(); install(); item_manager_pointer = MAIN_BASE + 0x300000; choose(103, NORMAL); invoke();
    assert(!history_available[0][index_for(103)] && strstr(history_status[0][index_for(103)], "unavailable")); no_mutation();
    fresh(); install(); database_pointer = MAIN_BASE + 0x400000; choose(103, NORMAL); invoke();
    assert(!history_available[0][index_for(103)] && strstr(history_status[0][index_for(103)], "unavailable")); no_mutation();
}
static void wardrobe_owned(void) {
    const unsigned categories[] = {0, 4, 9};
    for (unsigned i = 0; i < sizeof categories / sizeof *categories; i++) {
        fresh(); install();
        unsigned category = categories[i]; clothing_owned_results[category] = 0x101;
        choose(1602, NORMAL); invoke();
        size_t index = index_for(1602);
        assert(!history_available[0][index] && !strcmp(history_status[0][index], "You already own this outfit."));
        assert(!receipts_by_id[1602] && clothing_component_calls);
        for (unsigned c = 0; c < 10; c++) assert((clothing_queries[c] > 0) == (c <= category));
        assert(clothing_owned_results[category] == 0x101); no_mutation();
    }
    // Only the native Boolean low bit means owned; other result bits must not hide the outfit.
    fresh(); install();
    for (unsigned category = 0; category < 10; category++) clothing_owned_results[category] = 0x100;
    choose(1602, NORMAL); invoke();
    assert(history_available[0][index_for(1602)] && applies == 1 && confirmed == 1 && receipts_by_id[1602]);
    for (unsigned category = 0; category < 10; category++) assert(clothing_queries[category] >= 2);
}
static void wardrobe_unavailable(void) {
    for (unsigned which = 0; which < 9; which++) {
        fresh(); install();
        if (which == 0) unreadable(CLOTHING_CONTEXT);
        else if (which == 1) unreadable(CLOTHING_READY);
        else if (which == 2) clothing_ready = 0;
        else if (which == 3) clothing_component_pointer = 0;
        else if (which == 4) clothing_component_pointer = address(arena.clothing_component) + 1;
        else if (which == 5) clothing_component_pointer = MAIN_BASE + 0x500000;
        else if (which == 6) clothing_component_pointer = UINT64_MAX - 7;
        else if (which == 7) unreadable(address(arena.clothing_component));
        else {
            for (size_t i = 0; i < region_count; i++) if (regions[i].address == CLOTHING_READY) regions[i].size = 3;
        }
        choose(1602, NORMAL); invoke();
        size_t index = index_for(1602);
        assert(!history_available[0][index] && !strcmp(history_status[0][index],
               "The wardrobe is unavailable. Close this menu and try again."));
        assert(!receipts_by_id[1602]);
        assert((clothing_component_calls > 0) == (which >= 3 && which <= 7));
        for (unsigned category = 0; category < 10; category++) assert(!clothing_queries[category]);
        no_mutation();
    }
}
static void wardrobe_recheck(void) {
    fresh(); install(); choose(1602, CLOTHING_OWNED_DURING_PICKER); invoke();
    size_t index = index_for(1602);
    assert(history_available[0][index] && !history_available[1][index]);
    assert(!strcmp(history_status[1][index], "You already own this outfit."));
    assert(clothing_queries[4] >= 3 && clothing_owned_results[4] == 1 && !receipts_by_id[1602]);
    no_mutation();
}
static void unique_mission(void) {
    fresh(); install(); owned[662] = owned[663] = 1; choose(2003, NORMAL); invoke();
    assert(history_available[0][index_for(2003)]); assert(applies == 1 && confirmed == 1 && receipts_by_id[2003]);
    assert(owned[662] == 1 && owned[663] == 1 && gallery_closed);
}
static void claimed_recheck(void) {
    fresh(); install(); choose(103, CLAIMED_DURING_PICKER); invoke();
    assert(history_available[0][index_for(103)] && !history_available[1][index_for(103)]);
    assert(receipts_by_id[103] && queries[103] >= 2); no_mutation();
}
static void cancellation(void) {
    fresh(); install(); invoke(); no_mutation(); assert(menu_calls == 1);
    fresh(); install(); choose(103, UNWIND_DURING_PICKER); invoke(); no_mutation(); assert(g_rt_unwind);
    fresh(); install(); g_rt_unwind = 1; invoke(); no_mutation(); assert(!native_calls && !menu_calls);
    fresh(); install(); choose(0, BAD_INDEX); invoke(); no_mutation(); assert(menu_calls == 1);
}
static void concurrent(void) { fresh(); install(); choose(0, CONCURRENT_PICKER); invoke(); no_mutation(); assert(menu_calls == 1); }
static void failure_and_reset(void) {
    fresh(); install(); record_on_apply = false; choose(103, NORMAL); invoke();
    size_t i = index_for(103); assert(applies == 1 && !confirmed && failed[i] && cards);
    action_count = menu_calls = 0; choose(103, NORMAL); invoke();
    assert(!history_available[0][i] && strstr(history_status[0][i], "Restart")); assert(applies == 1);
    unsigned allocated = allocations; offline_gifts_reset(); assert(!cards && !failed[i]);
    record_on_apply = true; action_count = menu_calls = 0; choose(103, NORMAL); invoke();
    assert(history_available[0][i] && applies == 2 && confirmed == 1 && allocations > allocated);
    assert(overrides == 1);  // registered override persists; boot-owned pointers and failures do not
}
static void no_callback_save(void) {
    fresh(); install();
    choose(103, NORMAL); choose(2003, NORMAL); choose(1602, NORMAL); choose(1601, NORMAL); invoke();
    assert(applies == 4 && confirmed == 4 && gallery_closed && menu_calls == 5);
    const uint16_t ids[] = {103, 2003, 1602, 1601};
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
        size_t index = index_for(ids[i]); assert(receipts_by_id[ids[i]] && !history_available[4][index]);
        assert(strstr(history_status[4][index], "Save your progress after leaving this menu"));
    }
    // guest_call and dispatch_lookup reject all native save entry points; kernel_read_guest rejects its manager.
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "guards")) guards(); else if (!strcmp(argv[1], "invalid")) invalid_capture();
    else if (!strcmp(argv[1], "claim")) claim_and_instruction(); else if (!strcmp(argv[1], "preflight")) preflight();
    else if (!strcmp(argv[1], "wardrobe-owned")) wardrobe_owned();
    else if (!strcmp(argv[1], "wardrobe-unavailable")) wardrobe_unavailable();
    else if (!strcmp(argv[1], "wardrobe-recheck")) wardrobe_recheck();
    else if (!strcmp(argv[1], "unique")) unique_mission(); else if (!strcmp(argv[1], "recheck")) claimed_recheck();
    else if (!strcmp(argv[1], "cancel")) cancellation(); else if (!strcmp(argv[1], "concurrent")) concurrent();
    else if (!strcmp(argv[1], "reset")) failure_and_reset(); else if (!strcmp(argv[1], "no-save")) no_callback_save();
    else assert(!"unknown scenario");
    puts("PASS authored offline gift production-hook scenario"); return 0;
}
