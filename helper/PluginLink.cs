// The helper's side of BetaFeel.asi's control block (plugin/control.h: keep the two layouts in step). A switch set
// here is read by the plugin on its next frame; it is also written to plugins\BetaFeel.ini, which the plugin reads when
// the game starts, so the choice stays.
using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;

sealed class PluginLink : IDisposable
{
    const string Name = @"Local\BetaFeel.Control";
    const uint Magic = 0x4C464642, Version = 4;
    // offsets (control.h)
    const int O_MAGIC = 0, O_PID = 12, O_SWITCHES = 16, O_FHOOKS = 24, O_PHOOKS = 28, O_FRAME = 32, O_STEPS = 36,
              O_HEARTBEAT = 40, O_MATCH = 48, O_OLD = 56, O_SEQ = 60, O_BUILD = 64, O_FPS = 88, O_VERSION = 4,
              O_FAST = 92, O_MENU_MS = 96, O_LAB = 23, O_LAB_STATUS = 100, O_PE_HOOKED = 104, O_CAP = 108,
              O_CAP_STATE = 112;

    public enum Switch { Fighters = 0, Camera = 1, Poses = 2, Effects = 3, Others = 4, Hotkeys = 5, FastStart = 6 }
    public static readonly string[] IniKeys = { "Fighters", "Camera", "Poses", "Effects", "Others", "Hotkeys", "FastStart" };
    static readonly bool[] Defaults = { true, true, true, true, true, false, true };

    readonly MemoryMappedFile map;
    readonly MemoryMappedViewAccessor view;

    public PluginLink()
    {
        map = MemoryMappedFile.CreateOrOpen(Name, 4096);
        view = map.CreateViewAccessor(0, 4096);
    }
    public void Dispose() { view.Dispose(); map.Dispose(); }

    public record Status(bool Running, bool InMatch, double? Fps, uint FighterHooks, uint PoseHooks, uint OldPlugins,
                         string Build, uint SwitchSeq, uint SimSteps, FastState Fast, uint MenuMs, LabState Lab, bool PeHooked,
                         CapState Cap);
    // the frame rate cap (control.h CAP_*): Ready = can be set, On = the engine's limiter is held to it now
    public enum CapState { None = 0, Ready = 1, On = 2, BadBuild = 3 }
    // the lab tools' switches (control.h LAB_*, a bit set in one byte; BetaFeel.ini Lab=) and what they do now (LABS_*)
    [Flags] public enum Lab : byte { None = 0, Hitboxes = 1, FrameStep = 2 }
    [Flags] public enum LabState : uint { None = 0, Hitboxes = 1, StepReady = 2, Paused = 4 }
    // what fast start did at this game start (control.h / boot.cpp FAST_*)
    public enum FastState { Off = 0, Waiting = 1, Skipping = 2, Done = 3, Eula = 4, Stopped = 5, BadBuild = 6, OtherMod = 7 }

    // A plugin is running when the block carries its mark and version, its game process is alive and it wrote in the
    // last 2 s. The frame rate is the plugin's own (frames over about a second on its clock).
    public Status Read()
    {
        long now = Environment.TickCount64;
        bool alive = view.ReadUInt32(O_MAGIC) == Magic && view.ReadUInt32(O_VERSION) == Version &&
                     now - (long)view.ReadUInt64(O_HEARTBEAT) < 2000 && Alive((int)view.ReadUInt32(O_PID));
        if (!alive) return new Status(false, false, null, 0, 0, 0, "", 0, 0, FastState.Off, 0, LabState.None, false, CapState.None);
        uint fpsX100 = view.ReadUInt32(O_FPS);
        double? fps = fpsX100 > 0 ? fpsX100 / 100.0 : null;
        var b = new byte[24];
        view.ReadArray(O_BUILD, b, 0, 24);
        return new Status(true, now - (long)view.ReadUInt64(O_MATCH) < 1000, fps, view.ReadUInt32(O_FHOOKS), view.ReadUInt32(O_PHOOKS),
                          view.ReadUInt32(O_OLD), Encoding.ASCII.GetString(b).TrimEnd('\0'), view.ReadUInt32(O_SEQ), view.ReadUInt32(O_STEPS),
                          (FastState)view.ReadUInt32(O_FAST), view.ReadUInt32(O_MENU_MS), (LabState)view.ReadUInt32(O_LAB_STATUS),
                          view.ReadUInt32(O_PE_HOOKED) != 0, (CapState)view.ReadUInt32(O_CAP_STATE));
    }

