// BetaFeel.asi, positions half (was FighterSmooth; see main.cpp).
//
// The game's fixed-point sim (PfgFixed2DGame) steps at 60 Hz while the engine draws at up to the frame-rate cap, and
// nothing in between interpolates, so fighters, projectiles and the camera that follows them move in 60 Hz steps.
// After every frame's game-state Tick this moves each sim entity's scene component to the point between its last two
// sim positions that matches the leftover time of the fixed-step loop. Only what is drawn changes: before every sim
// step each component is put back on exactly its sim position (ParkAll), so the sim never reads a blended value. The
// picture runs about 8 ms (half a sim step) behind the sim on average. The camera, which the sim moves with
// SetActorTransform once a step, is blended the same way (location and rotation).
// Switches (control block, every frame): fighters, camera. Offsets are for build 17784632; the code bytes at every
// address are checked first and nothing is hooked if one differs.
#include "control.h"
#include <string.h>
#include <math.h>
#include <unordered_map>

namespace fs {
using bf::Log;

// ---- addresses (exe offsets) and the bytes expected there ----
const uintptr_t O_GS_TICK = 0x1268430;    // APfgFixedGameStateBase::Tick(float): accumulator + fixed-step loop
const uintptr_t O_FIXED_TICK = 0x1264c60; // one fixed sim step (called by the loop above)
const uintptr_t O_PUSH = 0x11d9e40;       // per entity, end of a step: fixed transform -> its scene component
const uintptr_t O_SET_REL = 0x4c22230;    // USceneComponent::SetRelativeLocationAndRotation(FVector&, FRotator&, bool, FHitResult*, ETeleportType)
const uintptr_t O_ACTOR_SET = 0x4584b80;  // AActor::SetActorTransform(FTransform&, bool, FHitResult*, ETeleportType): the sim's camera step
const uintptr_t VT_CAMERAS[] = {0x6419fb8, 0x5f63328};   // AMvsCamera, APfgCameraActor
const uint8_t P_GS[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x81, 0xa0, 0x04, 0x00, 0x00, 0x48, 0x8b, 0xd9};
const uint8_t P_FIXED[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
                           0x81, 0xa0, 0x04, 0x00, 0x00};
const uint8_t P_PUSH[] = {0x40, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x48, 0x83, 0x79, 0x60, 0x00};
const uint8_t P_ACTOR_SET[] = {0x40, 0x53, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0x99, 0xa8, 0x01, 0x00, 0x00};
const uint8_t P_SET_REL[] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x10, 0x49, 0x89, 0x73, 0x18, 0x57, 0x48, 0x81, 0xec,
                             0xb0, 0x00, 0x00, 0x00};

