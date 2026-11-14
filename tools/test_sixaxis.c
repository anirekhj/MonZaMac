// Authored CPU-only SixAxis ABI fixtures. No SDL initialization, controller or game files.
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle/sixaxis.c"
#include "../runtime/services/hid_set.c"

uint64_t g_alias_base, g_alias_size;
static bool handheld, available, demand, log_enabled;
static unsigned demand_calls, sample_reads, diagnostic_lines;
static char diagnostic_text[512], diagnostic_request[512];
static uint64_t clock_ns = 1000000000ull;
static InputMotionSnapshot physical;
static struct {
    uint32_t id, fullkey, handheld, invalid, mode;
    float p[2];
    uint32_t before;
    SixAxisState states[3];
    uint32_t after;
} arena;

bool am_handheld(void) { return handheld; }
uint64_t plat_now_ns(void) { return clock_ns; }
void input_motion_set_enabled(bool enabled) { demand = enabled; ++demand_calls; }
bool input_motion_snapshot(InputMotionSnapshot *out) {
    ++sample_reads;
    if (!available || !demand) return false;
    *out = physical;
    return true;
}
bool devlog_on(const char *subject) { assert(!strcmp(subject, "motion")); return log_enabled; }
void devlog(const char *subject, const char *fmt, ...) {
    assert(!strcmp(subject, "motion"));
    if (log_enabled) {
        ++diagnostic_lines;
        va_list args; va_start(args, fmt); vsnprintf(diagnostic_text, sizeof diagnostic_text, fmt, args); va_end(args);
        if (strstr(diagnostic_text, "SixAxis request")) snprintf(diagnostic_request, sizeof diagnostic_request, "%s", diagnostic_text);
    }
}
// Unrelated HID/settings service dependencies are never used by these scenarios.
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { (void)w; (void)r; assert(!"unexpected event request"); return NULL; }
KSharedMem *kernel_create_shared_memory(uint64_t size, void (*map)(KSharedMem *), uint32_t *h) {
    (void)size; (void)map; (void)h; assert(!"unexpected shared memory request"); return NULL;
}
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    (void)name; (void)handler; (void)state; assert(!"unexpected service creation"); return NULL;
}
void ipc_return_object(IpcResp *resp, Service *service) { (void)resp; (void)service; assert(!"unexpected object return"); }
const char *settings_language(void) { return "en-US"; }
static uint64_t ptr(const void *p) { return (uintptr_t)p; }
static void closef(float a, float b) { assert(fabsf(a - b) < 0.00001f); }
static void start(uint32_t *h) { Cpu c = {0}; c.x[0] = ptr(h); c.x[30] = 0x12340000; six_start(&c); }
static void stop(uint32_t *h) { Cpu c = {0}; c.x[0] = ptr(h); c.x[30] = 0x12350000; six_stop(&c); }
static unsigned states(uint32_t *h, int count) {
    Cpu c = {0}; c.x[0] = ptr(arena.states); c.x[1] = (uint32_t)count; c.x[2] = ptr(h); c.x[30] = 0x12360000;
    six_get_states(&c); return (unsigned)c.x[0];
}
static unsigned handles(uint32_t id, uint32_t style, int count) {
    arena.id = id;
    Cpu c = {0}; c.x[0] = ptr(arena.states); c.x[1] = (uint32_t)count;
    c.x[2] = ptr(&arena.id); c.x[3] = style; c.x[30] = 0x12390000; six_get_handles(&c); return (unsigned)c.x[0];
}
static void fresh(void) {
    handheld = false; available = true; log_enabled = false;
    g_alias_base = ptr(&arena); g_alias_size = sizeof arena;
    memset(&arena, 0xa5, sizeof arena);
    arena.fullkey = SIX_FULLKEY; arena.handheld = SIX_HANDHELD; arena.invalid = 0xffffffffu;
    memset(&physical, 0, sizeof physical);
    physical.sampling_number = 42; physical.delta_time_ns = 4000000;
    physical.acceleration[1] = 9.80665f; physical.acceleration_valid = physical.gyro_valid = physical.at_rest = true;
    physical.direction[0] = physical.direction[4] = physical.direction[8] = 1;
    six_reset(); demand_calls = sample_reads = diagnostic_lines = 0;
}
static void test_handles(void) {
    assert(handles(0, 1, 1) == 1);
    assert(*(uint32_t *)arena.states == SIX_FULLKEY);
    assert(((unsigned char *)arena.states)[4] == 0xa5);
    assert(handles(0, 1, 0) == 0 && handles(0, 1, -1) == 0);
    assert(handles(1, 1, 1) == 0 && handles(0x20, 2, 1) == 0 && handles(0, 2, 1) == 0);
    handheld = true;
    assert(handles(0x20, 2, 1) == 1);
    assert(*(uint32_t *)arena.states == SIX_HANDHELD);
    assert(handles(0, 1, 1) == 0 && handles(0x20, 1, 1) == 0);
    Cpu c = {0}; c.x[0] = ptr(arena.states); c.x[1] = 1; c.x[2] = 1; c.x[3] = 2;
    six_get_handles(&c); assert(c.x[0] == 0);
    c.x[0] = g_alias_base + g_alias_size - 1; c.x[1] = 1; c.x[2] = ptr(&arena.id); c.x[3] = 2;
    six_get_handles(&c); assert(c.x[0] == 0);
    assert(arena.before == 0xa5a5a5a5 && arena.after == 0xa5a5a5a5);
}
static void test_lifecycle(void) {
    assert(states(&arena.fullkey, 3) == 0 && sample_reads == 0);
    start(&arena.invalid); assert(!demand && !demand_calls);
    start(&arena.fullkey); start(&arena.fullkey); assert(demand && demand_calls == 1);
    start(&arena.handheld); assert(demand_calls == 2);
    stop(&arena.fullkey); stop(&arena.fullkey); assert(demand && demand_calls == 3);
    stop(&arena.handheld); assert(!demand && demand_calls == 4);
    start(&arena.fullkey); six_reset(); assert(!demand && states(&arena.fullkey, 1) == 0);
}
static void test_states(void) {
    start(&arena.fullkey);
    assert(states(&arena.fullkey, 3) == 1);
    assert(arena.states[0].sampling_number == 42 && arena.states[0].delta_time == 4000000);
    assert(arena.states[0].attributes == 1 && arena.states[0].reserved == 0);
    for (unsigned i = 0; i < sizeof(SixAxisState); ++i) assert(((unsigned char *)&arena.states[1])[i] == 0xa5);
    assert(states(&arena.fullkey, 3) == 1 && arena.states[0].sampling_number == 42);
    physical.sampling_number++; assert(states(&arena.fullkey, 1) == 1 && arena.states[0].sampling_number == 43);
    assert(states(&arena.fullkey, 0) == 0 && states(&arena.fullkey, -1) == 0);
    Cpu c = {0}; c.x[0] = ptr(&arena.fullkey); six_at_rest(&c); assert(c.x[0] == 1);
    physical.at_rest = false; c.x[0] = ptr(&arena.fullkey); six_at_rest(&c); assert(c.x[0] == 0);
    available = false;
    assert(states(&arena.fullkey, 1) == 0);
    c.x[0] = ptr(arena.states); c.x[1] = ptr(&arena.fullkey); six_get_state(&c);
    assert(arena.states[0].attributes == 0 && arena.states[0].sampling_number == 0);
    closef(arena.states[0].direction[0], 1);
    available = true; handheld = true; assert(states(&arena.fullkey, 1) == 0);
    start(&arena.handheld); assert(states(&arena.handheld, 1) == 1);
    physical.acceleration[0] = NAN; assert(states(&arena.handheld, 1) == 0);
    physical.acceleration_valid = false; assert(states(&arena.handheld, 1) == 1);
    closef(arena.states[0].acceleration[0], 0);
    physical.angular_velocity[2] = INFINITY; assert(states(&arena.handheld, 1) == 0);
    physical.gyro_valid = false; assert(states(&arena.handheld, 1) == 0);
    physical.acceleration_valid = true; physical.acceleration[0] = 0;
    assert(states(&arena.handheld, 1) == 1);  // accelerometer-only controllers still support tilt
    physical.direction[0] = NAN; assert(states(&arena.handheld, 1) == 0);
    assert(arena.before == 0xa5a5a5a5 && arena.after == 0xa5a5a5a5);
}
static void test_axes(void) {
    start(&arena.fullkey); assert(states(&arena.fullkey, 1) == 1);
    closef(arena.states[0].acceleration[0], 0); closef(arena.states[0].acceleration[1], 0);
    closef(arena.states[0].acceleration[2], -1);
    for (int i = 0; i < 9; ++i) closef(arena.states[0].direction[i], i % 4 ? 0 : 1);
    const float tau = 6.2831853071795864769f;
    physical.acceleration[0] = 9.80665f; physical.acceleration[1] = 19.6133f; physical.acceleration[2] = 29.41995f;
    for (int i = 0; i < 3; ++i) { physical.angular_velocity[i] = tau * (i + 1); physical.angle[i] = tau * (i + 4); }
    assert(states(&arena.fullkey, 1) == 1);
    closef(arena.states[0].acceleration[0], -1); closef(arena.states[0].acceleration[1], 3); closef(arena.states[0].acceleration[2], -2);
    closef(arena.states[0].angular_velocity[0], 1); closef(arena.states[0].angular_velocity[1], -3); closef(arena.states[0].angular_velocity[2], 2);
    closef(arena.states[0].angle[0], 4); closef(arena.states[0].angle[1], -6); closef(arena.states[0].angle[2], 5);
    // +90 degrees around SDL X maps to +90 around native X; native direction rows are world axes.
    const float quarter_x[9] = {1,0,0, 0,0,-1, 0,1,0};
    memcpy(physical.direction, quarter_x, sizeof quarter_x);
    assert(states(&arena.fullkey, 1) == 1);
    const float expected[9] = {1,0,0, 0,0,1, 0,-1,0};
    for (int i = 0; i < 9; ++i) closef(arena.states[0].direction[i], expected[i]);
    // Face-down physical controller: gravity and native face-up axis both reverse, independent of camera mode.
    physical.acceleration[0] = physical.acceleration[2] = 0; physical.acceleration[1] = -9.80665f;
    const float inverted[9] = {1,0,0, 0,-1,0, 0,0,-1};
    memcpy(physical.direction, inverted, sizeof inverted);
    assert(states(&arena.fullkey, 1) == 1);
    closef(arena.states[0].acceleration[2], 1); closef(arena.states[0].direction[8], -1);
}
static void test_controls(void) {
    Cpu c = {0}; c.x[0] = ptr(&arena.fullkey); six_fusion_enabled(&c); assert(c.x[0] == 1);
    c.x[0] = ptr(&arena.fullkey); c.x[1] = 0; six_enable_fusion(&c);
    c.x[0] = ptr(&arena.fullkey); six_fusion_enabled(&c); assert(c.x[0] == 0);
    c.x[0] = ptr(&arena.handheld); six_fusion_enabled(&c); assert(c.x[0] == 1);
    c.x[0] = ptr(&arena.fullkey); float p0 = .25f, p1 = 2.5f;
    memcpy(&c.v[0], &p0, 4); memcpy(&c.v[1], &p1, 4); six_set_fusion_parameters(&c);
    c.x[0] = ptr(&arena.p[0]); c.x[1] = ptr(&arena.p[1]); c.x[2] = ptr(&arena.fullkey); six_get_fusion_parameters(&c);
    closef(arena.p[0], p0); closef(arena.p[1], p1);
    c.x[0] = ptr(&arena.fullkey); p0 = NAN; memcpy(&c.v[0], &p0, 4); six_set_fusion_parameters(&c);
    c.x[0] = ptr(&arena.p[0]); c.x[1] = ptr(&arena.p[1]); c.x[2] = ptr(&arena.fullkey); six_get_fusion_parameters(&c);
    closef(arena.p[0], .25f);
    for (uint32_t m = 0; m < 3; ++m) {
        arena.mode = m; c.x[0] = ptr(&arena.fullkey); c.x[1] = ptr(&arena.mode); six_set_drift(&c);
        c.x[0] = ptr(&arena.fullkey); six_get_drift(&c); assert(c.x[0] == m);
    }
    arena.mode = 3; c.x[0] = ptr(&arena.fullkey); c.x[1] = ptr(&arena.mode); six_set_drift(&c);
    c.x[0] = ptr(&arena.fullkey); six_get_drift(&c); assert(c.x[0] == 2);
    six_reset(); c.x[0] = ptr(&arena.fullkey); six_fusion_enabled(&c); assert(c.x[0] == 1);
    c.x[0] = ptr(&arena.fullkey); six_get_drift(&c); assert(c.x[0] == 1);
}
static uint32_t ipc_motion_control(uint32_t command, uint32_t handle, uint32_t bytes) {
    // Deliberately unaligned payload: the service must decode the handle safely.
    uint8_t payload[17] = {0}; memcpy(payload + 1, &handle, sizeof handle);
    IpcReq request = {.cmd = command, .data = payload + 1, .data_size = bytes};
    IpcResp response = {0}; hid(NULL, &request, &response); return response.result;
}
static void test_ipc_activation(void) {
    handheld = true;
    assert(handles(0x20, 2, 1) == 1 && !demand);
    assert(states(&arena.handheld, 1) == 0 && !demand); // enumeration/reads never start sensing
    for (unsigned bytes = 0; bytes < 16; bytes++) {
        assert(ipc_motion_control(66, SIX_HANDHELD, bytes) != 0);
        assert(!demand && demand_calls == 0);
    }
    log_enabled = true;
    assert(handles(0x20, 2, 1) == 1 && !demand);
    assert(strstr(diagnostic_text, "SixAxis handles") && strstr(diagnostic_text, "guest_lr=0x12390000"));
    assert(ipc_motion_control(66, SIX_HANDHELD, 16) == 0 && demand && demand_calls == 1);
    assert(strstr(diagnostic_text, "origin=ipc"));
    assert(states(&arena.handheld, 1) == 1 && arena.states[0].sampling_number == 42);
    closef(arena.states[0].acceleration[2], -1);
    physical.sampling_number++;
    physical.acceleration[1] = -9.80665f;
    physical.direction[4] = physical.direction[8] = -1;
    assert(states(&arena.handheld, 1) == 1 && arena.states[0].sampling_number == 43);
    closef(arena.states[0].acceleration[2], 1); closef(arena.states[0].direction[8], -1);
    start(&arena.handheld); assert(demand_calls == 1 && strstr(diagnostic_text, "origin=hle"));
    assert(strstr(diagnostic_request, "api=start") && strstr(diagnostic_request, "guest_lr=0x12340000"));
    assert(ipc_motion_control(66, SIX_HANDHELD, 16) == 0 && demand_calls == 1);
    for (unsigned bytes = 0; bytes < 16; bytes++) {
        assert(ipc_motion_control(67, SIX_HANDHELD, bytes) != 0);
        assert(demand && demand_calls == 1 && states(&arena.handheld, 1) == 1);
    }
    assert(ipc_motion_control(66, 0xffffffff, 16) != 0);
    assert(ipc_motion_control(67, 0xffffffff, 16) != 0);
    IpcReq null_request = {.cmd = 67, .data_size = 16}; IpcResp null_response = {0};
    hid(NULL, &null_request, &null_response);
    assert(null_response.result != 0 && demand && demand_calls == 1);
    start(&arena.fullkey); assert(demand_calls == 2);
    assert(ipc_motion_control(67, SIX_HANDHELD, 16) == 0 && demand && demand_calls == 3);
    assert(states(&arena.handheld, 1) == 0);
    handheld = false; assert(states(&arena.fullkey, 1) == 1);
    stop(&arena.fullkey); assert(!demand && demand_calls == 4);
    assert(strstr(diagnostic_request, "api=stop") && strstr(diagnostic_request, "guest_lr=0x12350000"));
    assert(ipc_motion_control(67, SIX_FULLKEY, 16) == 0 && !demand && demand_calls == 4);
    assert(ipc_motion_control(66, SIX_FULLKEY, 16) == 0 && demand);
    six_reset(); hid_reset();
    assert(!demand && states(&arena.fullkey, 1) == 0);
}
static void *toggle(void *p) { for (int i = 0; i < 10000; ++i) { start(p); stop(p); } return NULL; }
static void test_parallel(void) {
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, toggle, &arena.fullkey));
    assert(!pthread_create(&b, NULL, toggle, &arena.handheld));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(!demand && !six_controls[0].started && !six_controls[1].started);
    start(&arena.fullkey); log_enabled = true;
    for (int i = 0; i < 100; ++i) assert(states(&arena.fullkey, 1) == 1);
    assert(diagnostic_lines == 1);
    assert(strstr(diagnostic_text, "api=states") && strstr(diagnostic_text, "guest_lr=0x12360000"));
    Cpu c = {0}; c.x[0] = ptr(arena.states); c.x[1] = ptr(&arena.fullkey); c.x[30] = 0x12370000;
    six_get_state(&c); assert(diagnostic_lines == 1); // another read API does not bypass the shared one-second throttle
    clock_ns += 1000000000ull; six_get_state(&c);
    assert(diagnostic_lines == 2 && strstr(diagnostic_text, "api=state") && strstr(diagnostic_text, "guest_lr=0x12370000"));
    clock_ns += 1000000000ull; c.x[0] = ptr(&arena.fullkey); c.x[30] = 0x12380000; six_at_rest(&c);
    assert(diagnostic_lines == 3 && strstr(diagnostic_text, "api=at-rest") && strstr(diagnostic_text, "guest_lr=0x12380000"));
}
int main(int argc, char **argv) {
    assert(argc == 2); fresh();
    if (!strcmp(argv[1], "handles")) test_handles();
    else if (!strcmp(argv[1], "lifecycle")) test_lifecycle();
    else if (!strcmp(argv[1], "states")) test_states();
    else if (!strcmp(argv[1], "axes")) test_axes();
    else if (!strcmp(argv[1], "controls")) test_controls();
    else if (!strcmp(argv[1], "parallel")) test_parallel();
    else if (!strcmp(argv[1], "ipc")) test_ipc_activation();
    else assert(!"unknown scenario");
    puts("PASS 1 SixAxis scenario");
    return 0;
}
