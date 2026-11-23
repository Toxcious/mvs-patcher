// BetaFeel.asi: what the two halves (fighters.cpp: positions and camera; poses.cpp: skeletons and effects) share.
//
// The control block is a small named shared-memory area, "Local\BetaFeel.Control", that the BetaFeel helper opens
// too. The switches in it are read on every frame, so a switch in the helper takes effect at once in a running game.
// The plugin fills them from BetaFeel.ini when it loads (the helper keeps that file in step with its switches).
// helper/PluginLink.cs mirrors this layout: keep the two in step.
#pragma once
#include <windows.h>
#include <stdint.h>

namespace bf {

const uint32_t CTL_MAGIC = 0x4C464642;   // "BFFL"
const uint32_t CTL_VERSION = 4;          // 2: fps measured in the plugin; 3: fast start, lab switches, 128 bytes;
                                         // 4: frame rate cap
const wchar_t CTL_NAME[] = L"Local\\BetaFeel.Control";

enum : uint32_t { HOOK_NONE = 0, HOOK_ON = 1, HOOK_BADBUILD = 2 };
enum : uint8_t { LAB_HITBOXES = 1, LAB_FRAMESTEP = 2 };                       // Control::lab (switches)
enum : uint32_t { LABS_HITBOXES = 1, LABS_STEP_READY = 2, LABS_PAUSED = 4 };  // Control::labStatus
enum : uint32_t { CAP_NONE = 0, CAP_READY = 1, CAP_ON = 2, CAP_BADBUILD = 3 };  // Control::capState

#pragma pack(push, 1)
struct Control {                       // offset
    uint32_t magic;                    // 0
    uint32_t version;                  // 4
    uint32_t size;                     // 8   sizeof(Control)
    uint32_t gamePid;                  // 12  the game process the plugin runs in
    // switches: 1 = on. Written by the plugin at load (from BetaFeel.ini), the helper and the hotkey. The helper
    // shows one Smoothing switch and sets the first five together; they stay separate here for testing.
    volatile uint8_t fighters;         // 16  fighters and projectiles between sim steps
    volatile uint8_t camera;           // 17  the camera too
    volatile uint8_t poses;            // 18  skeleton poses
    volatile uint8_t effects;          // 19  effects (Niagara ages, Cascade steps)
    volatile uint8_t others;           // 20  meshes not driven by the fighters' anim class (summons, props)
    volatile uint8_t hotkeys;          // 21  the hotkey (F8) switches smoothing in game too
    volatile uint8_t fastStartOn;      // 22  fast start (boot.cpp): read when the game starts
    volatile uint8_t lab;              // 23  lab tools: LAB_* bits
    // status: written by the plugin
    volatile uint32_t fighterHooks;    // 24  HOOK_*
    volatile uint32_t poseHooks;       // 28  HOOK_*
    volatile uint32_t frame;           // 32  drawn frames so far (the engine's GFrameNumber)
    volatile uint32_t simSteps;        // 36  fixed sim steps run so far
    volatile uint64_t heartbeat;       // 40  GetTickCount64() of the plugin's last status write (about 4 a second)
    volatile uint64_t matchTick;       // 48  GetTickCount64() when a match's game state last ticked
    volatile uint32_t oldPlugins;      // 56  bit 0: FighterSmooth.asi, bit 1: PoseSmooth.asi beside this one
    volatile uint32_t switchSeq;       // 60  +1 on every switch change made in game (hotkeys)
    char build[24];                    // 64  this plugin's build label
    volatile uint32_t fpsX100;         // 88  drawn frames per second over the last second, times 100 (0 = not yet)
    volatile uint32_t fastStart;       // 92  FAST_* (boot.cpp): what fast start did at this game start
    volatile uint32_t menuMs;          // 96  ms from launch to the main menu loading, when fast start took it there
    volatile uint32_t labStatus;       // 100 LABS_* (lab.cpp): hitboxes drawn, frame step possible here, sim paused
    volatile uint32_t peHooked;        // 104 1: ProcessEvent hooked (boot.cpp), so a paused Lab sim can be resumed by key
    // Frame rate cap (main.cpp): drawn frames a second the engine's own limiter is held to, 0 = left to the game
    // (its Vertical Sync and settings). Written by the plugin at load (BetaFeel.ini FrameCap) and by the helper.
    volatile uint32_t fpsCap;          // 108
    volatile uint32_t capState;        // 112 CAP_*
    volatile uint8_t devFlags;         // 116 developer build only: 1 = pose movement log, 2 = loose bones off
    uint8_t reserved[11];              // 117
};                                     // 128
#pragma pack(pop)
static_assert(sizeof(Control) == 128, "layout shared with helper/PluginLink.cs");

Control* Ctl();                        // never null once Start has run (a private block if the shared one failed)
void Log(const char* fmt, ...);
int IniInt(const wchar_t* name, int def);
void SaveSwitches();                   // the switches back to BetaFeel.ini (after a hotkey)
extern uint8_t* g_exe;                 // the game's exe
extern HMODULE g_self;
extern wchar_t g_ini[MAX_PATH];        // BetaFeel.ini beside the plugin

}  // namespace bf

namespace fs { void Start(); }          // fighters.cpp
namespace ps { void Start(); }          // poses.cpp
namespace fb { void Start(); }          // boot.cpp
namespace lab {                         // lab.cpp
extern volatile bool g_wake;            // a request is waiting: boot.cpp's ProcessEvent hook calls Poll
void Poll();
void OnGsTick(void* gs, uint32_t simTick);
void RequestToggle();
void RequestStep();
}