// ---- layouts ----
const size_t OBJ_FLAGS = 0x08;   // UObjectBase::ObjectFlags
const uint32_t RF_DEAD = 0x40000000 | 0x8000 | 0x10000; // MirroredGarbage | BeginDestroyed | FinishDestroyed
const size_t REL_LOC = 0x138;    // USceneComponent::RelativeLocation (3 doubles)
const size_t REL_ROT = 0x150;    // USceneComponent::RelativeRotation (3 doubles)
const size_t GS_ACC = 0x494;     // game state: float leftover of the fixed-step loop, in [-1/120, 1/120) after it
const size_t REC_COMP = 0x60;    // push record: the scene component it moves
const size_t REC_DIRTY = 0x248;  // push record: dirty flag (the push returns at once when clear and not forced)
const size_t ACTOR_ROOT = 0x1a8; // AActor::RootComponent
#ifdef BETAFEEL_DEV
// Measuring aid ([Dev] CameraLog=1): does the camera zoom by its field of view (not blended here) or by moving (blended)?
const size_t CAM_COMP = 0x2a8;   // ACameraActor::CameraComponent
const size_t CAM_FOV = 0x2b0;    // UCameraComponent::FieldOfView (float)
bool g_camLog;
float g_camFov = -1;
uint32_t g_camFovChanges, g_camMoves, g_camSteps, g_camLogTick;
double g_camLast[3];
extern uint32_t g_tick;
bool Same(const double* a, const double* b);
void CameraWatch(void* actor, uint8_t* root) {
    if (!g_camLog) return;
    __try {
        uint8_t* comp = *(uint8_t**)((uint8_t*)actor + CAM_COMP);
        float fov = comp ? *(float*)(comp + CAM_FOV) : -1;
        double* p = (double*)(root + REL_LOC);
        g_camSteps++;
        if (g_camFov >= 0 && fov != g_camFov) {
            if (g_camFovChanges++ < 20) Log("camera: field of view %.3f -> %.3f at step %u", g_camFov, fov, g_tick + 1);
        }
        if (!Same(p, g_camLast)) g_camMoves++;
        g_camFov = fov; memcpy(g_camLast, p, sizeof g_camLast);
        if (g_tick - g_camLogTick >= 300) {
            Log("camera, 5 s: %u steps, moved on %u, field of view changed on %u (now %.3f), at %.0f %.0f %.0f",
                g_camSteps, g_camMoves, g_camFovChanges, fov, p[0], p[1], p[2]);
            g_camSteps = g_camMoves = g_camFovChanges = 0; g_camLogTick = g_tick;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#endif

typedef uintptr_t (*Fn4)(void*, uintptr_t, uintptr_t, uintptr_t);
typedef void (*GsTickFn)(void*, float);
typedef uintptr_t (*ActorSetFn)(void*, void*, uintptr_t, void*, uintptr_t);
typedef void (*SetRelFn)(void*, const double*, const double*, uint8_t, void*, uint8_t);

GsTickFn g_origGs;
Fn4 g_origFixed, g_origPush;
SetRelFn g_setRel;
ActorSetFn g_origActorSet;

double g_snap = 400.0;   // cm moved in one sim step: a teleport, shown at once

struct Ent {
    uintptr_t vt;
    double prev[3], cur[3], last[3];   // last: what the component holds now (set by the sim or by this plugin)
    bool rot;                          // the rotation is blended too (the camera); fighters keep the sim's
    double rprev[3], rcur[3];
    uint32_t tick;                     // step of the latest push
    bool still, parked, dead;
};
std::unordered_map<uint8_t*, Ent> g_ents;
uint32_t g_tick;
DWORD g_gameThread;
void* g_gs;
bool g_inFixed;
bool g_wasOn;            // smoothing ran last frame (to put everything back once when it is switched off)

bool Same(const double* a, const double* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

// The component is still the object the sim pushed, nobody else moved it, and it is not being destroyed.
bool Valid(uint8_t* c, const Ent& e) {
    __try {
        if (*(uintptr_t*)c != e.vt) return false;
        if (*(uint32_t*)(c + OBJ_FLAGS) & RF_DEAD) return false;
        return Same((double*)(c + REL_LOC), e.last);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetRel(uint8_t* c, const double* p, const double* r) {
    double loc[3] = {p[0], p[1], p[2]}, rot[3];
    __try {
        if (r) memcpy(rot, r, sizeof rot);
        else memcpy(rot, c + REL_ROT, sizeof rot);
        g_setRel(c, loc, rot, 0, nullptr, 0);   // no sweep, ETeleportType::None, as the sim's own push
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Put(uint8_t* c, Ent& e, const double* p, const double* r = nullptr) {
    if (e.dead) return;
    if (!Valid(c, e) || !CallSetRel(c, p, r)) { e.dead = true; return; }
    memcpy(e.last, c + REL_LOC, sizeof e.last);
}

void Park(uint8_t* c, Ent& e) {
    if (!e.parked && !e.dead) Put(c, e, e.cur, e.rot ? e.rcur : nullptr);
    e.parked = true;
}

void ParkAll() { for (auto& kv : g_ents) Park(kv.first, kv.second); }

// ---- hooks ----
void Record(uint8_t* comp, bool rot);
void HookGsTick(void* gs, float dt) {
    g_gameThread = GetCurrentThreadId();
    if (gs != g_gs) { g_ents.clear(); g_gs = gs; }   // another match
    g_origGs(gs, dt);
    lab::OnGsTick(gs, g_tick);
    bf::Control* ctl = bf::Ctl();
    ctl->matchTick = GetTickCount64();
    bool on = ctl->fighters, cam = ctl->camera;
    if (!on && !cam) {                               // switched off: everything back on its sim position, once
        if (g_wasOn) { ParkAll(); Log("positions: off"); }
        g_wasOn = false;
        return;
    }
    if (!g_wasOn) Log("positions: fighters %s, camera %s", on ? "on" : "off", cam ? "on" : "off");
    g_wasOn = true;
    float acc = *(float*)((uint8_t*)gs + GS_ACC);
    double al = acc * 60.0 + 0.5;
    al = al < 0 ? 0 : al > 1 ? 1 : al;
    for (auto& kv : g_ents) {
        Ent& e = kv.second;
        if (e.dead) continue;
        if (e.still || !(e.rot ? cam : on)) { Park(kv.first, e); continue; }
        double p[3], r[3];
        for (int i = 0; i < 3; i++) p[i] = e.prev[i] + (e.cur[i] - e.prev[i]) * al;
        if (e.rot) {
            for (int i = 0; i < 3; i++) {
                double d = fmod(e.rcur[i] - e.rprev[i], 360.0);   // the short way round
                if (d > 180) d -= 360; else if (d < -180) d += 360;
                r[i] = e.rprev[i] + d * al;
            }
        }
        e.parked = false;
        Put(kv.first, e, p, e.rot ? r : nullptr);
    }
}

uintptr_t HookFixed(void* gs, uintptr_t a, uintptr_t b, uintptr_t c) {
    bool mine = GetCurrentThreadId() == g_gameThread;
    // Every step starts from exactly what the sim left: the blended spots are only for drawing. Without this, code
    // inside the step that reads a component (the camera's own update; anything the sim does not push again because
    // it is not dirty, as in a rollback that re-runs several steps in one frame) saw a blended value. What is drawn
    // does not change: the frame's game-state Tick blends again after the steps.
    if (mine) ParkAll();
    if (mine) g_inFixed = true;
    uintptr_t r = g_origFixed(gs, a, b, c);
    g_inFixed = false;
    if (!mine) return r;
    g_tick++;
    bf::Ctl()->simSteps = g_tick;
    for (auto it = g_ents.begin(); it != g_ents.end();) {
        Ent& e = it->second;
        if (e.tick != g_tick && !e.still) {   // not pushed this step: it stands where the last step put it
            memcpy(e.prev, e.cur, sizeof e.prev);
            e.still = true;
            e.parked = false;
        }
        if (g_tick - e.tick > 600) it = g_ents.erase(it);   // gone for 10 s
        else ++it;
    }
    return r;
}

uintptr_t HookPush(void* rec, uintptr_t force, uintptr_t b, uintptr_t c) {
    uint8_t* r = (uint8_t*)rec;
    bool go = (force & 0xff) || r[REC_DIRTY];
    uintptr_t ret = g_origPush(rec, force, b, c);
    if (!go || GetCurrentThreadId() != g_gameThread) return ret;
    uint8_t* comp = *(uint8_t**)(r + REC_COMP);
    if (comp) Record(comp, false);
    return ret;
}

// After the sim set a component this step: its new position becomes cur, the one before prev. Tracked whether the
// switches are on or not, so switching on blends from the next frame.
void Record(uint8_t* comp, bool rot) {
    double p[3], q[3];
    memcpy(p, comp + REL_LOC, sizeof p);
    memcpy(q, comp + REL_ROT, sizeof q);
    uint32_t t = g_tick + 1;   // g_tick is bumped when the step returns, after its pushes
    auto it = g_ents.find(comp);
    if (it == g_ents.end()) {
        Ent e = {};
        e.vt = *(uintptr_t*)comp;
        memcpy(e.prev, p, sizeof p); memcpy(e.cur, p, sizeof p); memcpy(e.last, p, sizeof p);
        e.rot = rot;
        memcpy(e.rprev, q, sizeof q); memcpy(e.rcur, q, sizeof q);
        e.tick = t; e.still = true; e.parked = true;
        g_ents.emplace(comp, e);
        return;
    }
    Ent& e = it->second;
    if (e.tick != t) {
        double d = sqrt((p[0] - e.cur[0]) * (p[0] - e.cur[0]) + (p[1] - e.cur[1]) * (p[1] - e.cur[1]) +
                        (p[2] - e.cur[2]) * (p[2] - e.cur[2]));
        memcpy(e.prev, d > g_snap ? p : e.cur, sizeof e.prev);
        memcpy(e.rprev, d > g_snap ? q : e.rcur, sizeof e.rprev);
    }
    memcpy(e.cur, p, sizeof p);
    memcpy(e.rcur, q, sizeof q);
    memcpy(e.last, p, sizeof p);
    e.vt = *(uintptr_t*)comp;
    e.tick = t;
    e.still = Same(e.prev, e.cur) && (!e.rot || Same(e.rprev, e.rcur));
    e.parked = true;   // the component holds the sim's position right now
    e.dead = false;
}

// The camera is not a pushed entity: its fixed update calls SetActorTransform once a step (its root has no parent).
uintptr_t HookActorSet(void* actor, void* xf, uintptr_t sweep, void* hit, uintptr_t tele) {
    uintptr_t ret = g_origActorSet(actor, xf, sweep, hit, tele);
    if (!g_inFixed || GetCurrentThreadId() != g_gameThread) return ret;
    __try {
        uintptr_t vt = *(uintptr_t*)actor - (uintptr_t)bf::g_exe;
        bool cam = false;
        for (uintptr_t v : VT_CAMERAS) cam |= vt == v;
        uint8_t* root = *(uint8_t**)((uint8_t*)actor + ACTOR_ROOT);
        if (cam && root) Record(root, true);
#ifdef BETAFEEL_DEV
        if (cam && root) CameraWatch(actor, root);
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// ---- 14-byte absolute-jump hooks ----
void* Hook(uint8_t* target, const uint8_t* expect, size_t len, void* detour) {
    // trampoline: the overwritten instructions (position-independent, checked) + jmp back
    uint8_t* tr = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return nullptr;
    memcpy(tr, expect, len);
    uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
    uintptr_t back = (uintptr_t)(target + len);
    memcpy(jmp + 6, &back, 8);
    memcpy(tr + len, jmp, 14);
    DWORD old;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    uintptr_t to = (uintptr_t)detour;
    memcpy(jmp + 6, &to, 8);
    memcpy(target, jmp, 14);
    for (size_t i = 14; i < len; i++) target[i] = 0x90;
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return tr;
}

void Start() {
    g_snap = bf::IniInt(L"FighterSnap", 400);
#ifdef BETAFEEL_DEV
    g_camLog = GetPrivateProfileIntW(L"Dev", L"CameraLog", 0, bf::g_ini) != 0;
#endif
    uint8_t* exe = bf::g_exe;
    struct { uintptr_t off; const uint8_t* p; size_t n; } sites[] = {
        {O_GS_TICK, P_GS, sizeof P_GS}, {O_FIXED_TICK, P_FIXED, sizeof P_FIXED},
        {O_PUSH, P_PUSH, sizeof P_PUSH}, {O_SET_REL, P_SET_REL, sizeof P_SET_REL},
        {O_ACTOR_SET, P_ACTOR_SET, sizeof P_ACTOR_SET}};
    for (auto& s : sites) {
        bool ok = false;
        __try { ok = memcmp(exe + s.off, s.p, s.n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ok) {
            Log("positions: exe+0x%llx differs from build 17784632 (or another plugin hooked it), nothing hooked", (unsigned long long)s.off);
            bf::Ctl()->fighterHooks = bf::HOOK_BADBUILD;
            return;
        }
    }
    g_setRel = (SetRelFn)(exe + O_SET_REL);
    g_origGs = (GsTickFn)Hook(exe + O_GS_TICK, P_GS, sizeof P_GS, (void*)HookGsTick);
    g_origFixed = (Fn4)Hook(exe + O_FIXED_TICK, P_FIXED, sizeof P_FIXED, (void*)HookFixed);
    g_origPush = (Fn4)Hook(exe + O_PUSH, P_PUSH, sizeof P_PUSH, (void*)HookPush);
    g_origActorSet = (ActorSetFn)Hook(exe + O_ACTOR_SET, P_ACTOR_SET, sizeof P_ACTOR_SET, (void*)HookActorSet);
    if (!g_origGs || !g_origFixed || !g_origPush || !g_origActorSet) { Log("positions: hooking failed"); return; }
    bf::Ctl()->fighterHooks = bf::HOOK_ON;
    Log("positions: hooks in, snap %.0f cm", g_snap);
}

}  // namespace fs
