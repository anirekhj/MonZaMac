// Authored native reward cards built from the local catalog, never distributed event-card assets.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "offline_gift_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OFFLINE_GIFT_CARD_SIZE 0x2c8
// Leaves out unchanged on failure; writes exactly CARD_SIZE bytes on success.
// This validates the supported catalog fields, not original-event authenticity or general Pokemon legality.
bool offline_gift_build_card(const OfflineGift *gift, uint32_t variation, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
