// Controller angular velocity -> camera stick. No SDL, game state, or host I/O.
#include "gyro_camera.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define STALE_NS 150000000ull
#define QUIET_NS 500000000ull
#define RATE_TAU 0.020
#define GRAVITY_TAU 0.080
#define GYRO_FLOOR 0.018
#define FULL_RATE 3.14159265358979323846
#define STICK_FLOOR 0.2

static void clear_motion(GyroCamera *c) {
    memset(c->filtered, 0, sizeof c->filtered);
    c->have_sample = false;
}
static void clear_quiet(GyroCamera *c) {
    c->quiet_start_ns = c->quiet_last_ns = 0;
    c->quiet_weight = 0;
    memset(c->quiet_sum, 0, sizeof c->quiet_sum);
    memset(c->quiet_square, 0, sizeof c->quiet_square);
}
static void activation(GyroCamera *c, bool active) {
    if (c->active == active) return;
    clear_motion(c);
    clear_quiet(c);
    c->active = active;
}
void gyro_camera_reset(GyroCamera *c) {
    if (c) memset(c, 0, sizeof *c);
}
bool gyro_camera_calibrated(const GyroCamera *c) { return c && c->calibrated; }

static bool vector_valid(const float v[3], double limit) {
    return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) &&
        fabs(v[0]) <= limit && fabs(v[1]) <= limit && fabs(v[2]) <= limit;
}
static double length3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Returns whether this accelerometer report is also quiet enough for calibration.
static bool gravity_sample(GyroCamera *c, uint64_t ns, const float accel[3]) {
    if (!vector_valid(accel, 100)) return false;
    double unit[3] = {accel[0], accel[1], accel[2]}, length = length3(unit);
    if (length < 3 || length > 20) return false;
    for (int i = 0; i < 3; i++) unit[i] /= length;
    double a = 1;
    if (c->have_gravity && ns > c->accel_ns && ns - c->accel_ns < STALE_NS)
        a = -expm1(-(double)(ns - c->accel_ns) / 1e9 / GRAVITY_TAU);
    for (int i = 0; i < 3; i++) c->gravity[i] += (unit[i] - c->gravity[i]) * a;
    double norm = length3(c->gravity);
    // Opposite directions can cancel during interpolation; reseed instead of
    // normalizing a tiny vector and amplifying accelerometer noise.
    if (norm < 1e-6) memcpy(c->gravity, unit, sizeof unit);
    else for (int i = 0; i < 3; i++) c->gravity[i] /= norm;
    c->have_gravity = true;
    c->accel_ns = ns;
    return fabs(length - 9.80665) <= 1.5;
}

static void calibration_sample(GyroCamera *c, uint64_t ns, const float gyro[3], bool accel_quiet) {
    if (c->active || c->calibrated || !accel_quiet) { clear_quiet(c); return; }
    double raw[3] = {gyro[0], gyro[1], gyro[2]};
    if (length3(raw) > 0.05) { clear_quiet(c); return; }
    if (!c->quiet_start_ns) {
        c->quiet_start_ns = c->quiet_last_ns = ns;
        memcpy(c->quiet_previous, raw, sizeof raw);
        return;
    }
    if (ns <= c->quiet_last_ns || ns - c->quiet_last_ns >= STALE_NS) {
        clear_quiet(c);
        c->quiet_start_ns = c->quiet_last_ns = ns;
        memcpy(c->quiet_previous, raw, sizeof raw);
        return;
    }
    double dt = (double)(ns - c->quiet_last_ns) / 1e9;
    for (int i = 0; i < 3; i++) {
        double before = c->quiet_previous[i];
        c->quiet_sum[i] += (before + raw[i]) * 0.5 * dt;
        c->quiet_square[i] += (before * before + raw[i] * raw[i]) * 0.5 * dt;
        c->quiet_previous[i] = raw[i];
    }
    c->quiet_weight += dt;
    c->quiet_last_ns = ns;
    if (ns - c->quiet_start_ns < QUIET_NS) return;
    for (int i = 0; i < 3; i++) {
        double mean = c->quiet_sum[i] / c->quiet_weight;
        double variance = c->quiet_square[i] / c->quiet_weight - mean * mean;
        if (variance > 0.0025 * 0.0025) { clear_quiet(c); return; }
    }
    for (int i = 0; i < 3; i++) c->bias[i] = c->quiet_sum[i] / c->quiet_weight;
    c->calibrated = true;
    clear_quiet(c);
}

