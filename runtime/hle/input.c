// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <pthread.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../devlog.h"
#include "../hle.h"
#include "../kernel.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../gyro_input.h"
#include "../platform.h"
#include "gyro_camera.h"
#include "controller_motion.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic uint64_t gyro_stick;  // signed X/Y packed together for a coherent guest read
static _Atomic uint64_t gyro_expires;  // platform monotonic clock; also stops aim if the UI thread stalls
static atomic_uint gyro_publish_sequence;
static _Atomic uint64_t gyro_reset_generation, gyro_published_generation;
static _Atomic int64_t sampling;
typedef struct InputStyleEvent { KEvent *event; struct InputStyleEvent *next; } InputStyleEvent;
static InputStyleEvent *style_events;  // g_klock; writable handles keep events alive until kernel reset

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    // a keyboard diagonal is a stick at 45 degrees, not at the square's corner (magnitude 1.41: the game does not clamp it)
    if (key_lx && key_ly) { key_lx = key_lx * 23170 / 32767; key_ly = key_ly * 23170 / 32767; }
    {   // LARECOMP_KEY_STICK=<0..1>: keyboard stick tilt (a walking-speed test)
        static float k = -1;
        if (k < 0) k = getenv("LARECOMP_KEY_STICK") ? (float)atof(getenv("LARECOMP_KEY_STICK")) : 1.0f;
        if (k < 1.0f) { key_lx = (int32_t)(key_lx * k); key_ly = (int32_t)(key_ly * k); }
    }
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    pad_rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    pad_ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    // This can run on the renderer: invalidate guest reads now, clear math on the UI thread.
    atomic_fetch_add(&gyro_reset_generation, 1);
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
}

volatile int g_debug_frame_request;  // F9 (LARECOMP_DEVTOOLS only): dump the next frame's passes (build/pass_*.png) + draw log

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (runtime/nvn/texpatch.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(void) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        default: break;
    }
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// Gamepad sensors and their math state are owned only by the UI/event thread.
// Guest Npad reads use the coherent packed output and never call SDL sensor APIs.
static GyroCamera gyro_camera;
static SDL_JoystickID pad_id;
static bool pads_scanned, pad_has_gyro, pad_has_accel, gyro_enabled, accel_enabled, gyro_enable_error, gyro_focused;
static uint64_t gyro_enabled_at, gyro_sample_at, gyro_accel_at, gyro_seen_generation;
static float gyro_accel[3];
static _Atomic int gyro_status = GYRO_STATUS_DISCONNECTED;
static atomic_bool gyro_calibrate_request;
extern bool menu_input_captured(void);

static bool gyro_script_blocked;
static bool gyro_scripted(void) {
    const char *no_input = getenv("LARECOMP_NO_USER_INPUT");
    return (no_input && atoi(no_input)) || getenv("LARECOMP_INPUT") || getenv("LARECOMP_HOST_INPUT") ||
        getenv("LARECOMP_BIND_TEST") || getenv("LARECOMP_INPUT_REPLAY") || getenv("LARECOMP_ROUTE") ||
        getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE");
}
// This channel reports physical motion, independent of camera gain/activation/calibration.
static ControllerMotion native_motion;
static atomic_bool motion_requested, motion_available;
static atomic_uint motion_sequence, motion_flags;
static _Atomic uint32_t motion_values[18];
static _Atomic uint64_t motion_number, motion_delta, motion_acceleration_expires, motion_gyro_expires;
static uint64_t motion_number_offset, motion_last_number;
static bool native_requested_main, accel_enable_error;

