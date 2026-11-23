using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.IO.Objects;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using Microsoft.Win32;

// Everything the window does, with no window in it (so it can be run and checked from a command line too).
public static class Patcher
{
    public const string SteamAppId = "1818750";
    const string ExeRel = @"MultiVersus\Binaries\Win64\MultiVersus-Win64-Shipping.exe";

    public record Recipe(string name, string game, List<RecipeAsset> assets);
    public record RecipeAsset(string path, long size, string sha256, string result, List<JsonElement[]> edits, List<RecipeEntry>? entries);
    // A launch entry the asset gets on top of its edits (Zen.AddEntry; make_pak.js "entries").
    public record RecipeEntry(string map, string key, string like, double? x, int? frames);

    // ---- Finding the game ----
    // Steam's own folder from the registry, then every library in libraryfolders.vdf; the first that has the game.
    public static string? FindGame()
    {
        var steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        var libs = new List<string>();
        if (steam != null)
        {
            libs.Add(steam.Replace('/', '\\'));
            var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
            if (File.Exists(vdf))
                foreach (var line in File.ReadAllLines(vdf))
                {
                    var t = line.Trim();
                    if (!t.StartsWith("\"path\"")) continue;
                    var parts = t.Split('"', StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length >= 3) libs.Add(parts[^1].Replace(@"\\", @"\"));
                }
        }
        foreach (var lib in libs.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var dir = Path.Combine(lib, "steamapps", "common", "MultiVersus");
            if (File.Exists(Path.Combine(dir, ExeRel))) return dir;
        }
        return null;
    }

    public static bool LooksLikeGame(string dir) => File.Exists(Path.Combine(dir, ExeRel));
    public static string PaksDir(string game) => Path.Combine(game, "MultiVersus", "Content", "Paks");
    public static string PluginsDir(string game) => Path.Combine(game, "MultiVersus", "Binaries", "Win64", "plugins");
    static readonly string? TestPaks = Environment.GetEnvironmentVariable("BETAFEEL_PAKS_DIR");
    public static string OverrideDir() => TestPaks ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MultiVersus", "Saved", "Paks");
    public static bool GameRunning() => TestPaks == null && Process.GetProcessesByName("MultiVersus-Win64-Shipping").Length > 0;

    public static Recipe LoadRecipe(string file) =>
        JsonSerializer.Deserialize<Recipe>(File.ReadAllText(file)) ?? throw new Exception("recipe.json could not be read");

    // ---- Building the pak ----
    // Reads each asset from the player's own game files, checks it is the build the recipe was made for, writes our
    // bytes at the recipe's offsets, checks the result, and packs everything with the same writer the tools use.
    public static void Build(string game, string aesKey, Recipe recipe, string outDir, Action<string> log)
    {
        var provider = new DefaultFileProvider(PaksDir(game), SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE5_1));
        provider.Initialize();
        provider.SubmitKey(new FGuid(), new FAesKey(aesKey));
        provider.PostMount();
        log($"game files mounted: {provider.Files.Count}");
        if (provider.Files.Count < 1000) throw new Exception("the game's files did not open (wrong key?)");

        var packages = new List<IoStore.Package>();
        var staged = new Dictionary<string, (string key, byte[] data)>(StringComparer.OrdinalIgnoreCase);
        foreach (var a in recipe.assets)
        {
            var key = provider.Files.Keys.FirstOrDefault(k => string.Equals(k, a.path, StringComparison.OrdinalIgnoreCase))
                      ?? throw new Exception($"not in this install: {a.path}");
            var data = provider.SaveAsset(key);
            if (data.Length != a.size || Hex(SHA256.HashData(data)) != a.sha256)
                throw new Exception($"{Path.GetFileName(a.path)} is not the file this patch was made for (another game version?)");
            // [offset, hex] writes our value; [to, from, length] moves the player's own bytes (read from the
            // original, so the order of the edits does not matter).
            var orig = (byte[])data.Clone();
            foreach (var e in a.edits)
            {
                int at = e[0].GetInt32();
                if (e.Length == 3) Array.Copy(orig, e[1].GetInt32(), data, at, e[2].GetInt32());
                else Convert.FromHexString(e[1].GetString()!).CopyTo(data, at);
            }
            staged[a.path] = (key, data);
        }
        // Launch entries, on top of every asset's edits (a model is read as edited, never as grown).
        var final = staged.ToDictionary(kv => kv.Key, kv => kv.Value.data, StringComparer.OrdinalIgnoreCase);
        int grown = 0;
        foreach (var a in recipe.assets)
            foreach (var en in a.entries ?? new())
            {
                if (!staged.TryGetValue(en.like, out var model)) throw new Exception($"recipe: model {en.like} is not in it");
                final[a.path] = Zen.AddEntry(final[a.path], model.data, en.map, en.key, en.x, en.frames);
                grown++;
            }
        if (grown > 0) log($"launch entries added: {grown}");
        foreach (var a in recipe.assets)
        {
            var (key, _) = staged[a.path];
            var data = final[a.path];
            if (Hex(SHA256.HashData(data)) != a.result) throw new Exception($"{Path.GetFileName(a.path)}: the patched file is not what it should be");
            var entry = (FIoStoreEntry)provider.Files[key];
            var header = entry.IoStoreReader.ContainerHeader!;
            var idx = Array.FindIndex(header.PackageIds, p => p.id == entry.ChunkId.ChunkId);
            if (entry.ChunkId.ChunkType != 1 || idx < 0) throw new Exception($"no package store entry for {a.path}");
            var se = header.StoreEntries[idx];
            packages.Add(new IoStore.Package(key, entry.ChunkId.ChunkId, se.ExportCount, se.ExportBundleCount, se.ImportedPackages.Select(p => p.id).ToArray(), data));
        }
        // The same order as the tools use (they pack a folder in ordinal path order), so the pak comes out identical.
        packages.Sort((x, y) => string.CompareOrdinal(x.Path.Replace('\\', '/'), y.Path.Replace('\\', '/')));
        IoStore.Write(outDir, recipe.name, packages);
        log($"built {recipe.name}: {packages.Count} assets");
    }

    // ---- Installing and removing ----
    // Two parts, each patched and unpatched on its own (user, 2026-11-22):
    //   Gameplay   the BetaFeel_* pak in Saved\Paks and MvsPakLoader.asi/.ini: the game plays like the beta. This is
    //              what the fingerprint covers. Offline only (no online play supports it): while it is on,
    //              OpenVersus is turned off (DisableOpenVersus / RestoreOpenVersus below).
    //   Smoothing  BetaFeel.asi/.ini: only how frames are drawn (nothing in the simulation changes), so it can stay
    //              on while playing others, OpenVersus included (it leaves OpenVersus alone).
    // Both need the ASI loader; it is installed with whichever comes first and removed (if this program put it there)
    // with whichever goes last.
    public enum Part { Gameplay, Smoothing }
    public enum PartState { Off, On, Outdated, Incomplete }
    static readonly string[] GameplayPlugins = { "MvsPakLoader.asi", "MvsPakLoader.ini" };

    public static void InstallGameplay(string game, string builtDir, string name, string pluginSrc, Action<string> log)
    {
        if (GameRunning()) throw new Exception("close the game first");
        var dst = OverrideDir();
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.GetFiles(dst, "BetaFeel_*")) { File.Delete(f); log($"removed {Path.GetFileName(f)}"); }
        foreach (var f in Directory.GetFiles(builtDir, name + ".*")) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
        log($"pak installed in {dst}");
        var owned = Owned(game);
        // Ours by definition: replaced every time (the gameplay plugin and its settings must match the fingerprint).
        foreach (var f in GameplayPlugins) CopyOurs(game, pluginSrc, f, owned, log);
        InstallLoader(game, pluginSrc, owned, log);
        SaveOwned(game, owned);
        DisableOpenVersus(game, log);
    }

