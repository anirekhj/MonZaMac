// Local gift archive. Native game routines own rewards, receipts and normal saves.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../hle.h"
#include "../kernel.h"
#include "../devlog.h"
#include "../offline_gifts.h"
#include "../offline_gift_card.h"
#include "../ui/menu.h"

// Runtime adapters for main build b1f12fd919eae86a (v2.0.2).
#define GIFT_ENTRY 0x7002c6e690ull
#define GIFT_ELIGIBLE 0x7002dc03b0ull
#define GIFT_APPLY 0x7002dbffe8ull
#define POKEMON_BLOCKED 0x7002c66284ull
#define ITEM_COUNT 0x70002bc834ull
#define ITEM_CAPACITY 0x7000a5a228ull
#define ITEM_MANAGER 0x7003ec32b0ull
#define ITEM_DATABASE 0x7003ec3320ull
#define CLOTHING_CONTEXT 0x700610a580ull
#define CLOTHING_READY 0x700610a980ull
#define CLOTHING_COMPONENT 0x700292dee0ull
#define CLOTHING_OWNED 0x700291e02cull
#define MAX_LOCAL_GIFTS 64
static const uint8_t supported_build[8] = {0xb1,0xf1,0x2f,0xd9,0x19,0xea,0xe8,0x6a};
extern uint8_t g_game_build[8];
static bool installed;
static atomic_flag busy = ATOMIC_FLAG_INIT;
static uint8_t *cards;
static bool cards_valid[MAX_LOCAL_GIFTS];
static bool failed[MAX_LOCAL_GIFTS];

RT_ON_RESET(offline_gifts_reset) {
    cards = NULL;  // Guest HLE memory belongs to one boot.
    memset(cards_valid, 0, sizeof cards_valid);
    memset(failed, 0, sizeof failed);
    atomic_flag_clear(&busy);
}

static bool supported(void) {
    return g_nmodules == 2 && !getenv("LUMIOSE_TEST_MODULE") && !memcmp(g_game_build, supported_build, sizeof supported_build);
}
static bool readable_pointer(uint64_t value) {
    uint8_t readable;
    return value && !(value & 7) && guest_contains(value, 8) && kernel_read_guest(&readable, value, sizeof readable);
}
static bool read_pointer(uint64_t at, uint64_t *value) {
    return kernel_read_guest(value, at, sizeof *value) && readable_pointer(*value);
}
static uint64_t call3(Cpu *c, uint64_t function, uint64_t a0, uint64_t a1, uint64_t a2) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = 0;
    guest_call(c, function);
    return c->x[0];
}
static uint64_t call(Cpu *c, uint64_t function, uint64_t a0, uint64_t a1) {
    return call3(c, function, a0, a1, 0);
}

static bool can_receive(Cpu *c, const OfflineGift *gift, char *reason, size_t size) {
    if (gift->kind == OG_POKEMON && (call(c, POKEMON_BLOCKED, 5, 0) & 1)) {
        snprintf(reason, size, "Your Pokemon storage is full or unavailable.");
        return false;
    }
    if (gift->kind == OG_CLOTHING) {
        uint32_t ready;
        uint8_t context;
        uint64_t wardrobe;
        if (!kernel_read_guest(&context, CLOTHING_CONTEXT, sizeof context) ||
            !kernel_read_guest(&ready, CLOTHING_READY, sizeof ready) || !ready ||
            !readable_pointer(wardrobe = call(c, CLOTHING_COMPONENT, CLOTHING_CONTEXT, 0))) {
            snprintf(reason, size, "The wardrobe is unavailable. Close this menu and try again.");
            return false;
        }
        // Native grants reject owned outfits before writing a receipt. Query all
        // ten wardrobe categories rather than duplicating the game's category database.
        for (unsigned i = 0; i < 6 && gift->clothing[i]; i++) {
            for (unsigned category = 0; category < 10; category++) {
                if (call3(c, CLOTHING_OWNED, wardrobe, category, gift->clothing[i]) & 1) {
                    snprintf(reason, size, "You already own this outfit.");
                    return false;
                }
            }
        }
    }
    uint64_t manager = 0, database = 0;
    for (unsigned i = 0; i < 6 && gift->items[i].id; i++) {
        if (!manager && (!read_pointer(ITEM_MANAGER, &manager) || !read_pointer(ITEM_DATABASE, &database))) {
            snprintf(reason, size, "The satchel is unavailable. Close this menu and try again.");
            return false;
        }
        OfflineGiftItem item = gift->items[i];
        uint32_t owned = (uint32_t)call(c, ITEM_COUNT, manager, item.id);
        uint32_t capacity = (uint32_t)call(c, ITEM_CAPACITY, database, item.id);
        if (!capacity || owned > capacity) {
            snprintf(reason, size, "This item cannot be received right now.");
            return false;
        }
        // Existing unique stones already fulfil that part of a mission reward.
        // Native addition clamps these to one while still applying the mission flag.
        if (gift->kind == OG_MISSION && capacity == 1 && owned == 1 && item.count == 1) continue;
        if (item.count > capacity - owned) {
            snprintf(reason, size, "Make room in your satchel before receiving this gift.");
            return false;
        }
    }
    return true;
}

