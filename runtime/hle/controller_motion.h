// Native controller orientation, independent of the optional gyro camera.
#pragma once
#include "../motion_input.h"

typedef struct {
    double orientation[4], acceleration[3], angular_velocity[3], angle[3];
    uint64_t acceleration_ns, gyro_ns, gravity_ns, sample_ns, delta_time_ns, sampling_number;
    bool have_gravity, acceleration_valid, gyro_valid;
} ControllerMotion;

void controller_motion_reset(ControllerMotion *motion);
void controller_motion_acceleration(ControllerMotion *motion, uint64_t timestamp_ns, const float data[3]);
void controller_motion_gyroscope(ControllerMotion *motion, uint64_t timestamp_ns, const float data[3]);
// Pure read: polling never integrates another copy of a sensor report. Clocks
// share the monotonic SDL event domain; old/invalid channels are marked absent.
bool controller_motion_read(const ControllerMotion *motion, uint64_t now_ns, InputMotionSnapshot *out);