void input_motion_set_enabled(bool enabled) { atomic_store(&motion_requested, enabled); }
static void motion_clear(void) {
    atomic_store(&motion_available, false);
    controller_motion_reset(&native_motion);
    motion_number_offset = motion_last_number;
    atomic_fetch_add(&motion_sequence, 1);
    atomic_store(&motion_flags, 0);
    atomic_fetch_add(&motion_sequence, 1);
}
static uint64_t motion_deadline(uint64_t host_now, uint64_t now, uint64_t stamp) {
    return stamp && now >= stamp && now - stamp < 150000000 ? host_now + 150000000 - (now - stamp) : 0;
}
static void motion_publish(void) {
    InputMotionSnapshot sample;
    uint64_t host_now = plat_now_ns(), now = SDL_GetTicksNS();
    if (!atomic_load(&motion_available) || !controller_motion_read(&native_motion, now, &sample)) {
        atomic_fetch_add(&motion_sequence, 1); atomic_store(&motion_flags, 0); atomic_fetch_add(&motion_sequence, 1);
        return;
    }
    uint64_t number = motion_number_offset + sample.sampling_number;
    if (number > motion_last_number) motion_last_number = number;
    float values[18];
    memcpy(values, sample.acceleration, 3 * sizeof(float));
    memcpy(values + 3, sample.angular_velocity, 3 * sizeof(float));
    memcpy(values + 6, sample.angle, 3 * sizeof(float));
    memcpy(values + 9, sample.direction, 9 * sizeof(float));
    unsigned flags = (sample.acceleration_valid ? 1u : 0u) | (sample.gyro_valid ? 2u : 0u) | (sample.at_rest ? 4u : 0u);
    atomic_fetch_add(&motion_sequence, 1);
    for (unsigned i = 0; i < 18; i++) { uint32_t bits; memcpy(&bits, &values[i], sizeof bits); atomic_store(&motion_values[i], bits); }
    atomic_store(&motion_number, motion_last_number); atomic_store(&motion_delta, sample.delta_time_ns);
    atomic_store(&motion_acceleration_expires, motion_deadline(host_now, now, native_motion.acceleration_ns));
    atomic_store(&motion_gyro_expires, motion_deadline(host_now, now, native_motion.gyro_ns));
    atomic_store(&motion_flags, flags);
    atomic_fetch_add(&motion_sequence, 1);
}
bool input_motion_snapshot(InputMotionSnapshot *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!atomic_load(&motion_requested) || !atomic_load(&motion_available)) return false;
    unsigned sequence = atomic_load(&motion_sequence);
    if (sequence & 1) return false;
    float values[18];
    for (unsigned i = 0; i < 18; i++) { uint32_t bits = atomic_load(&motion_values[i]); memcpy(&values[i], &bits, sizeof bits); }
    out->sampling_number = atomic_load(&motion_number); out->delta_time_ns = atomic_load(&motion_delta);
    unsigned flags = atomic_load(&motion_flags);
    uint64_t acceleration_expires = atomic_load(&motion_acceleration_expires), gyro_expires = atomic_load(&motion_gyro_expires);
    if (sequence != atomic_load(&motion_sequence) || !atomic_load(&motion_requested) || !atomic_load(&motion_available)) return false;
    uint64_t now = plat_now_ns();
    out->acceleration_valid = (flags & 1) && now < acceleration_expires;
    out->gyro_valid = (flags & 2) && now < gyro_expires;
    out->at_rest = (flags & 4) && out->acceleration_valid && out->gyro_valid;
    if (!out->acceleration_valid && !out->gyro_valid) return false;
    if (out->acceleration_valid) memcpy(out->acceleration, values, 3 * sizeof(float));
    if (out->gyro_valid) memcpy(out->angular_velocity, values + 3, 3 * sizeof(float));
    memcpy(out->angle, values + 6, 3 * sizeof(float)); memcpy(out->direction, values + 9, 9 * sizeof(float));
    return true;
}

static void motion_log_status(void) {
    static uint64_t last_log;
    if (!devlog_on("motion")) return;
    uint64_t host_now = plat_now_ns();
    if (last_log && host_now >= last_log && host_now - last_log < 1000000000) return;
    last_log = host_now;
    uint64_t now = SDL_GetTicksNS();
    double acceleration_age = native_motion.acceleration_ns && now >= native_motion.acceleration_ns ? (now - native_motion.acceleration_ns) / 1e6 : -1;
    double gyro_age = native_motion.gyro_ns && now >= native_motion.gyro_ns ? (now - native_motion.gyro_ns) / 1e6 : -1;
    devlog("motion", "provider requested=%d pad=%d focus=%d blocked=%d available=%d accel_enabled=%d gyro_enabled=%d accel_age_ms=%.1f gyro_age_ms=%.1f sample=%llu",
           atomic_load(&motion_requested), pad != NULL, gyro_focused, gyro_script_blocked, atomic_load(&motion_available),
           accel_enabled, gyro_enabled, acceleration_age, gyro_age, (unsigned long long)motion_last_number);
}

