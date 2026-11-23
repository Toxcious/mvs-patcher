// BetaFeel.asi, poses half (was PoseSmooth; see main.cpp).
//
// The game's sim steps 60 times a second and a fighter's pose changes only on those steps, while the engine hands the
// pose to the renderer on every drawn frame (measured with poseprobe/: 144 hand-offs, at most 60 pose changes a second).
// This sits at that hand-off (USkeletalMeshComponent::FinalizeBoneTransform). For every mesh driven by the game's
// fixed-step anim instance it keeps the last two sim poses and writes their blend into the bone buffer the renderer
// reads, timed by the leftover of the fixed-step loop exactly as the positions half times positions. The sim never
// reads these bones (it uses baked fixed-point data), so only the picture changes. Effects are advanced the same way.
//
// No code bytes are patched: vtable slots of the game's skeletal mesh, Niagara and Cascade classes are pointed at
// wrappers. Switches (control block, every frame): poses, effects, others (summons and props). The developer build
// (BETAFEEL_DEV) adds the measuring aids: the input recorder ([Dev] RecDir in BetaFeel.ini), the position log (F4),
// the stretch test (TestKey) and a counter line every 5 s (Stats). Offsets are for build 17784632; every slot is
// checked first and nothing is changed if one differs.
#include "control.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <share.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <intrin.h>
#include <ctype.h>

namespace ps {
using bf::Log;
// ---- addresses (exe offsets) and layouts ----
const uintptr_t O_FINALIZE = 0x4c30080;                       // USkeletalMeshComponent::FinalizeBoneTransform
const uintptr_t SLOTS[] = {0x5f87490, 0x5f88308, 0x5faa270};  // its slot in the game's three skeletal mesh vtables
// The engine's own USkeletalMeshComponent (vtable exe+0x704f710): each fighter carries one, so do stage props.
// Hooked too (ini EngineMeshes=1); outside a match nothing is done with them (no fighter game state seen).
const uintptr_t ENGINE_SLOT = 0x7050130;
const uintptr_t VT_PFG_ANIM = 0x5f4fa88;                      // PfgAnimInstance vtable
const uintptr_t O_FRAME_NUMBER = 0x7e74d9c;                   // GFrameNumber, +1 per drawn frame
const size_t CST = 0x5d0;         // USkinnedMeshComponent::ComponentSpaceTransformsArray[2] (TArray, 16 bytes each)
const size_t CST_READ = 0x618;    // CurrentReadComponentTransforms
const size_t ANIM_INST = 0x8c0;   // USkeletalMeshComponent::AnimScriptInstance
const size_t ANIM_GS = 0x4c0;     // PfgAnimInstance::TrackedGameState
const size_t GS_ACC = 0x494;      // game state: float leftover of the fixed-step loop, in [-1/120, 1/120) after it
// Effects (Niagara): the game runs them in "desired age" mode and moves that age on sim steps, so they advance
// 60 times a second too. UNiagaraComponent::TickComponent runs every drawn frame and advances the effect to the
// desired age; the wrapper hands it an age between the last two the game set.
const uintptr_t O_FX_TICK = 0x1a77380;                        // UNiagaraComponent::TickComponent
const uintptr_t FX_SLOTS[] = {0x61b8370, 0x61b9240};          // its slot in the two Niagara component vtables
const size_t FX_MODE = 0x648;     // AgeUpdateMode: 0 = frame time, 1 = desired age, 2 = desired age without seeking
const size_t FX_AGE = 0x64c;      // DesiredAge (float, seconds)
const size_t FX_LOCK = 0x65c;     // bLockDesiredAgeDeltaTimeToSeekDelta: advance only in whole seek steps
// Cascade (the older particle system; the game's fight effects). Measured: stage effects are ticked by the engine on
// every drawn frame, but the effects of moves and hits are ticked by the game itself, once per sim step with 1/60 s,
// from exe+0x263fac0. Those calls are held back here and the same time is handed to the component in slices, one per
// drawn frame, following the leftover of the fixed-step loop like the poses do. Whatever is not handed out when the
// game's next call comes is given first, so an effect never gets less or more time than the game gave it.
const uintptr_t O_PS_GAME_RET = 0x263fac0;                    // return address of the game's per-step tick call
const uintptr_t VT_PS = 0x6dee240;                            // UParticleSystemComponent vtable
const size_t OBJ_FLAGS = 0x8;                                 // UObject::ObjectFlags
const uint32_t RF_DEAD = 0x8000 | 0x10000 | 0x20000000 | 0x40000000;   // BeginDestroyed, FinishDestroyed, (mirrored) garbage
const uintptr_t O_PS_TICK = 0x4d67c10;                        // UParticleSystemComponent::TickComponent
const uintptr_t PS_SLOT = 0x6dee5e8;                          // its slot (index 117) in that class's vtable 0x6dee240
const int XF = 96;                // FTransform: rotation quat (4 doubles), translation (+0x20), scale (+0x40)

typedef void (*FinFn)(void*);
FinFn g_orig;
uint8_t*& g_exe = bf::g_exe;
CRITICAL_SECTION g_cs;

// ---- settings (the switches are read from the control block every frame) ----
double g_snap = 120.0;     // a bone that moves further than this (cm, in the mesh's own space) in one step: no blend
double g_snapDot = 0.7;    // the root turning further than this in one step (|quat dot| below it): no blend
double g_turnDot = 0.866;  // a bone counts as turned when it turns further than this in one step (0.866 = 60 degrees)
int g_turnShare = 25;      // this share of the bones (percent) turned in one step: a cut to another pose, no blend
bool g_liveBones = true;   // bones that change between two sim steps are the engine's own: see Live
bool g_engineMeshes = true; // also hook the engine's own skeletal mesh class (see ENGINE_SLOT)
bool PosesOn() { return bf::Ctl()->poses != 0; }
bool FxOn() { return bf::Ctl()->effects != 0; }
bool OthersOn() { return bf::Ctl()->others != 0; }
#ifdef BETAFEEL_DEV
bool g_stats = false;
int g_testKey = 0;         // stretches every smoothed skeleton by a fifth while on: proof that what is written is drawn
bool g_test = false;
bool g_pos = false;        // F4: position log, see PosLog
bool g_snapLog = false;    // [Dev] SnapLog=1: one line per pose shown at once for reasons 4-6 (up to 400 a session)
int g_snapLines = 0, g_liveLines = 0, g_pmLines = 0;
#else
const bool g_stats = false, g_test = false, g_pos = false;
#endif

struct Comp {
    std::vector<double> prev, cur;   // the last two sim poses
    uint64_t curHash = 0, wroteHash = 0;
    int bones = 0;
    float acc = 0;
    bool haveAcc = false, snap = true;
    uint32_t lastFrame = 0;
    uint32_t steps = 0;              // the sim step count when this mesh was last looked at
    int span = 1;                    // sim steps between prev and cur (more than 1 below 60 drawn frames a second)
    std::vector<double> eng;         // the engine's latest pose, where it changed bones between two steps (see AddLive)
    bool anyLive = false;            // eng differs from cur in some bones
    bool free = false;               // the engine itself animates this mesh on every drawn frame (see Hooked)
    uint32_t freeAt = 0;             // the sim step count when that was last seen
#ifdef BETAFEEL_DEV
    std::vector<double> devOut;      // what was written last frame, for the pose movement log
#endif
};
std::unordered_map<void*, Comp> g_comps;
uint32_t g_keyFrame;
bool g_keyDown;
// statistics for the log
uint32_t s_withTick, s_withoutTick, s_snaps, s_blends, s_foreign, s_noGs;
// why a pose was shown at once: 1 mesh not seen for a while, 2 smoothing just switched on, 3 pose changed without a
// sim step, 4 root turned, 5 a bone moved too far, 6 many bones turned (a cut to another pose)
uint32_t s_why[7];
uint32_t s_livePoses, s_liveMost, s_boneSnaps;   // poses taken over bone by bone between steps, most bones in one, far bones shown at once
LARGE_INTEGER g_freq, g_t0;

typedef void (*FxFn)(void*, float, int, void*);
FxFn g_fxOrig;
uint8_t* volatile g_gs;          // the match's game state, from the last fighter mesh seen
volatile uint32_t g_gsFrame;     // drawn frame it was seen on
struct Fx { float prev = 0, cur = 0; uint32_t lastFrame = 0; bool seen = false; };
std::unordered_map<void*, Fx> g_fxs;
uint32_t s_fxMode[3], s_fxBlend, s_fxSnap, s_fxStep60;
FxFn g_psOrig;
uint32_t s_psCalls, s_psStep60, s_psZero, s_psFrames, s_psLastFrame, s_psMaxPerFrame, s_psThisFrame;
double s_psDt;
struct Caller { uintptr_t ret; uint32_t n; } s_psCallers[8];
std::unordered_map<void*, uint32_t> s_psComps;
struct Ps { float dt = 0, given = 0; int type = 0; void* fn = nullptr; uint32_t stepFrame = 0, step = 0; };   // given: share of dt handed out
std::unordered_map<void*, Ps> g_pss;
DWORD g_psThread;
uint32_t g_psFrame, s_psHeld, s_psSlices, s_psFlush, s_psDropped;


uint64_t Hash(const double* p, int bones) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t* q = (const uint64_t*)p;
    for (int i = 0; i < bones * 12; i++) { h ^= q[i]; h *= 1099511628211ull; }
    return h;
}

