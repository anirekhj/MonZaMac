// Rate-based controller gyro camera math. Owned and called by one host thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double sensitivity;  // clamped to 0.1..5; nonfinite values use 1
    bool invert_x, invert_y;
} GyroCameraConfig;

typedef struct {
    double bias[3], gravity[3], filtered[3];
    double quiet_sum[3], quiet_square[3], quiet_previous[3], quiet_weight;
    uint64_t sample_ns, accel_ns, update_ns, quiet_start_ns, quiet_last_ns;
    bool active, have_sample, have_gravity, calibrated;
} GyroCamera;

// Disconnect/reconnect or manual calibration: clear all history and learned bias.
// Bias is learned after 500 ms continuously quiet with active=false.
void gyro_camera_reset(GyroCamera *camera);
bool gyro_camera_calibrated(const GyroCamera *camera);

// sample_time_ns and update's now_ns share one monotonic host clock (arrival time
// is suitable). Gyro is rad/s. Optional fresh accelerometer data is m/s^2; pass
// NULL when unavailable/stale. Supply the same activation gate here
// and in update, so calibration never runs while gyro aiming is active.
void gyro_camera_sample(GyroCamera *camera, uint64_t sample_time_ns,
                        const float gyro[3], const float accel[3], bool active);

// Outputs normalized right-stick deflection, including the game's 20% radial
// deadzone compensation. No angle/motion is accumulated between reads. Release,
// ineligible gameplay, invalid clocks, or >=150 ms without a report stop output.
// Ordinary release/menu transitions preserve calibration; reset clears it.
void gyro_camera_update(GyroCamera *camera, uint64_t now_ns, bool active,
                        const GyroCameraConfig *config, float *rx, float *ry);