static bool gyro_active(void) {
    return pad && gyro_enabled && gyro_focused && atomic_load(&gyro_reset_generation) == gyro_seen_generation && !gyro_script_blocked && !menu_input_captured() &&
        (g_settings.gyro_camera == GYRO_CAM_ALWAYS || (g_settings.gyro_camera == GYRO_CAM_HOLD &&
         SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000));
}
static void gyro_publish(float x, float y) {
    int32_t rx = isfinite(x) ? (int32_t)lroundf(fmaxf(-1, fminf(x, 1)) * 32767) : 0;
    int32_t ry = isfinite(y) ? (int32_t)lroundf(fmaxf(-1, fminf(y, 1)) * 32767) : 0;
    uint64_t expires = 0;
    if (rx || ry) {
        uint64_t host_now = plat_now_ns(), now = SDL_GetTicksNS();
        if (!gyro_sample_at || now < gyro_sample_at || now - gyro_sample_at >= 150000000) rx = ry = 0;
        else expires = host_now + (150000000 - (now - gyro_sample_at));
    }
    atomic_fetch_add(&gyro_publish_sequence, 1);  // odd: only the UI thread publishes
    atomic_store(&gyro_expires, expires);
    atomic_store(&gyro_stick, (uint64_t)(uint32_t)rx | (uint64_t)(uint32_t)ry << 32);
    atomic_store(&gyro_published_generation, gyro_seen_generation);
    atomic_fetch_add(&gyro_publish_sequence, 1);
}
static uint64_t gyro_guest_stick(void) {
    unsigned sequence = atomic_load(&gyro_publish_sequence);
    if (sequence & 1) return 0;
    uint64_t axes = atomic_load(&gyro_stick), expires = atomic_load(&gyro_expires);
    uint64_t generation = atomic_load(&gyro_published_generation);
    if (sequence != atomic_load(&gyro_publish_sequence) || generation != atomic_load(&gyro_reset_generation)) return 0;
    return axes && plat_now_ns() < expires ? axes : 0;
}
static void gyro_deactivate(void) {
    float x, y;
    gyro_camera_update(&gyro_camera, SDL_GetTicksNS(), false, NULL, &x, &y);
    gyro_publish(0, 0);
}
static void gyro_stop_sensors(void) {
    if (pad && gyro_enabled) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, false);
    if (pad && accel_enabled) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, false);
    gyro_enabled = accel_enabled = false;
    gyro_enabled_at = gyro_sample_at = gyro_accel_at = 0;
    gyro_deactivate(); motion_clear();
}
static void input_pad_open(SDL_JoystickID id) {
    if (pad) return;
    pad = SDL_OpenGamepad(id);
    if (!pad) return;
    pad_id = SDL_GetGamepadID(pad);
    pad_has_gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    pad_has_accel = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    gyro_enabled = accel_enabled = gyro_enable_error = accel_enable_error = false;
    native_requested_main = false; motion_clear();
    gyro_camera_reset(&gyro_camera);
    gyro_publish(0, 0);
    atomic_store(&gyro_status, pad_has_gyro ? GYRO_STATUS_OFF : GYRO_STATUS_UNAVAILABLE);
    LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad));
    note_family();
}
static void input_pad_close(void) {
    gyro_stop_sensors();
    if (pad) SDL_CloseGamepad(pad);
    pad = NULL; pad_id = 0;
    pad_has_gyro = pad_has_accel = gyro_enable_error = false;
    gyro_camera_reset(&gyro_camera);
    atomic_store(&gyro_status, GYRO_STATUS_DISCONNECTED);
    note_family();
    recompute_pad();
}
static void input_pad_scan(void) {
    if (pads_scanned) return;
    pads_scanned = true;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && !pad; i++) input_pad_open(ids[i]);
    SDL_free(ids);
}
GyroInputStatus input_gyro_status(void) { return (GyroInputStatus)atomic_load(&gyro_status); }
void input_gyro_calibrate(void) {
    atomic_store(&gyro_calibrate_request, true);
    atomic_fetch_add(&gyro_reset_generation, 1);
}

void input_gyro_update(SDL_Window *w) {
    gyro_script_blocked = gyro_scripted();
    uint64_t generation = atomic_load(&gyro_reset_generation);
    if (generation != gyro_seen_generation) { gyro_deactivate(); gyro_seen_generation = generation; }
    input_pad_scan();
    if (pad && !SDL_GamepadConnected(pad)) { input_pad_close(); pads_scanned = false; input_pad_scan(); }
    SDL_WindowFlags flags = w ? SDL_GetWindowFlags(w) : 0;
    gyro_focused = (flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    bool available = pad && gyro_focused && !gyro_script_blocked;
    if (!available && atomic_load(&motion_available)) motion_clear();
    atomic_store(&motion_available, available);
    if (!pad) { gyro_publish(0, 0); atomic_store(&gyro_status, GYRO_STATUS_DISCONNECTED); motion_log_status(); return; }
    bool requested = atomic_load(&motion_requested);
    if (requested && !native_requested_main) gyro_enable_error = accel_enable_error = false;
    native_requested_main = requested;
    bool calibrate = atomic_exchange(&gyro_calibrate_request, false);
    if (calibrate) {
        gyro_camera_reset(&gyro_camera); gyro_sample_at = gyro_accel_at = 0;
        gyro_enable_error = accel_enable_error = false; gyro_publish(0, 0);
    }
    bool camera = (g_settings.gyro_camera == GYRO_CAM_HOLD || g_settings.gyro_camera == GYRO_CAM_ALWAYS) && !gyro_script_blocked;
    bool native = requested && available;
    bool want_gyro = pad_has_gyro && ((camera && pad_has_gyro) || native);
    bool want_accel = pad_has_accel && ((camera && pad_has_gyro) || native);
    uint64_t now = SDL_GetTicksNS();
    if (!want_gyro && !want_accel) {
        gyro_stop_sensors(); gyro_enable_error = accel_enable_error = false;
    } else {
        if ((want_gyro && !gyro_enabled && !gyro_enable_error) || (want_accel && !accel_enabled && !accel_enable_error))
            SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, true);
        if (!want_gyro) {
            if (gyro_enabled) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, false);
            gyro_enabled = gyro_enable_error = false; gyro_sample_at = 0; gyro_deactivate();
        } else if (!gyro_enabled && !gyro_enable_error) {
            gyro_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            gyro_enable_error = !gyro_enabled;
            if (gyro_enabled) gyro_enabled_at = now;
            else devlog("gyro", "could not enable gamepad gyro: %s", SDL_GetError());
        }
        if (!want_accel) {
            if (accel_enabled) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, false);
            accel_enabled = accel_enable_error = false; gyro_accel_at = 0;
        } else if (!accel_enabled && !accel_enable_error) {
            accel_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            accel_enable_error = !accel_enabled;
            if (!accel_enabled) devlog("gyro", "accelerometer unavailable; native tilt unavailable, camera uses local yaw: %s", SDL_GetError());
        }
    }
    // Native data is still available when the optional camera is Off or unavailable.
    motion_publish(); motion_log_status();
    if (!pad_has_gyro) { gyro_publish(0, 0); atomic_store(&gyro_status, GYRO_STATUS_UNAVAILABLE); return; }
    if (!camera) { gyro_deactivate(); atomic_store(&gyro_status, GYRO_STATUS_OFF); return; }
    if (gyro_enable_error) { gyro_publish(0, 0); atomic_store(&gyro_status, GYRO_STATUS_ERROR); return; }
    GyroCameraConfig config = {g_settings.gyro_speed, g_settings.invert_gyro_x, g_settings.invert_gyro_y};
    float x, y; gyro_camera_update(&gyro_camera, now, gyro_active(), &config, &x, &y); gyro_publish(x, y);
    bool stale = gyro_sample_at ? now < gyro_sample_at || now - gyro_sample_at >= 150000000 : now - gyro_enabled_at >= 1000000000;
    atomic_store(&gyro_status, stale ? GYRO_STATUS_ERROR : gyro_camera_calibrated(&gyro_camera) ? GYRO_STATUS_READY : GYRO_STATUS_CALIBRATING);
}

