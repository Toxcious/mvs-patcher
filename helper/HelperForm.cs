// The helper's window. Home: the game (find, play), then the two parts each with Patch / Unpatch: Client side
// (BetaFeel.asi: smoothing, fast start, hitboxes) first, Beta gameplay (tagged EXPERIMENTAL) last. Activity: the log.
// About: the game's folders. Look: Ui.cs.
sealed class HelperForm : Form
{
    string game = "";
    readonly Patcher.Recipe? recipe;
    readonly string recipeError = "";
    readonly PluginLink link = new();
    readonly System.Windows.Forms.Timer timer = new() { Interval = 500 };
    bool busy, syncing, wasRunning, procRunning;
    uint seenSeq;
    int tick;
    Patcher.PartState gpState, smState;
    int ovsActive, ovsOff;   // OpenVersus files that would load / that Beta gameplay turned off
    string? gpError, smError;

    // pages
    readonly Panel content = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly Dictionary<NavButton, Control> pages = new();

    // game card
    readonly StatusLine gameStatus = new();
    readonly Label gamePath = Theme.Text_("", Theme.Small, Theme.Muted);
    readonly FlatBtn playBtn = new("Play", true);
    // beta gameplay card
    readonly StatusLine gpStatus = new();
    readonly WrapLabel gpNote = Theme.Wrap("");
    readonly FlatBtn gpPatch = new("Patch", true), gpUnpatch = new("Unpatch");
    readonly BusyBar gpBusy = new();
    // smoothing card
    readonly StatusLine smStatus = new();
    readonly WrapLabel smNote = Theme.Wrap("");
    readonly FlatBtn smPatch = new("Patch", true), smUnpatch = new("Unpatch");
    readonly BusyBar smBusy = new();
    readonly Toggle smoothToggle = new(), hotkeyToggle = new(), fastToggle = new(), hitboxToggle = new();
    readonly Label hitboxLabel = Theme.Text_("Hitboxes", Theme.Strong);
    readonly Label smoothLabel = Theme.Text_("Smoothing", Theme.Strong), hotkeyLabel = Theme.Text_("", Theme.Body),
                   fastLabel = Theme.Text_("Fast start", Theme.Strong);
    readonly WrapLabel fastNote = Theme.Wrap("");
    // frame rate: none (the screen's own rate), or 2, 3 or 4 pictures for each of the game's 60 steps a second
    static readonly uint[] CapValues = { 0, 120, 180, 240 };
    readonly FlatBtn[] capBtns = { new("Screen rate"), new("2x \u00b7 120"), new("3x \u00b7 180"), new("4x \u00b7 240") };
    readonly WrapLabel capNote = Theme.Wrap("", null, Theme.Muted);
    uint cap;
    int screenHz;
    readonly WrapLabel liveLabel = Theme.Wrap("");
    // activity
    readonly TextBox logBox = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, BorderStyle = BorderStyle.None, BackColor = Theme.Card, ForeColor = Theme.Text, Font = Theme.Log };
    // about
    readonly WrapLabel aboutGame = Theme.Wrap("");

    public HelperForm()
    {
        Text = "MVS Patcher";
        Icon = Theme.AppIcon();
        BackColor = Theme.Bg; ForeColor = Theme.Text; Font = Theme.Body;
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(900, 800);
        MinimumSize = new Size(760, 560);
        StartPosition = FormStartPosition.CenterScreen;
        try { recipe = Patcher.LoadRecipe(Program.Dir("recipe.json")); }
        catch (Exception e) { recipeError = "recipe.json is missing or broken beside MVSPatcher.exe (" + e.Message + ")"; }

        // ---- sidebar ----
        var side = new Panel { Dock = DockStyle.Left, Width = 200, BackColor = Theme.Side, Padding = new Padding(0, 0, 0, 12) };
        var brand = new Panel { Dock = DockStyle.Top, Height = 84, BackColor = Theme.Side };
        var logo = new PictureBox { Image = Icon.ToBitmap(), SizeMode = PictureBoxSizeMode.Zoom, Size = new Size(32, 32), Location = new Point(22, 26), BackColor = Theme.Side };
        var name = new Label { Text = "MVS Patcher", Font = Theme.Brand, ForeColor = Theme.Text, AutoSize = true, Location = new Point(62, 22), BackColor = Theme.Side };
        var sub = new Label { Text = "MultiVersus helper", Font = Theme.Small, ForeColor = Theme.Muted, AutoSize = true, Location = new Point(64, 50), BackColor = Theme.Side };
        brand.Controls.AddRange(new Control[] { logo, name, sub });
        var ver = new Label { Text = "v" + Application.ProductVersion + " · game build 17784632", Font = Theme.Small, ForeColor = Theme.Off, Dock = DockStyle.Bottom, Height = 24, TextAlign = ContentAlignment.MiddleLeft, Padding = new Padding(22, 0, 0, 0), BackColor = Theme.Side };
        var navHome = new NavButton("Home", "");
        var navLog = new NavButton("Activity", "");
        var navAbout = new NavButton("About", "");
        // Dock=Top stacks in reverse order of adding
        side.Controls.Add(navAbout); side.Controls.Add(navLog); side.Controls.Add(navHome);
        side.Controls.Add(new Panel { Dock = DockStyle.Top, Height = 8, BackColor = Theme.Side });
        side.Controls.Add(brand);
        side.Controls.Add(ver);

        pages[navHome] = HomePage();
        pages[navLog] = ActivityPage();
        pages[navAbout] = AboutPage();
        foreach (var (btn, page) in pages) { btn.Click += (_, _) => Show(btn); page.Visible = false; content.Controls.Add(page); }
        Controls.Add(content);
        Controls.Add(side);
        Show(navHome);

        HandleCreated += (_, _) => Theme.DarkTitleBar(this);
        Load += (_, _) => { FindGame(false); Refresh_(true); };
        timer.Tick += (_, _) => Refresh_(false);
        timer.Start();
        FormClosed += (_, _) => { timer.Stop(); link.Dispose(); };
    }

    void Show(NavButton which)
    {
        foreach (var (btn, page) in pages) { btn.Selected = btn == which; page.Visible = btn == which; }
    }

    // A scrolling page with a title, a line under it and a stack of cards.
    static (Panel page, VStack stack) Page(string title, string? subtitle)
    {
        var page = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.Bg, Padding = new Padding(28, 22, 28, 8) };
        var stack = new VStack { Dock = DockStyle.Top };
        stack.Add(Theme.Text_(title, Theme.PageTitle));
        if (subtitle != null) { var s = Theme.Text_(subtitle, Theme.Body, Theme.Muted); s.Margin = new Padding(0, 0, 0, 16); stack.Add(s); }
        else stack.Controls[^1].Margin = new Padding(0, 0, 0, 16);
        page.Controls.Add(stack);
        return (page, stack);
    }

    static FlowLayoutPanel Flow(params Control[] c)
    {
        var f = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = true, Margin = new Padding(0), Padding = new Padding(0), BackColor = Color.Transparent };
        f.Controls.AddRange(c);
        return f;
    }

    static Label Gap(int h) => new() { AutoSize = false, Height = h, Width = 1, Margin = new Padding(0), BackColor = Color.Transparent };

    Control HomePage()
    {
        var (page, stack) = Page("Home", "Make MultiVersus play and look like the 2023 beta.");

        // The game
        var gameCard = new Card("Game");
        var change = new LinkText("Change folder");
        change.Click += (_, _) => FindGame(true);
        var left = new VStack();
        left.Add(gameStatus, Flow(gamePath, change));
        playBtn.Padding = new Padding(30, 9, 30, 9);
        playBtn.Margin = new Padding(16, 0, 0, 0);
        gameCard.Add(new Split(left, playBtn));
        stack.Add(gameCard);

        // Client side (BetaFeel.asi)
        var sm = new Card("Client side");
        sm.Add(smStatus);
        sm.Add(Theme.Wrap("Changes how the game looks and starts, never how it plays, so it can stay on when you play " +
            "on OpenVersus or with players who do not have it."));
        smoothLabel.Margin = hotkeyLabel.Margin = fastLabel.Margin = new Padding(0, 4, 0, 0);
        smoothToggle.Margin = hotkeyToggle.Margin = fastToggle.Margin = new Padding(0, 2, 12, 0);
        var row1 = Flow(smoothToggle, smoothLabel); row1.Margin = new Padding(0, 14, 0, 0);
        var smDesc = Theme.Wrap("Fighters, camera, poses and effects drawn between the game's 60 steps a second, so " +
            "motion is smooth at any frame rate. Switches at once, even in a match.", null, Theme.Muted);
        smDesc.Margin = new Padding(56, 0, 0, 0);
        var row2 = Flow(hotkeyToggle, hotkeyLabel); row2.Margin = new Padding(0, 6, 0, 0);
        var capCap = Theme.Text_("Frame rate", Theme.Small, Theme.Muted); capCap.Margin = new Padding(0, 16, 0, 0);
        var rowCap = Flow(capBtns); rowCap.Margin = new Padding(0, 2, 0, 0);
        capNote.Margin = new Padding(0, 6, 0, 0);
        for (int i = 0; i < capBtns.Length; i++)
        {
            uint v = CapValues[i];
            capBtns[i].Click += (_, _) => SetCap(v);
        }
        var row3 = Flow(fastToggle, fastLabel); row3.Margin = new Padding(0, 14, 0, 0);
        var fastDesc = Theme.Wrap("Skips the intro and the \"Enter MultiVerse!\" screen: the game opens on the main " +
            "menu. Applies the next time the game starts. A first start's EULA is never skipped.", null, Theme.Muted);
        fastDesc.Margin = fastNote.Margin = new Padding(56, 0, 0, 0);
        // lab tools
        hitboxLabel.Margin = new Padding(0, 4, 0, 0);
        hitboxToggle.Margin = new Padding(0, 2, 12, 0);
        var labCap = Theme.Text_("Lab tools", Theme.Small, Theme.Muted); labCap.Margin = new Padding(0, 16, 0, 0);
        var row4 = Flow(hitboxToggle, hitboxLabel); row4.Margin = new Padding(0, 6, 0, 0);
        var hitDesc = Theme.Wrap("Draws the game's own collision boxes in every match: red hits, blue hurtboxes, " +
            "projectiles and shields. Display only. Switches at once.", null, Theme.Muted);
        hitDesc.Margin = new Padding(56, 0, 0, 0);
        liveLabel.Margin = new Padding(0, 14, 0, 0);
        sm.Add(row1, smDesc, row2, capCap, rowCap, capNote, row3, fastDesc, fastNote, labCap, row4, hitDesc, liveLabel, smBusy, Flow(smPatch, smUnpatch), smNote);
        stack.Add(sm);

        // Beta gameplay (last: experimental)
        var gp = new Card("Beta gameplay", "EXPERIMENTAL");
        gp.Add(gpStatus);
        var gpDesc = Theme.Wrap("Movement, hitstun and move timing from the 2023 beta. Offline only, for the Lab and " +
            "playing on your own: it changes the game itself, so it cannot be played online. While it is patched, " +
            "OpenVersus is turned off so the game cannot join its servers; Unpatch turns OpenVersus back on.");
        gpDesc.Margin = new Padding(0, 0, 0, 12);
        gp.Add(gpDesc, gpBusy, Flow(gpPatch, gpUnpatch), gpNote);
        stack.Add(gp);

        playBtn.Click += (_, _) =>
        {
            // OpenVersus reinstalled or updated itself while beta gameplay is patched: off again before the game starts
            if (HaveGame && Patcher.State(game, Patcher.Part.Gameplay, recipe?.name ?? "", Program.Dir("plugins")) != Patcher.PartState.Off)
                try { Patcher.DisableOpenVersus(game, Log); }
                catch (Exception e) { Log("error: could not turn OpenVersus off (" + e.Message + "); not starting the game"); return; }
                finally { Refresh_(true); }
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("steam://rungameid/" + Patcher.SteamAppId) { UseShellExecute = true });
            Log("starting the game through Steam");
        };
        gpPatch.Click += async (_, _) => await Run(Patcher.Part.Gameplay, "Patching beta gameplay", () =>
        {
            var tmp = Path.Combine(Path.GetTempPath(), "BetaFeel_build");
            if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
            var key = ExeKey.FromGame(game);
            Log("game key read from the game's exe");
            Patcher.Build(game, key, recipe!, tmp, Log);
            Patcher.InstallGameplay(game, tmp, recipe!.name, Program.Dir("plugins"), Log);
            Directory.Delete(tmp, true);
        });
        gpUnpatch.Click += async (_, _) => await Run(Patcher.Part.Gameplay, "Unpatching beta gameplay", () => Patcher.RemoveGameplay(game, Log));
        smPatch.Click += async (_, _) => await Run(Patcher.Part.Smoothing, "Patching the client side", () => Patcher.InstallSmoothing(game, Program.Dir("plugins"), Log));
        smUnpatch.Click += async (_, _) => await Run(Patcher.Part.Smoothing, "Unpatching the client side", () => Patcher.RemoveSmoothing(game, Log));
        smoothToggle.CheckedChanged += (_, _) => { if (!syncing) SetSwitches(PluginLink.Smoothing, smoothToggle.Checked, "smoothing"); };
        hotkeyToggle.CheckedChanged += (_, _) => { if (!syncing) SetSwitches(new[] { PluginLink.Switch.Hotkeys }, hotkeyToggle.Checked, "in-game hotkey"); };
        smoothLabel.Click += (_, _) => { if (smoothToggle.Enabled) smoothToggle.Checked = !smoothToggle.Checked; };
        hotkeyLabel.Click += (_, _) => { if (hotkeyToggle.Enabled) hotkeyToggle.Checked = !hotkeyToggle.Checked; };
        fastToggle.CheckedChanged += (_, _) => { if (!syncing) SetSwitches(new[] { PluginLink.Switch.FastStart }, fastToggle.Checked, "fast start (from the next game start)"); };
        fastLabel.Click += (_, _) => { if (fastToggle.Enabled) fastToggle.Checked = !fastToggle.Checked; };
        hitboxToggle.CheckedChanged += (_, _) => { if (!syncing) SetLab(PluginLink.Lab.Hitboxes, hitboxToggle.Checked, "hitboxes"); };
        hitboxLabel.Click += (_, _) => { if (hitboxToggle.Enabled) hitboxToggle.Checked = !hitboxToggle.Checked; };
        return page;
    }

    Control ActivityPage()
    {
        var (page, stack) = Page("Activity", "What MVS Patcher did this session.");
        page.AutoScroll = false;
        var card = new Card { Manual = true, Dock = DockStyle.Fill, Padding = new Padding(14) };
        card.Controls.Add(logBox);
        var copy = new FlatBtn("Copy all");
        copy.Click += (_, _) => { if (logBox.TextLength > 0) Clipboard.SetText(logBox.Text); };
        var bar = Flow(copy); bar.Dock = DockStyle.Bottom; bar.Padding = new Padding(0, 10, 0, 0);
        stack.Dock = DockStyle.Top;
        page.Controls.Add(card);
        page.Controls.Add(bar);
        page.Controls.SetChildIndex(card, 0);
        return page;
    }

    Control AboutPage()
    {
        var (page, stack) = Page("About", null);
        var where = new Card("Your game");
        where.Add(aboutGame);
        var open1 = new FlatBtn("Open game folder");
        var open2 = new FlatBtn("Open plugins folder");
        var open3 = new FlatBtn("Open patch folder");
        open1.Click += (_, _) => Open(game);
        open2.Click += (_, _) => Open(Patcher.PluginsDir(game));
        open3.Click += (_, _) => Open(Patcher.OverrideDir());
        var opens = Flow(open1, open2, open3); opens.Margin = new Padding(0, 8, 0, 0);
        where.Add(opens);
        stack.Add(where);
        return page;
    }

    void Open(string dir)
    {
        if (!Directory.Exists(dir)) { Log("not there yet: " + dir); return; }
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", "\"" + dir + "\"") { UseShellExecute = true });
    }

    void FindGame(bool ask)
    {
        var dir = Patcher.FindGame();
        if (dir == null || ask)
        {
            using var dlg = new FolderBrowserDialog { Description = "The MultiVersus folder (the one with MultiVersus\\Binaries in it)", SelectedPath = dir ?? "", UseDescriptionForTitle = true };
            if (dlg.ShowDialog(this) == DialogResult.OK) dir = dlg.SelectedPath;
            else if (ask) return;
        }
        if (dir != null && Patcher.LooksLikeGame(dir))
        {
            game = dir; Log("game: " + dir);
            Refresh_(true); LoadSwitches(link.Read().Running);
        }
        else Log("game not found; press Change folder and choose its folder");
    }

    bool HaveGame => game.Length > 0 && Patcher.LooksLikeGame(game);

    async Task Run(Patcher.Part part, string what, Action work)
    {
        if (!HaveGame) { Log("find the game first"); return; }
        busy = true;
        if (part == Patcher.Part.Gameplay) gpError = null; else smError = null;
        (part == Patcher.Part.Gameplay ? gpBusy : smBusy).Active = true;
        Refresh_(true);
        Log(what + "...");
        try { await Task.Run(work); Log("done"); }
        catch (Exception e)
        {
            Log("error: " + e.Message);
            if (part == Patcher.Part.Gameplay) gpError = e.Message; else smError = e.Message;
        }
        finally
        {
            busy = false;
            gpBusy.Active = smBusy.Active = false;
            Refresh_(true);
            LoadSwitches(link.Read().Running);
        }
    }

    // ---- smoothing switches ----
    void SetSwitches(PluginLink.Switch[] which, bool on, string what)
    {
        if (!HaveGame) return;
        foreach (var s in which)
        {
            link.Set(s, on);                       // a running game picks it up on its next frame
            PluginLink.IniSet(game, s, on);        // and the next game start
        }
        Log($"{what}: {(on ? "on" : "off")}");
    }

    void SetCap(uint fps)
    {
        if (!HaveGame) return;
        link.SetCap(fps);                          // a running game picks it up within a moment
        PluginLink.IniSetCap(game, fps);           // and the next game start
        cap = fps;
        Log(fps == 0 ? "frame rate: the screen's rate (no cap)" : $"frame rate: capped at {fps} a second ({fps / 60.0:0.#} pictures per game step)");
        Refresh_(false);
    }

    // The primary screen's refresh rate now, 0 if Windows does not say.
    static int ScreenHz()
    {
        var dm = new byte[220];                    // DEVMODEW: dmSize at 68, dmDisplayFrequency at 184
        BitConverter.GetBytes((ushort)220).CopyTo(dm, 68);
        try { return EnumDisplaySettingsW(null, -1, dm) ? BitConverter.ToInt32(dm, 184) : 0; } catch { return 0; }
    }
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string? device, int mode, byte[] devMode);

    void SetLab(PluginLink.Lab bit, bool on, string what)
    {
        if (!HaveGame) return;
        var l = labFromGame ? link.GetLab() : PluginLink.IniGetLab(game);
        l = (on ? l | bit : l & ~bit) & PluginLink.Lab.Hitboxes;   // frame step is not offered: always off
        link.SetLab(l);                            // a running game picks it up on its next frame
        PluginLink.IniSetLab(game, l);             // and the next game start
        Log($"{what}: {(on ? "on" : "off")}");
    }
    bool labFromGame;

    // The switches show what the running game uses, or else what BetaFeel.ini says for its next start. Smoothing is on
    // when all five parts are (a hand-edited ini may have some off; switching it on turns all five on).
    void LoadSwitches(bool fromGame)
    {
        if (!HaveGame) return;
        bool Get(PluginLink.Switch s) => fromGame ? link.Get(s) : PluginLink.IniGet(game, s);
        syncing = true;
        smoothToggle.Checked = PluginLink.Smoothing.All(Get);
        hotkeyToggle.Checked = Get(PluginLink.Switch.Hotkeys);
        fastToggle.Checked = PluginLink.IniGet(game, PluginLink.Switch.FastStart);   // the next start's, always
        labFromGame = fromGame;
        var lab = fromGame ? link.GetLab() : PluginLink.IniGetLab(game);
        hitboxToggle.Checked = lab.HasFlag(PluginLink.Lab.Hitboxes);
        cap = fromGame ? link.GetCap() : PluginLink.IniGetCap(game);
        syncing = false;
        hotkeyLabel.Text = $"{PluginLink.HotkeyName(game)} switches smoothing in game";
    }

    // Twice a second: the game and the plugin; the files every 2 s (or at once when asked, or when the game starts or stops).
    void Refresh_(bool files)
    {
        var st = link.Read();
        bool running = Patcher.GameRunning();
        if (running != procRunning) { procRunning = running; files = true; }
        if (++tick % 4 == 0) files = true;
        if (st.Running != wasRunning || (st.Running && st.SwitchSeq != seenSeq))
        {
            LoadSwitches(st.Running);
            seenSeq = st.SwitchSeq; wasRunning = st.Running;
        }
        if (files && HaveGame && recipe != null)
        {
            gpState = Patcher.State(game, Patcher.Part.Gameplay, recipe.name, Program.Dir("plugins"));
            smState = Patcher.State(game, Patcher.Part.Smoothing, recipe.name, Program.Dir("plugins"));
            try { ovsActive = Patcher.OpenVersusActive(game).Count; ovsOff = Patcher.OpenVersusOff(game).Count; } catch { ovsActive = ovsOff = 0; }
        }

        // game card
        if (!HaveGame) { gameStatus.Set(Theme.Bad, "Game not found"); gamePath.Text = "Press Change folder and choose the MultiVersus folder."; }
        else
        {
            if (!running) gameStatus.Set(Theme.Off, "Not running");
            else if (!st.Running) gameStatus.Set(Theme.Good, "Running");
            else gameStatus.Set(Theme.Good, "Running · " + (st.InMatch ? "in a match" : "in the menus") + (st.Fps is double f ? $" · {f:0} fps" : ""));
            if (gamePath.Text != game) gamePath.Text = game;
        }
        playBtn.Enabled = HaveGame && !running;
        playBtn.Text = running ? "Running" : "Play";

        bool canPatch = HaveGame && !busy && !running;
        string closeFirst = running ? "Close the game to patch or unpatch." : "";

        // beta gameplay card
        if (recipe == null) { gpStatus.Set(Theme.Bad, "Cannot patch"); SetNote(gpNote, recipeError, Theme.Bad); gpPatch.Enabled = gpUnpatch.Enabled = false; }
        else
        {
            if (!HaveGame) gpStatus.Set(Theme.Off, "-");
            else switch (gpState)
            {
                case Patcher.PartState.On: gpStatus.Set(Theme.Good, "Patched"); break;
                case Patcher.PartState.Off: gpStatus.Set(Theme.Off, "Not patched (the game plays as released)"); break;
                case Patcher.PartState.Outdated: gpStatus.Set(Theme.Warn, "An older version is patched: press Update"); break;
                default: gpStatus.Set(Theme.Warn, "Partly patched: press Repair, or Unpatch"); break;
            }
            gpPatch.Text = gpState switch { Patcher.PartState.On => "Patch again", Patcher.PartState.Outdated => "Update", Patcher.PartState.Incomplete => "Repair", _ => "Patch" };
            gpPatch.Enabled = canPatch;
            gpUnpatch.Enabled = canPatch && gpState != Patcher.PartState.Off;
            bool gpIn = gpState != Patcher.PartState.Off;
            if (gpError != null) SetNote(gpNote, "Did not finish: " + gpError, Theme.Bad);
            else if (gpIn && ovsActive > 0)
                SetNote(gpNote, (running ? "OpenVersus is in this game too: close the game now and do not play online. " :
                    "OpenVersus is installed again: Play or Patch again turns it off. ") + closeFirst, Theme.Bad);
            else if (running && gpIn) SetNote(gpNote, closeFirst + " While it is patched, the game is for offline play only.", Theme.Warn);
            else if (gpIn && ovsOff > 0) SetNote(gpNote, "OpenVersus is turned off while this is patched. Unpatch turns it back on.", Theme.Muted);
            else SetNote(gpNote, closeFirst, Theme.Muted);
        }

        // smoothing card
        bool installed = HaveGame && File.Exists(Path.Combine(Patcher.PluginsDir(game), "BetaFeel.asi"));
        if (!HaveGame) smStatus.Set(Theme.Off, "-");
        else switch (smState)
        {
            case Patcher.PartState.On: smStatus.Set(Theme.Good, "Patched"); break;
            case Patcher.PartState.Off: smStatus.Set(Theme.Off, "Not patched"); break;
            default:
                smStatus.Set(Theme.Warn, installed ? "An older version is patched: press Update" : "The old FighterSmooth / PoseSmooth are there: press Update"); break;
        }
        smPatch.Text = smState switch { Patcher.PartState.On => "Patch again", Patcher.PartState.Off => "Patch", _ => "Update" };
        smPatch.Enabled = canPatch;
        smUnpatch.Enabled = canPatch && installed;
        smoothToggle.Enabled = hotkeyToggle.Enabled = fastToggle.Enabled = hitboxToggle.Enabled = installed;
        // frame rate: the chosen cap, and what it means on this screen
        if (files || screenHz == 0) screenHz = ScreenHz();
        for (int i = 0; i < capBtns.Length; i++) { capBtns[i].Enabled = installed; capBtns[i].Primary = installed && cap == CapValues[i]; }
        string hz = screenHz > 0 ? $"{screenHz} Hz" : "refresh rate";
        string capText; Color capColor = Theme.Muted;
        if (!installed) capText = "";
        else if (st.Running && st.Cap == PluginLink.CapState.BadBuild) { capText = "The cap is off in this game: the game version differs."; capColor = Theme.Warn; }
        else if (cap == 0)
            capText = "No cap: with Vertical Sync on in the game's settings, the game draws at your screen's " + hz +
                (screenHz > 0 ? $", {screenHz / 60.0:0.#} pictures for each of its 60 steps a second" : "") +
                ". Smoothing fills in however many your screen shows. Best for most screens.";
        else if (screenHz > 0 && cap > screenHz) { capText = $"{cap} a second is more than your screen shows ({hz}), so the extra pictures are never seen. Screen rate is the better choice here."; capColor = Theme.Warn; }
        else if (screenHz > 0 && cap == screenHz) capText = $"{cap} a second, the same as your screen: {cap / 60.0:0.#} pictures for each game step.";
        else
            capText = $"Capped at {cap} a second: {cap / 60.0:0.#} pictures for each game step. Below your screen's " + hz +
                " this only looks even with G-Sync or FreeSync on; without it, Screen rate is smoother." +
                (Array.IndexOf(CapValues, cap) < 0 ? " (Set in BetaFeel.ini.)" : "");
        if (capNote.Text != capText) capNote.Text = capText;
        capNote.ForeColor = capColor;
        smoothLabel.ForeColor = hotkeyLabel.ForeColor = fastLabel.ForeColor = hitboxLabel.ForeColor = installed ? Theme.Text : Theme.Muted;
        string fast = !st.Running ? "" : st.Fast switch
        {
            PluginLink.FastState.Done => $"This start: main menu {st.MenuMs / 1000.0:0.0} s after launch.",
            PluginLink.FastState.Eula => "This start: stopped at the EULA, which is left to you.",
            PluginLink.FastState.Stopped => "This start: the intro did not go as expected, so it was left alone.",
            PluginLink.FastState.BadBuild => "This start: off (the game version differs).",
            PluginLink.FastState.OtherMod => "This start: off (another mod changed the game code fast start needs).",
            PluginLink.FastState.Waiting or PluginLink.FastState.Skipping => "Skipping the intro...",
            _ => "",
        };
        if (fastNote.Text != fast) fastNote.Text = fast;
        fastNote.ForeColor = st.Fast is PluginLink.FastState.Stopped or PluginLink.FastState.BadBuild or PluginLink.FastState.OtherMod ? Theme.Warn : Theme.Good;
        string live;
        if (!installed) live = "";
        else if (!st.Running) live = running ? "The game was started before this was patched: restart it to use it." : "The switches apply as soon as the game starts.";
        else
        {
            static string H(uint h) => h == 1 ? "working" : h == 2 ? "off (the game version differs)" : "not loaded";
            live = st.FighterHooks == 1 && st.PoseHooks == 1 ? "In the game: smoothing is hooked in and working." :
                   $"In the game: smoothing of positions and camera {H(st.FighterHooks)}; of poses and effects {H(st.PoseHooks)}.";
            if (st.OldPlugins != 0) live += " The old FighterSmooth / PoseSmooth are still there: close the game and press Update.";
        }
        if (liveLabel.Text != live) liveLabel.Text = live;
        if (smError != null) SetNote(smNote, "Did not finish: " + smError, Theme.Bad);
        else if (HaveGame && (gpState != Patcher.PartState.Off || installed) && !Patcher.HasLoader(game)) SetNote(smNote, "No ASI loader beside the game: press Patch again to add it.", Theme.Warn);
        else SetNote(smNote, closeFirst, Theme.Muted);

        aboutGame.Text = HaveGame ? $"Game folder: {game}\r\nPlugins: {Patcher.PluginsDir(game)}\r\nPatch: {Patcher.OverrideDir()}" : "The game has not been found yet.";
    }

    static void SetNote(WrapLabel l, string text, Color c)
    {
        if (l.Text != text) l.Text = text;
        l.ForeColor = c;
    }

    void Log(string s)
    {
        if (InvokeRequired) { BeginInvoke(() => Log(s)); return; }
        logBox.AppendText($"{DateTime.Now:HH:mm:ss}  {s}{Environment.NewLine}");
    }
}
