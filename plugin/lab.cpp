// BetaFeel.asi, lab tools (2026-11-22; see main.cpp).
//
//   Hitboxes   The game's own debug collider renderer (APfgDebugColliderRenderer, still in the shipping build but
//              never spawned) is spawned into the match and told to draw hitboxes, hurtboxes, projectiles and
//              projectile shields (red: hit, blue: hurt). Display only: it reads the sim's colliders, it changes none.
//   Frame step The Lab's own debug game state (PFGFixedGameStateDebugBP_C) has PauseTick / ResumeTick. A key pauses
//              and resumes the sim, another runs exactly one sim step (resume, then pause again right after the
//              step). Other game states (online matches) have no such functions, so there it does nothing. (Its own
//              TickOneFrame did not move the sim when called from outside, 2026-11-22.)
// Runs on the game thread: from the game state's Tick (fighters.cpp calls OnGsTick) and, for key requests, from
// ProcessEvent (boot.cpp calls Poll while g_wake is set), because a paused game state is not ticked at all.
// Switches: control block `lab`.
// Offsets are for build 17784632 (fighters.cpp checks the code bytes before anything is hooked).
#include "control.h"
#include <string.h>

namespace fb { bool NameIs(const uint8_t* p, const char* s); uint8_t* FindFunc(uint8_t* o, const char* name); }   // boot.cpp

