// Native controller motion -> nn::hid SixAxis ABI. No camera-to-stick settings enter this path.
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "../hle.h"
#include "../devlog.h"
#include "../motion_input.h"
#include "../sixaxis.h"
#include "../services/am.h"

// Public ABI and handle packing: switchbrew/libnx, services/hid.h and services/hid.c.
// SixAxis units/basis: G, revolutions/s, revolutions; x right, y forward, z out of face.
// direction stores each device axis in world space (rows), unlike InputMotionSnapshot's matrix.
typedef struct {
    uint64_t delta_time, sampling_number;
    float acceleration[3], angular_velocity[3], angle[3], direction[9];
    uint32_t attributes, reserved;
} SixAxisState;
_Static_assert(sizeof(SixAxisState) == 0x60, "SixAxis state size");
_Static_assert(offsetof(SixAxisState, acceleration) == 0x10, "SixAxis acceleration");
_Static_assert(offsetof(SixAxisState, angular_velocity) == 0x1c, "SixAxis angular velocity");
_Static_assert(offsetof(SixAxisState, angle) == 0x28, "SixAxis angle");
_Static_assert(offsetof(SixAxisState, direction) == 0x34, "SixAxis direction");
_Static_assert(offsetof(SixAxisState, attributes) == 0x58, "SixAxis attributes");

enum { SIX_FULLKEY = 0x00020003, SIX_HANDHELD = 0x00022004 };
typedef struct {
    bool started, fusion;
    float fusion_parameters[2];
    uint32_t zero_drift_mode;
} SixAxisControl;
#define SIX_CONTROL_INIT {false, true, {0.03f, 0.4f}, 1}
static SixAxisControl six_controls[2] = {SIX_CONTROL_INIT, SIX_CONTROL_INIT};
static pthread_mutex_t six_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t six_last_log;

static bool six_read_u32(uint64_t p, uint32_t *out) {
    if (!guest_contains(p, sizeof *out)) return false;
    memcpy(out, (const void *)(uintptr_t)p, sizeof *out);
    return true;
}
static int six_slot(uint32_t handle) {
    return handle == SIX_FULLKEY ? 0 : handle == SIX_HANDHELD ? 1 : -1;
}
static int six_arg_slot(uint64_t p) {
    uint32_t handle;
    return six_read_u32(p, &handle) ? six_slot(handle) : -1;
}
static bool six_current_slot(int slot) { return slot == (am_handheld() ? 1 : 0); }
static void six_demand(void) {
    input_motion_set_enabled(six_controls[0].started || six_controls[1].started);
}
RT_ON_RESET(six_reset) {
    pthread_mutex_lock(&six_lock);
    const SixAxisControl initial = SIX_CONTROL_INIT;
    six_controls[0] = six_controls[1] = initial;
    six_last_log = 0;
    six_demand();
    pthread_mutex_unlock(&six_lock);
}

