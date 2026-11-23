// MVS Patcher (2026-11-23; was "BetaFeel", before that "the launcher"; the plugin and pak it installs keep the BetaFeel name). A window to keep open beside the game: it patches and unpatches
// the two parts on their own, starts the game, and switches the
// smoothing plugin in a running game. The window is HelperForm.cs (look: Ui.cs); the work is Patcher.cs; the link to a
// running BetaFeel.asi is PluginLink.cs.
//
//   Beta gameplay  BetaFeel_* pak + MvsPakLoader.asi/.ini: changes the game, so offline only (OpenVersus is
//                  turned off while it is patched); --fingerprint hashes exactly this part.
//   Smoothing      BetaFeel.asi/.ini: changes only what is drawn, so it can stay on when playing others.
//
// Files it expects beside MVSPatcher.exe: recipe.json (from make_pak.js), plugins\MvsPakLoader.asi/.ini,
// plugins\BetaFeel.asi/.ini (see plugin/), loader\xinput1_3.dll (Ultimate ASI Loader; installed only where there is
// no loader yet). It ships no game files: the pak is rebuilt from the player's own install.
//
// THE AES KEY (decided 2026-11-22: the port project's way): read from the player's own game exe at patch time
// (ExeKey.cs), kept in memory only, never shipped or written anywhere.
//
// Command line (for checking without the window):
//   MVSPatcher.exe --build <out dir> [--key-file <json with a "key" field>] [--game <dir>] [--recipe <file>]
//   MVSPatcher.exe --key-check [--key-file <json>] [--game <dir>]   (does the exe's key match the file's; prints no key)
//   MVSPatcher.exe --fingerprint [--game <dir>]
//   MVSPatcher.exe --state [--game <dir>]                            (what is installed, per part)
//   MVSPatcher.exe --install <built dir> | --install-gameplay <built dir> | --install-smoothing   [--game <dir>]
//   MVSPatcher.exe --remove | --remove-gameplay | --remove-smoothing   [--game <dir>]
// For tests only: BETAFEEL_PAKS_DIR=<dir> puts the pak there instead of the player's Saved\Paks (and then does not
// wait for the game to be closed, since the real one is not touched).
using System.Text.Json;

static class Program
{
    public static string Dir(string sub) => Path.Combine(AppContext.BaseDirectory, sub);

    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length > 0) return Cli(args);
        ApplicationConfiguration.Initialize();
        Application.Run(new HelperForm());
        return 0;
    }

    static string? Arg(string[] a, string name) { int i = Array.IndexOf(a, name); return i >= 0 && i + 1 < a.Length ? a[i + 1] : null; }

    static int Cli(string[] a)
    {
        AttachConsole(-1);
        try
        {
            var game = Arg(a, "--game") ?? Patcher.FindGame() ?? throw new Exception("game not found; pass --game <dir>");
            string RecipeName() => Patcher.LoadRecipe(Dir("recipe.json")).name;
            var plugins = Dir("plugins");
            if (a.Contains("--fingerprint")) { Console.WriteLine("gameplay fingerprint: " + Patcher.Fingerprint(game)); return 0; }
            if (a.Contains("--state"))
            {
                foreach (var p in Enum.GetValues<Patcher.Part>()) Console.WriteLine($"{p}: {Patcher.State(game, p, RecipeName(), plugins)}");
                Console.WriteLine("ASI loader: " + (Patcher.HasLoader(game) ? "there" : "none"));
                Console.WriteLine($"OpenVersus: {Patcher.OpenVersusActive(game).Count} files on, {Patcher.OpenVersusOff(game).Count} turned off by this program");
                return 0;
            }
            if (Arg(a, "--install") is string built) { Patcher.Install(game, built, RecipeName(), plugins, Console.WriteLine); return 0; }
            if (Arg(a, "--install-gameplay") is string built2) { Patcher.InstallGameplay(game, built2, RecipeName(), plugins, Console.WriteLine); return 0; }
            if (a.Contains("--install-smoothing")) { Patcher.InstallSmoothing(game, plugins, Console.WriteLine); return 0; }
            if (a.Contains("--remove")) { Patcher.Remove(game, Console.WriteLine); return 0; }
            if (a.Contains("--remove-gameplay")) { Patcher.RemoveGameplay(game, Console.WriteLine); return 0; }
            if (a.Contains("--remove-smoothing")) { Patcher.RemoveSmoothing(game, Console.WriteLine); return 0; }
            var keyFile = Arg(a, "--key-file");
            string? fileKey = keyFile == null ? null : JsonDocument.Parse(File.ReadAllText(keyFile)).RootElement.GetProperty("key").GetString()!;
            if (a.Contains("--key-check"))
            {
                var exeKey = ExeKey.FromGame(game);
                Console.WriteLine("key found in the exe" + (fileKey == null ? "" :
                    string.Equals(exeKey.Replace("0x", ""), fileKey.Replace("0x", ""), StringComparison.OrdinalIgnoreCase) ? "; it matches the key file" : "; it does NOT match the key file"));
                return 0;
            }
            var outDir = Arg(a, "--build") ?? throw new Exception("usage: --build <out dir> [--key-file <file>]");
            var key = fileKey ?? ExeKey.FromGame(game);
            var recipe = Patcher.LoadRecipe(Arg(a, "--recipe") ?? Dir("recipe.json"));
            Patcher.Build(game, key, recipe, outDir, Console.WriteLine);
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine("error: " + e.Message); return 1; }
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll")] static extern bool AttachConsole(int pid);
}
