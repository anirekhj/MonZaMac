// Physical controller motion snapshot, independent of gyro-to-stick camera settings.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t sampling_number, delta_time_ns;
    float acceleration[3];     // SDL device axes, m/s^2 (including gravity)
    float angular_velocity[3]; // SDL device axes, radians/s
    float angle[3];            // radians
    float direction[9];        // row-major device-to-world basis; SDL world +Y is up
    bool acceleration_valid, gyro_valid, at_rest;
} InputMotionSnapshot;

// Guest-thread safe. The SixAxis adapter aggregates its started handles first.
void input_motion_set_enabled(bool enabled);
// Bounded coherent read; false for stale, inactive or unavailable physical data.
// sampling_number advances on physical sensor updates, never on guest reads.
bool input_motion_snapshot(InputMotionSnapshot *out);

#ifdef __cplusplus
}
#endif
