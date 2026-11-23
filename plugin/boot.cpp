// BetaFeel.asi, fast start (2026-11-22; see main.cpp).
//
// The game opens on a splash level whose game mode (BP_SplashScreenGameMode_C, native MvsSplashScreenGameMode) runs
// a list of start-up steps: the epilepsy warning, the logo garden, then the "Enter MultiVerse!" title, which waits for
// a key. Each step ends with the game mode's OnStepSuccess, and after the last one the game loads the frontend (the
// main menu). With the FastStart switch on, this calls OnStepSuccess itself, a step at a time, from the moment the
// list starts (InitStartUpFlow), so the game goes straight on to the main menu. Measured 2026-11-22 with Frida:
// main menu about 9.6 s after launch with no key press, against about 14 s to the title plus a key.
//
// Hooked: UObject::ProcessEvent (every Blueprint event and function call). Only the start-up looks at the calls; once
// the main menu loads (or anything unexpected happens) the hook passes everything straight through, apart from one
// flag test for the lab tools (lab.cpp: a pause or step key pressed while the Lab's sim is paused, when the game
// state's own Tick no longer runs). The hook goes in at every start, fast start on or off.
// Never skipped: the EULA, when the game shows it (a first start); skipping stops there and leaves it to the player.
// Offsets are for build 17784632; the code bytes are checked first and nothing is hooked if they differ, unless
// they differ only by another mod's jmp hook (OpenVersus), which BetaFeel then goes in front of (ChainHook).
#include "control.h"
#include <string.h>

namespace fs { void* Hook(uint8_t* target, const uint8_t* expect, size_t len, void* detour); }   // fighters.cpp

