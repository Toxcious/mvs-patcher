// MvsPakLoader.asi: lets the final MultiVersus build mount unsigned paks (.pak and .utoc/.ucas without a valid .sig).
//
// The game registers its pak signing key from a static initialiser: `lea rcx, <key callback>; jmp <register>`.
// Turning that jmp into ret before the initialiser runs leaves the engine without a signing key, so it checks no
// signatures. This is the patch OpenVersus applied (its "PakLoader" setting); the byte pattern is the one it used.
// On Steam build 17784632 the pattern matches once, at RVA 0x9A7E80, and the jmp is at RVA 0x9A7EB7.
//
// Loaded by Ultimate ASI Loader (xinput1_3.dll) from Binaries\Win64\plugins. Writes MvsPakLoader.log beside itself.
// Nothing on disk is changed: the patch is one byte in memory, each launch.
//
// It also rewrites two native movement defaults (gravity, terminal fall speed) when MvsPakLoader.ini asks for it:
// see PatchNumbers below.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const int16_t PAT[] = {
    0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xE9, -1, -1, -1, -1, 0xCC, 0xCC, 0xCC, 0xCC,
    0x48, 0x83, 0xEC, 0x28, 0xE8, -1, -1, -1, -1, 0x48, 0x89, 0x05, -1, -1, -1, -1,
    0x48, 0x83, 0xC4, 0x28, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xE9, -1, -1, -1, -1, 0xCC, 0xCC, 0xCC, 0xCC,
    0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xE9, -1, -1, -1, -1, 0xCC, 0xCC, 0xCC, 0xCC};
static const size_t PAT_LEN = sizeof PAT / sizeof PAT[0];
static const size_t JMP_AT = 48 + 7;            // the signing-key stub is the second `lea; jmp` of the match
static const uintptr_t KNOWN_RVA = 0x9A7E80;    // where the match is on build 17784632

static FILE* g_log;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static bool Matches(const uint8_t* p) {
    for (size_t i = 0; i < PAT_LEN; i++)
        if (PAT[i] >= 0 && p[i] != (uint8_t)PAT[i]) return false;
    return true;
}

// Native movement defaults. The constructors of MvsBaseMovementComponent (exe+0x22E6070) and
// MvsCharacterMovementComponent (exe+0x264F7B0) build their fixed-point defaults by parsing number text, so
// rewriting that text before any object exists changes the default. Each text is used by those constructors only.
//   "65"   -> Gravity = text / 10 (a scale on -980; 6.5 as shipped)
//   "2800" -> TerminalVelocity and DefaultTerminalVelocity = -text
// Values come from MvsPakLoader.ini beside this file ([Movement] GravityTenths=, TerminalVelocity=); a missing
// file or key leaves the game's own value. Only applied when the shipped text is where build 17784632 has it.
struct NumberText { const wchar_t* key; uintptr_t rva; const char* shipped; size_t room; };
static const NumberText NUMBERS[] = {
    {L"GravityTenths", 0x63B49F0, "65", 3},
    {L"TerminalVelocity", 0x63B4A04, "2800", 7},
};

static void PatchNumbers(uint8_t* exe, const wchar_t* ini) {
    for (const NumberText& t : NUMBERS) {
        wchar_t w[16];
        DWORD len = GetPrivateProfileStringW(L"Movement", t.key, L"", w, 16, ini);
        if (len == 0) { Log("%ls: not set; the game's %s stays", t.key, t.shipped); continue; }
        char v[16];
        bool ok = len <= t.room;
        for (DWORD i = 0; ok && i < len; i++) { ok = w[i] >= L'0' && w[i] <= L'9'; v[i] = (char)w[i]; }
        if (!ok) { Log("%ls: value must be 1 to %zu digits; the game's %s stays", t.key, t.room, t.shipped); continue; }
        v[len] = 0;
        char* at = (char*)(exe + t.rva);
        size_t shippedLen = strlen(t.shipped);
        if (memcmp(at, t.shipped, shippedLen + 1) != 0) { Log("%ls: \"%s\" is not at exe+0x%llX (another build?); nothing changed", t.key, t.shipped, (unsigned long long)t.rva); continue; }
        DWORD old;
        if (!VirtualProtect(at, t.room + 1, PAGE_READWRITE, &old)) { Log("%ls: VirtualProtect failed (%lu); nothing changed", t.key, GetLastError()); continue; }
        memset(at, 0, t.room + 1);
        memcpy(at, v, len);
        VirtualProtect(at, t.room + 1, old, &old);
        Log("%ls: \"%s\" at exe+0x%llX is now \"%s\"", t.key, t.shipped, (unsigned long long)t.rva, v);
    }
}

