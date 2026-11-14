#include "controller_motion.h"
#include <math.h>
#include <string.h>

#define MOTION_STALE_NS 150000000ull
#define STANDARD_GRAVITY 9.80665

static bool valid_vector(const float data[3]) {
    return data && isfinite(data[0]) && isfinite(data[1]) && isfinite(data[2]) &&
        fabsf(data[0]) <= 100 && fabsf(data[1]) <= 100 && fabsf(data[2]) <= 100;
}
static double norm3(const double v[3]) { return sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
static bool fresh(uint64_t now, uint64_t stamp) { return stamp && now >= stamp && now - stamp < MOTION_STALE_NS; }
static void multiply(const double a[4], const double b[4], double out[4]) {
    double result[4] = {
        a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3],
        a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2],
        a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1],
        a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0]
    };
    memcpy(out, result, sizeof result);
}
static void normalize(double q[4]) {
    double n = sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (!isfinite(n) || n < 1e-12) { q[0] = 1; q[1] = q[2] = q[3] = 0; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
static void matrix(const double q[4], float out[9]) {
    double w=q[0], x=q[1], y=q[2], z=q[3];
    out[0]=1-2*(y*y+z*z); out[1]=2*(x*y-w*z); out[2]=2*(x*z+w*y);
    out[3]=2*(x*y+w*z); out[4]=1-2*(x*x+z*z); out[5]=2*(y*z-w*x);
    out[6]=2*(x*z-w*y); out[7]=2*(y*z+w*x); out[8]=1-2*(x*x+y*y);
}
static void advance(ControllerMotion *m, uint64_t ns) {
    if (ns <= m->sample_ns) return;
    m->delta_time_ns = m->sample_ns ? ns - m->sample_ns : 0;
    m->sample_ns = ns;
    m->sampling_number++;
}
void controller_motion_reset(ControllerMotion *m) {
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->orientation[0] = 1;
}

static void correct_gravity(ControllerMotion *m) {
    uint64_t ns = m->acceleration_ns, previous = m->gravity_ns;
    if (!m->acceleration_valid || !ns || ns <= previous) return;
    double length = norm3(m->acceleration);
    // Significant linear acceleration must not be interpreted as a tilt. Raw
    // acceleration is still reported; the gyroscope carries orientation then.
    if (fabs(length - STANDARD_GRAVITY) > 1.5) return;
    m->gravity_ns = ns;
    float r[9]; matrix(m->orientation, r);
    double world[3] = {0};
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) world[row] += r[row*3+col] * m->acceleration[col] / length;
    double axis[3] = {-world[2], 0, world[0]};
    double sine = norm3(axis), cosine = fmax(-1, fmin(world[1], 1));
    double angle = atan2(sine, cosine);
    double alpha = !m->have_gravity || !fresh(ns, previous) ? 1 : -expm1(-(double)(ns-previous)/1e9/0.1);
    m->have_gravity = true;
    if (sine < 1e-9 && cosine > 0) return;
    if (sine < 1e-9) { axis[0] = 1; axis[1] = axis[2] = 0; sine = 1; }
    else for (int i = 0; i < 3; i++) axis[i] /= sine;
    double half_angle = angle * alpha * 0.5;
    double correction[4] = {cos(half_angle), axis[0]*sin(half_angle), axis[1]*sin(half_angle), axis[2]*sin(half_angle)};
    multiply(correction, m->orientation, m->orientation);
    normalize(m->orientation);
}

void controller_motion_acceleration(ControllerMotion *m, uint64_t ns, const float data[3]) {
    if (!m) return;
    if (!ns || (m->acceleration_ns && ns <= m->acceleration_ns)) return;
    m->acceleration_ns = ns;  // Invalid new reports still advance the ordering watermark.
    m->acceleration_valid = valid_vector(data);
    if (!m->acceleration_valid) return;
    for (int i = 0; i < 3; i++) m->acceleration[i] = data[i];
    advance(m, ns);
    // Paired reports may arrive in either order. Correct after the matching
    // gyro step, rather than correcting to a future pose then integrating it
    // again. Accelerometer-only devices still update their tilt immediately.
    if (!m->gyro_valid || !fresh(ns, m->gyro_ns) || ns <= m->gyro_ns) correct_gravity(m);
}

void controller_motion_gyroscope(ControllerMotion *m, uint64_t ns, const float data[3]) {
    if (!m) return;
    if (!ns || (m->gyro_ns && ns <= m->gyro_ns)) return;
    uint64_t previous = m->gyro_ns;
    bool previous_valid = m->gyro_valid;
    m->gyro_ns = ns;
    m->gyro_valid = valid_vector(data);
    if (!m->gyro_valid) return;
    for (int i = 0; i < 3; i++) m->angular_velocity[i] = data[i];
    advance(m, ns);
    if (previous_valid && fresh(ns, previous)) {
        double dt = (double)(ns - previous)/1e9, rate = norm3(m->angular_velocity);
        for (int i = 0; i < 3; i++) m->angle[i] += m->angular_velocity[i] * dt;
        if (rate > 1e-12) {
            double half_angle = rate * dt * 0.5, scale = sin(half_angle)/rate;
            double increment[4] = {cos(half_angle), data[0]*scale, data[1]*scale, data[2]*scale};
            multiply(m->orientation, increment, m->orientation);
            normalize(m->orientation);
        }
    }
    if (m->acceleration_valid && fresh(ns, m->acceleration_ns)) correct_gravity(m);
}

bool controller_motion_read(const ControllerMotion *m, uint64_t ns, InputMotionSnapshot *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->direction[0] = out->direction[4] = out->direction[8] = 1;
    if (!m || !ns || ns < m->sample_ns) return false;
    out->acceleration_valid = m->acceleration_valid && fresh(ns, m->acceleration_ns);
    out->gyro_valid = m->gyro_valid && fresh(ns, m->gyro_ns);
    if (!out->acceleration_valid && !out->gyro_valid) return false;
    out->sampling_number = m->sampling_number;
    out->delta_time_ns = m->delta_time_ns;
    for (int i = 0; i < 3; i++) {
        if (out->acceleration_valid) out->acceleration[i] = m->acceleration[i];
        if (out->gyro_valid) out->angular_velocity[i] = m->angular_velocity[i];
        out->angle[i] = m->angle[i];
    }
    matrix(m->orientation, out->direction);
    out->at_rest = out->acceleration_valid && out->gyro_valid &&
        fabs(norm3(m->acceleration)-STANDARD_GRAVITY) < 0.4 && norm3(m->angular_velocity) < 0.03;
    return true;
}