namespace fb {
using bf::Log;

const uintptr_t O_PE = 0x2d3d810;       // UObject::ProcessEvent(UFunction*, void* parms)
const uintptr_t O_NAMES = 0x8101f10;    // FNamePool block table
const uint8_t P_PE[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x10,
                        0x01, 0x00, 0x00};
// UObject / UStruct layout (UE 5.1)
const size_t O_FLAGS = 0x08, O_CLASS = 0x18, O_NAME = 0x20, S_SUPER = 0x50, S_CHILDREN = 0x58, F_NEXT = 0x38;
const uint32_t RF_DEAD = 0x40000000 | 0x8000 | 0x10000;
const size_t GM_STEP = 0x408;           // MvsSplashScreenGameMode::CurrentStep (byte)
const DWORD STEP_GAP_MS = 200;          // between two skipped steps (each step's widget gets a frame or more)

enum : uint32_t { FAST_OFF = 0, FAST_WAITING = 1, FAST_SKIPPING = 2, FAST_DONE = 3, FAST_EULA = 4, FAST_STOPPED = 5,
                  FAST_BADBUILD = 6, FAST_OTHERMOD = 7 };

typedef void (*PeFn)(void*, void*, void*);
PeFn g_orig;
volatile bool g_active;                 // false: ProcessEvent passes straight through
uint8_t* g_gm;                          // the splash game mode, once its step list starts
uint8_t* g_onStep;                      // its OnStepSuccess
DWORD g_thread;                         // the game thread (the one that started the step list)
ULONGLONG g_gmAt, g_last, g_procStart;
int g_skips;
bool g_inSkip;

void SetState(uint32_t s) { bf::Ctl()->fastStart = s; }

// The FName at p is exactly s (number part 0). Only plain (one-byte) names, which all class and function names are.
bool NameIs(const uint8_t* p, const char* s) {
    uint32_t id = *(const uint32_t*)p;
    if (*(const uint32_t*)(p + 4) != 0) return false;
    const uint8_t* blk = ((const uint8_t* const*)(bf::g_exe + O_NAMES))[id >> 16];
    if (!blk) return false;
    const uint8_t* e = blk + 2 * (id & 0xffff);
    uint16_t h = *(const uint16_t*)e;
    size_t len = h >> 6, n = strlen(s);
    return !(h & 1) && len == n && memcmp(e + 2, s, n) == 0;
}
bool NameStarts(const uint8_t* p, const char* s) {
    uint32_t id = *(const uint32_t*)p;
    const uint8_t* blk = ((const uint8_t* const*)(bf::g_exe + O_NAMES))[id >> 16];
    if (!blk) return false;
    const uint8_t* e = blk + 2 * (id & 0xffff);
    uint16_t h = *(const uint16_t*)e;
    size_t len = h >> 6, n = strlen(s);
    return !(h & 1) && len >= n && memcmp(e + 2, s, n) == 0;
}
const uint8_t* ClassOf(const uint8_t* o) { return *(const uint8_t* const*)(o + O_CLASS); }

uint8_t* FindFunc(uint8_t* o, const char* name) {
    for (uint8_t* c = *(uint8_t**)(o + O_CLASS); c; c = *(uint8_t**)(c + S_SUPER))
        for (uint8_t* f = *(uint8_t**)(c + S_CHILDREN); f; f = *(uint8_t**)(f + F_NEXT))
            if (NameIs(f + O_NAME, name)) return f;
    return nullptr;
}

ULONGLONG SinceLaunch() { return GetTickCount64() - g_procStart; }

void Stop(uint32_t state, const char* why) {
    g_active = false;
    g_gm = nullptr;
    SetState(state);
    if (state == FAST_DONE) bf::Ctl()->menuMs = (uint32_t)SinceLaunch();
    Log("fast start: %s (%.1f s after launch)", why, SinceLaunch() / 1000.0);
}

// Before every call while active. Returns true when the start-up should be stepped on after this call.
bool Look(uint8_t* obj, uint8_t* fn) {
    __try {
        const uint8_t* cls = ClassOf(obj);
        if (!g_gm) {
            if (NameIs(fn + O_NAME, "InitStartUpFlow") && NameIs(cls + O_NAME, "BP_SplashScreenGameMode_C")) {
                g_gm = obj;
                g_thread = GetCurrentThreadId();
                g_gmAt = g_last = GetTickCount64();
                g_onStep = FindFunc(obj, "OnStepSuccess");
                if (!g_onStep) { Stop(FAST_STOPPED, "no OnStepSuccess on the splash game mode; left alone"); return false; }
                SetState(FAST_SKIPPING);
                Log("fast start: the start-up steps began %.1f s after launch, skipping them", SinceLaunch() / 1000.0);
            }
            return false;
        }
        if (GetCurrentThreadId() != g_thread || obj == g_gm) return false;
        if (NameStarts(cls + O_NAME, "Frontend")) { Stop(FAST_DONE, "the main menu is loading"); return false; }
        if (NameIs(cls + O_NAME, "WBP_EulaPopup_C") && NameIs(fn + O_NAME, "BP_OnActivated")) {
            Stop(FAST_EULA, "the EULA is up, left to the player");
            return false;
        }
        if (GetTickCount64() - g_gmAt > 30000) { Stop(FAST_STOPPED, "still on the start-up after 30 s; left alone"); return false; }
        if (g_skips >= 8 || g_inSkip) return false;   // all steps done: waiting for the frontend
        return GetTickCount64() - g_last >= STEP_GAP_MS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Stop(FAST_STOPPED, "could not read a call; left alone");
        return false;
    }
}

void Step() {
    g_inSkip = true;
    g_last = GetTickCount64();
    uint8_t parms[64] = {};
    __try {
        if (*(uint32_t*)(g_gm + O_FLAGS) & RF_DEAD) { g_skips = 8; g_inSkip = false; return; }
        uint8_t before = g_gm[GM_STEP];
        g_orig(g_gm, g_onStep, parms);
        uint8_t after = g_gm ? g_gm[GM_STEP] : before;
        g_skips++;
        Log("fast start: step %u -> %u", before, after);
        if (after == before) g_skips = 8;             // the list did not move on: the last step is done
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Stop(FAST_STOPPED, "a step failed; left alone");
    }
    g_inSkip = false;
}

void HookPe(uint8_t* obj, uint8_t* fn, void* parms) {
    while (!g_orig) YieldProcessor();   // only in the instant between the jump going in and Start storing it
    if (!g_active || !obj || !fn) {
        if (lab::g_wake) lab::Poll();
        g_orig(obj, fn, parms);
        return;
    }
    bool step = Look(obj, fn);
    g_orig(obj, fn, parms);
    if (step && g_active && g_gm) Step();
}

// Executable memory within 2 GB below the exe, for a rel32 jump from inside it.
uint8_t* AllocNear(uint8_t* at) {
    uintptr_t base = (uintptr_t)at & ~(uintptr_t)0xffff;
    for (uintptr_t a = base - 0x10000; a > 0x10000 && base - a < 0x7ff00000; a -= 0x10000) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)a, &mi, sizeof mi)) continue;
        if (mi.State != MEM_FREE) {                           // skip below this allocation (the loop steps down)
            uintptr_t below = (uintptr_t)mi.AllocationBase & ~(uintptr_t)0xffff;
            if (below && below < a) a = below;
            continue;
        }
        if (void* p = VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
    }
    return nullptr;
}