static void input_gyro_sensor(const SDL_GamepadSensorEvent *e) {
    if (!pad || e->which != pad_id || gyro_script_blocked) return;
    uint64_t now = SDL_GetTicksNS();
    // SDL event timestamps share GetTicksNS's domain; sensor_timestamp may use a
    // device clock. Preserve queue age so old reports cannot become fresh aim.
    if (e->timestamp > now) return;  // A future/corrupt report must not become fresh motion.
    uint64_t stamp = e->timestamp ? e->timestamp : now;
    if (now - stamp >= 150000000) return;
    if (e->sensor == SDL_SENSOR_ACCEL && accel_enabled) {
        memcpy(gyro_accel, e->data, sizeof gyro_accel); gyro_accel_at = stamp;
        if (atomic_load(&motion_available)) controller_motion_acceleration(&native_motion, stamp, e->data);
        motion_publish();
    } else if (e->sensor == SDL_SENSOR_GYRO && gyro_enabled) {
        if (atomic_load(&motion_available)) controller_motion_gyroscope(&native_motion, stamp, e->data);
        motion_publish();
        if (gyro_sample_at && stamp <= gyro_sample_at) return;
        const float *accel = gyro_accel_at && stamp >= gyro_accel_at && stamp - gyro_accel_at < 100000000 ? gyro_accel : NULL;
        gyro_camera_sample(&gyro_camera, stamp, e->data, accel, gyro_active());
        bool valid = true;
        for (unsigned i = 0; i < 3; i++) if (!isfinite(e->data[i]) || fabsf(e->data[i]) > 100) valid = false;
        if (valid) gyro_sample_at = stamp;
    }
}

// Right stick snap (Controls > Right stick snap): a direction within `deg` degrees of an axis is held on that axis.
static void snap_to_axis(int32_t *x, int32_t *y, int deg) {
    if (deg <= 0 || (!*x && !*y)) return;
    double a = fmod(fabs(atan2((double)*y, (double)*x) * 180.0 / M_PI), 90.0);  // (0 on an axis, 45 on a diagonal)
    if (fmin(a, 90.0 - a) >= deg) return;
    if (fabs((double)*x) >= fabs((double)*y)) *y = 0;  // near the horizontal axis: drop the vertical part
    else *x = 0;
}

