using System.Diagnostics;
using System.Drawing.Drawing2D;

namespace BrawlRecompiled;

// The launcher window: a dark header with the title, a nav column (Play, Display, Controls, Mods) and one card per
// page. Everything it changes goes to <game folder>\UserData\Config.toml (engine\ in a release, Play\ in the dev
// tree), read by the game at start.
sealed class MainForm : Form
{
    static readonly Color Night = ColorTranslator.FromHtml("#1A1E2C"), Card = ColorTranslator.FromHtml("#262B3C"),
        Snow = ColorTranslator.FromHtml("#F2F2F6"), Dim = ColorTranslator.FromHtml("#B8BCCB"), Orange = ColorTranslator.FromHtml("#FF8C28"),
        Red = ColorTranslator.FromHtml("#D83A3A"), Edge = ColorTranslator.FromHtml("#3C4358"), Hover = ColorTranslator.FromHtml("#343A50"),
        Gold = ColorTranslator.FromHtml("#FFC43A");

    readonly bool selfTest;
    readonly string? screenshotDir;
    // Fonts are in points and grow with Windows display scaling; every pixel size goes through S() so it grows too.
    static float k = 1f;
    static int S(int v) => (int)Math.Round(v * k);
    static Size S(int w, int h) => new(S(w), S(h));
    static Padding S(int l, int t, int r, int b) => new(S(l), S(t), S(r), S(b));
    Button chooseDisc = null!;
    // play = the folder holding the game exe and its UserData: engine\ in a release, Play\ in the dev tree.
    readonly string root, play, gameExe, userData, configPath, modsDir, sdDir, discDir;
    // The one version number: <Version> in BrawlRecompiled.Launcher.csproj (package-brawl.ps1 names the zip after it).
    static readonly string AppVersion = typeof(MainForm).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";
    readonly GameConfig config;
    readonly Panel content = new() { Dock = DockStyle.Fill };
    readonly List<(Button button, Control page)> pages = new();
    Label playStatus = null!, dumpHelp = null!, gameNote = null!, sdStatus = null!;
    ListView modList = null!;
    Func<bool>? cancelPadCapture;  // the Controls page's gamepad capture: Escape ends it

    public MainForm(bool selfTest, string? screenshotDir = null)
    {
        this.selfTest = selfTest;
        this.screenshotDir = screenshotDir;
        k = DeviceDpi / 96f;
        content.Padding = S(16, 12, 20, 16);
        (root, play, gameExe) = FindGame();
        // Zips drop the hidden attribute: keep the release's engine\ folder out of sight so only this exe shows.
        try
        {
            var engineDir = new DirectoryInfo(play);
            if (engineDir.Exists && engineDir.Name.Equals("engine", StringComparison.OrdinalIgnoreCase) && !engineDir.Attributes.HasFlag(FileAttributes.Hidden))
                engineDir.Attributes |= FileAttributes.Hidden;
        }
        catch (Exception) { }  // read-only media or no rights: cosmetic only
        userData = Path.Combine(play, "UserData");
        configPath = Path.Combine(userData, "Config.toml");
        modsDir = Path.Combine(userData, "mods");
        sdDir = Path.Combine(userData, "SD");
        discDir = Path.Combine(root, "Disc", "DATA");
        config = new GameConfig(configPath);
        EnsureDefaults();
        Gamepads.Init(play);

        Text = "Brawl Recompiled  v" + AppVersion;
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);  // the Smash Ball, embedded from smash.ico
        var area = Screen.PrimaryScreen?.WorkingArea.Size ?? new Size(S(1100), S(760));
        ClientSize = new Size(Math.Min(S(1100), area.Width - 40), Math.Min(S(760), area.Height - 60));
        MinimumSize = new Size(Math.Min(S(940), area.Width), Math.Min(S(640), area.Height));
        BackColor = Night;
        ForeColor = Snow;
        Font = new Font("Segoe UI", 10.5f);
        KeyPreview = true;
        StartPosition = FormStartPosition.CenterScreen;
        DoubleBuffered = true;

        var header = new DoubleBufferedPanel { Dock = DockStyle.Top, Height = S(150) };
        header.Paint += PaintHeader;
        var nav = new BufferedFlow { Dock = DockStyle.Left, Width = S(220), FlowDirection = FlowDirection.TopDown, Padding = S(16, 18, 0, 0), WrapContents = false };
        var frame = new System.Windows.Forms.Timer { Interval = 33 };
        frame.Tick += (_, _) =>
        {
            if (!animate || WindowState == FormWindowState.Minimized) return;
            header.Invalidate();
            float glow = (float)(Math.Sin(T * 3) + 1) / 2;  // PLAY breathes between two reds
            playButton.BackColor = Color.FromArgb(Lerp(Red.R, 255, glow), Lerp(Red.G, 100, glow), Lerp(Red.B, 70, glow));
        };
        frame.Start();
        Controls.Add(content);
        Controls.Add(nav);
        Controls.Add(header);

