// Shared native SixAxis activation for direct SDK imports and the HID service.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SIXAXIS_ORIGIN_HLE, SIXAXIS_ORIGIN_IPC } SixAxisOrigin;
// Idempotent per logical handle; aggregate demand stays enabled until every
// started handle is stopped, unless the explicit host policy remains enabled.
// Returns false for unsupported/invalid handles.
bool sixaxis_set_started(uint32_t handle, bool started, SixAxisOrigin origin);
// Explicit host policy, independent of the camera-to-stick feature. This does
// not change the guest's Start/Stop flags and never fabricates a motion report.
// The UI owner reapplies the saved setting after a guest reboot resets policy.
void sixaxis_set_always_available(bool enabled);

#ifdef __cplusplus
}
#endif