bool SafeHash(const double* p, int bones, uint64_t& h) {
    __try { h = Hash(p, bones); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// What the hook needs from the component, read under SEH: its bone buffer and the fixed-step leftover.
struct View { double* data; int bones; float acc; bool other = false; };
// Names from the engine's name pool (exe+0x8101f10 is its Blocks[]; an entry is a 2-byte header, length << 6,
// bit 0 wide, then the text). UObject: class +0x18, name +0x20, outer +0x28 in this build.
const uintptr_t O_NAMES = 0x8101f10;
bool ObjName(uint8_t* obj, char* out, size_t cap) {
    __try {
        uint32_t i = *(uint32_t*)(obj + 0x20);
        uint8_t* blk = *(uint8_t**)(g_exe + O_NAMES + 8 * (i >> 16));
        uint8_t* e = blk + (i & 0xffff) * 2;
        uint16_t h = *(uint16_t*)e, len = h >> 6;
        if (h & 1 || len == 0 || len >= cap) return false;
        memcpy(out, e + 2, len); out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::unordered_map<std::string, uint32_t> g_others;   // anim class / owner class of meshes not driven by PfgAnimInstance
void OtherNames(uint8_t* c, uint8_t* anim, char* a, char* o) {
    __try {
        ObjName(*(uint8_t**)(anim + 0x18), a, 128);
        uint8_t* owner = *(uint8_t**)(c + 0x28);
        if (owner) ObjName(*(uint8_t**)(owner + 0x18), o, 128);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
std::unordered_map<void*, uint8_t> g_otherSeen;      // anim instances already named (names are read once each)
void NoteOther(uint8_t* c, uint8_t* anim) {
    if (g_otherSeen.size() > 4096) g_otherSeen.clear();
    if (g_otherSeen[anim]++) return;
    char a[128] = "?", o[128] = "?";
    OtherNames(c, anim, a, o);
    std::string k = std::string(a) + " on " + o;
    if (g_others[k]++ == 0) Log("other mesh: anim class %s, owner %s (%s)", a, o, OthersOn() ? "smoothed with the fighters' step" : "left as is");
}
bool Look(uint8_t* c, View& v) {
    __try {
        uint8_t* anim = *(uint8_t**)(c + ANIM_INST);
        if (!anim) return false;
        uint32_t frame = *(uint32_t*)(g_exe + O_FRAME_NUMBER);
        if (*(uintptr_t*)anim - (uintptr_t)g_exe != VT_PFG_ANIM) {
            // Not a fighter's anim instance (summons such as Aquaman's dolphin, props): no game state of its own to
            // read, so it follows the match's fixed step as seen on the fighters' meshes, if one was seen just now.
            s_foreign++;
            if (!g_gs || frame - g_gsFrame >= 8) return false;
            NoteOther(c, anim);
            if (!OthersOn()) return false;
            v.acc = *(float*)(g_gs + GS_ACC);
            v.other = true;
        } else {
            uint8_t* gs = *(uint8_t**)(anim + ANIM_GS);
            if (gs) {
                v.acc = *(float*)(gs + GS_ACC);
                g_gs = gs; g_gsFrame = frame;
            } else {
                // A fighter-class anim instance with no game state of its own: meshes spawned for a move (Aquaman's
                // dolphin is a PfgTrackedSkeletalMeshComponent). Same as the other meshes: the match's step.
                s_noGs++;
                if (!OthersOn() || !g_gs || frame - g_gsFrame >= 8) return false;
                v.acc = *(float*)(g_gs + GS_ACC);
                v.other = true;
            }
        }
        if (!(v.acc > -0.05f && v.acc < 0.05f)) return false;
        int32_t idx = *(int32_t*)(c + CST_READ);
        if (idx < 0 || idx > 1) return false;
        v.data = *(double**)(c + CST + 16 * idx);
        v.bones = *(int32_t*)(c + CST + 16 * idx + 8);
        return v.data && v.bones > 0 && v.bones <= 1024;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Too far apart to be one motion: a turn-around of the root, or a bone that jumps. Returns the reason (s_why), 0 = none.
struct JumpInfo { double rootDot = 1, maxDist = 0; int turned = 0, nFar = 0, farBone = -1; };
int Jump(const double* a, const double* b, int bones, JumpInfo& ji) {
    ji.rootDot = fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    double lim = g_snap * g_snap, far2 = 0;
    int nFar = 0;     // bones that move further than PoseSnap in the step: a few are shown at once on their own (a
                      // prop or weapon bone put somewhere else), many is a new pose
    int turned = 0;   // bones that turn more than PoseTurnDegrees in the step: many of them is a new pose (a mirrored
                      // animation on a turn-around, a cut to another move), not motion
    int odd = 0;      // far or turned
    for (int i = 0; i < bones; i++) {
        const double* p = a + i * 12;
        const double* q = b + i * 12;
        double d = (p[4] - q[4]) * (p[4] - q[4]) + (p[5] - q[5]) * (p[5] - q[5]) + (p[6] - q[6]) * (p[6] - q[6]);
        if (d > far2) { far2 = d; ji.farBone = i; }
        bool f = d > lim, t = fabs(p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3]) < g_turnDot;
        if (f) nFar++;
        if (t) turned++;
        if (f || t) odd++;
    }
    ji.maxDist = sqrt(far2); ji.turned = turned; ji.nFar = nFar;
    if (ji.rootDot < g_snapDot) return 4;
    if (odd * 100 > bones * g_turnShare) return nFar > turned ? 5 : 6;
    return 0;
}

// The few bones that moved further than PoseSnap in the step are shown at once; the rest of the pose still blends.
void SnapFar(double* prev, const double* cur, int bones) {
    double lim = g_snap * g_snap;
    for (int i = 0; i < bones; i++) {
        double* p = prev + i * 12;
        const double* q = cur + i * 12;
        double d = (p[4] - q[4]) * (p[4] - q[4]) + (p[5] - q[5]) * (p[5] - q[5]) + (p[6] - q[6]) * (p[6] - q[6]);
        if (d > lim) memcpy(p, q, XF);
    }
}

// ---- bones the engine moves by itself ----
// The sim poses a fighter once per step, but some fighters carry a few bones that the engine moves on every drawn
// frame with the frame's own time (the Joker has three). Their pose therefore changes between two sim steps too, in
// those bones only, and that used to count as "a pose the sim did not make": the whole body was shown at once on every
// such frame, which left these fighters with next to no pose smoothing. Now a change between two steps in a minority
// of the bones is kept apart (Comp::eng, the engine's latest pose): the body keeps its blend, and what the engine did
// to a bone since the step's pose is added on top of the blend in full (AddLive). At the next step the blend starts
// from the engine's latest pose, so such a bone moves on from where it was drawn. Only when most of the pose changes
// between two steps is it shown at once.
const double LIVE_EPS = 1e-6;      // a bone counts as changed past this, in any of its numbers
bool BoneDiffers(const double* a, const double* b) {
    for (int k = 0; k < 11; k++) if (k != 7 && fabs(a[k] - b[k]) > LIVE_EPS) return true;
    return false;
}
// How many bones of the engine's new pose differ from the kept one (-1: could not read); exact = bit for bit.
int CountDiff(const double* now, const double* cur, int bones, int& exact) {
    __try {
        int n = 0; exact = 0;
        for (int i = 0; i < bones; i++) {
            if (BoneDiffers(now + i * 12, cur + i * 12)) n++;
            if (memcmp(now + i * 12, cur + i * 12, XF)) exact++;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void QMul(double* r, const double* a, const double* b) {   // x y z w; applies b first, then a
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// out holds the blend of the last two sim poses. Where the engine has moved a bone since the newer of them (eng against
// cur), that movement is put on top: the turn in the bone's own frame, the shift as it is, the engine's scale.
bool AddLive(double* out, const double* cur, const double* eng, int bones) {
    __try {
        for (int i = 0; i < bones; i++) {
            const double* c = cur + i * 12;
            const double* e = eng + i * 12;
            if (!BoneDiffers(e, c)) continue;
            double* o = out + i * 12;
            double inv[4] = {-c[0], -c[1], -c[2], c[3]}, d[4], r[4];
            QMul(d, inv, e);
            QMul(r, o, d);
            double len = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
            len = len > 1e-12 ? 1 / sqrt(len) : 0;
            for (int k = 0; k < 4; k++) o[k] = len ? r[k] * len : e[k];
            for (int k = 4; k < 7; k++) o[k] += e[k] - c[k];
            for (int k = 8; k < 11; k++) o[k] = e[k];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Blend(double* out, const double* a, const double* b, int bones, double t) {
    __try {
        for (int i = 0; i < bones; i++) {
            const double* p = a + i * 12;
            const double* q = b + i * 12;
            double* o = out + i * 12;
            double dot = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
            double s = dot < 0 ? -t : t, r[4], len = 0;
            for (int k = 0; k < 4; k++) { r[k] = p[k] * (1 - t) + q[k] * s; len += r[k] * r[k]; }
            len = len > 1e-12 ? 1 / sqrt(len) : 0;
            for (int k = 0; k < 4; k++) o[k] = len ? r[k] * len : q[k];
            for (int k = 4; k < 7; k++) o[k] = p[k] + (q[k] - p[k]) * t;      // translation
            for (int k = 8; k < 11; k++) o[k] = p[k] + (q[k] - p[k]) * t;     // scale
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Copy(double* dst, const double* src, int bones) {
    __try { memcpy(dst, src, (size_t)bones * XF); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Stretch(double* d, int bones) {
    __try { for (int i = 0; i < bones; i++) for (int k = 4; k < 7; k++) d[i * 12 + k] *= 1.2; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Once per drawn frame (inside the lock): a switch change, the developer keys, the counters, and forgetting meshes and
// effects not seen for a while (also in the players' build: the maps would otherwise only grow).
bool g_wasOn = true;
void Housekeeping(uint32_t frame) {
    if (frame == g_keyFrame) return;
    g_keyFrame = frame;
    bool on = PosesOn();
    if (on != g_wasOn) {
        for (auto& kv : g_comps) kv.second.snap = true;
        Log("poses: %s", on ? "on" : "off");
        g_wasOn = on;
    }
#ifdef BETAFEEL_DEV
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    static bool testDown;
    bool td = pid == GetCurrentProcessId() && g_testKey && (GetAsyncKeyState(g_testKey) & 0x8000);
    if (td && !testDown) { g_test = !g_test; Log("test stretch %s", g_test ? "on" : "off"); }
    testDown = td;
    static bool posDown;
    bool pd = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F4) & 0x8000);
    if (pd && !posDown) { g_pos = !g_pos; MessageBeep(g_pos ? MB_ICONASTERISK : MB_ICONHAND); Log("position log %s", g_pos ? "on" : "off"); }
    posDown = pd;
#endif
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (double(now.QuadPart - g_t0.QuadPart) / g_freq.QuadPart < 5.0) return;
    g_t0 = now;
    if (g_stats) {
            Log("5 s: %zu meshes; pose changes with a sim step %u, without one %u; blended frames %u, snaps %u, other anim classes %u, fighter anim class without game state %u",
                g_comps.size(), s_withTick, s_withoutTick, s_blends, s_snaps, s_foreign, s_noGs);
            Log("     snaps by reason: not seen %u, switched on %u, no sim step %u, root turned %u, bone far %u, many bones turned %u",
                s_why[1], s_why[2], s_why[3], s_why[4], s_why[5], s_why[6]);
            memset(s_why, 0, sizeof s_why);
            Log("     between steps: %u poses with only the engine's own bones changed (most in one: %u bones); %u poses with single far bones shown at once",
                s_livePoses, s_liveMost, s_boneSnaps);
            s_livePoses = s_liveMost = s_boneSnaps = 0;
            Log("     effects: ticks by age mode %u / %u / %u (frame time / desired age / no seek), with a 1/60 s step %u; smoothed %u, shown at once %u; %zu tracked",
                s_fxMode[0], s_fxMode[1], s_fxMode[2], s_fxStep60, s_fxBlend, s_fxSnap, g_fxs.size());
            Log("     cascade: %u ticks of %zu components on %u drawn frames (most on one frame %u); step 1/60 s on %u, zero on %u, average step %.5f s",
                s_psCalls, s_psComps.size(), s_psFrames, s_psMaxPerFrame, s_psStep60, s_psZero, s_psCalls ? s_psDt / s_psCalls : 0.0);
            for (auto& cl : s_psCallers) if (cl.n) Log("       called from exe+0x%llx: %u", (unsigned long long)(cl.ret - (uintptr_t)g_exe), cl.n);
            Log("       game-ticked effects: %u steps held back, %u slices handed out, %u remainders at the next step, %u dropped; %zu tracked",
                s_psHeld, s_psSlices, s_psFlush, s_psDropped, g_pss.size());
            s_psHeld = s_psSlices = s_psFlush = s_psDropped = 0;
            s_psCalls = s_psStep60 = s_psZero = s_psFrames = s_psMaxPerFrame = s_psThisFrame = 0; s_psDt = 0;
            memset(s_psCallers, 0, sizeof s_psCallers); s_psComps.clear();
            s_fxMode[0] = s_fxMode[1] = s_fxMode[2] = s_fxBlend = s_fxSnap = s_fxStep60 = 0;
            s_withTick = s_withoutTick = s_blends = s_snaps = s_foreign = s_noGs = 0;
    }
    for (auto it = g_fxs.begin(); it != g_fxs.end();) it = it->second.lastFrame + 3000 < frame ? g_fxs.erase(it) : ++it;
    for (auto it = g_comps.begin(); it != g_comps.end();) it = it->second.lastFrame + 3000 < frame ? g_comps.erase(it) : ++it;
}

#ifdef BETAFEEL_DEV
// ---- recorder: scripted controller input and a per-frame position record, for measuring moves without a person ----
// Off unless PoseSmooth.ini names a folder (RecDir). While idle the plugin looks there for job.txt about twice a
// second. A job is lines of text ("clock ms" on a line of its own makes the steps milliseconds, for menus: no record):
//     name <word>            the record is written to <RecDir>\<word>.rec.csv
//     steps <n>              how many sim steps to record
//     <step> <CONTROL> <v>   from that sim step on, the scripted pad holds CONTROL at v
// CONTROL is a key (K<virtual-key code>, e.g. K68 for D; v is 1 or 0; posted to the game window), a pad button (A B X Y LB RB LS RS START BACK DU DD DL DR; v is 1 or 0), a trigger (LT RT; 0..255) or a stick
// axis (LX LY RX RY; -32767..32767). The pad is fed to the game through its import of XInputGetState (one pointer in
// the import table; no code bytes), for the controller index RecPad. From the first job until the game closes that
// index reports a connected, idle pad when no real one is there, so the game does not see it unplugged after a trial.
// The record has one line per fighter mesh per drawn frame: drawn frame, sim step count, steps that ran this frame,
// the fixed-step leftover, the mesh, its world X Y Z and the pad buttons and left stick at that moment.
struct XPad { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
struct XState { uint32_t packet; XPad pad; };
typedef DWORD (WINAPI *XGetFn)(DWORD, XState*);
const uintptr_t O_XINPUT_GET = 0x5c8ca88;      // import slot of XINPUT1_3.dll ordinal 2 (XInputGetState)
struct RecEv { int step; int kind; int value; };   // kind: 0..15 button bit, 16 LT, 17 RT, 18..21 LX LY RX RY, 256 + vk: a key
struct Rec {
    wchar_t dir[MAX_PATH] = L"";
    int padIdx = 0;
    XGetFn orig = nullptr;
    volatile bool virt = false, active = false;
    XPad pad = {};
    uint32_t packet = 1;
    std::vector<RecEv> evs; size_t next = 0;
    int step = 0, total = 0, ran = 0;
    FILE* out = nullptr;
    wchar_t tmp[MAX_PATH], fin[MAX_PATH];
    uint32_t frame = 0, checkFrame = 0;
    float acc = 0; bool haveAcc = false;
    LARGE_INTEGER t = {};
    bool ms = false;                 // a job on the wall clock (menus): steps are milliseconds, nothing is recorded
    ULONGLONG t0 = 0, lastStep = 0;
    HWND wnd = nullptr; WNDPROC wndOrig = nullptr;   // the game window, and its own procedure once ours is in front of it
    int held = 0;                    // keys the job holds down
    uint32_t calls[4] = {};          // XInputGetState calls seen per controller index
} g_rec;

DWORD WINAPI HookedXGet(DWORD idx, XState* st) {
    DWORD r = g_rec.orig(idx, st);
    if (idx < 4) g_rec.calls[idx]++;
    if ((int)idx != g_rec.padIdx || !g_rec.virt || !st) return r;
    if (r != ERROR_SUCCESS) { memset(st, 0, sizeof *st); r = ERROR_SUCCESS; }
    if (g_rec.active) st->pad = g_rec.pad;
    st->packet = ++g_rec.packet;
    return r;
}

int RecControl(const char* s) {
    static const char* names[] = {"DU", "DD", "DL", "DR", "START", "BACK", "LS", "RS", "LB", "RB", "", "", "A", "B", "X", "Y", "LT", "RT", "LX", "LY", "RX", "RY"};
    for (int i = 0; i < 22; i++) if (names[i][0] && !_stricmp(s, names[i])) return i;
    if ((s[0] == 'K' || s[0] == 'k') && isdigit((unsigned char)s[1])) { int vk = atoi(s + 1); if (vk > 0 && vk < 256) return 256 + vk; }
    return -1;
}
// The game also asks Windows directly which window is in front (it takes no input otherwise): its three imports for
// that answer with its own window once a job has run.
typedef HWND (WINAPI *WndFn)();
WndFn g_fgOrig[3];
volatile bool g_recHold;
HWND WINAPI RecFg0() { return g_recHold && g_rec.wnd ? g_rec.wnd : g_fgOrig[0](); }
HWND WINAPI RecFg1() { return g_recHold && g_rec.wnd ? g_rec.wnd : g_fgOrig[1](); }
HWND WINAPI RecFg2() { return g_recHold && g_rec.wnd ? g_rec.wnd : g_fgOrig[2](); }
// While a job has run, the game is kept from learning that its window went to the background: it drops all input
// there otherwise. Its window procedure is wrapped and the three messages that say so are held back.
LRESULT CALLBACK RecWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS) return 0;
    return CallWindowProcW(g_rec.wndOrig, h, m, w, l);
}
void RecApply(const RecEv& e) {
    XPad& p = g_rec.pad;
    if (e.kind >= 256) {             // a key, posted to the game window as the keyboard would
        UINT vk = e.kind - 256, sc = MapVirtualKeyW(vk, 0);
        LPARAM lp = 1 | ((LPARAM)sc << 16) | ((vk >= 0x21 && vk <= 0x28) ? 0x1000000 : 0);
        if (g_rec.wnd) PostMessageW(g_rec.wnd, e.value ? WM_KEYDOWN : WM_KEYUP, vk, e.value ? lp : (lp | 0xC0000000));
        g_rec.held += e.value ? 1 : g_rec.held > 0 ? -1 : 0;
        return;
    }
    if (e.kind < 16) { if (e.value) p.buttons |= 1 << e.kind; else p.buttons &= ~(1 << e.kind); }
    else if (e.kind == 16) p.lt = (uint8_t)e.value;
    else if (e.kind == 17) p.rt = (uint8_t)e.value;
    else (e.kind == 18 ? p.lx : e.kind == 19 ? p.ly : e.kind == 20 ? p.rx : p.ry) = (int16_t)e.value;
}
BOOL CALLBACK FindGame(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[32];
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || !GetClassNameW(h, cls, 32) || wcscmp(cls, L"UnrealWindow"))
        return TRUE;
    *(HWND*)out = h;
    return FALSE;
}
void RecFinish(const char* why) {
    if (g_rec.out) { fprintf(g_rec.out, "end,%s,%d\n", why, g_rec.step); fclose(g_rec.out); g_rec.out = nullptr; MoveFileExW(g_rec.tmp, g_rec.fin, MOVEFILE_REPLACE_EXISTING); }
    g_rec.active = false;
    g_rec.pad = XPad(); g_rec.held = 0;
    Log("recorder: job ended (%s) after %d steps; controller reads so far %u %u %u %u", why, g_rec.step, g_rec.calls[0], g_rec.calls[1], g_rec.calls[2], g_rec.calls[3]);
}
void RecLoad() {
    wchar_t job[MAX_PATH];
    swprintf_s(job, L"%s\\job.txt", g_rec.dir);
    if (GetFileAttributesW(job) == INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfsopen(job, L"r", _SH_DENYNO);
    if (!f) return;
    char line[256], name[64] = "trial", a[64];
    g_rec.evs.clear(); g_rec.total = 120; g_rec.ms = false;
    int bad = 0, s, v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf_s(line, "name %63s", name, (unsigned)sizeof name) == 1) continue;
        if (sscanf_s(line, "steps %d", &v) == 1) { g_rec.total = v; continue; }
        if (!strncmp(line, "clock ms", 8)) { g_rec.ms = true; continue; }
        if (sscanf_s(line, "%d %63s %d", &s, a, (unsigned)sizeof a, &v) == 3 && RecControl(a) >= 0) { g_rec.evs.push_back({s, RecControl(a), v}); continue; }
        for (char* p = line; *p; p++) if (*p > ' ') { bad++; break; }
    }
    fclose(f);
    DeleteFileW(job);
    for (char* p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') *p = '_';
    if (bad || g_rec.total < 1 || g_rec.total > 600000) { Log("recorder: job.txt has %d line(s) it cannot read, or a bad step count: not run", bad); return; }
    swprintf_s(g_rec.tmp, L"%s\\%S.rec.tmp", g_rec.dir, name);
    swprintf_s(g_rec.fin, L"%s\\%S.rec.csv", g_rec.dir, name);
    g_rec.out = _wfsopen(g_rec.tmp, L"w", _SH_DENYNO);
    if (!g_rec.out) { Log("recorder: cannot write the record in RecDir"); return; }
    fprintf(g_rec.out, "frame,step,ran,acc,mesh,bones,x,y,z,buttons,lx,ly\n");
    g_rec.t0 = g_rec.lastStep = GetTickCount64();
    g_rec.step = 0; g_rec.next = 0; g_rec.pad = XPad();
    g_rec.held = 0;
    if (!g_rec.wnd || !IsWindow(g_rec.wnd)) { g_rec.wnd = nullptr; g_rec.wndOrig = nullptr; EnumWindows(FindGame, (LPARAM)&g_rec.wnd); }
    if (g_rec.wnd && !g_rec.wndOrig) {
        g_rec.wndOrig = (WNDPROC)SetWindowLongPtrW(g_rec.wnd, GWLP_WNDPROC, (LONG_PTR)RecWnd);
        Log("recorder: the game window %s kept listening in the background", g_rec.wndOrig ? "is" : "could not be");
    }
    if (g_rec.wnd && GetForegroundWindow() != g_rec.wnd) {     // already in the background: tell it it is in front
        PostMessageW(g_rec.wnd, WM_ACTIVATEAPP, 1, 0);
        PostMessageW(g_rec.wnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageW(g_rec.wnd, WM_SETFOCUS, 0, 0);
    }
    g_recHold = true;
    bool padUsed = false;
    for (auto& e : g_rec.evs) padUsed = padUsed || e.kind < 256;
    if (padUsed && !g_rec.virt) {          // make the engine look at the controllers again: it only polls absent ones after this
        g_rec.virt = true;
        if (g_rec.wnd) PostMessageW(g_rec.wnd, WM_DEVICECHANGE, 0x0007, 0);
    }
    g_rec.active = true;
    Log("recorder: job %s, %d steps, %zu input changes", name, g_rec.total, g_rec.evs.size());
}
// Called for every fighter mesh hand-off, inside the lock.
void RecMesh(void* self, uint32_t frame, float acc, int bones) {
    Rec& r = g_rec;
    if (!r.dir[0] || !r.orig) return;
    if (frame != r.frame) {                    // first mesh of a drawn frame: count the sim steps that ran
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = r.t.QuadPart ? double(now.QuadPart - r.t.QuadPart) / g_freq.QuadPart : 0;
        r.t = now;
        bool gap = frame != r.frame + 1 || !r.haveAcc;
        r.ran = gap ? 0 : (int)floor((r.acc + dt - acc) * 60.0 + 0.5);
        if (!gap && acc < r.acc - 1e-5f && r.ran < 1) r.ran = 1;      // the leftover only drops when a step ran
        if (r.ran < 0) r.ran = 0;
        if (r.ran > 4) r.ran = 4;
        r.frame = frame; r.acc = acc; r.haveAcc = true;
        if (r.active && !r.ms) {
            if (r.ran) r.lastStep = GetTickCount64();
            r.step += r.ran;
            while (r.next < r.evs.size() && r.evs[r.next].step <= r.step) RecApply(r.evs[r.next++]);
            if (r.step >= r.total) RecFinish("done");
        }
    }
    if (!r.active || r.ms || !r.out) return;
    double p[3];
    __try { memcpy(p, (uint8_t*)self + 0x278, sizeof p); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    fprintf(r.out, "%u,%d,%d,%.5f,%p,%d,%.3f,%.3f,%.3f,%u,%d,%d\n", frame, r.step, r.ran, acc, self, bones, p[0], p[1], p[2], r.pad.buttons | (r.held ? 0x10000u : 0), r.pad.lx, r.pad.ly);
}
// Looks for jobs, runs the wall-clock ones (menus have no sim steps to count) and ends a step job that gets no steps.
DWORD WINAPI RecThread(void*) {
    for (int n = 0;; n++) {
        Sleep(10);
        EnterCriticalSection(&g_cs);
        Rec& r = g_rec;
        ULONGLONG now = GetTickCount64();
        if (!r.active) { if (n % 25 == 0) RecLoad(); }
        else if (r.ms) {
            r.step = (int)(now - r.t0);
            while (r.next < r.evs.size() && r.evs[r.next].step <= r.step) RecApply(r.evs[r.next++]);
            if (r.step >= r.total) RecFinish("done");
        } else if (now - r.lastStep > 5000) RecFinish("no sim steps for 5 s");
        LeaveCriticalSection(&g_cs);
    }
}
void RecStart() {
    GetPrivateProfileStringW(L"Dev", L"RecDir", L"", g_rec.dir, MAX_PATH, bf::g_ini);
    g_rec.padIdx = GetPrivateProfileIntW(L"Dev", L"RecPad", 0, bf::g_ini);
    if (!g_rec.dir[0]) return;
    void** slot = (void**)(g_exe + O_XINPUT_GET);
    HMODULE m = nullptr; wchar_t mod[MAX_PATH] = L"";
    bool ok = false;
    __try {
        ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)*slot, &m)
            && GetModuleFileNameW(m, mod, MAX_PATH) && wcsstr(_wcslwr(mod), L"xinput");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD old;
    if (!ok || !VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { Log("recorder: the controller import is not where expected (%ls): recorder off", mod); return; }
    g_rec.orig = (XGetFn)*slot;
    *slot = (void*)HookedXGet;
    VirtualProtect(slot, 8, old, &old);
    static const struct { uintptr_t slot; const char* name; WndFn fn; } fg[3] = {
        {0x5c8c288, "GetForegroundWindow", RecFg0}, {0x5c8c228, "GetFocus", RecFg1}, {0x5c8c340, "GetActiveWindow", RecFg2}};
    HMODULE user = GetModuleHandleW(L"user32.dll");
    for (int i = 0; i < 3; i++) {
        void** sl = (void**)(g_exe + fg[i].slot);
        bool same = false;
        __try { same = user && *sl == (void*)GetProcAddress(user, fg[i].name); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!same || !VirtualProtect(sl, 8, PAGE_READWRITE, &old)) { g_fgOrig[i] = (WndFn)GetProcAddress(user, fg[i].name); Log("recorder: the import of %s is not where expected: left alone", fg[i].name); continue; }
        g_fgOrig[i] = (WndFn)*sl;
        *sl = (void*)fg[i].fn;
        VirtualProtect(sl, 8, old, &old);
    }
    CloseHandle(CreateThread(nullptr, 0, RecThread, nullptr, 0, nullptr));
    Log("recorder: on, jobs from %ls, controller %d", g_rec.dir, g_rec.padIdx);
}

void PsFrame(uint32_t frame);
// Measuring aid (F4 toggles): one log line per fighter mesh per drawn frame with the leftover of the fixed step and
// the raw doubles of the mesh component at 0x138 (RelativeLocation) and 0x1c0..0x280 (where ComponentToWorld should
// be), to read distances such as a dodge's from the log.
std::unordered_map<void*, uint32_t> g_posSeen;
void PosLog(void* self, uint32_t frame) {
    uint32_t& last = g_posSeen[self];
    if (last == frame) return;
    last = frame;
    double v[29]; float acc = 9;
    __try {
        memcpy(v, (uint8_t*)self + 0x138, 3 * sizeof(double));
        memcpy(v + 3, (uint8_t*)self + 0x1c0, 26 * sizeof(double));   // the last three, +0x278..0x288: world X, Y, Z
        if (g_gs) acc = *(float*)(g_gs + GS_ACC);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    char line[900]; int n = sprintf_s(line, "pos,%u,%p,%.5f", frame, self, acc);
    for (int i = 0; i < 29 && n > 0 && n < 860; i++) {
        double x = v[i];
        n += (x == x && fabs(x) < 1e7 && (fabs(x) > 1e-4 || x == 0)) ? sprintf_s(line + n, sizeof line - n, ",%.2f", x) : sprintf_s(line + n, sizeof line - n, ",-");
    }
    Log("%s", line);
}
// Measuring aid (Control::devFlags bit 1): one line per fighter mesh per drawn frame with how far the drawn pose moved
// since the frame before: summed over the sim's bones, the furthest of them, and summed over the engine's own bones.
// Even pacing = the first number follows the game time that passed, frame by frame.
void PoseMoveLog(void* self, Comp& c, const View& v, uint32_t frame, int ran, double al, uint32_t steps) {
    std::vector<double> now((size_t)v.bones * 12);
    if (!Copy(now.data(), v.data, v.bones)) return;
    if (c.devOut.size() == now.size()) {
        double base = 0, top = 0, loose = 0; int nl = 0;
        for (int i = 0; i < v.bones; i++) {
            const double* p = now.data() + i * 12;
            const double* q = c.devOut.data() + i * 12;
            double d = sqrt((p[4] - q[4]) * (p[4] - q[4]) + (p[5] - q[5]) * (p[5] - q[5]) + (p[6] - q[6]) * (p[6] - q[6]));
            if (c.anyLive && BoneDiffers(c.eng.data() + i * 12, c.cur.data() + i * 12)) { loose += d; nl++; }
            else { base += d; if (d > top) top = d; }
        }
        Log("pm,%u,%p,%d,%.3f,%.2f,%.2f,%.2f,%d", frame, self, ran, al, base, top, loose, nl);
        g_pmLines++;
    }
    c.devOut.swap(now);
}
#else
void PsFrame(uint32_t frame);
#endif
void Hooked(void* self) {
    g_orig(self);
    uint32_t frame = *(uint32_t*)(g_exe + O_FRAME_NUMBER);
    PsFrame(frame);
#ifdef BETAFEEL_DEV
    if (g_pos) PosLog(self, frame);
#endif
    EnterCriticalSection(&g_cs);
    Housekeeping(frame);
    View v;
    bool mine = Look((uint8_t*)self, v);
#ifdef BETAFEEL_DEV
    if (mine && !v.other) RecMesh(self, frame, v.acc, v.bones);
#endif
    if (!PosesOn() || !mine) { LeaveCriticalSection(&g_cs); return; }
    Comp& c = g_comps[self];
    size_t n = (size_t)v.bones * 12;
    if (c.bones != v.bones) { c = Comp(); c.bones = v.bones; c.prev.assign(n, 0); c.cur.assign(n, 0); c.eng.assign(n, 0); }
    bool stale = c.lastFrame + 4 < frame;          // not seen for a while (hidden, respawned): start over
    c.lastFrame = frame;
    // Sim steps since this mesh was last looked at: counted by the positions half on every fixed step, so it is right
    // at any frame rate. Without those hooks a drop of the leftover is taken as one step, which only holds above 60
    // drawn frames a second (at 60 the leftover stands still, below 60 it rises on frames with one step).
    bf::Control* ctl = bf::Ctl();
    uint32_t steps = ctl->simSteps;
    int ran = !c.haveAcc ? 0 : ctl->fighterHooks == bf::HOOK_ON ? (int)(steps - c.steps) : v.acc < c.acc - 1e-5f ? 1 : 0;
    bool ticked = ran > 0;
    c.acc = v.acc; c.haveAcc = true; c.steps = steps;
    uint64_t h = 0;
    if (!SafeHash(v.data, v.bones, h)) { LeaveCriticalSection(&g_cs); return; }
    bool fresh = h != c.wroteHash && h != c.curHash;    // the engine put a new pose there (not ours, not the same one)
    if (!fresh && c.anyLive && h == c.curHash && h != c.wroteHash) c.anyLive = false;   // the engine put the sim pose back
    // A change between two sim steps in a minority of the bones: the engine's own (see AddLive), kept apart from the blend.
    bool part = false;
    bool liveOn = g_liveBones;
#ifdef BETAFEEL_DEV
    if (ctl->devFlags & 2) liveOn = false;
#endif
    if (fresh && !ticked && !stale && !c.snap) {
        int exact = 0, differ = liveOn || g_stats ? CountDiff(v.data, c.cur.data(), v.bones, exact) : -1;
        if (liveOn && differ >= 0 && differ * 2 <= v.bones && Copy(c.eng.data(), v.data, v.bones)) {
            part = true;
            c.anyLive = differ > 0;
            s_withoutTick++; s_livePoses++;
            if ((uint32_t)differ > s_liveMost) s_liveMost = differ;
        } else if (liveOn && differ >= 0) {
            // Most of the pose changed between two steps: the engine animates this mesh by the drawn frame (a stage
            // prop playing an animation of its own). It is already smooth; blending it on the frames with a sim step
            // would only hold it back there. Left alone until that has not happened for half a second of steps.
            c.free = true; c.freeAt = steps;
        }
#ifdef BETAFEEL_DEV
        if (g_snapLog && g_liveLines < 40) {
            g_liveLines++;
            Log("between steps: frame %u mesh %p%s: %d of %d bones differ (%d bit for bit), %s", frame, self,
                v.other ? " (other)" : "", differ, v.bones, exact, part ? "kept on top of the blend" : "whole pose shown at once");
        }
#endif
    }
    if (part) {
    } else if (fresh || stale || c.snap) {
        if (c.free && steps - c.freeAt > 30) c.free = false;
        // a pose change with no sim step is not ours to time, nor is any pose of a mesh the engine animates by the frame
        int why = stale ? 1 : c.snap ? 2 : !ticked || c.free ? 3 : 0;
        if (fresh) { if (ticked) s_withTick++; else s_withoutTick++; }
        c.prev.swap(c.cur);
        if (!Copy(c.cur.data(), v.data, v.bones)) { g_comps.erase(self); LeaveCriticalSection(&g_cs); return; }
        c.curHash = Hash(c.cur.data(), v.bones);
        // Bones the engine moved since the last step go on from where the engine had them, not from a step ago (the
        // other bones are the same in both).
        if (c.anyLive && !why) c.prev = c.eng;
        c.anyLive = false;
        JumpInfo ji;
        if (!why) why = Jump(c.prev.data(), c.cur.data(), v.bones, ji);
        if (why) { c.prev = c.cur; s_snaps++; s_why[why]++; }
        else if (ji.nFar) { SnapFar(c.prev.data(), c.cur.data(), v.bones); s_boneSnaps++; }
#ifdef BETAFEEL_DEV
        if (g_snapLog && (why >= 4 || ji.nFar) && g_snapLines < 400) {
            g_snapLines++;
            Log("snap: frame %u mesh %p%s why %d, root dot %.3f, furthest bone %.0f cm (bone %d), %d bones far, turned %d of %d bones", frame, self,
                v.other ? " (other)" : "", why, ji.rootDot, ji.maxDist, ji.farBone, ji.nFar, ji.turned, v.bones);
        }
#endif
        c.snap = false;
        c.span = ran > 4 ? 4 : ran > 1 ? ran : 1;
    } else if (ticked) {
        c.prev = c.cur;                                 // a sim step that left the pose as it was (idle, hit pause)
        c.span = 1;
    }
    // (If the buffer still holds our last blend, the engine did not evaluate this frame: blend again from what we kept.)
    double al = v.acc * 60.0 + 0.5;
    al = al < 0 ? 0 : al > 1 ? 1 : al;
    // Below 60 drawn frames a second several steps run between two poses: prev is span steps old, and the point one
    // step before the newest (where the positions half draws) is further along the way from prev to cur.
    if (c.span > 1) al = 1 - (1 - al) / c.span;
    bool same = !c.anyLive && memcmp(c.prev.data(), c.cur.data(), n * 8) == 0;
    if (g_test) {
        if (Blend(v.data, c.prev.data(), c.cur.data(), v.bones, al) && Stretch(v.data, v.bones)) SafeHash(v.data, v.bones, c.wroteHash);
    } else if (same) {
        if (h != c.curHash) { Copy(v.data, c.cur.data(), v.bones); c.wroteHash = c.curHash; }
    } else if (Blend(v.data, c.prev.data(), c.cur.data(), v.bones, al)
               && (!c.anyLive || AddLive(v.data, c.cur.data(), c.eng.data(), v.bones))) {
        SafeHash(v.data, v.bones, c.wroteHash);
        s_blends++;
    }
#ifdef BETAFEEL_DEV
    if ((ctl->devFlags & 1) && g_pmLines < 90000) PoseMoveLog(self, c, v, frame, ran, al, steps);
#endif
    LeaveCriticalSection(&g_cs);
}

// ---- effects ----
struct FxView { uint8_t mode, lock; float age, acc; };
bool FxLook(uint8_t* c, FxView& v) {
    __try {
        v.mode = c[FX_MODE]; v.lock = c[FX_LOCK]; v.age = *(float*)(c + FX_AGE);
        uint8_t* gs = g_gs;
        v.acc = gs ? *(float*)(gs + GS_ACC) : 9.f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FxSet(uint8_t* c, float age, uint8_t lock) {
    __try { *(float*)(c + FX_AGE) = age; c[FX_LOCK] = lock; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HookedFx(void* self, float dt, int type, void* fn) {
    uint8_t* c = (uint8_t*)self;
    FxView v;
    if (!FxLook(c, v)) { g_fxOrig(self, dt, type, fn); return; }
    uint32_t frame = *(uint32_t*)(g_exe + O_FRAME_NUMBER);
    EnterCriticalSection(&g_cs);
    if (v.mode < 3) s_fxMode[v.mode]++;
    if (fabsf(dt - 1.f / 60) < 2e-4f) s_fxStep60++;
    // only while a match's game state was seen this frame or the last few, and its leftover is sane
    bool live = FxOn() && (v.mode == 1 || v.mode == 2) && frame - g_gsFrame < 8 && v.acc > -0.05f && v.acc < 0.05f;
    float show = v.age;
    if (live) {
        Fx& x = g_fxs[self];
        bool stale = !x.seen || x.lastFrame + 4 < frame;
        x.lastFrame = frame; x.seen = true;
        if (stale) { x.prev = x.cur = v.age; }
        else if (v.age != x.cur) {
            x.prev = x.cur; x.cur = v.age;
            // restarted, or jumped ahead by more than a few steps: show at once
            if (x.cur < x.prev || x.cur - x.prev > 0.1f) { x.prev = x.cur; s_fxSnap++; }
        }
        float al = v.acc * 60.f + 0.5f;
        al = al < 0 ? 0 : al > 1 ? 1 : al;
        if (x.cur != x.prev) { show = x.prev + (x.cur - x.prev) * al; s_fxBlend++; }
    }
    LeaveCriticalSection(&g_cs);
    if (live && show != v.age && FxSet(c, show, 0)) {
        g_fxOrig(self, dt, type, fn);
        FxSet(c, v.age, v.lock);      // the game's own value back, so its next change is seen
    } else {
        g_fxOrig(self, dt, type, fn);
    }
}

bool PsAlive(void* self) {
    __try {
        // the last two tests are the ones the game itself makes right before its call (exe+0x263fa89 .. 0x263fa9f)
        uint8_t* p = (uint8_t*)self;
        return *(uintptr_t*)self - (uintptr_t)g_exe == VT_PS && !(*(uint32_t*)(p + OBJ_FLAGS) & RF_DEAD)
            && p[0x30] == 0 && (p[0x9a] & 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GsAcc(float& acc) {
    __try { uint8_t* gs = g_gs; if (!gs) return false; acc = *(float*)(gs + GS_ACC); return acc > -0.05f && acc < 0.05f; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HookedPs(void* self, float dt, int type, void* fn) {
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    uint32_t frame = *(uint32_t*)(g_exe + O_FRAME_NUMBER);
    bool game = ret - (uintptr_t)g_exe == O_PS_GAME_RET;
    float owed = 0; bool hold = false;
    EnterCriticalSection(&g_cs);
    s_psCalls++; s_psDt += dt;
    if (fabsf(dt - 1.f / 60) < 2e-4f) s_psStep60++;
    if (dt == 0) s_psZero++;
    if (frame != s_psLastFrame) { s_psLastFrame = frame; s_psFrames++; s_psThisFrame = 0; }
    if (++s_psThisFrame > s_psMaxPerFrame) s_psMaxPerFrame = s_psThisFrame;
    s_psComps[self]++;
    for (auto& cl : s_psCallers) { if (cl.ret == ret || !cl.n) { cl.ret = ret; cl.n++; break; } }
    auto it = g_pss.find(self);
    if (it != g_pss.end()) {          // what the last step still owes comes first, whoever calls
        owed = it->second.dt * (1 - it->second.given);
        if (owed > 1e-7f) s_psFlush++;
        it->second.dt = 0; it->second.given = 0;
    }
    // hold back only the game's own per-step call, while a match's game state was seen in the last frames
    if (game && FxOn() && dt > 0 && dt < 0.05f && frame - g_gsFrame < 8) {
        Ps& p = g_pss[self];
        p.dt = dt; p.given = 0; p.type = type; p.fn = fn; p.stepFrame = frame; p.step = bf::Ctl()->simSteps;
        g_psThread = GetCurrentThreadId();
        hold = true; s_psHeld++;
    } else if (it != g_pss.end()) g_pss.erase(self);
    LeaveCriticalSection(&g_cs);
    if (owed > 1e-7f) g_psOrig(self, owed, type, fn);
    if (!hold) g_psOrig(self, dt, type, fn);
}

// Once per drawn frame, from the first fighter mesh hand-off: give every held effect its slice for this frame.
void PsFrame(uint32_t frame) {
    if (frame == g_psFrame) return;
    struct Job { void* self; float dt; int type; void* fn; };
    Job jobs[64]; int n = 0;
    EnterCriticalSection(&g_cs);
    if (frame == g_psFrame || g_pss.empty() || GetCurrentThreadId() != g_psThread) { g_psFrame = frame; LeaveCriticalSection(&g_cs); return; }
    g_psFrame = frame;
    float acc;
    if (GsAcc(acc)) {
        float al = acc * 60.f + 0.5f;
        al = al < 0 ? 0 : al > 1 ? 1 : al;
        // "The game stopped ticking it" is counted in sim steps where they are known: a count of drawn frames would
        // cut an effect off between two steps on a fast screen (6 frames is less than one step above 360 a second).
        bool counted = bf::Ctl()->fighterHooks == bf::HOOK_ON;
        uint32_t steps = bf::Ctl()->simSteps;
        for (auto it = g_pss.begin(); it != g_pss.end();) {
            Ps& p = it->second;
            bool gone = counted ? steps - p.step > 3 || frame - p.stepFrame > 4000 : p.stepFrame + 6 < frame;
            if (gone || !PsAlive(it->first)) {                        // the game stopped ticking it, or it is gone
                if (p.dt > 0) s_psDropped++;
                it = g_pss.erase(it); continue;
            }
            if (p.dt > 0 && al > p.given + 1e-4f && n < 64) {
                jobs[n++] = { it->first, p.dt * (al - p.given), p.type, p.fn };
                p.given = al;
            }
            ++it;
        }
    }
    s_psSlices += n;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < n; i++) g_psOrig(jobs[i].self, jobs[i].dt, jobs[i].type, jobs[i].fn);
}

void Start() {
    g_snap = bf::IniInt(L"PoseSnap", 120);
    g_snapDot = bf::IniInt(L"PoseSnapTurnPercent", 70) / 100.0;
    g_engineMeshes = bf::IniInt(L"EngineMeshes", 1) != 0;
    int deg = bf::IniInt(L"PoseTurnDegrees", 60);
    g_turnDot = cos((deg < 1 ? 1 : deg > 179 ? 179 : deg) * 3.14159265358979 / 360.0);   // |quat dot| = cos(angle / 2)
    g_turnShare = bf::IniInt(L"PoseTurnSharePercent", 25);
    g_liveBones = bf::IniInt(L"PoseLiveBones", 1) != 0;
#ifdef BETAFEEL_DEV
    g_stats = GetPrivateProfileIntW(L"Dev", L"Stats", 0, bf::g_ini) != 0;
    g_testKey = GetPrivateProfileIntW(L"Dev", L"TestKey", 0, bf::g_ini);
    g_snapLog = GetPrivateProfileIntW(L"Dev", L"SnapLog", 0, bf::g_ini) != 0;
    g_pos = GetPrivateProfileIntW(L"Dev", L"PosLog", 0, bf::g_ini) != 0;   // the position log on from the start (F4 switches it)
#endif
    InitializeCriticalSection(&g_cs);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_orig = (FinFn)(g_exe + O_FINALIZE);
    for (uintptr_t s : SLOTS) {
        bool ok = false;
        __try { ok = *(uintptr_t*)(g_exe + s) == (uintptr_t)g_orig; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ok) { Log("poses: exe+0x%llx is not the expected vtable slot (another build, or another plugin): nothing changed", (unsigned long long)s); bf::Ctl()->poseHooks = bf::HOOK_BADBUILD; return; }
    }
    for (uintptr_t s : SLOTS) {
        DWORD old;
        void** slot = (void**)(g_exe + s);
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { Log("could not unprotect a vtable slot"); return; }
        *slot = (void*)Hooked;
        VirtualProtect(slot, 8, old, &old);
    }
    bf::Ctl()->poseHooks = bf::HOOK_ON;
    if (g_engineMeshes) {
        bool ok = false;
        __try { ok = *(uintptr_t*)(g_exe + ENGINE_SLOT) == (uintptr_t)g_orig; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        DWORD old;
        void** slot = (void**)(g_exe + ENGINE_SLOT);
        if (ok && VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { *slot = (void*)Hooked; VirtualProtect(slot, 8, old, &old); Log("engine skeletal meshes: hooked"); }
        else Log("engine skeletal meshes: slot not as expected, left alone");
    }
    // effects: same kind of wrapper on the Niagara component's per-frame tick
    g_fxOrig = (FxFn)(g_exe + O_FX_TICK);
    bool fxOk = true;
    for (uintptr_t s : FX_SLOTS) {
        bool ok = false;
        __try { ok = *(uintptr_t*)(g_exe + s) == (uintptr_t)g_fxOrig; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        fxOk = fxOk && ok;
    }
    if (!fxOk) Log("effect tick slots are not as expected: effects left alone");
    else for (uintptr_t s : FX_SLOTS) {
        DWORD old;
        void** slot = (void**)(g_exe + s);
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { Log("could not unprotect an effect vtable slot"); break; }
        *slot = (void*)HookedFx;
        VirtualProtect(slot, 8, old, &old);
    }
    Log("effects: %s", !fxOk ? "not hooked" : "hooked (Effects switch)");
    g_psOrig = (FxFn)(g_exe + O_PS_TICK);
    {
        bool ok = false;
        void** slot = (void**)(g_exe + PS_SLOT);
        __try { ok = *(uintptr_t*)slot == (uintptr_t)g_psOrig; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        DWORD old;
        if (ok && VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { *slot = (void*)HookedPs; VirtualProtect(slot, 8, old, &old); Log("cascade: hooked (Effects switch)"); }
        else Log("cascade tick slot is not as expected: left alone");
    }
#ifdef BETAFEEL_DEV
    RecStart();
#endif
    Log("poses: in, snap %.0f cm, turn %.2f", g_snap, g_snapDot);
}
}  // namespace ps