// ShortHopLaunchModifier (character +0x5A0, 0.7 as shipped): a ground jump whose button is already up at lift-off
// launches at 0.7 x power. The beta has no such cut (a 1-frame tap rises at full speed for the short-hop
// frames), and with the 1-frame squat of gen_jump.js only a tap of 1 to 2 frames still gets it, so taps of
// different lengths gave different heights. The constructor reads the number from the text "0.7" at
// exe+0x642B8F0, which an unrelated static also reads, so the text is left alone; instead the constructor's
//   exe+0x264FC38  lea rdx, [rip+0x3DDBCB1]    ; -> "0.7"
// is pointed at the game's own text "1.0" at exe+0x5FAB6CC. Ini: [Movement] ShortHopFull=1 (missing or 0: as shipped).
static void PatchShortHop(uint8_t* exe, const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"Movement", L"ShortHopFull", 0, ini) != 1) { Log("ShortHopFull: not set; the game's 0.7 stays"); return; }
    static const uint8_t lea[7] = {0x48, 0x8D, 0x15, 0xB1, 0xBC, 0xDD, 0x03};
    uint8_t* at = exe + 0x264FC38;
    const uintptr_t one = 0x5FAB6CC;
    if (memcmp(at, lea, 7) != 0 || memcmp(exe + 0x642B8F0, "0.7", 4) != 0 || memcmp(exe + one - 1, "\0" "1.0", 5) != 0) {
        Log("ShortHopFull: the shipped code or texts are not where build 17784632 has them; nothing changed"); return;
    }
    int32_t disp = (int32_t)(one - (0x264FC38 + 7));
    DWORD old;
    if (!VirtualProtect(at + 3, 4, PAGE_EXECUTE_READWRITE, &old)) { Log("ShortHopFull: VirtualProtect failed (%lu); nothing changed", GetLastError()); return; }
    memcpy(at + 3, &disp, 4);
    VirtualProtect(at + 3, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 7);
    Log("ShortHopFull: ShortHopLaunchModifier now reads \"1.0\" (exe+0x264FC38 points at exe+0x%llX)", (unsigned long long)one);
}

static void SkipSigningKey(uint8_t* exe);

static void Start(HMODULE self) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    if (!n || !name || _wcsicmp(name + 1, L"MultiVersus-Win64-Shipping.exe") != 0) return;   // some other program: do nothing

    uint8_t* exe = (uint8_t*)GetModuleHandleW(nullptr);
    n = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* dot = wcsrchr(path, L'.');
    if (n && dot) {
        wcscpy_s(dot, path + MAX_PATH - dot, L".log");
        g_log = _wfsopen(path, L"w", _SH_DENYNO);
        wcscpy_s(dot, path + MAX_PATH - dot, L".ini");
        PatchNumbers(exe, path);
        PatchShortHop(exe, path);
    }
    SkipSigningKey(exe);
}

static void SkipSigningKey(uint8_t* exe) {
    auto* nt = (IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* text = nullptr;
    size_t textSize = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sec[i].Name, ".text", 6) == 0) { text = exe + sec[i].VirtualAddress; textSize = sec[i].Misc.VirtualSize; }
    if (!text || textSize < PAT_LEN) { Log("no .text section found; nothing patched"); return; }

    uint8_t* hit = nullptr;
    if (KNOWN_RVA + PAT_LEN <= (uintptr_t)(text - exe) + textSize && Matches(exe + KNOWN_RVA)) {
        hit = exe + KNOWN_RVA;
    } else {
        // Another build: take the pattern only if it matches exactly once.
        int count = 0;
        for (uint8_t* p = text; p + PAT_LEN <= text + textSize; p++)
            if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D && Matches(p)) { hit = p; count++; }
        if (count != 1) { Log("signing-key stub not found (pattern matches: %d); nothing patched, unsigned paks will not load", count); return; }
    }

    uint8_t* jmp = hit + JMP_AT;
    DWORD old;
    if (!VirtualProtect(jmp, 1, PAGE_EXECUTE_READWRITE, &old)) { Log("VirtualProtect failed (%lu); nothing patched", GetLastError()); return; }
    *jmp = 0xC3;
    VirtualProtect(jmp, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), jmp, 1);
    Log("signing-key registration skipped: jmp at exe+0x%llX is now ret (%s); unsigned paks load",
        (unsigned long long)(jmp - exe), hit == exe + KNOWN_RVA ? "known build" : "found by pattern");
}

BOOL WINAPI DllMain(HMODULE self, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        Start(self);
    }
    return TRUE;
}