// Settings > Controls > Mouse camera: which button, held down, lets the mouse turn the camera. "No button" does it all the
// time, with the cursor held inside the window (input_mouse_update), so it cannot leave the window and spin the camera.
extern bool menu_input_captured(void);  // runtime/ui/menu.mm
// With the left button turning the camera ("Left button" / "Any button"), a left press is not the camera until the cursor
// moves: released before that, it was a tap (the touch screen, below); moved, it becomes the camera drag.
static bool left_pending;
static float left_x, left_y;
static SDL_WindowID left_win;
#define TAP_SLOP 6.0f  // points the cursor may move before a left press becomes a camera drag
static bool mouse_camera_on(uint32_t state) {
    if (left_pending) state &= ~SDL_BUTTON_LMASK;
    if (g_settings.mouse_camera == MOUSE_CAM_ALWAYS) return true;
    if (g_settings.mouse_camera != MOUSE_CAM_HOLD) return false;
    switch (g_settings.mouse_camera_button) {
    case MOUSE_BTN_LEFT: return state & SDL_BUTTON_LMASK;
    case MOUSE_BTN_MIDDLE: return state & SDL_BUTTON_MMASK;
    case MOUSE_BTN_ANY: return state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK);
    default: return state & SDL_BUTTON_RMASK;
    }
}

// Main thread, after each batch of events (runtime/nvn/metal.m ui_run): while the game has the controls the mouse is in
// relative mode: the cursor is hidden (nothing in the game is clicked) and held in place, so the camera never stops at the
// window's edge; the menu and the name entry get the cursor back. Online (the Lumiose module, whose interface is clicked
// with the mouse) only while the mouse turns the camera: all the time with Mouse camera "Always", while the chosen button
// is held with "While holding a button"; the rest of the time the cursor is shown.
static bool online_mouse(void) { return g_nmodules > 2; }  // (the Lumiose module is loaded: runtime/loader.c)
static bool mouse_held;
static uint64_t mouse_seen;  // SDL_GetTicks of the last mouse movement or click (the idle cursor hides)
static bool cursor_idle;     // hidden for being idle (Settings > Controls > Hide the idle cursor)
// Back to the start screen: it has no menu to take the mouse back, so the cursor must come back here.
void input_mouse_release(SDL_Window *w) {
    SDL_SetWindowRelativeMouseMode(w, false);
    SDL_ShowCursor();
    mouse_held = false;
    cursor_idle = false;
}
void input_mouse_update(SDL_Window *w) {
    bool want = !menu_input_captured() && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) &&
                (!online_mouse() || g_settings.mouse_camera == MOUSE_CAM_ALWAYS ||
                 (g_settings.mouse_camera == MOUSE_CAM_HOLD && mouse_camera_on(SDL_GetMouseState(NULL, NULL))));
    if (want != mouse_held) {
        SDL_SetWindowRelativeMouseMode(w, want);
        mouse_held = want;
    }
    if (want) return;  // (captured: hidden anyway)
    if (menu_input_captured()) mouse_seen = SDL_GetTicks();  // (the menu takes the mouse's events: the cursor counts as in use)
    bool idle = online_mouse() && !menu_input_captured() && g_settings.cursor_hide_s > 0 &&
                SDL_GetTicks() - mouse_seen > (uint64_t)g_settings.cursor_hide_s * 1000;
    if (idle != cursor_idle) {
        if (idle) SDL_HideCursor(); else SDL_ShowCursor();
        cursor_idle = idle;
    }
}

// The touch screen, from the mouse: the left button held is one finger where the cursor is, in the touch screen's own
// coordinates (1280x720 over the game's picture, whatever the window and the render size). Only the Lumiose module asks
// for it (nn::hid::GetTouchScreenStates, imported by its interface; the game imports none of it). When the left button also
// turns the camera, only a tap (pressed and released without moving) is a touch. None while the menu has the controls.
static pthread_mutex_t view_lock = PTHREAD_MUTEX_INITIALIZER;
static double view[7];  // the picture in the drawable: x, y, w, h (pixels); the drawable's width; the shown part (uv y, height)
static float view_crop[4];
static _Atomic int touch_x, touch_y;
static _Atomic bool touch_down, touch_pressed;  // (pressed: a press not yet reported, so a click between two polls counts)
#define TOUCH_W 1280
#define TOUCH_H 720

// The presenter (runtime/nvn/metal.m), each frame: where the shown part of the game's picture is in the drawable
// (pixels), the drawable's width, and which part of the picture that is (uv origin and size, present_crop).
void input_present_view(double vx, double vy, double vw, double vh, double dw, const float crop[4]) {
    pthread_mutex_lock(&view_lock);
    view[0] = vx, view[1] = vy, view[2] = vw, view[3] = vh, view[4] = dw;
    memcpy(view_crop, crop, sizeof view_crop);
    pthread_mutex_unlock(&view_lock);
}
static bool touch_point(SDL_WindowID id, float wx, float wy) {  // window points -> touch coordinates; false: off the picture
    SDL_Window *w = SDL_GetWindowFromID(id);
    int ww = 0, wh = 0;
    if (!w || !SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0) return false;
    pthread_mutex_lock(&view_lock);
    double v[5]; memcpy(v, view, sizeof v);
    float k[4]; memcpy(k, view_crop, sizeof k);
    pthread_mutex_unlock(&view_lock);
    if (v[2] <= 0 || v[3] <= 0 || v[4] <= 0) return false;
    double scale = v[4] / ww;  // points -> the drawable's pixels
    double u = (wx * scale - v[0]) / v[2], t = (wy * scale - v[1]) / v[3];  // 0..1 over the shown part
    if (u < 0 || u >= 1 || t < 0 || t >= 1) return false;
    u = k[0] + u * k[2], t = k[1] + t * k[3];  // over the whole picture
    atomic_store(&touch_x, (int)(u * TOUCH_W));
    atomic_store(&touch_y, (int)(t * TOUCH_H));
    return true;
}
static bool left_is_camera(void) {
    return g_settings.mouse_camera == MOUSE_CAM_HOLD &&
           (g_settings.mouse_camera_button == MOUSE_BTN_LEFT || g_settings.mouse_camera_button == MOUSE_BTN_ANY);
}

