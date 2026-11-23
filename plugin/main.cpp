// BetaFeel.asi: render smoothing for MultiVersus (Steam build 17784632, UE 5.1.1). Source of record:
// plugin/ in the MVS Patcher repository (2026-11-22: FighterSmooth and PoseSmooth merged; build.cmd builds it).
//
//   fighters.cpp  fighters, projectiles and the camera drawn between their last two sim positions (was FighterSmooth)
//   poses.cpp     skeleton poses and effects drawn between their last two sim steps (was PoseSmooth)
//   boot.cpp      fast start: the intro steps and the title screen skipped, straight to the main menu
//   lab.cpp       lab tools: the game's own hitbox renderer, and pausing / stepping the Lab's sim a frame at a time
//   main.cpp      settings, log, the control block the BetaFeel helper switches things with, the optional hotkey, fps,
//                 the frame rate cap
//
// No in-game overlay: on/off and status live in the helper. Settings: BetaFeel.ini beside the plugin. The developer
// build (build.cmd dev, BETAFEEL_DEV) adds the measuring aids: the input recorder, the position log and stretch test.
#include "control.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <share.h>

namespace bf {

uint8_t* g_exe;
HMODULE g_self;
wchar_t g_ini[MAX_PATH];
FILE* g_log;
CRITICAL_SECTION g_logCs;
Control* g_ctl;
Control g_private;          // used if the shared block cannot be made
const uintptr_t O_FRAME_NUMBER = 0x7e74d9c;   // GFrameNumber (checked by fighters.cpp before use there too)

Control* Ctl() { return g_ctl; }

// ---- frame rate cap ----
// The engine's own limiter (console variable t.MaxFPS; it waits at the start of a frame, before input is read) is
// held to Control::fpsCap. UEngine::GetMaxFPS (exe+0x4a41e90) reads the value through a pointer at exe+0x831b590 to
// two floats (game thread's copy, render thread's copy); the limiter reads it on every frame, so writing the two
// floats is enough. With the cap at 0 nothing is written and the game's own value stands (its settings set it at
// start and whenever they are applied); the game's value is put back when the cap is switched off.
const uintptr_t O_GET_MAXFPS = 0x4a41e90, O_MAXFPS_REF = 0x831b590;
const uint8_t P_GET_MAXFPS[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x1d, 0xf3, 0x96, 0x8d, 0x03};
bool g_capOk;
float g_capOurs = -1, g_capGames = 0;   // what this wrote last (-1: nothing), and the game's own value from before

void CapStart() {
    __try { g_capOk = memcmp(g_exe + O_GET_MAXFPS, P_GET_MAXFPS, sizeof P_GET_MAXFPS) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_capOk = false; }
    g_ctl->capState = g_capOk ? CAP_READY : CAP_BADBUILD;
    if (!g_capOk) Log("frame cap: exe+0x%llx differs from build 17784632, the cap is left alone", (unsigned long long)O_GET_MAXFPS);
}

void CapApply(Control* c) {
    if (!g_capOk) return;
    uint32_t cap = c->fpsCap;
    float want = cap < 60 ? 0.f : cap > 1000 ? 1000.f : (float)cap;   // below the sim's 60 a second: not a cap we set
    __try {
        float* v = *(float**)(g_exe + O_MAXFPS_REF);   // null until the engine has registered the variable
        if (!v) return;
        float now = v[0];
        if (want > 0) {
            if (now != want) {
                if (now != g_capOurs) g_capGames = now;          // the game set a value of its own since
                v[0] = v[1] = want;
                if (g_capOurs != want) Log("frame cap: %.0f a second (the game's own limit was %.0f)", want, g_capGames);
                g_capOurs = want;
            }
            c->capState = CAP_ON;
        } else {
            if (g_capOurs >= 0) {
                if (now == g_capOurs) v[0] = v[1] = g_capGames;   // the game's own value back
                Log("frame cap: off (the game's own limit, %.0f, is back)", g_capGames);
                g_capOurs = -1;
            }
            c->capState = CAP_READY;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char line[1024];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, a);
    va_end(a);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n] = '\n'; line[n + 1] = 0;
    EnterCriticalSection(&g_logCs);
    fputs(line, g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

int IniInt(const wchar_t* name, int def) {
    wchar_t b[32];
    GetPrivateProfileStringW(L"BetaFeel", name, L"", b, 32, g_ini);
    return b[0] ? (int)wcstol(b, nullptr, 0) : def;
}

void SaveSwitches() {
    const struct { const wchar_t* k; volatile uint8_t* v; } s[] = {
        {L"Fighters", &g_ctl->fighters}, {L"Camera", &g_ctl->camera}, {L"Poses", &g_ctl->poses},
        {L"Effects", &g_ctl->effects}, {L"Others", &g_ctl->others}};
    for (auto& x : s) WritePrivateProfileStringW(L"BetaFeel", x.k, *x.v ? L"1" : L"0", g_ini);
}

void OpenControl() {
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, CTL_NAME);
    void* p = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 4096) : nullptr;   // kept open until the game closes
    g_ctl = p ? (Control*)p : &g_private;
    Control* c = g_ctl;
    // The plugin's own start is the authority: the helper writes BetaFeel.ini whenever a switch changes.
    c->version = CTL_VERSION;
    c->size = sizeof(Control);
    c->gamePid = GetCurrentProcessId();
    c->fighters = IniInt(L"Fighters", 1) != 0;
    c->camera = IniInt(L"Camera", 1) != 0;
    c->poses = IniInt(L"Poses", 1) != 0;
    c->effects = IniInt(L"Effects", 1) != 0;
    c->others = IniInt(L"Others", 1) != 0;
    c->hotkeys = IniInt(L"Hotkeys", 0) != 0;
    c->fastStartOn = IniInt(L"FastStart", 1) != 0;
    c->lab = (uint8_t)IniInt(L"Lab", 0);
    c->fpsCap = (uint32_t)IniInt(L"FrameCap", 0);
    c->capState = CAP_NONE;
    c->fighterHooks = c->poseHooks = HOOK_NONE;
    c->frame = c->simSteps = c->fpsX100 = 0;
    c->labStatus = c->peHooked = 0;
    c->matchTick = 0;
    c->heartbeat = GetTickCount64();
#ifdef BETAFEEL_DEV
    strcpy_s(c->build, "dev 2026-11-22b");
#else
    strcpy_s(c->build, "2026-11-22b");
#endif
    MemoryBarrier();
    c->magic = CTL_MAGIC;
    if (!p) Log("control block: could not be shared (error %lu); the helper cannot switch things", GetLastError());
}

// About 50 times a second: the hotkey (when on, and only while the game is in front) and the status the helper shows.
// The frame rate is measured here, frames counted against this thread's own clock over about a second, so a reader
// that looks at a different moment still gets a steady number.
DWORD WINAPI StatusThread(void*) {
    int key = IniInt(L"Hotkey", VK_F8);
    int pauseKey = IniInt(L"LabPauseKey", VK_F5), stepKey = IniInt(L"LabStepKey", VK_F6);
    bool pauseDown = false, stepDown = false;
    bool beep = IniInt(L"Beep", 0) != 0;
    bool down = false;
    volatile uint32_t* frameNo = nullptr;
    __try { if (*(uint16_t*)(g_exe + 0x44bf9b3) == 0x05ff) frameNo = (uint32_t*)(g_exe + O_FRAME_NUMBER); }   // inc [GFrameNumber]
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    LARGE_INTEGER freq, t0 = {}, now;
    QueryPerformanceFrequency(&freq);
    uint32_t f0 = 0;
    ULONGLONG lastBeat = 0;
    for (;;) {
        Sleep(20);
        Control* c = g_ctl;
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        bool k = pid == GetCurrentProcessId() && c->hotkeys && key && (GetAsyncKeyState(key) & 0x8000);
        if (k && !down) {
            // one switch for all of it, as in the helper: on if anything was off
            uint8_t v = !(c->fighters && c->camera && c->poses && c->effects && c->others);
            c->fighters = c->camera = c->poses = c->effects = c->others = v;
            c->switchSeq++;
            SaveSwitches();
            if (beep) MessageBeep(v ? MB_ICONASTERISK : MB_ICONHAND);
            Log("hotkey: smoothing %s", v ? "on" : "off");
        }
        down = k;
        bool lab = pid == GetCurrentProcessId() && (c->lab & LAB_FRAMESTEP);
        bool kp = lab && pauseKey && (GetAsyncKeyState(pauseKey) & 0x8000), ks = lab && stepKey && (GetAsyncKeyState(stepKey) & 0x8000);
        if (kp && !pauseDown) lab::RequestToggle();
        if (ks && !stepDown) lab::RequestStep();
        pauseDown = kp; stepDown = ks;
        CapApply(c);
        QueryPerformanceCounter(&now);
        if (frameNo) {
            uint32_t f = *frameNo;
            c->frame = f;
            double dt = t0.QuadPart ? double(now.QuadPart - t0.QuadPart) / freq.QuadPart : 0;
            if (!t0.QuadPart) { t0 = now; f0 = f; }
            else if (dt >= 1.0) { c->fpsX100 = (uint32_t)((f - f0) / dt * 100 + 0.5); t0 = now; f0 = f; }
        }
        ULONGLONG ms = GetTickCount64();
        if (ms - lastBeat >= 250) { c->heartbeat = ms; lastBeat = ms; }
    }
    return 0;
}

void Start(HMODULE self) {
    g_self = self;
    g_exe = (uint8_t*)GetModuleHandleW(nullptr);
    InitializeCriticalSection(&g_logCs);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* dot = wcsrchr(path, L'.');
    if (!n || !dot) return;
    wcscpy_s(dot, path + MAX_PATH - dot, L".ini");
    wcscpy_s(g_ini, path);
    if (IniInt(L"Log", 1)) {
        wcscpy_s(dot, path + MAX_PATH - dot, L".log");
        g_log = _wfsopen(path, L"w", _SH_DENYNO);   // readable while the game runs
    }
    OpenControl();
    Log("BetaFeel.asi %s, game pid %lu", g_ctl->build, GetCurrentProcessId());
    // The two plugins this replaces must not run beside it. Whichever loads second finds the hook sites taken and
    // backs off on its own; this only tells the helper, which removes them.
    wchar_t* slash = wcsrchr(path, L'\\');
    uint32_t old = 0;
    if (slash) {
        const wchar_t* names[] = {L"FighterSmooth.asi", L"PoseSmooth.asi"};
        for (int i = 0; i < 2; i++) {
            wcscpy_s(slash + 1, path + MAX_PATH - slash - 1, names[i]);
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) { old |= 1u << i; Log("%ls is beside this plugin: remove it", names[i]); }
        }
    }
    g_ctl->oldPlugins = old;
    fs::Start();
    ps::Start();
    fb::Start();
    CapStart();
    CloseHandle(CreateThread(nullptr, 0, StatusThread, nullptr, 0, nullptr));
    Log("switches: fighters %d, camera %d, poses %d, effects %d, summons/props %d, hotkeys %d, fast start %d, lab %d, frame cap %u",
        g_ctl->fighters, g_ctl->camera, g_ctl->poses, g_ctl->effects, g_ctl->others, g_ctl->hotkeys, g_ctl->fastStartOn,
        g_ctl->lab, g_ctl->fpsCap);
}

}  // namespace bf

BOOL WINAPI DllMain(HMODULE self, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        bf::Start(self);
    }
    return TRUE;
}
