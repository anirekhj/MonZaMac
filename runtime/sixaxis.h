// Shared native SixAxis activation for direct SDK imports and the HID service.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SIXAXIS_ORIGIN_HLE, SIXAXIS_ORIGIN_IPC } SixAxisOrigin;
// Idempotent per logical handle; aggregate demand stays enabled until every
// started handle is stopped. Returns false for unsupported/invalid handles.
bool sixaxis_set_started(uint32_t handle, bool started, SixAxisOrigin origin);

#ifdef __cplusplus
}
#endif