void gyro_camera_sample(GyroCamera *c, uint64_t ns, const float gyro[3], const float accel[3], bool active) {
    if (!c) return;
    activation(c, active);
    // After a release/menu/focus reset, queued pre-transition reports must not
    // seed fresh motion just because they arrive after aiming is enabled again.
    if (!ns || (c->sample_ns && ns <= c->sample_ns) ||
        (active && !c->have_sample && ns < c->update_ns) || !vector_valid(gyro, 100)) {
        clear_motion(c);
        clear_quiet(c);
        return;
    }
    uint64_t elapsed = c->sample_ns ? ns - c->sample_ns : 0;
    if (elapsed >= STALE_NS) { clear_motion(c); clear_quiet(c); }
    bool accel_quiet = accel ? gravity_sample(c, ns, accel) : true;
    c->sample_ns = ns;
    calibration_sample(c, ns, gyro, accel_quiet);
    if (!active) return;
    double a = c->have_sample && elapsed ? -expm1(-(double)elapsed / 1e9 / RATE_TAU) : 1;
    for (int i = 0; i < 3; i++) {
        double rate = (double)gyro[i] - c->bias[i];
        c->filtered[i] += (rate - c->filtered[i]) * a;
    }
    c->have_sample = true;
}

void gyro_camera_update(GyroCamera *c, uint64_t ns, bool active,
                        const GyroCameraConfig *config, float *rx, float *ry) {
    if (rx) *rx = 0;
    if (ry) *ry = 0;
    if (!c) return;
    activation(c, active);
    if (!ns || (c->update_ns && ns < c->update_ns) || (c->sample_ns && ns < c->sample_ns)) {
        clear_motion(c);
        clear_quiet(c);
        c->sample_ns = c->accel_ns = 0;
        c->have_gravity = false;
        c->update_ns = ns;
        return;
    }
    c->update_ns = ns;
    if (!c->sample_ns || ns - c->sample_ns >= STALE_NS) {
        clear_motion(c);
        clear_quiet(c);
        return;
    }
    if (!active || !c->have_sample) return;
    double yaw = c->filtered[1], pitch = c->filtered[0];
    if (c->have_gravity && ns >= c->accel_ns && ns - c->accel_ns < STALE_NS) {
        const double *g = c->gravity;
        yaw = c->filtered[0] * g[0] + c->filtered[1] * g[1] + c->filtered[2] * g[2];
        // Local X projected into the plane perpendicular to gravity. Reduce
        // pitch near an edge-on controller, where that direction is ill-defined.
        double norm = sqrt(fmax(0, 1 - g[0] * g[0]));
        pitch = (c->filtered[0] - yaw * g[0]) / fmax(norm, 0.3);
    }
    double speed = hypot(yaw, pitch);
    if (!isfinite(speed) || speed <= GYRO_FLOOR) return;
    double sensitivity = config && isfinite(config->sensitivity) ? config->sensitivity : 1;
    sensitivity = fmin(5, fmax(0.1, sensitivity));
    double magnitude = STICK_FLOOR + (1 - STICK_FLOOR) * fmin(1, speed * sensitivity / FULL_RATE);
    double x = -yaw * magnitude / speed, y = pitch * magnitude / speed;
    if (config && config->invert_x) x = -x;
    if (config && config->invert_y) y = -y;
    if (rx) *rx = (float)x;
    if (ry) *ry = (float)y;
}