namespace lab {
using bf::Log;

const uintptr_t O_PE = 0x2d3d810;           // UObject::ProcessEvent
const uintptr_t O_OBJS = 0x81c5130;         // GUObjectArray: NumElements +0, chunk table +0x18 (XORed)
const uint64_t OBJS_XOR = 0x1b5deafd6b4068cull;
const size_t O_FLAGS = 0x08, O_CLASS = 0x18, O_NAME = 0x20, FN_PARMS = 0xc6;
const uint32_t RF_DEAD = 0x40000000 | 0x8000 | 0x10000;
const size_t GS_RENDERER = 0x460;           // APfgFixedGameStateBase::DebugColliderRenderer
const size_t R_SHOW = 0x2f8;                // APfgDebugColliderRenderer::CollisionTypeShowState, TMap<EPfgCollisionType, bool>
// EPfgCollisionType: World 0, Player 1, WorldJumpThrough 2, Hit 3, Hurt 4, BlastBox 5, WorldSensor 6, Projectile 7,
// ProjectileShield 8, GameplayTrigger 9, SpawnedObject 10
const uint32_t SHOWN = (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8);

typedef void (*PeFn)(void*, void*, void*);

uint8_t* g_gs;                              // the game state seen last
uint8_t* g_renderer;
bool g_spawnTried, g_shown;
uint8_t* g_cls;                             // UClass PfgDebugColliderRenderer
uint8_t* g_statics;                         // Default__GameplayStatics
uint8_t *g_fnBegin, *g_fnFinish, *g_fnPause, *g_fnResume;
bool g_paused, g_stepping, g_stepTried;
uint32_t g_stepFrom, g_lastTick;           // g_lastTick: simTick at the last OnGsTick
volatile LONG g_reqToggle, g_reqStep;
volatile bool g_wake;
DWORD g_thread;                             // the game thread (the one that ticks the game state)
bool g_inPoll;

void Pe(void* o, void* f, void* parms) { ((PeFn)(bf::g_exe + O_PE))(o, f, parms); }

uint8_t* FindObject(const char* name, const char* cls) {
    __try {
        uint8_t* objs = bf::g_exe + O_OBJS;
        int num = *(int*)objs;
        uint8_t** chunks = (uint8_t**)(*(uint64_t*)(objs + 0x18) ^ OBJS_XOR);
        uint8_t* chunk = nullptr;
        for (int i = 0; i < num; i++) {
            if (!(i & 0xffff)) chunk = chunks[i >> 16];
            uint8_t* item = chunk + (i & 0xffff) * 0x18;
            uint8_t* o = *(uint8_t**)item;
            if (!o || (*(uint32_t*)(item + 8) & 0x30000000)) continue;
            uint8_t* c = *(uint8_t**)(o + O_CLASS);
            if (c && fb::NameIs(o + O_NAME, name) && fb::NameIs(c + O_NAME, cls)) return o;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

void Identity(uint8_t* xf) {                // FTransform (doubles): rotation quat at +0, translation +32, scale +64
    memset(xf, 0, 96);
    ((double*)xf)[3] = 1;
    ((double*)xf)[8] = ((double*)xf)[9] = ((double*)xf)[10] = 1;
}

uint8_t* Spawn(uint8_t* gs) {
    if (!g_cls) g_cls = FindObject("PfgDebugColliderRenderer", "Class");
    if (!g_statics) g_statics = FindObject("Default__GameplayStatics", "GameplayStatics");
    if (!g_cls || !g_statics) { Log("lab: hitboxes: the game's collider renderer was not found"); return nullptr; }
    if (!g_fnBegin) g_fnBegin = fb::FindFunc(g_statics, "BeginDeferredActorSpawnFromClass");
    if (!g_fnFinish) g_fnFinish = fb::FindFunc(g_statics, "FinishSpawningActor");
    if (!g_fnBegin || !g_fnFinish || *(uint16_t*)(g_fnBegin + FN_PARMS) != 136 || *(uint16_t*)(g_fnFinish + FN_PARMS) != 120) {
        Log("lab: hitboxes: the spawn functions are not as expected");
        return nullptr;
    }
    alignas(16) uint8_t p[144] = {};
    *(void**)p = gs;                        // WorldContextObject
    *(void**)(p + 8) = g_cls;               // ActorClass
    Identity(p + 16);                       // SpawnTransform
    p[112] = 1;                             // CollisionHandlingOverride: AlwaysSpawn
    Pe(g_statics, g_fnBegin, p);
    uint8_t* actor = *(uint8_t**)(p + 128);
    if (!actor) { Log("lab: hitboxes: the collider renderer could not be spawned"); return nullptr; }
    alignas(16) uint8_t q[128] = {};
    *(void**)q = actor;
    Identity(q + 16);
    Pe(g_statics, g_fnFinish, q);
    *(uint8_t**)(gs + GS_RENDERER) = actor;
    Log("lab: hitboxes: collider renderer spawned");
    return actor;
}

bool Alive(uint8_t* o) { return o && !(*(uint32_t*)(o + O_FLAGS) & RF_DEAD); }

void Show(uint8_t* r, bool on) {
    uint8_t* data = *(uint8_t**)(r + R_SHOW);
    int num = *(int*)(r + R_SHOW + 8);
    if (!data || num < 0 || num > 32) return;
    for (int i = 0; i < num; i++) {
        uint8_t* e = data + 12 * i;         // TSetElement<TPair<uint8, bool>>: key, value, hash next, hash index
        if (e[0] < 11) e[1] = on && (SHOWN >> e[0] & 1);
    }
}

void Status() {
    uint32_t s = 0;
    if (g_renderer && g_shown) s |= bf::LABS_HITBOXES;
    if (g_fnPause && g_fnResume) s |= bf::LABS_STEP_READY;
    if (g_paused) s |= bf::LABS_PAUSED;
    bf::Ctl()->labStatus = s;
}

void Pause(bool on) {
    uint8_t parms[16] = {};
    Pe(g_gs, on ? g_fnPause : g_fnResume, parms);
    g_paused = on;
}

void RequestToggle() { InterlockedExchange(&g_reqToggle, 1); g_wake = true; }
void RequestStep() { InterlockedExchange(&g_reqStep, 1); g_wake = true; }

// The pause and step keys (game thread, game state known). Stepping ends in OnGsTick, after the sim has run a step.
void Requests() {
    g_wake = false;
    if (InterlockedExchange(&g_reqToggle, 0)) { Pause(!g_paused); g_stepping = false; Log("lab: sim %s", g_paused ? "paused" : "resumed"); }
    if (InterlockedExchange(&g_reqStep, 0) && !g_stepping) {
        if (!g_paused) { Pause(true); Log("lab: sim paused"); }
        else { Pause(false); g_paused = true; g_stepping = true; g_stepFrom = g_lastTick; }
    }
}

// From ProcessEvent while a request waits: on the game thread only, and only with frame step possible here.
void Poll() {
    if (g_inPoll || GetCurrentThreadId() != g_thread) return;
    g_inPoll = true;
    __try {
        if (g_gs && g_fnPause && g_fnResume && (bf::Ctl()->lab & bf::LAB_FRAMESTEP)) { Requests(); Status(); }
        else g_wake = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_wake = false; }
    g_inPoll = false;
}

// After every game-state Tick (game thread). simTick: sim steps run so far (fighters.cpp).
void OnGsTick(void* gsv, uint32_t simTick) {
    uint8_t* gs = (uint8_t*)gsv;
    uint8_t lab = bf::Ctl()->lab;
    g_thread = GetCurrentThreadId();
    g_lastTick = simTick;
    __try {
        if (gs != g_gs) {                   // another match
            g_gs = gs;
            g_renderer = nullptr;
            g_spawnTried = g_shown = g_paused = g_stepping = false;
            g_fnPause = fb::FindFunc(gs, "PauseTick");
            g_fnResume = fb::FindFunc(gs, "ResumeTick");
            InterlockedExchange(&g_reqToggle, 0);
            InterlockedExchange(&g_reqStep, 0);
        }
        // hitboxes
        bool want = (lab & bf::LAB_HITBOXES) != 0;
        if (g_renderer && !Alive(g_renderer)) g_renderer = nullptr;
        if (want && !g_renderer && !g_spawnTried) {
            g_spawnTried = true;
            uint8_t* have = *(uint8_t**)(gs + GS_RENDERER);
            g_renderer = Alive(have) ? have : Spawn(gs);
        }
        if (g_renderer && want != g_shown) { Show(g_renderer, want); g_shown = want; Log("lab: hitboxes %s", want ? "shown" : "hidden"); }
        // frame step
        // without the ProcessEvent hook a paused sim could not be resumed (boot.cpp), so no pausing at all then
        bool step = (lab & bf::LAB_FRAMESTEP) && g_fnPause && g_fnResume && bf::Ctl()->peHooked;
        if (!step) {
            if (g_paused) { Pause(false); Log("lab: frame step off, sim resumed"); }
            g_stepping = false;
            InterlockedExchange(&g_reqToggle, 0);
            InterlockedExchange(&g_reqStep, 0);
        } else {
            if (g_stepping && simTick != g_stepFrom) { Pause(true); g_stepping = false; }
            if (!g_inPoll) { g_inPoll = true; Requests(); g_inPoll = false; }
        }
        Status();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_renderer = nullptr;
        g_spawnTried = true;
        Log("lab: a call failed; lab tools left alone in this match");
    }
}

}  // namespace lab