    // ---- OpenVersus while Beta gameplay is on ----
    // Beta gameplay must never reach OpenVersus's servers (user, 2026-11-23): everyone there plays the game as
    // released. So patching it turns OpenVersus off and unpatching turns it back on. Off = its plugin
    // (OpenVersus*.asi, wherever the ASI loader looks) and its paks (OVS_* in Saved\Paks, which our pak loader would
    // otherwise mount too) renamed with OffSuffix: nothing is deleted, and the name says who did it.
    public const string OffSuffix = ".mvspatcher-off";
    static readonly EnumerationOptions Deep = new() { RecurseSubdirectories = true, IgnoreInaccessible = true, MatchCasing = MatchCasing.CaseInsensitive };
    static readonly EnumerationOptions Flat = new() { IgnoreInaccessible = true, MatchCasing = MatchCasing.CaseInsensitive };

    // Files of OpenVersus's matching a pattern, in every place the ASI loader loads from, and in Saved\Paks.
    static IEnumerable<string> OpenVersusFiles(string game, string asiPattern, string pakPattern)
    {
        var win64 = Path.GetDirectoryName(PluginsDir(game))!;
        IEnumerable<string> Find(string dir, string pattern, EnumerationOptions o) =>
            Directory.Exists(dir) ? Directory.EnumerateFiles(dir, pattern, o) : Enumerable.Empty<string>();
        return Find(win64, asiPattern, Flat)
            .Concat(Find(PluginsDir(game), asiPattern, Deep))
            .Concat(Find(Path.Combine(win64, "scripts"), asiPattern, Deep))
            .Concat(Find(OverrideDir(), pakPattern, Flat))
            .ToList();
    }

