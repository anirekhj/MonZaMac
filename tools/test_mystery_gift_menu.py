#!/usr/bin/env python3
"""Exercise the actual offline gift picker with real ImGui, without a GPU, game, save or controller."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''

#include "runtime/ui/menu.mm"
#include <assert.h>
#include <map>

bool g_za_frame_hook = false;
void osk_cancel(void) {}
extern "C" {
int g_start_screen = 1;
static bool temporal_available = true;
bool fg_supported(void) { return true; }
bool temporal_supported(void) { return temporal_available; }
bool rt_za_smooth_anim_supported(void) { return true; }
SDL_Window *ui_window(void) { return NULL; }
void renderer_get_scale(float *sx, float *sy) { *sx = *sy = 1; }
double output_scale_dynamic_factor(void) { return 1; }
void am_set_handheld(bool on) { g_settings.handheld = on; }
bool bind_settings_load(const char *key, const char *value) { return false; }
void bind_settings_save(FILE *f) {}
void bind_capture_cancel(void) {}
void input_reset(void) {}
void backend_represent(void) {}
uint64_t renderer_present_count(void) { return 0; }
volatile int g_rt_unwind = 0;
}


extern "C" { const char *ui_tr(const char *s) { return s; } bool bind_capture_event(const SDL_Event *) { return false; } Settings g_settings = {}; void settings_save(void) {} }

#include <thread>
struct Row { ImGuiID id; ImGuiWindow *window; };
static std::map<std::string, Row> rows;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *, ImGuiID, const ImRect &, const ImGuiLastItemData *) {}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *ctx, ImGuiID id, const char *label, ImGuiItemStatusFlags) {
    if (!strcmp(label,"Claim gift") || !strcmp(label,"Back") || !strncmp(label,"Gift ",5))
        rows[label]={id,ctx->CurrentWindow};
}
void ImGuiTestEngineHook_Log(ImGuiContext *,const char *,...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *,ImGuiID){return NULL;}
static void frame(const char *activate=NULL, ImGuiKey key=ImGuiKey_None) {
    ImGuiIO &io=ImGui::GetIO();
    if(key!=ImGuiKey_None)io.AddKeyEvent(key,true);
    ImGui::NewFrame();
    if(activate){Row r=rows.at(activate);ImGui::FocusWindow(r.window);ImGui::SetFocusID(r.id,r.window);
        ImGui::SetNavCursorVisible(true); GImGui->NavActivateId=GImGui->NavActivateDownId=GImGui->NavActivatePressedId=r.id;}
    mystery_gift_draw(io,1);
    ImGui::Render();
    if(key!=ImGuiKey_None)io.AddKeyEvent(key,false);
}
static void open_fixture() {
    g_gift_options.clear();
    for(int i=0;i<19;i++)g_gift_options.push_back({"Gift "+std::to_string(i),"Gift details",i?"Available":"Already claimed",i!=0});
    g_gift_selected=0;g_gift_result=-2;g_gift_appearing=true;g_gift_active=true;
    frame();frame();
}
int main(){
    ImGui::CreateContext();ImGuiIO &io=ImGui::GetIO();io.DisplaySize=ImVec2(1920,1080);io.DeltaTime=1.f/60;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags|=ImGuiBackendFlags_HasGamepad;io.IniFilename=NULL;
    g_font=g_font_big=io.Fonts->AddFontDefault();unsigned char *p;int w,h;io.Fonts->GetTexDataAsRGBA32(&p,&w,&h);
    GImGui->TestEngineHookItems=true;
    open_fixture();assert(menu_input_captured());assert(rows.count("Gift 18"));
    frame("Claim gift");assert(g_gift_result==-2); // unavailable gift cannot be claimed
    frame("Gift 1");frame();assert(g_gift_selected==1);
    frame("Claim gift");assert(g_gift_result==1);
    open_fixture();frame("Gift 0");frame();
    frame(NULL,ImGuiKey_DownArrow);frame();assert(g_gift_selected==1);
    frame(NULL,ImGuiKey_GamepadDpadDown);frame();assert(g_gift_selected==2);
    frame(NULL,ImGuiKey_GamepadStart);frame();assert(g_gift_result==2);
    open_fixture();frame(NULL,ImGuiKey_GamepadStart);frame();assert(g_gift_result==-2);
    frame("Back");assert(g_gift_result==-1);
    open_fixture();SDL_Event e={};e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_ESCAPE;
    assert(menu_handle_event(&e));assert(g_gift_result==-1);
    open_fixture();e={};e.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;e.gbutton.button=SDL_GAMEPAD_BUTTON_SOUTH;
    assert(menu_handle_event(&e));assert(g_gift_result==-1);
    open_fixture();e={};e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_F1;
    assert(menu_handle_event(&e));assert(!menu_is_open());assert(g_gift_result==-2);
    menu_reset();assert(!menu_input_captured());assert(g_gift_options.empty());assert(g_gift_result==-1);
    assert(menu_mystery_gift_run(NULL,1)==-1);assert(menu_mystery_gift_run(NULL,0)==-1);
    g_rt_unwind=1;MenuGiftOption one={"Gift","Details","Available",true};
    assert(menu_mystery_gift_run(&one,1)==-1);g_rt_unwind=0;
    char title[]="Copied gift";MenuGiftOption available={title,"Details","Available",true};int returned=-9;
    std::thread waiter([&]{returned=menu_mystery_gift_run(&available,1);});
    for(int i=0;i<1000&&!g_gift_active.load();i++)SDL_Delay(1);
    assert(g_gift_active.load());title[0]='X';
    pthread_mutex_lock(&gift_lock);assert(g_gift_options[0].name=="Copied gift");pthread_mutex_unlock(&gift_lock);
    assert(menu_mystery_gift_run(&available,1)==-1);g_gift_result=0;waiter.join();
    assert(returned==0&&!g_gift_active.load()&&!g_gift_busy.load());assert(g_gift_options.empty());
    std::thread resetting([&]{returned=menu_mystery_gift_run(&available,1);});
    for(int i=0;i<1000&&!g_gift_active.load();i++)SDL_Delay(1);
    assert(g_gift_active.load());menu_reset();resetting.join();assert(returned==-1&&!menu_input_captured());
    ImGui::DestroyContext();puts("PASS: unavailable/claim, 19 gift rows, unavailable/claim, keyboard+controller navigation, Start claim, cancel, shortcuts, reset, invalid/unwind, copied lifetime, blocking result, concurrent rejection, reset cancellation");
}

'''


def main():
    if sys.platform != "darwin":
        print("SKIP: gift picker test needs macOS UI headers")
        return
    env = os.environ.copy()
    developer = Path("/Applications/Xcode.app/Contents/Developer")
    if "DEVELOPER_DIR" not in env and developer.is_dir():
        env["DEVELOPER_DIR"] = str(developer)
    if "SDKROOT" not in env:
        env["SDKROOT"] = subprocess.check_output(["xcrun", "--show-sdk-path"], env=env, text=True).strip()
    cflags = libs = None
    if shutil.which("pkg-config"):
        try:
            cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl3"], text=True))
            libs = shlex.split(subprocess.check_output(["pkg-config", "--libs", "sdl3"], text=True))
        except subprocess.CalledProcessError:
            pass
    if cflags is None:
        candidates = [Path(env["SDL3_PREFIX"])] if "SDL3_PREFIX" in env else [Path("/opt/homebrew/opt/sdl3"), Path("/usr/local/opt/sdl3")]
        prefix = next((p for p in candidates if (p / "include/SDL3/SDL.h").is_file()), None)
        if prefix is None:
            raise SystemExit("SDL3 development files are required; set SDL3_PREFIX or install the repository dependencies.")
        cflags, libs = ["-I" + str(prefix / "include")], ["-L" + str(prefix / "lib"), "-lSDL3"]
    with tempfile.TemporaryDirectory(prefix="mystery-gift-menu-") as name:
        tmp = Path(name)
        source, binary = tmp / "check.mm", tmp / "check"
        source.write_text(SOURCE)
        imgui = ROOT / "third_party/imgui"
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-O1", "-fobjc-arc", "-DIMGUI_ENABLE_TEST_ENGINE",
                        "-I", str(ROOT), "-I", str(imgui), "-I", str(imgui / "backends"), *cflags,
                        str(source), str(ROOT / "runtime/ui/option_rows.mm"),
                        *(str(imgui / f"{part}.cpp") for part in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")),
                        *libs, "-Wl,-dead_strip", "-framework", "AppKit", "-framework", "Metal",
                        "-framework", "UniformTypeIdentifiers", "-o", str(binary)], check=True, env=env)
        env = {k: v for k, v in env.items() if not k.startswith("LARECOMP_")}
        subprocess.run([str(binary)], cwd=tmp, env=env, check=True)


if __name__ == "__main__":
    main()