// mouse camera: the right stick from how fast the mouse moves with the right button held. Motion is added up between
// the game's reads of its controller (input_handle_event, in 1/16 px) and turned into a speed (px/s, smoothed over
// ~30 ms) and that into a deflection: past the stick's dead zone at once (else slow moves do nothing), full at
// MOUSE_FULL px/s divided by mouse_speed. A per-event gain (the old way) made 3-4 px already full tilt, so the
// speed setting did nothing and the camera turned in jerks.
static _Atomic int32_t mouse_acc_x, mouse_acc_y;
#define MOUSE_FULL 1200.0  // px/s for full tilt at speed 1
#define MOUSE_FLOOR 0.2    // the stick's dead zone
#define MOUSE_PX 3.0       // the mouse's base sensitivity: counts a motion of 1 px as 3 (three times the first version's)
#define TRACKPAD_PX 72.0   // px per wheel unit of a two-finger drag (felt right at the 300% speed with 24: 100% now has that feel)
static void mouse_stick(int32_t *rx, int32_t *ry) {
    static uint64_t last;
    static double vx, vy;
    uint64_t now = SDL_GetTicksNS();
    double dt = last ? (double)(now - last) / 1e9 : 0;
    if (!last || dt > 0.25) { last = now; atomic_exchange(&mouse_acc_x, 0); atomic_exchange(&mouse_acc_y, 0); vx = vy = 0; }
    else if (dt >= 0.004) {  // (the game reads several times a frame: speeds over at least 4 ms)
        double tx = atomic_exchange(&mouse_acc_x, 0) / 16.0 / dt, ty = atomic_exchange(&mouse_acc_y, 0) / 16.0 / dt;
        double a = 1 - exp(-dt / 0.03);
        vx += (tx - vx) * a, vy += (ty - vy) * a;
        last = now;
    }
    double v = sqrt(vx * vx + vy * vy);
    if (v < 15) { *rx = *ry = 0; return; }  // (at rest: below ~15 px/s)
    double m = v * g_settings.mouse_speed / MOUSE_FULL;
    double d = (MOUSE_FLOOR + (1 - MOUSE_FLOOR) * (m > 1 ? 1 : m)) * 32767 / v;
    *rx = (int32_t)(vx * d), *ry = (int32_t)(-vy * d);
}
void input_handle_event(const SDL_Event *e) {
    // (not the scroll: a two-finger drag on the trackpad turns the camera, it does not bring the cursor back)
    if (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP)
        mouse_seen = SDL_GetTicks();
    input_pad_scan();
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        // F9: dump the next frame's passes and draw log (developers: LARECOMP_DEVTOOLS=1; a player's key press must not write files)
        static int devtools = -1;
        if (devtools < 0) devtools = getenv("LARECOMP_DEVTOOLS") != NULL;
        if (devtools && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_F9)
            g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        recompute_keyboard();
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        input_pad_open(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && pad_id == e->gdevice.which) { input_pad_close(); pads_scanned = false; }
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        input_gyro_sensor(&e->gsensor);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        gyro_focused = false; gyro_deactivate(); motion_clear();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        recompute_pad();
        // A trigger release must not wait for another sensor report or UI tick.
        if ((g_settings.gyro_camera == GYRO_CAM_HOLD && e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
             e->gaxis.which == pad_id && e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER && e->gaxis.value <= 16000) ||
            !gyro_active()) gyro_deactivate();
        break;
    case SDL_EVENT_MOUSE_WHEEL:  // Mouse camera "Trackpad": two fingers on the trackpad turn the camera (the mouse wheel too)
        if (g_settings.trackpad_camera) {
            // (wheel units to pixels: a two-finger drag is a few units a frame; the speed setting scales it as the mouse's)
            double sx = g_settings.invert_mouse_x ? -1 : 1, sy = g_settings.invert_mouse_y ? 1 : -1;
            atomic_fetch_add(&mouse_acc_x, (int32_t)lround(sx * e->wheel.x * TRACKPAD_PX * 16));
            atomic_fetch_add(&mouse_acc_y, (int32_t)lround(sy * e->wheel.y * TRACKPAD_PX * 16));
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button != SDL_BUTTON_LEFT || g_settings.mouse_camera == MOUSE_CAM_ALWAYS || !online_mouse()) break;
        if (left_is_camera()) {  // a tap is the touch screen, a drag the camera
            bool press = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (press) {
                left_pending = !mouse_held && touch_point(e->button.windowID, e->button.x, e->button.y);
                left_x = e->button.x, left_y = e->button.y, left_win = e->button.windowID;
            } else if (left_pending) {  // released where it was pressed: one tap (reported once, then lifted)
                left_pending = false;
                if (touch_point(left_win, left_x, left_y)) atomic_store(&touch_pressed, true);
                atomic_store(&touch_down, false);
            }
            break;
        }
        {
            bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_held &&
                        touch_point(e->button.windowID, e->button.x, e->button.y);
            if (down) atomic_store(&touch_pressed, true);
            atomic_store(&touch_down, down);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:  // the chosen button held (or none): the mouse turns the camera (right stick)
        if (left_pending && (fabsf(e->motion.x - left_x) > TAP_SLOP || fabsf(e->motion.y - left_y) > TAP_SLOP))
            left_pending = false;  // moved: the camera from here on
        if (atomic_load(&touch_down) && !mouse_held && !touch_point(e->motion.windowID, e->motion.x, e->motion.y))
            atomic_store(&touch_down, false);  // (dragged off the picture: the finger is lifted)
        if (mouse_camera_on(e->motion.state)) {
            atomic_fetch_add(&mouse_acc_x, (int32_t)lround((g_settings.invert_mouse_x ? -1 : 1) * e->motion.xrel * MOUSE_PX * 16));
            atomic_fetch_add(&mouse_acc_y, (int32_t)lround((g_settings.invert_mouse_y ? -1 : 1) * e->motion.yrel * MOUSE_PX * 16));
        }
        break;
    }
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/nvn/metal.m
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening)
RT_ON_RESET(input_reboot_reset) {
    g_pad_obj = 0; g_inject_plus = 0;
    pthread_mutex_lock(&g_klock);
    while (style_events) { InputStyleEvent *next = style_events->next; free(style_events); style_events = next; }
    pthread_mutex_unlock(&g_klock);
}  // Reset hooks run before kernel_reset releases the boot's handles.
void input_frame(uint64_t frame) {
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("LARECOMP_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
// Route recording: every change of the pad state goes to a file, with the frame counted from the start of the
// recording (Developer tab, or LARECOMP_INPUT_REC=<file> from the start of the run). LARECOMP_INPUT_REPLAY=<file>
// plays such a file back from LARECOMP_INPUT_REPLAY_AT (default 0) on.
static FILE *g_rec;
static uint64_t g_rec_origin;

bool route_record_start(const char *file) {
    extern uint64_t renderer_present_count(void);
    if (g_rec) fclose(g_rec);
    g_rec = fopen(file, "w");
    g_rec_origin = renderer_present_count();
    return g_rec != NULL;
}
void route_record_stop(void) { if (g_rec) fclose(g_rec); g_rec = NULL; }
bool route_recording(void) { return g_rec != NULL; }

static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != (g_settings.handheld ? 0x20u : 0u)) return;  // Same mode/id reported by the applet operation mode.
    extern uint64_t g_watch_lo, g_watch_hi;
    if (!g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (!g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t b = menu ? script_buttons : key_buttons | pad_buttons | script_buttons;
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly) + script_ly);
    int32_t mrx, mry;
    mouse_stick(&mrx, &mry);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + mrx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry + mry) + script_ry);
    snap_to_axis(&rx, &ry, g_settings.rstick_snap);
    if (!menu) {  // Fine gyro aim is intentionally not cardinal-snapped with the manual stick.
        uint64_t gyro = gyro_guest_stick();
        rx = clamp_axis(rx + (int32_t)(uint32_t)gyro);
        ry = clamp_axis(ry + (int32_t)(uint32_t)(gyro >> 32));
    }
    extern bool mirror_input(void);  // mirror mode, the world on screen (runtime/hle/mirror.c): left and right swap
    bool mirrored = !menu && mirror_input();
    if (mirrored) {
        lx = -lx;
        uint64_t l = (b >> BTN_LEFT) & 1, r = (b >> BTN_RIGHT) & 1;
        b = (b & ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) | (l << BTN_RIGHT) | (r << BTN_LEFT);
    }
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        if (mirrored) dx = -dx;
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    {   // LARECOMP_INPUT_REC=<file>: log every change of the pad state (presented frame, buttons, sticks);
        // LARECOMP_INPUT_REPLAY=<file>: play such a log back exactly (the state at frame N is the last entry <= N)
        extern uint64_t renderer_present_count(void);
        static int rec_on = -1;
        static uint64_t replay_at;
        static struct { uint64_t f, b; int32_t lx, ly, rx, ry; } *tape;
        static size_t ntape;
        if (rec_on < 0) {
            rec_on = 1;
            if (getenv("LARECOMP_INPUT_REC")) route_record_start(getenv("LARECOMP_INPUT_REC"));
            if (getenv("LARECOMP_INPUT_REPLAY_AT")) replay_at = strtoull(getenv("LARECOMP_INPUT_REPLAY_AT"), NULL, 10);
            char rpath[200];
            const char *replay = getenv("LARECOMP_INPUT_REPLAY");
            if (getenv("LARECOMP_ROUTE")) snprintf(rpath, sizeof rpath, "routes/%s/route.txt", getenv("LARECOMP_ROUTE")), replay = rpath;
            if (replay) {
                FILE *rf = fopen(replay, "r");
                unsigned long long f, bb;
                int l1, l2, r1, r2;
                size_t cap = 0;
                while (rf && fscanf(rf, "%llu %llx %d %d %d %d", &f, &bb, &l1, &l2, &r1, &r2) == 6) {
                    if (ntape == cap) { cap = cap ? cap * 2 : 1024; tape = realloc(tape, cap * sizeof *tape); }
                    tape[ntape].f = f; tape[ntape].b = bb; tape[ntape].lx = l1; tape[ntape].ly = l2; tape[ntape].rx = r1; tape[ntape++].ry = r2;
                }
                if (rf) fclose(rf);
                LOG("[input] replaying %zu pad states from %s\n", ntape, replay);
            }
        }
        uint64_t fr = renderer_present_count();
        if (ntape && fr >= replay_at) {  // (before it a scripted LARECOMP_INPUT can take the save to where the route starts)
            b = 0; lx = ly = rx = ry = 0;
            fr -= replay_at;
            size_t lo = 0, hi = ntape;  // last entry with f <= fr
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (tape[mid].f <= fr) lo = mid + 1; else hi = mid; }
            if (lo) { b = tape[lo - 1].b; lx = tape[lo - 1].lx; ly = tape[lo - 1].ly; rx = tape[lo - 1].rx; ry = tape[lo - 1].ry; }
        }
        static uint64_t pb; static int32_t plx, ply, prx, pry;
        if (g_rec && (b != pb || lx != plx || ly != ply || rx != prx || ry != pry)) {
            fprintf(g_rec, "%llu %llx %d %d %d %d\n", (unsigned long long)(renderer_present_count() - g_rec_origin), (unsigned long long)b, lx, ly, rx, ry);
            fflush(g_rec);
            pb = b; plx = lx; ply = ly; prx = rx; pry = ry;
        }
    }
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// Controller style follows the same configured operation mode as am_handheld().
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = g_settings.handheld ? (id == 0x20 ? 2 : 0) : (id == 0 ? 1 : 0);
}