// Another mod (OpenVersus, seen 2026-11-22) hooked ProcessEvent before us the usual way: a 5-byte jmp rel32 over the
// first two instructions' 6 bytes, the rest of the function untouched, its trampoline running the stolen bytes and
// jumping back to +6. Then we go in front of it: the jmp is pointed at our relay, and our "original" is where it led,
// so every call runs our hook, then the other mod's, then the game's. 1 chained, 0 not a hook (another build),
// 2 a hook we cannot follow.
int ChainHook() {
    uint8_t* at = bf::g_exe + O_PE;
    __try {
        if (at[0] != 0xe9 || memcmp(at + 6, P_PE + 6, sizeof P_PE - 6) != 0) return at[0] == 0xe9 || at[0] == 0xff ? 2 : 0;
        uint8_t* prev = at + 5 + *(int32_t*)(at + 1);
        volatile uint8_t probe = *prev; (void)probe;          // it must lead somewhere readable
        uint8_t* relay = AllocNear(at);
        if (!relay) return 2;
        uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
        uintptr_t to = (uintptr_t)HookPe;
        memcpy(jmp + 6, &to, 8);
        memcpy(relay, jmp, 14);
        FlushInstructionCache(GetCurrentProcess(), relay, 14);
        g_orig = (PeFn)prev;                                  // before the jmp moves: HookPe waits for it
        DWORD old;
        if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old)) { g_orig = nullptr; return 2; }
        *(volatile int32_t*)(at + 1) = (int32_t)(relay - (at + 5));
        VirtualProtect(at, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 8);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_orig = nullptr; return 2; }
}

void Start() {
    FILETIME c, x, k, u;
    ULONGLONG now = GetTickCount64();
    g_procStart = now;
    if (GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) {   // the process start on the tick clock
        FILETIME n; GetSystemTimeAsFileTime(&n);
        ULONGLONG ago = ((ULARGE_INTEGER{n.dwLowDateTime, n.dwHighDateTime}).QuadPart -
                         (ULARGE_INTEGER{c.dwLowDateTime, c.dwHighDateTime}).QuadPart) / 10000;
        if (ago < now) g_procStart = now - ago;
    }
    bf::Ctl()->menuMs = 0;
    bool on = bf::Ctl()->fastStartOn;
    bool ok = false;
    __try { ok = memcmp(bf::g_exe + O_PE, P_PE, sizeof P_PE) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_active = on;
    if (ok) {
        g_orig = (PeFn)fs::Hook(bf::g_exe + O_PE, P_PE, sizeof P_PE, (void*)HookPe);
    } else {
        int how = ChainHook();
        if (how != 1) {
            g_active = false;
            SetState(on ? (how == 2 ? FAST_OTHERMOD : FAST_BADBUILD) : FAST_OFF);
            if (how == 2) Log("ProcessEvent: another mod hooked it in a way BetaFeel cannot follow, nothing hooked (no fast start)");
            else Log("ProcessEvent: exe+0x%llx differs from build 17784632, nothing hooked (no fast start)", (unsigned long long)O_PE);
            return;
        }
    }
    if (!g_orig) { g_active = false; SetState(on ? FAST_STOPPED : FAST_OFF); Log("ProcessEvent: hooking failed"); return; }
    bf::Ctl()->peHooked = 1;
    SetState(on ? FAST_WAITING : FAST_OFF);
    Log("fast start: %s%s", on ? "on" : "off", ok ? "" : " (ProcessEvent: in front of another mod's hook, OpenVersus's or the like)");
}

}  // namespace fb