static void six_neutral(SixAxisState *out) {
    memset(out, 0, sizeof *out);
    out->direction[0] = out->direction[4] = out->direction[8] = 1;
}
static bool six_finite(const InputMotionSnapshot *s) {
    for (int i = 0; i < 3; ++i) {
        if (s->acceleration_valid && !isfinite(s->acceleration[i])) return false;
        if (s->gyro_valid && (!isfinite(s->angular_velocity[i]) || !isfinite(s->angle[i]))) return false;
    }
    for (int i = 0; i < 9; ++i) if (!isfinite(s->direction[i])) return false;
    return s->sampling_number && (s->acceleration_valid || s->gyro_valid);
}
static void six_convert(const InputMotionSnapshot *in, SixAxisState *out) {
    // B maps SDL's proper right/up/toward-player frame to right/forward/face-up: (x,-z,y).
    // SDL accelerometers measure specific force; SixAxis reports the opposite gravity convention.
    const int axis[3] = {0, 2, 1};
    const float sign[3] = {1, -1, 1};
    const float gravity = 9.80665f, turn = 6.2831853071795864769f;
    six_neutral(out);
    out->delta_time = in->delta_time_ns;
    out->sampling_number = in->sampling_number;
    out->attributes = 1;  // connected, never claim an interpolated sample
    for (int i = 0; i < 3; ++i) {
        if (in->acceleration_valid) out->acceleration[i] = -sign[i] * in->acceleration[axis[i]] / gravity;
        if (in->gyro_valid) {
            out->angular_velocity[i] = sign[i] * in->angular_velocity[axis[i]] / turn;
            out->angle[i] = sign[i] * in->angle[axis[i]] / turn;
        }
        for (int j = 0; j < 3; ++j)
            out->direction[3 * i + j] = sign[i] * sign[j] * in->direction[3 * axis[j] + axis[i]];
    }
}
// six_lock serializes the started handle state with native demand and read diagnostics.
static bool six_latest(int slot, SixAxisState *out, bool *at_rest, const char *api, uint64_t guest_lr) {
    InputMotionSnapshot sample = {0};
    const char *reason = "invalid-handle";
    bool valid = false;
    six_neutral(out);
    *at_rest = false;
    if (slot >= 0) {
        if (!six_controls[slot].started) reason = "stopped";
        else if (!six_current_slot(slot)) reason = "inactive-mode";
        else if (!input_motion_snapshot(&sample)) reason = "unavailable-or-stale";
        else if (!six_finite(&sample)) reason = "invalid-sample";
        else {
            six_convert(&sample, out);
            *at_rest = sample.at_rest;
            valid = true;
            reason = "ready";
        }
    }
    if (devlog_on("motion")) {
        uint64_t now = plat_now_ns();
        if (!six_last_log || now < six_last_log || now - six_last_log >= 1000000000ull) {
            six_last_log = now;
            devlog("motion", "SixAxis mode=%s slot=%d api=%s guest_lr=%#llx status=%s sample=%llu accel=%d gyro=%d rest=%d G=(%.3f,%.3f,%.3f) dirZ=(%.3f,%.3f,%.3f)",
                   am_handheld() ? "handheld" : "docked", slot, api, (unsigned long long)guest_lr, reason, (unsigned long long)out->sampling_number,
                   sample.acceleration_valid, sample.gyro_valid, *at_rest,
                   out->acceleration[0], out->acceleration[1], out->acceleration[2],
                   out->direction[6], out->direction[7], out->direction[8]);
        }
    }
    return valid;
}

HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", six_get_handles) {
    uint64_t out = c->x[0];
    uint32_t id = UINT32_MAX, style = (uint32_t)c->x[3];
    bool handheld = am_handheld();
    uint32_t handle = handheld ? SIX_HANDHELD : SIX_FULLKEY;
    bool valid = (int32_t)c->x[1] > 0 && six_read_u32(c->x[2], &id) &&
                 id == (handheld ? 0x20u : 0u) && (style & (handheld ? 2u : 1u)) &&
                 guest_contains(out, sizeof handle);
    if (valid) memcpy((void *)(uintptr_t)out, &handle, sizeof handle);
    c->x[0] = valid ? 1 : 0;
    devlog("motion", "SixAxis handles mode=%s id=%#x styles=%#x count=%u handle=%#x guest_lr=%#llx",
           handheld ? "handheld" : "docked", id, style, valid ? 1 : 0, valid ? handle : 0, (unsigned long long)c->x[30]);
}
bool sixaxis_set_started(uint32_t handle, bool started, SixAxisOrigin origin) {
    int slot = six_slot(handle);
    const char *source = origin == SIXAXIS_ORIGIN_IPC ? "ipc" : "hle";
    if (slot < 0) {
        devlog("motion", "SixAxis %s origin=%s handle=%#x rejected=invalid-handle", started ? "start" : "stop", source, handle);
        return false;
    }
    pthread_mutex_lock(&six_lock);
    bool changed = six_controls[slot].started != started;
    if (changed) {
        six_controls[slot].started = started;
        six_demand();
    }
    devlog("motion", "SixAxis %s origin=%s handle=%#x slot=%d changed=%d native-demand=%d", started ? "start" : "stop", source,
           handle, slot, changed, six_controls[0].started || six_controls[1].started);
    pthread_mutex_unlock(&six_lock);
    return true;
}
static void six_set_started(Cpu *c, bool started) {
    uint32_t handle;
    if (six_read_u32(c->x[0], &handle)) {
        devlog("motion", "SixAxis request api=%s handle=%#x guest_lr=%#llx", started ? "start" : "stop", handle, (unsigned long long)c->x[30]);
        sixaxis_set_started(handle, started, SIXAXIS_ORIGIN_HLE);
    } else devlog("motion", "SixAxis %s origin=hle rejected=invalid-handle-pointer guest_lr=%#llx", started ? "start" : "stop", (unsigned long long)c->x[30]);
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", six_start) { six_set_started(c, true); }
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", six_stop) { six_set_started(c, false); }
HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", six_get_state) {
    if (!guest_contains(c->x[0], sizeof(SixAxisState))) return;
    int slot = six_arg_slot(c->x[1]);
    SixAxisState state;
    bool rest;
    pthread_mutex_lock(&six_lock);
    six_latest(slot, &state, &rest, "state", c->x[30]);
    pthread_mutex_unlock(&six_lock);
    memcpy((void *)(uintptr_t)c->x[0], &state, sizeof state);
}
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", six_get_states) {
    uint64_t out = c->x[0];
    int slot = six_arg_slot(c->x[2]);
    c->x[0] = 0;
    if ((int32_t)c->x[1] <= 0 || !guest_contains(out, sizeof(SixAxisState))) return;
    SixAxisState state;
    bool rest;
    pthread_mutex_lock(&six_lock);
    bool valid = six_latest(slot, &state, &rest, "states", c->x[30]);
    pthread_mutex_unlock(&six_lock);
    // One real latest report, not a fabricated history at the guest's polling rate.
    if (valid) { memcpy((void *)(uintptr_t)out, &state, sizeof state); c->x[0] = 1; }
}
HLE_FUNC("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", six_at_rest) {
    int slot = six_arg_slot(c->x[0]);
    SixAxisState state;
    bool rest;
    pthread_mutex_lock(&six_lock);
    bool valid = six_latest(slot, &state, &rest, "at-rest", c->x[30]);
    pthread_mutex_unlock(&six_lock);
    c->x[0] = valid && rest;
}

