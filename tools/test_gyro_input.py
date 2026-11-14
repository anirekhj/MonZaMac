#!/usr/bin/env python3
"""Compile actual input routing against synthetic SDL reports; never initialize SDL/hardware."""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if sys.platform != 'darwin' or platform.machine() != 'arm64':
    print('SKIP: input routing fixture needs macOS Apple Silicon headers')
    raise SystemExit(0)
SOURCE = r'''
#include "runtime/hle.h"
#undef HLE_FUNC
#undef RT_ON_RESET
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#define RT_ON_RESET(fn) static void fn(void)
#include "runtime/hle/input.c"
#include <assert.h>

Settings g_settings;
int g_nmodules=2;
int16_t g_bind_pad[BIND_PAD_COUNT],g_bind_key[BIND_KEY_COUNT];
uint64_t g_watch_lo,g_watch_hi;
char g_window_shot_request[256];
static uint64_t clock_ns=1000000000;
static bool overlay, connected=true, available=true, have_gyro=true, have_accel=true;
static bool fail_gyro,fail_accel;
static int trigger,open_count,capability_calls,enable_gyro,enable_accel,disable_gyro,disable_accel;
static int sensor_queries;
static SDL_WindowFlags window_flags=SDL_WINDOW_INPUT_FOCUS;
static SDL_Gamepad *fake_pad=(SDL_Gamepad *)(uintptr_t)1;
static SDL_Window *fake_window=(SDL_Window *)(uintptr_t)2;
uint64_t plat_now_ns(void){return 5000000000000ull+clock_ns;}
Uint64 SDL_GetTicksNS(void){return clock_ns;}
Uint64 SDL_GetTicks(void){return clock_ns/1000000;}
SDL_JoystickID *SDL_GetGamepads(int *n){*n=available?1:0;SDL_JoystickID *ids=malloc(2*sizeof *ids);ids[0]=42;ids[1]=0;return ids;}
void SDL_free(void *p){free(p);}
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id){assert(id==42);open_count++;return fake_pad;}
void SDL_CloseGamepad(SDL_Gamepad *p){assert(p==fake_pad);}
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *p){assert(p==fake_pad);return 42;}
bool SDL_GamepadConnected(SDL_Gamepad *p){assert(p==fake_pad);return connected;}
bool SDL_GamepadHasSensor(SDL_Gamepad *p,SDL_SensorType t){assert(p==fake_pad);capability_calls++;return t==SDL_SENSOR_GYRO?have_gyro:have_accel;}
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *p,SDL_SensorType t,bool on){
 assert(p==fake_pad);sensor_queries++;
 if(t==SDL_SENSOR_GYRO){if(on)enable_gyro++;else disable_gyro++;return !on||!fail_gyro;}
 assert(t==SDL_SENSOR_ACCEL);if(on)enable_accel++;else disable_accel++;return !on||!fail_accel;
}
void SDL_SetEventEnabled(Uint32 t,bool on){assert(t==SDL_EVENT_GAMEPAD_SENSOR_UPDATE&&on);}
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *p){assert(p==fake_pad);return SDL_GAMEPAD_TYPE_PS5;}
const char *SDL_GetGamepadName(SDL_Gamepad *p){assert(p==fake_pad);return "Synthetic controller";}
const char *SDL_GetError(void){return "synthetic sensor error";}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *p,SDL_GamepadAxis axis){assert(p==fake_pad);return axis==SDL_GAMEPAD_AXIS_LEFT_TRIGGER?trigger:0;}
bool SDL_GetGamepadButton(SDL_Gamepad *p,SDL_GamepadButton b){(void)p;(void)b;return false;}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w){assert(w==fake_window);return window_flags;}
bool menu_input_captured(void){return overlay;}
bool mirror_input(void){return false;}
uint64_t renderer_present_count(void){return 123;}
void devlog(const char *s,const char *fmt,...){(void)s;(void)fmt;}
bool devlog_on(const char *s){(void)s;return false;}
int bind_pad_from_name(const char *s){(void)s;return -1;}
void bind_capture_begin(int a,int b){(void)a;(void)b;}
void bind_reset(int a){(void)a;}
int bind_slot_from_name(const char *s){(void)s;return -1;}
bool bind_capture_event(const SDL_Event *e){(void)e;return false;}
void watch_arm(void){}
static void report(Uint32 id,SDL_SensorType sensor,float x,float y,float z,uint64_t stamp){
 SDL_Event e={0};e.type=SDL_EVENT_GAMEPAD_SENSOR_UPDATE;e.gsensor.which=id;
 e.gsensor.sensor=sensor;e.gsensor.timestamp=stamp;e.gsensor.sensor_timestamp=1;
 e.gsensor.data[0]=x;e.gsensor.data[1]=y;e.gsensor.data[2]=z;input_handle_event(&e);
}
static void sample(float pitch,float yaw){
 clock_ns+=4000000;report(42,SDL_SENSOR_ACCEL,0,9.80665f,0,clock_ns);
 report(42,SDL_SENSOR_GYRO,pitch,yaw,0,clock_ns);input_gyro_update(fake_window);
}
static void read_axes(int *x,int *y){uint64_t p=atomic_load(&gyro_stick);*x=(int32_t)p;*y=(int32_t)(p>>32);}
static void assert_zero(void){int x,y;read_axes(&x,&y);assert(x==0&&y==0);}
static void axis_event(void){SDL_Event e={0};e.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;e.gaxis.which=42;e.gaxis.axis=SDL_GAMEPAD_AXIS_LEFT_TRIGGER;e.gaxis.value=trigger;input_handle_event(&e);}
int main(int argc,char **argv){
 assert(argc==2);g_settings.gyro_speed=1;g_settings.mouse_speed=1;
 for(int i=0;i<BIND_PAD_COUNT;i++)g_bind_pad[i]=-1;
 if(!strcmp(argv[1],"unavailable"))have_gyro=false;
 if(!strcmp(argv[1],"no-accel"))have_accel=false;
 if(!strcmp(argv[1],"accel-failure"))fail_accel=true;
 if(!strcmp(argv[1],"gyro-failure"))fail_gyro=true;
 input_gyro_update(fake_window);
 assert(open_count==1&&capability_calls==2&&enable_gyro==0&&enable_accel==0);assert_zero();
 if(!have_gyro){assert(input_gyro_status()==GYRO_STATUS_UNAVAILABLE);return 0;}
 assert(input_gyro_status()==GYRO_STATUS_OFF);
 g_settings.gyro_camera=GYRO_CAM_HOLD;input_gyro_update(fake_window);
 if(!strcmp(argv[1],"scripted")){assert(enable_gyro==0&&input_gyro_status()==GYRO_STATUS_OFF);return 0;}
 if(fail_gyro){assert(input_gyro_status()==GYRO_STATUS_ERROR);fail_gyro=false;input_gyro_calibrate();input_gyro_update(fake_window);assert(input_gyro_status()==GYRO_STATUS_CALIBRATING);}
 assert(enable_gyro>=1);assert(input_gyro_status()==GYRO_STATUS_CALIBRATING);
 if(fail_accel)assert(gyro_enabled&&!accel_enabled&&disable_gyro==0);
 for(int i=0;i<160;i++)sample(0,0);
 assert(input_gyro_status()==GYRO_STATUS_READY);assert(gyro_camera_calibrated(&gyro_camera));assert_zero();
 int caps=capability_calls;trigger=32767;axis_event();sample(.2f,.5f);
 int x,y;read_axes(&x,&y);assert(x<0&&y>0);assert(capability_calls==caps);
 uint64_t original=atomic_load(&gyro_stick),last=gyro_sample_at;
 report(999,SDL_SENSOR_GYRO,99,99,99,clock_ns);assert(gyro_sample_at==last);
 report(42,SDL_SENSOR_GYRO,99,99,99,clock_ns+999999999);assert(gyro_sample_at==last);
 report(42,SDL_SENSOR_GYRO,99,99,99,clock_ns-200000000);assert(gyro_sample_at==last);
 assert(atomic_load(&gyro_stick)==original);
 clock_ns+=4000000;report(42,SDL_SENSOR_GYRO,NAN,0,0,clock_ns);input_gyro_update(fake_window);
 assert_zero();assert(gyro_sample_at==last);sample(.2f,.5f);original=atomic_load(&gyro_stick);
 g_settings.invert_gyro_x=true;input_gyro_update(fake_window);read_axes(&x,&y);assert(x>0&&y>0);
 g_settings.invert_gyro_x=false;g_settings.gyro_speed=2;input_gyro_update(fake_window);read_axes(&x,&y);assert(x<(int32_t)original);
 trigger=0;axis_event();assert_zero();
 trigger=32767;axis_event();input_gyro_update(fake_window);assert_zero(); // release/repress without a new sample
 sample(.2f,.5f);assert(gyro_guest_stick());
 SDL_Event released={0};released.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;released.gaxis.which=42;
 released.gaxis.axis=SDL_GAMEPAD_AXIS_LEFT_TRIGGER;released.gaxis.value=0;
 input_handle_event(&released);assert_zero(); // SDL already reports held again, but queued release must clear history.
 axis_event();input_gyro_update(fake_window);assert_zero();sample(.2f,.5f);
 SDL_Event lost={0};lost.type=SDL_EVENT_WINDOW_FOCUS_LOST;input_handle_event(&lost);
 input_gyro_update(fake_window);assert_zero(); // focus loss/gain before the next report
 sample(.2f,.5f);assert(gyro_guest_stick());
 bool had_sample=gyro_camera.have_sample;input_reset();assert(!gyro_guest_stick());assert(gyro_camera.have_sample==had_sample);
 input_gyro_update(fake_window);assert_zero(); // renderer-safe reset request is cleared on the owner thread
 sample(.2f,.5f);assert(gyro_guest_stick());
 atomic_fetch_add(&gyro_publish_sequence,1);assert(!gyro_guest_stick());atomic_fetch_add(&gyro_publish_sequence,1);
 clock_ns++;report(42,SDL_SENSOR_GYRO,.2f,.5f,0,0);assert(gyro_sample_at==clock_ns); // arrival fallback, not device clock
 trigger=0;axis_event();assert_zero();input_gyro_update(fake_window);
 g_settings.gyro_camera=GYRO_CAM_ALWAYS;input_gyro_update(fake_window);sample(.2f,.5f);read_axes(&x,&y);assert(x<0);
 input_handle_event(&released);assert(gyro_guest_stick()); // Always ignores a queued L2 release.
 // Guest menus are intentionally not inferred; Always needs no trigger.
 overlay=true;input_gyro_update(fake_window);assert_zero();overlay=false;sample(.2f,.5f);
 window_flags=SDL_WINDOW_INPUT_FOCUS|SDL_WINDOW_HIDDEN;input_gyro_update(fake_window);assert_zero();
 window_flags=0;input_gyro_update(fake_window);assert_zero();window_flags=SDL_WINDOW_INPUT_FOCUS;sample(.2f,.5f);
 // No service update after this point: guest-side expiry must still neutralize stale aim.
 clock_ns+=150000000;
 uint8_t state_bytes[0x28];uint32_t id=0;Cpu cpu={0};cpu.x[0]=(uintptr_t)state_bytes;cpu.x[1]=(uintptr_t)&id;
 int queries_before=sensor_queries;fill_state(&cpu);assert(*(int32_t *)(state_bytes+24)==0&&*(int32_t *)(state_bytes+28)==0);assert(sensor_queries==queries_before);
 // Fine gyro Y survives cardinal snapping of the manual X stick; manual buttons survive too.
 gyro_sample_at=clock_ns;gyro_publish(0,.3f);key_rx=10000;key_ry=1000;key_buttons=1;g_settings.rstick_snap=30;
 fill_state(&cpu);assert(*(int32_t *)(state_bytes+24)==10000);assert(*(int32_t *)(state_bytes+28)>9000);assert(*(uint64_t *)(state_bytes+8)&1);
 key_rx=key_ry=key_buttons=0;sample(.2f,.5f);
 setenv("LARECOMP_ROUTE","synthetic-unused-route",1);input_gyro_update(fake_window);assert_zero();assert(input_gyro_status()==GYRO_STATUS_OFF);
 unsetenv("LARECOMP_ROUTE");input_gyro_update(fake_window);sample(.2f,.5f);
 input_gyro_calibrate();input_gyro_update(fake_window);assert_zero();assert(!gyro_camera_calibrated(&gyro_camera));
 g_settings.gyro_camera=GYRO_CAM_OFF;input_gyro_update(fake_window);assert_zero();assert(disable_gyro>=1);
 g_settings.gyro_camera=GYRO_CAM_ALWAYS;input_gyro_update(fake_window);sample(.2f,.5f);
 connected=false;available=false;input_gyro_update(fake_window);assert_zero();assert(input_gyro_status()==GYRO_STATUS_DISCONNECTED);
 connected=true;available=true;SDL_Event added={0};added.type=SDL_EVENT_GAMEPAD_ADDED;added.gdevice.which=42;input_handle_event(&added);input_gyro_update(fake_window);
 assert(open_count==2&&capability_calls==4&&!gyro_camera_calibrated(&gyro_camera));
 puts("PASS: actual input routing, startup/hotplug, sensor gating, timestamps, calibration, stale guest expiry and merge");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='monzamac-gyro-input-') as directory:
    tmp = Path(directory)
    fixture = tmp/'input.c'
    fixture.write_text(SOURCE)
    binary = tmp/'check'
    sdk = os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
    prefix = Path(os.environ.get('SDL3_PREFIX','/opt/homebrew/opt/sdl3'))
    subprocess.run(['xcrun','clang','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-Wno-unused-parameter','-Wno-c23-extensions','-fno-strict-aliasing','-isysroot',sdk,
                    '-I',str(ROOT),'-I',str(ROOT/'runtime'),'-I',str(ROOT/'runtime/nvn'),'-I',str(prefix/'include'),
                    str(fixture),str(ROOT/'runtime/hle/gyro_camera.c'),'-L',str(prefix/'lib'),'-lSDL3',
                    '-Wl,-rpath,'+str(prefix/'lib'),'-Wl,-dead_strip','-o',str(binary)],check=True)
    env = {k:v for k,v in os.environ.items() if not k.startswith(('LARECOMP_','LUMIOSE_','SDL_'))}
    for case in ('normal','unavailable','no-accel','accel-failure','gyro-failure','scripted'):
        case_env = dict(env)
        if case == 'scripted': case_env['LARECOMP_INPUT_REPLAY']='synthetic-unused-route'
        subprocess.run([str(binary),case],check=True,cwd=tmp,env=case_env,timeout=10)
print('PASS: six synthetic SDL routing groups; no SDL_Init or hardware discovery')