    static bool Alive(int pid)
    {
        if (pid <= 0) return false;
        try { using var p = Process.GetProcessById(pid); return !p.HasExited; } catch { return false; }
    }

    public bool Get(Switch s) => view.ReadByte(O_SWITCHES + (int)s) != 0;
    public void Set(Switch s, bool on) => view.Write(O_SWITCHES + (int)s, (byte)(on ? 1 : 0));
    // The one Smoothing switch the helper shows: the first five together.
    public static readonly Switch[] Smoothing = { Switch.Fighters, Switch.Camera, Switch.Poses, Switch.Effects, Switch.Others };

    // The frame rate cap, in drawn frames a second; 0 = none (the game's own settings decide).
    public uint GetCap() => view.ReadUInt32(O_CAP);
    public void SetCap(uint fps) => view.Write(O_CAP, fps);
    public static uint IniGetCap(string game) => (uint)Math.Max(0, GetPrivateProfileIntW("BetaFeel", "FrameCap", 0, IniPath(game)));
    public static void IniSetCap(string game, uint fps)
    {
        if (!File.Exists(IniPath(game))) return;
        WritePrivateProfileStringW("BetaFeel", "FrameCap", fps.ToString(), IniPath(game));
    }

    public Lab GetLab() => (Lab)view.ReadByte(O_LAB);
    public void SetLab(Lab l) => view.Write(O_LAB, (byte)l);

    // ---- BetaFeel.ini (the choices kept for the next game start) ----
    public static Lab IniGetLab(string game) => (Lab)(GetPrivateProfileIntW("BetaFeel", "Lab", 0, IniPath(game)) & 3);
    public static void IniSetLab(string game, Lab l)
    {
        if (!File.Exists(IniPath(game))) return;
        WritePrivateProfileStringW("BetaFeel", "Lab", ((int)l).ToString(), IniPath(game));
    }
    // The Lab's pause and step keys' names ("F5", "F6"), from LabPauseKey / LabStepKey.
    public static string KeyName(string game, string key, int def)
    {
        var b = new StringBuilder(32);
        GetPrivateProfileStringW("BetaFeel", key, "", b, 32, IniPath(game));
        var t = b.ToString().Split(';')[0].Trim();
        int vk = def;
        try { if (t.Length > 0) vk = t.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? Convert.ToInt32(t[2..], 16) : int.Parse(t); } catch { }
        return vk == 0 ? "(no key)" : ((Keys)vk).ToString();
    }
    public static string IniPath(string game) => Path.Combine(Patcher.PluginsDir(game), "BetaFeel.ini");
    public static bool IniGet(string game, Switch s) =>
        GetPrivateProfileIntW("BetaFeel", IniKeys[(int)s], Defaults[(int)s] ? 1 : 0, IniPath(game)) != 0;
    public static void IniSet(string game, Switch s, bool on)
    {
        if (!File.Exists(IniPath(game))) return;   // not installed: nothing to keep it in
        WritePrivateProfileStringW("BetaFeel", IniKeys[(int)s], on ? "1" : "0", IniPath(game));
    }

    // The in-game key's name ("F8"), from BetaFeel.ini's Hotkey (a Windows virtual-key code, 0x77 by default).
    public static string HotkeyName(string game)
    {
        var b = new StringBuilder(32);
        GetPrivateProfileStringW("BetaFeel", "Hotkey", "", b, 32, IniPath(game));
        var t = b.ToString().Split(';')[0].Trim();
        int vk = 0x77;
        try { if (t.Length > 0) vk = t.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? Convert.ToInt32(t[2..], 16) : int.Parse(t); } catch { }
        return vk == 0 ? "the hotkey" : ((Keys)vk).ToString();
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern int GetPrivateProfileStringW(string section, string key, string def, StringBuilder buf, int size, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern int GetPrivateProfileIntW(string section, string key, int def, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool WritePrivateProfileStringW(string section, string key, string value, string file);
}