// These preferences round-trip per logical sensor. Physical orientation uses the host's approximate fusion;
// Nintendo's proprietary fusion gains / drift policies are not reproduced by these configuration adapters.
HLE_FUNC("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", six_fusion_enabled) {
    int slot = six_arg_slot(c->x[0]);
    pthread_mutex_lock(&six_lock);
    c->x[0] = slot >= 0 && six_controls[slot].fusion;
    pthread_mutex_unlock(&six_lock);
}
HLE_FUNC("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", six_enable_fusion) {
    int slot = six_arg_slot(c->x[0]);
    pthread_mutex_lock(&six_lock);
    if (slot >= 0) six_controls[slot].fusion = c->x[1] != 0;
    pthread_mutex_unlock(&six_lock);
}
HLE_FUNC("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", six_set_fusion_parameters) {
    int slot = six_arg_slot(c->x[0]);
    float p[2];
    memcpy(&p[0], &c->v[0], sizeof(float)); memcpy(&p[1], &c->v[1], sizeof(float));
    if (!isfinite(p[0]) || !isfinite(p[1]) || p[0] < 0 || p[0] > 1) return;
    pthread_mutex_lock(&six_lock);
    if (slot >= 0) memcpy(six_controls[slot].fusion_parameters, p, sizeof p);
    pthread_mutex_unlock(&six_lock);
}
HLE_FUNC("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", six_get_fusion_parameters) {
    int slot = six_arg_slot(c->x[2]);
    if (slot < 0 || !guest_contains(c->x[0], sizeof(float)) || !guest_contains(c->x[1], sizeof(float))) return;
    pthread_mutex_lock(&six_lock);
    memcpy((void *)(uintptr_t)c->x[0], &six_controls[slot].fusion_parameters[0], sizeof(float));
    memcpy((void *)(uintptr_t)c->x[1], &six_controls[slot].fusion_parameters[1], sizeof(float));
    pthread_mutex_unlock(&six_lock);
}
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", six_set_drift) {
    int slot = six_arg_slot(c->x[0]);
    uint32_t mode;
    if (!six_read_u32(c->x[1], &mode) || mode > 2) return;
    pthread_mutex_lock(&six_lock);
    if (slot >= 0) six_controls[slot].zero_drift_mode = mode;
    pthread_mutex_unlock(&six_lock);
}
HLE_FUNC("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", six_get_drift) {
    int slot = six_arg_slot(c->x[0]);
    pthread_mutex_lock(&six_lock);
    c->x[0] = slot >= 0 ? six_controls[slot].zero_drift_mode : 1;
    pthread_mutex_unlock(&six_lock);
}