// GetTouchScreenStates<1>(TouchScreenState<1> *out, int count) -> states written. TouchScreenState<1>: {s64 sampling,
// s32 count, u32 reserved, TouchState[1]}; TouchState (0x28): {s64 delta_time, u32 attributes (1 start, 2 end), s32 finger,
// s32 x, s32 y, s32 diameter x, s32 diameter y, s32 angle, u32 reserved}. The latest state only.
HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_touch_states) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    static int64_t samples;
    static bool was_down;
    if (count < 1 || !s) { c->x[0] = 0; return; }
    memset(s, 0, 0x38);
    *(int64_t *)s = ++samples;
    bool pressed = atomic_exchange(&touch_pressed, false);
    bool down = (atomic_load(&touch_down) || pressed) && !menu_input_captured();
    if (down) {
        *(int32_t *)(s + 8) = 1;
        uint8_t *t = s + 0x10;
        *(uint32_t *)(t + 8) = was_down ? 0 : 1;  // start
        *(int32_t *)(t + 0x10) = atomic_load(&touch_x);
        *(int32_t *)(t + 0x14) = atomic_load(&touch_y);
        *(int32_t *)(t + 0x18) = *(int32_t *)(t + 0x1c) = 15;
        if (!was_down && devlog_on("touch"))  // (LARECOMP_DEVLOG=touch: where each press lands, to check the mapping)
            devlog("touch", "press at %d,%d (of %dx%d)", atomic_load(&touch_x), atomic_load(&touch_y), TOUCH_W, TOUCH_H);
    }
    was_down = down && atomic_load(&touch_down);
    c->x[0] = 1;
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected. Later mode changes wake every bound id so the SDK can
// discover both the old controller disconnect and the new controller style.
void input_style_changed(void) {
    pthread_mutex_lock(&g_klock);
    for (InputStyleEvent *item = style_events; item; item = item->next) kernel_signal(item->event);
    pthread_mutex_unlock(&g_klock);
}
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    InputStyleEvent *item = malloc(sizeof *item);
    if (!item) FATAL("cannot retain controller style event\n");
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    item->event = ev; item->next = style_events; style_events = item;
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Vibration devices: accepted but inert (the SDK's vibration node graph stays recompiled).
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}