        AddPage(nav, "Play", BuildPlayPage());
        AddPage(nav, "Display", BuildDisplayPage());
        AddPage(nav, "Controls", BuildControlsPage());
        AddPage(nav, "Mods", BuildModsPage());
        ShowPage(pages[0].button);
        Shown += (_, _) =>
        {
            if (screenshotDir != null)
            {
                // Headless visual check: every page drawn to <dir>\<page>.png.
                Directory.CreateDirectory(screenshotDir);
                foreach (var p in pages)
                {
                    ShowPage(p.button);
                    Application.DoEvents();
                    using var bmp = new Bitmap(Width, Height);
                    DrawToBitmap(bmp, new Rectangle(Point.Empty, Size));
                    bmp.Save(Path.Combine(screenshotDir, p.button.Text.Replace(' ', '_') + ".png"));
                }
                Close();
            }
            else if (selfTest) { foreach (var p in pages) ShowPage(p.button); Environment.ExitCode = !File.Exists(gameExe) ? 6 : !Gamepads.Ready ? 4 : Gamepads.Connected().All(p => Gamepads.Opens(p.id)) ? 0 : 5; Close(); }
        };
    }

    // The release keeps the game in engine\ (renamed so nobody double-clicks it); the dev tree keeps it in Play\.
    // Search up from the launcher's own folder, so launcher\publish\ works in the dev tree too.
    public const string EngineExe = "brawl-engine.exe";
    static (string root, string dir, string exe) FindGame()
    {
        var start = new DirectoryInfo(AppContext.BaseDirectory);
        for (var d = start; d != null; d = d.Parent)
        {
            foreach (var (dir, exe) in new[] { ("engine", EngineExe), ("Play", "SuperSmashBrosBrawl.exe") })
                if (File.Exists(Path.Combine(d.FullName, dir, exe))) return (d.FullName, Path.Combine(d.FullName, dir), Path.Combine(d.FullName, dir, exe));
        }
        return (start.FullName, Path.Combine(start.FullName, "engine"), Path.Combine(start.FullName, "engine", EngineExe));
    }

    // This disc's save folder (per extracted disc, as the game keeps them).
    string SaveDataDir() => WiiSave.DataDir(userData, DiscStatus().path, WiiDisc.Game);

    // The packaged defaults: borderless 1080p, the game's own 16:9, no frame interpolation (it is still being checked
    // on Brawl). Keys the user already has are left alone.
    void EnsureDefaults()
    {
        bool changed = false;
        foreach (var (key, value) in new[] { ("display_mode", "\"borderless\""), ("resolution_multiplier", "2.25"), ("widescreen", "true"), ("aspect_mode", "1"),
                     ("frame_interpolation_fps", "0"), ("texture_replacements", "true"), ("vsync", "true") })
            if (config.Get("video", key) == null) { config.Set("video", key, value); changed = true; }
        if (changed) config.Save();
    }

    void AddPage(FlowLayoutPanel nav, string title, Control page)
    {
        var button = new Button
        {
            Text = title, Size = S(188, 50), FlatStyle = FlatStyle.Flat, BackColor = Card, ForeColor = Snow,
            Font = new Font("Segoe UI", 13f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft, Padding = S(12, 0, 0, 0),
            Margin = S(0, 0, 0, 10), Cursor = Cursors.Hand, UseCompatibleTextRendering = false,
        };
        button.FlatAppearance.BorderColor = Edge;
        button.FlatAppearance.BorderSize = S(2);
        button.Click += (_, _) => ShowPage(button);
        button.MouseEnter += (_, _) => { if (button.BackColor == Card) button.BackColor = Hover; };
        button.MouseLeave += (_, _) => { if (button.BackColor == Hover) button.BackColor = Card; };
        page.Dock = DockStyle.Fill;
        page.Visible = false;
        content.Controls.Add(page);
        nav.Controls.Add(button);
        pages.Add((button, page));
    }

    void ShowPage(Button button)
    {
        foreach (var (b, page) in pages)
        {
            bool on = b == button;
            b.BackColor = on ? Orange : Card;
            b.ForeColor = on ? Night : Snow;
            page.Visible = on;
        }
        RefreshStatus();
    }

    // ----- The header: the title over a dark sky with drifting embers -----
    // Everything moves on one clock; with Windows animations turned off the scene stands still.
    readonly bool animate = SystemInformation.UIEffectsEnabled;
    readonly Stopwatch clock = Stopwatch.StartNew();
    double T => animate ? clock.Elapsed.TotalSeconds : 1.0;
    Bitmap? headerTitle;
    Button playButton = null!;

    static int Lerp(int a, int b, float f) => (int)(a + (b - a) * f);

    void BuildHeaderTitle(Size size)
    {
        headerTitle?.Dispose();
        var r = new Rectangle(Point.Empty, size);
        headerTitle = new Bitmap(size.Width, size.Height);
        using var g = Graphics.FromImage(headerTitle);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var pen = new Pen(Orange, S(4))) g.DrawLine(pen, 0, r.Height - S(2), r.Width, r.Height - S(2));
        using var path = new GraphicsPath();
        var fmt = new StringFormat { Alignment = StringAlignment.Center };
        // Shrink the title if the window is narrow.
        float em = Math.Min(S(50), (r.Width - S(80)) / 9.5f);
        path.AddString("BRAWL RECOMPILED", new FontFamily("Segoe UI"), (int)FontStyle.Bold, em, new Point(r.Width / 2, S(22)), fmt);
        using (var outline = new Pen(Color.FromArgb(255, 12, 14, 22), S(7)) { LineJoin = LineJoin.Round }) g.DrawPath(outline, path);
        using (var fill = new LinearGradientBrush(new Rectangle(0, S(20), 10, (int)em + S(14)), Gold, Orange, LinearGradientMode.Vertical)) g.FillPath(fill, path);
        using var sub = new Font("Segoe UI", 10.5f, FontStyle.Bold);
        var subText = "Super Smash Bros. Brawl (Wii) on PC  -  made by JosiahG  -  v" + AppVersion;
        var subSize = g.MeasureString(subText, sub);
        var subRect = new RectangleF(r.Width / 2f - subSize.Width / 2 - S(10), S(22) + em * 1.3f, subSize.Width + S(20), subSize.Height + S(2));
        using (var tag = Rounded(Rectangle.Round(subRect), S(8))) { g.FillPath(new SolidBrush(Card), tag); g.DrawPath(new Pen(Edge, S(2)), tag); }
        g.DrawString(subText, sub, new SolidBrush(Snow), new PointF(r.Width / 2f, subRect.Y + S(1)), fmt);
    }

    void PaintHeader(object? sender, PaintEventArgs e)
    {
        var g = e.Graphics;
        var r = ((Control)sender!).ClientRectangle;
        if (r.Width <= 0 || r.Height <= 0) return;
        if (headerTitle == null || headerTitle.Size != r.Size) BuildHeaderTitle(r.Size);
        double t = T;
        using (var sky = new LinearGradientBrush(r, ColorTranslator.FromHtml("#0E1120"), Card, LinearGradientMode.Vertical)) g.FillRectangle(sky, r);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        // Embers rise and fade, each on its own lane.
        for (int i = 0; i < 24; i++)
        {
            double life = (t * (0.12 + 0.04 * (i % 5)) + i * 0.37) % 1.0;
            float x = (float)((i * 97 % 100) / 100.0 * r.Width + Math.Sin(t * 1.3 + i) * S(8));
            float y = (float)(r.Height - life * r.Height);
            int size = S(3 + i % 3);
            using var ember = new SolidBrush(Color.FromArgb((int)(170 * (1 - life)), i % 2 == 0 ? Orange : Gold));
            g.FillEllipse(ember, x, y, size, size);
        }
        g.DrawImageUnscaled(headerTitle!, 0, 0);
    }

    // A card holding one page's controls, top to bottom.
    static (Panel card, FlowLayoutPanel body) NewCard(string heading)
    {
        var card = new DoubleBufferedPanel { Padding = S(22, 16, 22, 16), BackColor = Night };
        card.Paint += (s, e) =>
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var r = ((Control)s!).ClientRectangle;
            r.Width -= S(8); r.Height -= S(8);
            r.Offset(S(2), S(2));
            using var path = Rounded(r, S(14));
            g.FillPath(new SolidBrush(Card), path);
            g.DrawPath(new Pen(Edge, S(3)), path);
        };
        // The body sizes to its content and the card scrolls only when that is taller than the card.
        var body = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Card };
        card.AutoScroll = true;
        body.Controls.Add(new Label { Text = heading, Font = new Font("Segoe UI", 18f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 0, 0, 10) });
        card.Controls.Add(body);
        return (card, body);
    }

    static GraphicsPath Rounded(Rectangle r, int radius)
    {
        var p = new GraphicsPath();
        int d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static Label Note(string text, int width = 700) => new()
    {
        Text = text, AutoSize = true, MaximumSize = new Size(S(width), 0), ForeColor = Dim, Margin = S(0, 6, 0, 6),
    };

    static Button Btn(string text, EventHandler onClick, Color? back = null)
    {
        var fill = back ?? Orange;
        var b = new Button
        {
            Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, BackColor = fill, ForeColor = fill == Orange || fill == Gold ? Night : Snow,
            Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Padding = S(10, 4, 10, 4), Margin = S(0, 6, 10, 6), Cursor = Cursors.Hand,
        };
        b.FlatAppearance.BorderColor = fill == Card ? Edge : fill;
        b.FlatAppearance.BorderSize = S(2);
        b.Click += onClick;
        return b;
    }

    // ----- Play -----
    Control BuildPlayPage()
    {
        var (card, body) = NewCard("Ready to brawl?");
        playButton = Btn("PLAY", (_, _) => Launch(), Red);
        playButton.ForeColor = Color.White;
        playButton.Font = new Font("Segoe UI", 24f, FontStyle.Bold);
        playButton.Padding = S(60, 8, 60, 8);
        body.Controls.Add(playButton);
        playStatus = Note("");
        playStatus.Font = new Font("Segoe UI", 10.5f, FontStyle.Bold);
        playStatus.ForeColor = Snow;
        body.Controls.Add(playStatus);
        AddDiscSetup(body);
        body.Controls.Add(gameNote = Note("The game starts borderless at 1080p in its own 16:9 mode. " +
                               "In the game, F10 opens the settings menu (resolution, aspect, controllers, audio) and F11 switches between fullscreen and a window."));
        var row = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        row.Controls.Add(Btn("Open game folder", (_, _) => Process.Start("explorer.exe", play), Card));
        row.Controls.Add(Btn("Open saves folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(SaveDataDir()).FullName), Card));
        row.Controls.Add(Btn("Import Wii / Dolphin save...", (_, _) => ImportSave(), Card));
        body.Controls.Add(row);
        return card;
    }

    // A Dolphin save folder, or a data.bin a homebrew save manager exported from a Wii (Brawl's save is no-copy).
    void ImportSave()
    {
        const string saveDir = @"Wii\title\00010000\52534245\data";
        var dolphin = new[] { Environment.SpecialFolder.ApplicationData, Environment.SpecialFolder.MyDocuments }
            .Select(f => Path.Combine(Environment.GetFolderPath(f), "Dolphin Emulator", saveDir)).FirstOrDefault(Directory.Exists);
        using var dialog = new OpenFileDialog
        {
            Title = "Choose a Super Smash Bros. Brawl save: autosv0.bin in a Dolphin save folder, or a data.bin",
            Filter = "Dolphin or Wii save (autosv0.bin, data.bin)|autosv0.bin;data.bin|All files|*.*",
            InitialDirectory = dolphin ?? "",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        var dataDir = SaveDataDir();
        if (Directory.Exists(dataDir) && Directory.EnumerateFiles(dataDir).Any() &&
            MessageBox.Show(this, "This replaces your current save files. They are kept as a backup next to them (data.backup-<date>).\n\nImport the save?",
                "Import save", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        try
        {
            var files = WiiSave.Import(dialog.FileName, dataDir);
            MessageBox.Show(this, "Imported " + string.Join(", ", files) + ".", "Import save", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            MessageBox.Show(this, "The save could not be imported:\n" + e.Message, "Import save", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    void Launch()
    {
        var (ok, text, _) = DiscStatus();
        if (!ok)
        {
            MessageBox.Show(this, text + "\n\nChoose your disc image with the button under PLAY first.", "Brawl Recompiled", MessageBoxButtons.OK, MessageBoxIcon.Information);
            ShowPage(pages[0].button);
            return;
        }
        if (!File.Exists(gameExe))
        {
            MessageBox.Show(this, "The game files (the engine folder) are missing. Unzip the whole download again, keeping the engine folder next to this launcher.", "Brawl Recompiled", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        Gamepads.Release();
        var game = Process.Start(new ProcessStartInfo(gameExe) { WorkingDirectory = play, UseShellExecute = false });
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => BeginInvoke(Gamepads.Resume);
        }
        else Gamepads.Resume();
        WindowState = FormWindowState.Minimized;
    }

    // ----- Display -----
    Control BuildDisplayPage()
    {
        var (card, body) = NewCard("Display");
        body.Controls.Add(Choice("Display mode", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Windowed", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }));
        body.Controls.Add(Choice("Resolution", "resolution_multiplier", new[] { ("1080p", "2.25"), ("4K (2160p)", "4.5"), ("Match the screen", "0"), ("1440p", "3"), ("720p", "1.5"), ("480p (original)", "1") }));
        // The game's own 16:9 mode (its widescreen switch) or 4:3; "fill" stretches the 16:9 camera to wider screens.
        body.Controls.Add(Choice("Aspect ratio", "aspect_mode", new[] { ("16:9 (the game's own widescreen)", "1"), ("Fill the screen (ultrawide)", "2"), ("Original 4:3", "0") },
            also: value => config.Set("video", "widescreen", value == "0" ? "false" : "true")));
        body.Controls.Add(Choice("Frame interpolation", "frame_interpolation_fps", new[] { ("Off (60 FPS)", "0"), ("120 FPS (experimental)", "120"), ("180 FPS (experimental)", "180") }));
        body.Controls.Add(Choice("Vsync", "vsync", new[] { ("On (no screen tearing)", "true"), ("Off", "false") }));
        body.Controls.Add(Choice("FPS counter", "show_fps", new[] { ("Hidden", "false"), ("Shown", "true") }));
        body.Controls.Add(Note("Changes are saved at once and used the next time the game starts. The same settings are in the game's F10 menu. " +
                               "Frame interpolation inserts frames between the game's 60 fps (it needs a display faster than 60 Hz); turn it off if you see artifacts."));
        return card;
    }

    Control Choice(string label, string key, (string label, string value)[] items, string section = "video", Action<string>? also = null)
    {
        var row = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 4, 0, 4) };
        row.Controls.Add(new Label { Text = label, AutoSize = false, Size = S(200, 32), ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(300), FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10.5f), Margin = S(3, 4, 3, 4), BackColor = Night, ForeColor = Snow };
        foreach (var item in items) combo.Items.Add(item.label);
        string current = config.Get(section, key) ?? "";
        int index = Array.FindIndex(items, i => i.value.Trim('"') == current || (double.TryParse(i.value, out var a) && double.TryParse(current, out var b) && a == b));
        combo.SelectedIndex = index < 0 ? 0 : index;
        combo.SelectedIndexChanged += (_, _) =>
        {
            var value = items[combo.SelectedIndex].value;
            config.Set(section, key, value);
            also?.Invoke(value);
            config.Save();
        };
        row.Controls.Add(combo);
        return row;
    }

    // ----- Controls -----
    // Every gamepad is a GameCube controller on its port (runtime hle/input/pad.cpp), laid out automatically for its
    // type. The Gamepad view changes one port's buttons through the game's [controller] expr_<port>_<control>
    // expressions (runtime input_bindings.cpp, Dolphin's input names). Only the pads actually connected are listed,
    // refreshed every second. The keyboard is set up in the game's own F10 menu.
    static readonly (string key, string label)[] GcControls =
    {
        ("buttons_a", "A"), ("buttons_b", "B"), ("buttons_x", "X"), ("buttons_y", "Y"), ("buttons_z", "Z"), ("buttons_start", "Start"),
        ("d_pad_up", "D-pad up"), ("d_pad_down", "D-pad down"), ("d_pad_left", "D-pad left"), ("d_pad_right", "D-pad right"),
        ("triggers_l", "L"), ("triggers_r", "R"),
    };
    // SDL inputs (Gamepads.Active tokens): the name the game's expressions use, and the printed names on Xbox,
    // PlayStation and Nintendo pads.
    static readonly (string token, string expr, string xbox, string ps, string nintendo)[] PadInputs =
    {
        ("a", "Button A", "A", "Cross", "B"), ("b", "Button B", "B", "Circle", "A"), ("x", "Button X", "X", "Square", "Y"), ("y", "Button Y", "Y", "Triangle", "X"),
        ("leftshoulder", "Shoulder L", "LB", "L1", "L"), ("rightshoulder", "Shoulder R", "RB", "R1", "R"),
        ("lefttrigger", "Trigger L", "LT", "L2", "ZL"), ("righttrigger", "Trigger R", "RT", "R2", "ZR"),
        ("back", "Back", "View", "Create", "Minus"), ("start", "Start", "Menu", "Options", "Plus"),
        ("leftstick", "Thumb L", "Left stick click", "L3", "Left stick click"), ("rightstick", "Thumb R", "Right stick click", "R3", "Right stick click"),
        ("dpup", "Pad N", "D-pad up", "D-pad up", "D-pad up"), ("dpdown", "Pad S", "D-pad down", "D-pad down", "D-pad down"),
        ("dpleft", "Pad W", "D-pad left", "D-pad left", "D-pad left"), ("dpright", "Pad E", "D-pad right", "D-pad right", "D-pad right"),
        ("leftx-", "Axis X-", "Left stick left", "Left stick left", "Left stick left"), ("leftx+", "Axis X+", "Left stick right", "Left stick right", "Left stick right"),
        ("lefty-", "Axis Y-", "Left stick up", "Left stick up", "Left stick up"), ("lefty+", "Axis Y+", "Left stick down", "Left stick down", "Left stick down"),
        ("rightx-", "Axis Z-", "Right stick left", "Right stick left", "Right stick left"), ("rightx+", "Axis Z+", "Right stick right", "Right stick right", "Right stick right"),
        ("righty-", "Axis Zr-", "Right stick up", "Right stick up", "Right stick up"), ("righty+", "Axis Zr+", "Right stick down", "Right stick down", "Right stick down"),
    };

    static string PadLabel(int style, string expr)
    {
        var input = PadInputs.FirstOrDefault(i => i.expr == expr);
        if (input.token == null) return expr;
        return style switch { 1 => input.ps, 2 => input.nintendo, _ => input.xbox };
    }

    Control BuildControlsPage()
    {
        var (card, body) = NewCard("Controls");
        var inputRow = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 0, 0, 6) };
        inputRow.Controls.Add(new Label { Text = "Input", AutoSize = false, Size = S(110, 32), ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var inputCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(460), FlatStyle = FlatStyle.Flat, Margin = S(3, 4, 3, 4), BackColor = Night, ForeColor = Snow };
        inputRow.Controls.Add(inputCombo);
        body.Controls.Add(inputRow);
        var gamepad = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        var keyboard = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        body.Controls.Add(gamepad);
        body.Controls.Add(keyboard);

        // Gamepad: one button per GameCube control; click it, then press the controller input to bind (Escape or a click cancels).
        int style = 0, port = 1;
        uint padId = 0;
        gamepad.Controls.Add(Note("This controller is a GameCube controller on its port and is set up automatically: the face buttons by position, " +
                                  "the bumpers L and R, the right trigger Z, the left stick to move and the right stick as the C-stick. " +
                                  "To give a control another button as well, click it and press that button on your controller (Escape cancels); " +
                                  "the automatic button keeps working too."));
        var padTable = new TableLayoutPanel { AutoSize = true, ColumnCount = 4, Margin = S(0, 4, 0, 4) };
        gamepad.Controls.Add(padTable);
        Button? capturingPad = null;
        var heldAtStart = new HashSet<string>();
        var padTimer = new System.Windows.Forms.Timer { Interval = 30 };
        string ExprKey(string control) => $"expr_{port}_{control}";
        void FillPadTable()
        {
            padTable.SuspendLayout();
            padTable.Controls.Clear();
            for (int i = 0; i < GcControls.Length; i++)
            {
                var (key, label) = GcControls[i];
                padTable.Controls.Add(new Label { Text = label, AutoSize = true, ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Margin = S(0, 12, 12, 0) }, (i / 6) * 2, i % 6);
                string? custom = config.Get("controller", ExprKey(key));
                var b = Btn(custom == null ? "Automatic" : PadLabel(style, custom), null!, custom == null ? Card : Hover);
                b.AutoSize = false;
                b.Size = S(240, 40);
                b.Margin = S(0, 4, 18, 4);
                b.Tag = key;
                b.Click += (_, _) =>
                {
                    if (capturingPad != null) { EndPadCapture(null); return; }
                    capturingPad = b;
                    b.Text = "Press a button...";
                    b.BackColor = Orange;
                    b.ForeColor = Night;
                    heldAtStart = Gamepads.Active(padId);
                    padTimer.Start();
                };
                padTable.Controls.Add(b, (i / 6) * 2 + 1, i % 6);
            }
            padTable.ResumeLayout();
        }
        void EndPadCapture(string? expr)
        {
            padTimer.Stop();
            if (capturingPad == null) return;
            if (expr != null) { config.SetString("controller", ExprKey((string)capturingPad.Tag!), expr); config.Save(); }
            capturingPad = null;
            FillPadTable();
        }
        padTimer.Tick += (_, _) =>
        {
            var held = Gamepads.Active(padId);
            heldAtStart.IntersectWith(held);  // a key held when the capture began counts once it is released and pressed again
            var pressed = held.FirstOrDefault(t => !heldAtStart.Contains(t) && PadInputs.Any(i => i.token == t));
            if (pressed != null) EndPadCapture(PadInputs.First(i => i.token == pressed).expr);
        };
        cancelPadCapture = () => { if (capturingPad == null) return false; EndPadCapture(null); return true; };
        gamepad.Controls.Add(Btn("Reset to automatic", (_, _) =>
        {
            EndPadCapture(null);
            foreach (var (key, _) in GcControls) config.Remove("controller", ExprKey(key));
            config.Save();
            FillPadTable();
        }, Card));

        // Keyboard: the game's own keyboard controller.
        keyboard.Controls.Add(Note("The keyboard can be a GameCube controller too. Turn it on in the game: press F10, open the Controller tab, " +
                                   "tick \"Keyboard and mouse\" and take the WASD + mouse preset or set each key yourself. It replaces the gamepad on that port."));

        // The input list: connected pads first (in port order), then the keyboard.
        var shown = new List<(uint id, string name, int style)>();
        void ShowSelected()
        {
            int i = inputCombo.SelectedIndex;
            bool isPad = i >= 0 && i < shown.Count;
            gamepad.Visible = isPad;
            keyboard.Visible = !isPad;
            if (isPad) { EndPadCapture(null); style = shown[i].style; padId = shown[i].id; port = i + 1; FillPadTable(); }
        }
        void RefreshInputs()
        {
            var pads = Gamepads.Connected();
            if (pads.Count == 0 && screenshotDir != null) pads.Add((0, "Test controller (screenshot mode)", 0));
            if (pads.SequenceEqual(shown) && inputCombo.Items.Count > 0) return;
            uint? selectedId = inputCombo.SelectedIndex >= 0 && inputCombo.SelectedIndex < shown.Count ? shown[inputCombo.SelectedIndex].id : null;
            bool keyboardSelected = inputCombo.Items.Count > 0 && inputCombo.SelectedIndex == shown.Count;
            shown.Clear();
            shown.AddRange(pads);
            inputCombo.Items.Clear();
            for (int i = 0; i < pads.Count; i++) inputCombo.Items.Add($"Port {i + 1}: {pads[i].name}  ({new[] { "Xbox", "PlayStation", "Nintendo" }[pads[i].style]} layout)");
            inputCombo.Items.Add(pads.Count == 0 ? "Keyboard  (no controller connected - plug one in and it appears here)" : "Keyboard");
            int keep = selectedId is uint id ? shown.FindIndex(p => p.id == id) : -1;
            inputCombo.SelectedIndex = keyboardSelected ? shown.Count : keep >= 0 ? keep : 0;
            ShowSelected();
        }
        inputCombo.SelectedIndexChanged += (_, _) => ShowSelected();
        RefreshInputs();
        var poll = new System.Windows.Forms.Timer { Interval = 1000 };
        poll.Tick += (_, _) => { if (card.Visible) RefreshInputs(); };
        poll.Start();
        return card;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Escape && cancelPadCapture?.Invoke() == true) { e.Handled = true; return; }
        base.OnKeyDown(e);
    }

    // ----- Game disc (part of the Play page) -----
    // One button sets the disc up (or changes it later); extraction progress shows under it, and the dump
    // instructions only while no disc is set up.
    void AddDiscSetup(FlowLayoutPanel body)
    {
        var progress = new ProgressBar { Size = S(640, 22), Margin = S(0, 6, 0, 2), Visible = false };
        var progressText = Note("");
        progressText.Visible = false;
        progressText.TextChanged += (_, _) => progressText.Visible = progressText.Text.Length > 0;  // no empty gap
        var choose = chooseDisc = Btn("Choose disc image (.iso, .ciso, .wbfs)...", null!);
        choose.AutoSizeMode = AutoSizeMode.GrowAndShrink;  // "Change disc..." is shorter
        choose.Click += async (_, _) =>
        {
            using var dialog = new OpenFileDialog { Title = "Choose your Super Smash Bros. Brawl disc image", Filter = "Wii disc images (*.iso;*.ciso;*.wbfs)|*.iso;*.ciso;*.wbfs|All files|*.*" };
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            WiiDisc disc;
            try { disc = WiiDisc.Open(dialog.FileName); }
            catch (Exception ex) { progressText.Text = "Could not read that image: " + ex.Message; return; }
            using (disc)
            {
                if (disc.Problem() is { Length: > 0 } problem) { progressText.Text = problem; return; }
                choose.Enabled = false;
                progress.Visible = true;
                var staging = discDir + ".new";
                try
                {
                    if (Directory.Exists(staging)) Directory.Delete(staging, true);
                    var report = new Progress<(int done, int total, string name)>(p =>
                    {
                        progress.Maximum = p.total;
                        progress.Value = Math.Min(p.done, p.total);
                        progressText.Text = $"Extracting {p.done}/{p.total}  {p.name}";
                    });
                    await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                    if (disc.Holes > 0) throw new InvalidDataException("the image is incomplete (blocks missing); dump the disc again.");
                    if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                    Directory.Move(staging, discDir);
                    config.SetString("paths", "dvd_root", "../../Disc/DATA");  // relative to <game folder>\UserData (one level below the root in both layouts), so the folder can move
                    config.Save();
                    progressText.Text = "Done. The disc is extracted and ready.";
                }
                catch (Exception ex)
                {
                    progressText.Text = "Extraction failed: " + ex.Message;
                }
                finally
                {
                    choose.Enabled = true;
                    progress.Visible = false;
                    RefreshStatus();
                }
            }
        };
        body.Controls.Add(choose);
        body.Controls.Add(progress);
        body.Controls.Add(progressText);
        body.Controls.Add(dumpHelp = Note(
            "This port runs your own copy of Super Smash Bros. Brawl (USA, RSBE01). It needs a disc image of the disc you own, " +
            "which the launcher decrypts and extracts once into its Disc folder (about 7 GB; the disc is dual-layer, so give it a few minutes).\n\n" +
            "Making a legal image of your disc (Wii or Wii U consoles can read the disc; PC drives can't):\n" +
            "1. On a Wii with the Homebrew Channel, run CleanRip (or USB Loader GX's install option).\n" +
            "2. Insert Super Smash Bros. Brawl and dump it to an SD card or USB drive. CleanRip writes a .iso " +
            "(it may be split into .part files: join them with  copy /b  on the PC); USB Loader GX writes a .wbfs.\n" +
            "3. Copy the file to this PC and choose it with the button above. .iso, .ciso and .wbfs all work.\n" +
            "The USA release, revision 1, works (the European and Japanese discs do not). " +
            "A Dolphin-extracted folder also works: put it in Disc\\DATA next to the launcher."));
    }

    (bool ok, string text, string path) DiscStatus()
    {
        string? configured = config.Get("paths", "dvd_root");
        string path = configured == null ? discDir : Path.GetFullPath(Path.Combine(userData, configured));
        if (!File.Exists(Path.Combine(path, "sys", "boot.bin")))
            return (false, "No game disc set up yet. Choose your Super Smash Bros. Brawl disc image below; the launcher extracts it into its Disc folder.", path);
        var header = new byte[8];
        using (var f = File.OpenRead(Path.Combine(path, "sys", "boot.bin"))) f.ReadExactly(header, 0, 8);
        string id = System.Text.Encoding.ASCII.GetString(header, 0, 6);
        if (id != WiiDisc.Game || header[7] != WiiDisc.GameRevision)
            return (false, $"The disc folder holds {id} revision {header[7]}, not Super Smash Bros. Brawl USA revision {WiiDisc.GameRevision}: {path}", path);
        return (true, "Super Smash Bros. Brawl (USA) is ready: " + path, path);
    }

    void RefreshStatus()
    {
        var (ok, text, _) = DiscStatus();
        playStatus.Text = ok ? "Disc: ready.  SD card: " + SdNames() + ".  Mods: " + ModNames() : text;
        playStatus.ForeColor = ok ? Snow : Orange;
        chooseDisc.Text = ok ? "Change disc..." : "Choose disc image (.iso, .ciso, .wbfs)...";
        chooseDisc.BackColor = ok ? Card : Orange;
        chooseDisc.ForeColor = ok ? Snow : Night;
        dumpHelp.Visible = !ok;
        gameNote.Visible = ok;
        if (sdStatus != null) sdStatus.Text = "On the card now: " + SdNames() + ".";
    }

    string ModNames()
    {
        var names = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).Where(d => !File.Exists(Path.Combine(d, "disabled"))).Select(Path.GetFileName).ToArray() : Array.Empty<string>();
        return names.Length == 0 ? "none" : string.Join(", ", names);
    }

    // The folders on the virtual SD card (a mod's SD card contents, Project+'s "Project+", "private" and "apps").
    string SdNames()
    {
        var names = Directory.Exists(sdDir) ? Directory.GetDirectories(sdDir).Select(Path.GetFileName).ToArray() : Array.Empty<string>();
        return names.Length == 0 ? "empty" : string.Join(", ", names);
    }

    // ----- Mods -----
    Control BuildModsPage()
    {
        var (card, body) = NewCard("Mods");
        body.Controls.Add(new Label { Text = "SD card mods (Project+ and the like)", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 4, 0, 2) });
        body.Controls.Add(Note("The SD card folder is the game's SD card, the way a Wii sees one. Copy a mod's SD card contents into it: for Project+, " +
                               "the Project+, private and apps folders from its Wii download. Its Gecko codes (RSBE01.GCT and gc.txt) load with it, " +
                               "and the game's own SD features (snapshots, replays, custom stages) use the card too. The card is rebuilt when the folder changes."));
        sdStatus = Note("");
        sdStatus.ForeColor = Snow;
        body.Controls.Add(sdStatus);
        body.Controls.Add(Btn("Open SD card folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(sdDir).FullName)));
        body.Controls.Add(new Label { Text = "File and texture mods", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 14, 0, 2) });
        body.Controls.Add(Note("How to install one:\n" +
                               "1. Click Open mods folder.\n" +
                               "2. Put the mod's folder in it (one folder per mod: files\\ mirrors the disc, textures\\ holds texture replacements).\n" +
                               "3. Click Refresh. Ticked mods load when the game starts; untick one to turn it off."));
        var row = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        row.Controls.Add(Btn("Open mods folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(modsDir).FullName)));
        row.Controls.Add(Btn("Refresh", (_, _) => { LoadMods(); RefreshStatus(); }, Card));
        body.Controls.Add(row);
        modList = new ListView { View = View.Details, CheckBoxes = true, Size = S(660, 200), FullRowSelect = true, Font = new Font("Segoe UI", 10.5f), Margin = S(0, 6, 0, 6), HeaderStyle = ColumnHeaderStyle.Nonclickable, BackColor = Night, ForeColor = Snow, BorderStyle = BorderStyle.FixedSingle };
        modList.Columns.Add("Installed mods", S(630));
        modList.ItemCheck += (_, e) =>
        {
            var dir = (string)modList.Items[e.Index].Tag!;
            var marker = Path.Combine(dir, "disabled");
            if (e.NewValue == CheckState.Checked) File.Delete(marker); else File.WriteAllText(marker, "");
            BeginInvoke(RefreshStatus);
        };
        body.Controls.Add(modList);
        LoadMods();
        return card;
    }

    void LoadMods()
    {
        modList.Items.Clear();
        if (!Directory.Exists(modsDir)) return;
        foreach (var dir in Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.Ordinal))
            modList.Items.Add(new ListViewItem(Path.GetFileName(dir)) { Tag = dir, Checked = !File.Exists(Path.Combine(dir, "disabled")) });
    }

    sealed class BufferedFlow : FlowLayoutPanel
    {
        public BufferedFlow() { DoubleBuffered = true; ResizeRedraw = true; }
    }

    sealed class DoubleBufferedPanel : Panel
    {
        public DoubleBufferedPanel() { DoubleBuffered = true; ResizeRedraw = true; }
    }
}