    // Plugins that are not this program's and would load at the next start (OpenVersus or anyone else's).
    static List<string> OtherPlugins(string game)
    {
        var plugins = PluginsDir(game);
        var win64 = Path.GetDirectoryName(plugins)!;
        IEnumerable<string> Find(string dir, EnumerationOptions o) =>
            Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "*.asi", o) : Enumerable.Empty<string>();
        string[] mine = { Path.Combine(plugins, "BetaFeel.asi"), Path.Combine(plugins, "MvsPakLoader.asi") };
        return Find(win64, Flat).Concat(Find(plugins, Deep)).Concat(Find(Path.Combine(win64, "scripts"), Deep))
            .Where(f => f.EndsWith(".asi", StringComparison.OrdinalIgnoreCase) && !mine.Contains(f, StringComparer.OrdinalIgnoreCase))
            .ToList();
    }

    // OpenVersus files that would load at the next start.
    public static List<string> OpenVersusActive(string game) => OpenVersusFiles(game, "OpenVersus*.asi", "OVS_*")
        .Where(f => !f.EndsWith(OffSuffix, StringComparison.OrdinalIgnoreCase)
                 && (Path.GetFileName(f).StartsWith("OVS_", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".asi", StringComparison.OrdinalIgnoreCase)))
        .ToList();

    // OpenVersus files this program turned off.
    public static List<string> OpenVersusOff(string game) => OpenVersusFiles(game, "OpenVersus*.asi" + OffSuffix, "OVS_*" + OffSuffix).ToList();

    public static void DisableOpenVersus(string game, Action<string> log)
    {
        var on = OpenVersusActive(game);
        // a newer copy (OpenVersus reinstalled or updated meanwhile) replaces an older turned-off one
        foreach (var f in on) File.Move(f, f + OffSuffix, true);
        if (on.Count > 0) log($"OpenVersus turned off while beta gameplay is patched ({on.Count} files: {string.Join(", ", on.Select(Path.GetFileName))})");
    }

    public static void RestoreOpenVersus(string game, Action<string> log)
    {
        int n = 0;
        // OpenVersus installed again meanwhile (any version): that install stays, and no old plugin comes back beside
        // it, or two OpenVersus versions would load. Its turned-off copies are left as they are.
        // Likewise, of several turned-off plugins (OpenVersus reinstalled as a newer version and turned off again)
        // only the newest comes back: its file name carries the version (OpenVersus_2026.11.10.04.asi).
        bool ovsPluginOn = OpenVersusActive(game).Any(f => f.EndsWith(".asi", StringComparison.OrdinalIgnoreCase));
        var off = OpenVersusOff(game);
        var newestPlugin = off.Where(f => f.EndsWith(".asi" + OffSuffix, StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(f => Path.GetFileName(f), StringComparer.OrdinalIgnoreCase).ThenByDescending(File.GetLastWriteTimeUtc)
            .FirstOrDefault();
        foreach (var f in off)
        {
            var orig = f[..^OffSuffix.Length];
            bool plugin = orig.EndsWith(".asi", StringComparison.OrdinalIgnoreCase);
            if (File.Exists(orig) || (plugin && (ovsPluginOn || f != newestPlugin)))
            {
                log($"left {Path.GetFileName(f)}: OpenVersus has a newer {(plugin ? "plugin" : Path.GetFileName(orig))} there");
                continue;
            }
            File.Move(f, orig); n++;
        }
        if (n > 0) log($"OpenVersus turned back on ({n} files)");
    }

    public static void InstallSmoothing(string game, string pluginSrc, Action<string> log)
    {
        if (GameRunning()) throw new Exception("close the game first");
        var plugins = PluginsDir(game);
        var owned = Owned(game);
        CopyOurs(game, pluginSrc, "BetaFeel.asi", owned, log);
        // The player's own smoothing settings are kept (the helper's switches write them).
        var bfIni = Path.Combine(plugins, "BetaFeel.ini");
        if (!File.Exists(bfIni) && File.Exists(Path.Combine(pluginSrc, "BetaFeel.ini"))) { File.Copy(Path.Combine(pluginSrc, "BetaFeel.ini"), bfIni); owned.Add(bfIni); log("BetaFeel.ini installed"); }
        // BetaFeel.asi replaces the two smoothing plugins it was made from; they must not load beside it. Their
        // settings files stay unless this program put them there.
        foreach (var old in new[] { "FighterSmooth", "PoseSmooth" })
        {
            var asi = Path.Combine(plugins, old + ".asi");
            if (File.Exists(asi)) { File.Delete(asi); owned.Remove(asi); log($"{old}.asi removed (now part of BetaFeel.asi)"); }
            var ini = Path.Combine(plugins, old + ".ini");
            if (owned.Contains(ini)) { if (File.Exists(ini)) File.Delete(ini); owned.Remove(ini); }
        }
        InstallLoader(game, pluginSrc, owned, log);
        SaveOwned(game, owned);
    }

    // Both parts (the command line's --install).
    public static void Install(string game, string builtDir, string name, string pluginSrc, Action<string> log)
    {
        InstallGameplay(game, builtDir, name, pluginSrc, log);
        InstallSmoothing(game, pluginSrc, log);
    }

    static void CopyOurs(string game, string pluginSrc, string f, HashSet<string> owned, Action<string> log)
    {
        var src = Path.Combine(pluginSrc, f);
        if (!File.Exists(src)) return;
        var plugins = PluginsDir(game);
        Directory.CreateDirectory(plugins);
        var dstf = Path.Combine(plugins, f);
        if (!File.Exists(dstf)) owned.Add(dstf);
        File.Copy(src, dstf, true); log($"{f} installed");
    }

    // The ASI loader (Ultimate ASI Loader, MIT) only where there is none yet.
    static void InstallLoader(string game, string pluginSrc, HashSet<string> owned, Action<string> log)
    {
        var win64 = Path.GetDirectoryName(PluginsDir(game))!;
        var loaderSrc = Path.Combine(Path.GetDirectoryName(pluginSrc)!, "loader");
        var existing = LoaderNames.FirstOrDefault(n => File.Exists(Path.Combine(win64, n)));
        if (existing != null) log($"ASI loader already there ({existing}); left as is");
        else if (File.Exists(Path.Combine(loaderSrc, "xinput1_3.dll")))
        {
            foreach (var f in new[] { "xinput1_3.dll", "LICENSE_Ultimate_ASI_Loader.txt" })
            {
                var dstf = Path.Combine(win64, f);
                if (File.Exists(dstf)) continue;
                File.Copy(Path.Combine(loaderSrc, f), dstf); owned.Add(dstf);
            }
            log("ASI loader installed (xinput1_3.dll, Ultimate ASI Loader)");
        }
        else log("warning: no ASI loader beside the game exe and none bundled; the plugins will not load");
    }

    public static bool HasLoader(string game) => LoaderNames.Any(n => File.Exists(Path.Combine(Path.GetDirectoryName(PluginsDir(game))!, n)));

    // Names an ASI loader goes by beside the exe.
    static readonly string[] LoaderNames = { "xinput1_3.dll", "dinput8.dll", "version.dll", "winmm.dll", "dsound.dll", "d3d11.dll", "winhttp.dll" };
    // Files the helper created (not ones that were there before it), so Unpatch removes only those.
    static string OwnedFile(string game) => Path.Combine(PluginsDir(game), "BetaFeel.installed.txt");
    static HashSet<string> Owned(string game) =>
        File.Exists(OwnedFile(game)) ? new(File.ReadAllLines(OwnedFile(game)).Where(l => l.Length > 0), StringComparer.OrdinalIgnoreCase) : new(StringComparer.OrdinalIgnoreCase);
    static void SaveOwned(string game, HashSet<string> owned)
    {
        if (owned.Count > 0) File.WriteAllLines(OwnedFile(game), owned.OrderBy(x => x, StringComparer.Ordinal));
        else if (File.Exists(OwnedFile(game))) File.Delete(OwnedFile(game));
    }

    public static void RemoveGameplay(string game, Action<string> log)
    {
        if (GameRunning()) throw new Exception("close the game first");
        var dst = OverrideDir();
        if (Directory.Exists(dst))
            foreach (var f in Directory.GetFiles(dst, "BetaFeel_*")) { File.Delete(f); log($"removed {Path.GetFileName(f)}"); }
        var owned = Owned(game);
        foreach (var f in GameplayPlugins) RemoveFile(Path.Combine(PluginsDir(game), f), owned, log);
        // OpenVersus back first, so the loader check below sees it.
        RestoreOpenVersus(game, log);
        RemoveLoaderIfUnused(game, owned, log);
        SaveOwned(game, owned);
    }

    public static void RemoveSmoothing(string game, Action<string> log)
    {
        if (GameRunning()) throw new Exception("close the game first");
        var owned = Owned(game);
        RemoveFile(Path.Combine(PluginsDir(game), "BetaFeel.asi"), owned, log);
        // The settings file only if this program made it (a player's own edits otherwise stay for next time).
        var ini = Path.Combine(PluginsDir(game), "BetaFeel.ini");
        if (owned.Contains(ini)) RemoveFile(ini, owned, log);
        RemoveLoaderIfUnused(game, owned, log);
        SaveOwned(game, owned);
    }

    // Both parts (the command line's --remove).
    public static void Remove(string game, Action<string> log)
    {
        RemoveGameplay(game, log);
        RemoveSmoothing(game, log);
    }

    static void RemoveFile(string p, HashSet<string> owned, Action<string> log)
    {
        if (File.Exists(p)) { File.Delete(p); log($"removed {Path.GetFileName(p)}"); }
        owned.Remove(p);
    }

    // The loader goes (if this program put it there) once neither part needs it, and only if no other plugin does:
    // OpenVersus installed afterwards (by hand or by MVS Buddy, which also puts its own copy over ours) loads through
    // the same xinput1_3.dll. Then the loader stays and is no longer counted as ours.
    static void RemoveLoaderIfUnused(string game, HashSet<string> owned, Action<string> log)
    {
        var plugins = PluginsDir(game);
        if (File.Exists(Path.Combine(plugins, "MvsPakLoader.asi")) || File.Exists(Path.Combine(plugins, "BetaFeel.asi"))) return;
        var win64 = Path.GetDirectoryName(plugins)!;
        var others = OtherPlugins(game);
        if (others.Count > 0)
        {
            foreach (var f in new[] { "xinput1_3.dll", "LICENSE_Ultimate_ASI_Loader.txt" })
                if (owned.Remove(Path.Combine(win64, f)) && f.EndsWith(".dll"))
                    log($"left xinput1_3.dll: other plugins load through it ({string.Join(", ", others.Select(Path.GetFileName))})");
            return;
        }
        foreach (var f in new[] { "xinput1_3.dll", "LICENSE_Ultimate_ASI_Loader.txt" })
        {
            var p = Path.Combine(win64, f);
            if (owned.Contains(p)) RemoveFile(p, owned, log);
        }
    }

    // ---- What is installed ----
    // On: every file there and the same as the bundled one. Outdated: there, but from another build (Patch updates
    // it; for smoothing also the old FighterSmooth/PoseSmooth). Incomplete: some of the part's files are missing.
    public static PartState State(string game, Part part, string recipeName, string pluginSrc)
    {
        var plugins = PluginsDir(game);
        bool Same(string f) => File.Exists(Path.Combine(pluginSrc, f)) && FileEq(Path.Combine(plugins, f), Path.Combine(pluginSrc, f));
        if (part == Part.Gameplay)
        {
            var dst = OverrideDir();
            var paks = Directory.Exists(dst) ? Directory.GetFiles(dst, "BetaFeel_*.pak") : Array.Empty<string>();
            bool asi = File.Exists(Path.Combine(plugins, "MvsPakLoader.asi"));
            if (paks.Length == 0 && !asi) return PartState.Off;
            if (paks.Length == 0 || !asi) return PartState.Incomplete;
            bool current = paks.Length == 1 && Path.GetFileNameWithoutExtension(paks[0]) == recipeName;
            return current && GameplayPlugins.All(Same) ? PartState.On : PartState.Outdated;
        }
        bool bf = File.Exists(Path.Combine(plugins, "BetaFeel.asi"));
        bool old = File.Exists(Path.Combine(plugins, "FighterSmooth.asi")) || File.Exists(Path.Combine(plugins, "PoseSmooth.asi"));
        if (!bf) return old ? PartState.Outdated : PartState.Off;
        return Same("BetaFeel.asi") && !old ? PartState.On : PartState.Outdated;
    }

    static bool FileEq(string a, string b)
    {
        try
        {
            var fa = new FileInfo(a); var fb = new FileInfo(b);
            return fa.Exists && fb.Exists && fa.Length == fb.Length && File.ReadAllBytes(a).AsSpan().SequenceEqual(File.ReadAllBytes(b));
        }
        catch { return false; }
    }

    // ---- The fingerprint (same rules as fingerprint.js) ----
    public static string Fingerprint(string game)
    {
        var parts = new List<string>();
        var dst = OverrideDir();
        var paks = Directory.Exists(dst) ? Directory.GetFiles(dst, "BetaFeel_*").OrderBy(f => Path.GetFileName(f), StringComparer.Ordinal).ToList() : new();
        foreach (var f in paks) parts.Add($"pak {Path.GetFileName(f)}:{Hex(SHA256.HashData(File.ReadAllBytes(f)))}");
        if (paks.Count == 0) parts.Add("pak:none");
        var asi = Path.Combine(PluginsDir(game), "MvsPakLoader.asi");
        parts.Add("MvsPakLoader.asi:" + (File.Exists(asi) ? Hex(SHA256.HashData(File.ReadAllBytes(asi))) : "none"));
        var ini = Path.Combine(PluginsDir(game), "MvsPakLoader.ini");
        if (File.Exists(ini))
        {
            string section = ""; var keys = new List<string>();
            foreach (var line in File.ReadAllLines(ini))
            {
                var t = line.Split(';')[0].Trim();
                if (t.Length == 0) continue;
                if (t.StartsWith('[') && t.EndsWith(']')) { section = t[1..^1].ToLowerInvariant(); continue; }
                var eq = t.IndexOf('=');
                if (eq > 0 && section == "movement") keys.Add($"{t[..eq].Trim().ToLowerInvariant()}={t[(eq + 1)..].Trim()}");
            }
            keys.Sort(StringComparer.Ordinal);
            parts.Add("MvsPakLoader.ini [Movement]:" + (keys.Count > 0 ? string.Join(";", keys) : "empty"));
        }
        else parts.Add("MvsPakLoader.ini:none");
        var h = Hex(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join("\n", parts)))).ToUpperInvariant();
        return $"{h[..4]}-{h[4..8]}-{h[8..12]}";
    }

    public static string Hex(byte[] b) => Convert.ToHexString(b).ToLowerInvariant();
}