static void gift_picker(Cpu *c) {
    if (!supported()) { GuestFn original = dispatch_lookup(GIFT_ENTRY); if (original) original(c); return; }
    if (atomic_flag_test_and_set(&busy)) return;
    Cpu saved = *c;
    uint64_t page, model, manager, receipts, rewards;
    bool changed = false;
    size_t count = offline_gift_count();
    if (!count || count > MAX_LOCAL_GIFTS || !read_pointer(c->x[0], &page) ||
        !read_pointer(page + 0x178, &model) || !read_pointer(model + 0x198, &manager) ||
        !read_pointer(manager + 0x30, &receipts) || !read_pointer(manager + 0x38, &rewards)) {
        devlog("gift", "local picker rejected an invalid page/model/manager");
        goto done;
    }
    // Native callbacks can outlive the picker. Cards stay immutable in guest
    // memory until the boot ends, including across repeated menu openings.
    if (!cards) {
        cards = hle_alloc(MAX_LOCAL_GIFTS * OFFLINE_GIFT_CARD_SIZE);
        uint32_t variation = (uint32_t)rt_ticks();
        for (size_t i = 0; i < count; i++)
            cards_valid[i] = offline_gift_build_card(offline_gift_at(i), variation, cards + i * OFFLINE_GIFT_CARD_SIZE, OFFLINE_GIFT_CARD_SIZE);
    }
    for (;;) {
        if (g_rt_unwind) break;
        MenuGiftOption options[MAX_LOCAL_GIFTS];
        char statuses[MAX_LOCAL_GIFTS][160];
        for (size_t i = 0; i < count; i++) {
            const OfflineGift *gift = offline_gift_at(i);
            bool available = false;
            const char *status;
            if (!cards_valid[i]) status = "Unavailable";
            else if (failed[i]) status = "The gift could not be completed. Restart the game before trying again.";
            else if (!(call(c, GIFT_ELIGIBLE, manager, (uintptr_t)(cards + i * OFFLINE_GIFT_CARD_SIZE)) & 1))
                status = changed ? "Received. Save your progress after leaving this menu." : "Already received";
            else if (!can_receive(c, gift, statuses[i], sizeof statuses[i])) status = statuses[i];
            else { status = "Available"; available = true; }
            options[i] = (MenuGiftOption){gift->name, gift->description, status, available};
        }
        int selected = menu_mystery_gift_run(options, (int)count);
        if (selected < 0 || selected >= (int)count || g_rt_unwind) break;
        const OfflineGift *gift = offline_gift_at((size_t)selected);
        uint8_t *card = cards + (size_t)selected * OFFLINE_GIFT_CARD_SIZE;
        if (!options[selected].available || !(call(c, GIFT_ELIGIBLE, manager, (uintptr_t)card) & 1) ||
            !can_receive(c, gift, statuses[selected], sizeof statuses[selected])) continue;
        call(c, GIFT_APPLY, manager, (uintptr_t)card);
        bool received = !(call(c, GIFT_ELIGIBLE, manager, (uintptr_t)card) & 1);
        devlog("gift", "local card %u kind=%u applied; receipt=%d", gift->id, gift->kind, received);
        if (!received) { failed[selected] = true; break; }
        changed = true;
    }
done:
    *c = saved;
    atomic_flag_clear(&busy);
}

void offline_gifts_install(void) {
    const uint64_t functions[] = {GIFT_ENTRY, GIFT_ELIGIBLE, GIFT_APPLY, POKEMON_BLOCKED, ITEM_COUNT, ITEM_CAPACITY,
                                  CLOTHING_COMPONENT, CLOTHING_OWNED};
    if (installed || !supported()) return;
    for (unsigned i = 0; i < sizeof functions / sizeof *functions; i++) if (!dispatch_lookup(functions[i])) return;
    hle_override(GIFT_ENTRY, gift_picker, "offline_mystery_gifts");
    installed = true;
    devlog("gift", "installed local Mystery Gift entry");
}
