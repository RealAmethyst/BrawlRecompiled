using System.Diagnostics;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace BrawlRecompiled;

// The Linux launcher window: the Windows launcher (launcher\MainForm.cs) page for page - a dark header, a nav column
// (Play, Display, Controls, Mods) and one card per page. Everything it changes goes to engine/UserData/Config.toml,
// read by the game at start. Keep the two in step when either changes.
sealed class MainWindow : Window
{
    public static readonly Color Night = Color.Parse("#1A1E2C"), Card = Color.Parse("#262B3C"), Snow = Color.Parse("#F2F2F6"),
        Dim = Color.Parse("#B8BCCB"), Orange = Color.Parse("#FF8C28"), Red = Color.Parse("#D83A3A"), Edge = Color.Parse("#3C4358"),
        Hover = Color.Parse("#343A50"), Gold = Color.Parse("#FFC43A");
    public static readonly IBrush SnowBrush = new SolidColorBrush(Snow), EdgeBrush = new SolidColorBrush(Edge);
    static readonly FontFamily Font = new("fonts:Inter#Inter");
    // WinForms sizes are in points; these are the same sizes in pixels (10.5pt, 13pt, 18pt, 24pt).
    const double Body = 14, NavSize = 17.3, Heading = 24, PlaySize = 32;

    readonly bool selfTest;
    readonly string? screenshotDir;
    // play = the folder holding the game programs and their UserData: engine/ in a release.
    readonly string root, play, gameExe, userData, configPath, modsDir, sdDir, discDir;
    static readonly string AppVersion = typeof(MainWindow).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";
    readonly GameConfig config;
    readonly Panel content = new() { Margin = new Thickness(16, 12, 20, 16) };
    readonly List<(PaperButton button, Control page)> pages = new();
    readonly StackPanel navButtons = new() { Margin = new Thickness(16, 18, 0, 0), VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left };
    TextBlock playStatus = null!, dumpHelp = null!, gameNote = null!, sdStatus = null!;
    StackPanel modList = null!;
    PaperButton chooseDisc = null!, playButton = null!;
    Func<bool>? cancelPadCapture;  // the Controls page's gamepad capture: Escape ends it

    public MainWindow(bool selfTest, string? screenshotDir = null)
    {
        this.selfTest = selfTest;
        this.screenshotDir = screenshotDir;
        (root, play, gameExe) = FindGame();
        userData = Path.Combine(play, "UserData");
        configPath = Path.Combine(userData, "Config.toml");
        modsDir = Path.Combine(userData, "mods");
        sdDir = Path.Combine(userData, "SD");
        discDir = Path.Combine(root, "Disc", "DATA");
        config = new GameConfig(configPath);
        EnsureDefaults();
        Gamepads.Init(play);

        Title = "Brawl Recompiled  v" + AppVersion;
        using (var icon = typeof(MainWindow).Assembly.GetManifestResourceStream("smash.ico")) if (icon != null) Icon = new WindowIcon(icon);
        Width = 1100;
        Height = 760;
        if (Screens.Primary is { } screen)
        {
            var area = screen.WorkingArea.Size.ToSize(screen.Scaling);
            Width = Math.Min(1100, area.Width - 40);
            Height = Math.Min(760, area.Height - 60);
        }
        MinWidth = Math.Min(940, Width);
        MinHeight = Math.Min(640, Height);
        Background = new SolidColorBrush(Night);
        FontFamily = Font;
        FontSize = Body;
        Foreground = SnowBrush;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;

        var header = new HeaderView(this) { Height = 150 };
        var grid = new Grid { RowDefinitions = new RowDefinitions("150,*"), ColumnDefinitions = new ColumnDefinitions("220,*") };
        Grid.SetColumnSpan(header, 2);
        Grid.SetRow(navButtons, 1);
        Grid.SetRow(content, 1);
        Grid.SetColumn(content, 1);
        grid.Children.Add(header);
        grid.Children.Add(navButtons);
        grid.Children.Add(content);
        Content = grid;

        var frame = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        frame.Tick += (_, _) =>
        {
            if (WindowState == WindowState.Minimized) return;
            header.InvalidateVisual();
            float glow = (float)(Math.Sin(T * 3) + 1) / 2;  // PLAY breathes between two reds
            playButton.Back = Color.FromRgb((byte)Lerp(Red.R, 255, glow), (byte)Lerp(Red.G, 100, glow), (byte)Lerp(Red.B, 70, glow));
        };
        frame.Start();

        AddPage("Play", BuildPlayPage());
        AddPage("Display", BuildDisplayPage());
        AddPage("Controls", BuildControlsPage());
        AddPage("Mods", BuildModsPage());
        ShowPage(pages[0].button);
        AddHandler(KeyDownEvent, OnKey, RoutingStrategies.Tunnel);
        Opened += async (_, _) =>
        {
            if (screenshotDir != null)
            {
                // Headless visual check: every page drawn to <dir>/<page>.png.
                Directory.CreateDirectory(screenshotDir);
                foreach (var p in pages)
                {
                    ShowPage(p.button);
                    await Task.Delay(400);
                    var size = new PixelSize((int)Bounds.Width, (int)Bounds.Height);
                    using var bmp = new RenderTargetBitmap(size);
                    bmp.Render(this);
                    bmp.Save(Path.Combine(screenshotDir, p.button.Text.Replace(' ', '_') + ".png"));
                }
                Close();
            }
            else if (selfTest)
            {
                foreach (var p in pages) ShowPage(p.button);
                int code = !File.Exists(gameExe) ? 6 : !Gamepads.Ready ? 4 : Gamepads.Connected().All(p => Gamepads.Opens(p.id)) ? 0 : 5;
                // posted: the lifetime cannot shut down while it is still starting
                Dispatcher.UIThread.Post(() => (Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime)?.Shutdown(code));
            }
        };
    }

    // The release keeps the game in engine/ next to this launcher; search up from the launcher's own folder.
    static (string root, string dir, string exe) FindGame()
    {
        var start = new DirectoryInfo(AppContext.BaseDirectory);
        for (var d = start; d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "engine", "brawl-engine")))
                return (d.FullName, Path.Combine(d.FullName, "engine"), Path.Combine(d.FullName, "engine", "brawl-engine"));
        return (start.FullName, Path.Combine(start.FullName, "engine"), Path.Combine(start.FullName, "engine", "brawl-engine"));
    }

    // This disc's save folder (per extracted disc, as the game keeps them).
    string SaveDataDir() => WiiSave.DataDir(userData, DiscStatus().path, WiiDisc.Game);

    // The packaged defaults: borderless 1080p, the game's own 16:9, no frame interpolation (still being checked on
    // Brawl). Keys the user already has are left alone.
    void EnsureDefaults()
    {
        bool changed = false;
        foreach (var (key, value) in new[] { ("display_mode", "\"borderless\""), ("resolution_multiplier", "2.25"), ("widescreen", "true"), ("aspect_mode", "1"),
                     ("frame_interpolation_fps", "0"), ("texture_replacements", "true"), ("vsync", "true") })
            if (config.Get("video", key) == null) { config.Set("video", key, value); changed = true; }
        if (changed) config.Save();
    }

    void AddPage(string title, Control page)
    {
        var button = new PaperButton(title, Card, NavSize)
        {
            Width = 188, Height = 50, Padding = new Thickness(12, 0, 0, 0), Margin = new Thickness(0, 0, 0, 10), HoverColor = Hover,
        };
        button.Click += () => ShowPage(button);
        page.IsVisible = false;
        content.Children.Add(page);
        navButtons.Children.Add(button);
        pages.Add((button, page));
    }

    void ShowPage(PaperButton button)
    {
        foreach (var (b, page) in pages)
        {
            bool on = b == button;
            b.Back = on ? Orange : Card;
            b.TextBrush = on ? new SolidColorBrush(Night) : SnowBrush;
            page.IsVisible = on;
        }
        RefreshStatus();
    }

    // ----- The header: the title over a dark sky with drifting embers -----
    readonly Stopwatch clock = Stopwatch.StartNew();
    double T => clock.Elapsed.TotalSeconds;

    static double Lerp(double a, double b, float f) => a + (b - a) * f;

    sealed class HeaderView : Control
    {
        readonly MainWindow w;
        public HeaderView(MainWindow window) { w = window; ClipToBounds = true; }

        public override void Render(DrawingContext g)
        {
            var r = new Rect(Bounds.Size);
            if (r.Width <= 0 || r.Height <= 0) return;
            double t = w.T;
            g.FillRectangle(new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                GradientStops = { new GradientStop(Color.Parse("#0E1120"), 0), new GradientStop(Card, 1) },
            }, r);
            // Embers rise and fade, each on its own lane.
            for (int i = 0; i < 24; i++)
            {
                double life = (t * (0.12 + 0.04 * (i % 5)) + i * 0.37) % 1.0;
                double x = (i * 97 % 100) / 100.0 * r.Width + Math.Sin(t * 1.3 + i) * 8;
                double y = r.Height - life * r.Height;
                double size = 3 + i % 3;
                g.DrawEllipse(new SolidColorBrush(i % 2 == 0 ? Orange : Gold, 0.67 * (1 - life)), null, new Rect(x, y, size, size));
            }
            g.DrawLine(new Pen(new SolidColorBrush(Orange), 4), new Point(0, r.Height - 2), new Point(r.Width, r.Height - 2));
            // Shrink the title if the window is narrow.
            double em = Math.Min(50, (r.Width - 80) / 9.5);
            var title = new FormattedText("BRAWL RECOMPILED", CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                new Typeface(Font, FontStyle.Normal, FontWeight.Bold), em, new SolidColorBrush(Color.Parse("#0C0E16")));
            var at = new Point(r.Width / 2 - title.Width / 2, 22);
            // The dark outline: the title stamped around a 3.5 px circle, then the gold-to-orange face on top.
            for (int a = 0; a < 16; a++)
                g.DrawText(title, at + new Point(Math.Cos(a * Math.PI / 8) * 3.5, Math.Sin(a * Math.PI / 8) * 3.5));
            title.SetForegroundBrush(new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                GradientStops = { new GradientStop(Gold, 0), new GradientStop(Orange, 1) },
            });
            g.DrawText(title, at);
            var sub = new FormattedText("Super Smash Bros. Brawl (Wii) on PC  -  made by JosiahG  -  v" + AppVersion, CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight, new Typeface(Font, FontStyle.Normal, FontWeight.Bold), Body, SnowBrush);
            var tag = new Rect(r.Width / 2 - sub.Width / 2 - 10, 22 + em * 1.3, sub.Width + 20, sub.Height + 4);
            g.DrawRectangle(new SolidColorBrush(Card), new Pen(EdgeBrush, 2), tag, 8, 8);
            g.DrawText(sub, new Point(r.Width / 2 - sub.Width / 2, tag.Y + 2));
        }
    }

    // A card holding one page's controls, top to bottom; it scrolls when they are taller than the window.
    static (Control card, StackPanel body) NewCard(string heading)
    {
        var body = new StackPanel();
        body.Children.Add(new TextBlock { Text = heading, FontSize = Heading, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 0, 0, 10) });
        var card = new Border
        {
            Background = new SolidColorBrush(Card), BorderBrush = EdgeBrush, BorderThickness = new Thickness(3), CornerRadius = new CornerRadius(14),
            Margin = new Thickness(2, 2, 8, 8),
            Child = new ScrollViewer { Content = body, Padding = new Thickness(22, 16, 22, 16) },
        };
        return (card, body);
    }

    static TextBlock Note(string text, double width = 700) => new()
    {
        Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = width, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 6, 0, 6),
        Foreground = new SolidColorBrush(Dim),
    };

    static TextBlock Subheading(string text) => new() { Text = text, FontSize = 16, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 10, 0, 2) };

    static PaperButton Btn(string text, Action? onClick, Color? back = null)
    {
        var b = new PaperButton(text, back ?? Orange, Body);
        if (onClick != null) b.Click += onClick;
        return b;
    }

    static StackPanel Row(params Control[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        foreach (var c in children) row.Children.Add(c);
        return row;
    }

    // ----- Play -----
    Control BuildPlayPage()
    {
        var (card, body) = NewCard("Ready to brawl?");
        playButton = new PaperButton("PLAY", Red, PlaySize) { TextBrush = Brushes.White, Padding = new Thickness(60, 8, 60, 8), Margin = new Thickness(0, 6, 10, 6) };
        playButton.Click += Launch;
        body.Children.Add(playButton);
        playStatus = Note("");
        playStatus.FontWeight = FontWeight.Bold;
        playStatus.Foreground = SnowBrush;
        body.Children.Add(playStatus);
        AddDiscSetup(body);
        body.Children.Add(gameNote = Note("The game starts borderless at 1080p in its own 16:9 mode. " +
                               "In the game, F10 opens the settings menu (resolution, aspect, controllers, audio) and F11 switches between fullscreen and a window."));
        body.Children.Add(Row(
            Btn("Open game folder", () => Open(play), Card),
            Btn("Open saves folder", () => Open(Directory.CreateDirectory(SaveDataDir()).FullName), Card),
            Btn("Import Wii / Dolphin save...", ImportSave, Card)));
        return card;
    }

    static void Open(string folder)
    {
        try { Process.Start(new ProcessStartInfo("xdg-open", $"\"{folder}\"") { UseShellExecute = false }); } catch (Exception) { }
    }

    // A Dolphin save folder, or a data.bin a homebrew save manager exported from a Wii (Brawl's save is no-copy).
    async void ImportSave()
    {
        const string saveDir = "Wii/title/00010000/52534245/data";
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var dolphin = new[] { ".local/share/dolphin-emu", ".var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu", ".dolphin-emu" }
            .Select(d => Path.Combine(home, d, saveDir)).FirstOrDefault(Directory.Exists);
        var file = await PickFile("Choose a Super Smash Bros. Brawl save: autosv0.bin in a Dolphin save folder, or a data.bin",
            "Dolphin or Wii save (autosv0.bin, data.bin)", new[] { "autosv0.bin", "data.bin" }, dolphin);
        if (file == null) return;
        var dataDir = SaveDataDir();
        if (Directory.Exists(dataDir) && Directory.EnumerateFiles(dataDir).Any() &&
            !await Message("Import save", "This replaces your current save files. They are kept as a backup next to them (data.backup-<date>).\n\nImport the save?", cancel: true)) return;
        try
        {
            var files = WiiSave.Import(file, dataDir);
            await Message("Import save", "Imported " + string.Join(", ", files) + ".");
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            await Message("Import save", "The save could not be imported:\n" + e.Message);
        }
    }

    // The desktop's file picker (XDG portal or GTK); a typed path where neither exists.
    async Task<string?> PickFile(string title, string typeName, string[] patterns, string? startDir)
    {
        try
        {
            if (StorageProvider.CanOpen)
            {
                var options = new FilePickerOpenOptions
                {
                    Title = title, AllowMultiple = false,
                    FileTypeFilter = new[] { new FilePickerFileType(typeName) { Patterns = patterns }, FilePickerFileTypes.All },
                };
                if (startDir != null) options.SuggestedStartLocation = await StorageProvider.TryGetFolderFromPathAsync(startDir);
                var files = await StorageProvider.OpenFilePickerAsync(options);
                return files.Count > 0 ? files[0].TryGetLocalPath() : null;
            }
        }
        catch (Exception) { }  // no portal or GTK after all: ask for the path instead
        var path = await AskPath(title);
        return string.IsNullOrWhiteSpace(path) ? null : path.Trim().Trim('"', '\'');
    }

    async Task<string?> AskPath(string title)
    {
        var box = new TextBox { Width = 560, Watermark = "/home/you/Downloads/Super Smash Bros. Brawl.iso" };
        var dialog = Dialog(title, Note(title + "\n\nType or paste the file's full path:", 560), box, out var ok, out var cancel);
        ok.Click += () => dialog.Close(box.Text);
        cancel.Click += () => dialog.Close(null);
        return await dialog.ShowDialog<string?>(this);
    }

    // The Windows launcher's MessageBox: OK, or OK and Cancel.
    async Task<bool> Message(string title, string text, bool cancel = false)
    {
        var dialog = Dialog(title, Note(text, 520), null, out var ok, out var no);
        no.IsVisible = cancel;
        ok.Click += () => dialog.Close(true);
        no.Click += () => dialog.Close(false);
        return await dialog.ShowDialog<bool>(this);
    }

    static Window Dialog(string title, Control text, Control? extra, out PaperButton ok, out PaperButton cancel)
    {
        var body = new StackPanel { Margin = new Thickness(22, 16, 22, 16) };
        body.Children.Add(text);
        if (extra != null) body.Children.Add(extra);
        ok = Btn("OK", null);
        cancel = Btn("Cancel", null, Card);
        var buttons = Row(ok, cancel);
        buttons.HorizontalAlignment = HorizontalAlignment.Right;
        body.Children.Add(buttons);
        return new Window
        {
            Title = title, Content = body, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false, ShowInTaskbar = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, Background = new SolidColorBrush(Night), Foreground = SnowBrush,
            FontFamily = Font, FontSize = Body,
        };
    }

    async void Launch()
    {
        var (ok, text, _) = DiscStatus();
        if (!ok)
        {
            await Message("Brawl Recompiled", text + "\n\nChoose your disc image with the button under PLAY first.");
            ShowPage(pages[0].button);
            return;
        }
        // A disc put in Disc/DATA by hand (a Dolphin-extracted folder) has no [paths] dvd_root yet, and the game has no
        // folder picker on Linux to ask for it: point the game at the folder the launcher found.
        if (config.Get("paths", "dvd_root") == null) { config.SetString("paths", "dvd_root", "../../Disc/DATA"); config.Save(); }
        if (!File.Exists(gameExe))
        {
            await Message("Brawl Recompiled", "The game files (the engine folder) are missing. Unpack the whole download again, keeping the engine folder next to this launcher.");
            return;
        }
        Gamepads.Release();
        var start = new ProcessStartInfo(gameExe) { WorkingDirectory = play, UseShellExecute = false };
        // The game's few bundled libraries (libpng) sit beside it.
        start.Environment["LD_LIBRARY_PATH"] = play + (Environment.GetEnvironmentVariable("LD_LIBRARY_PATH") is { Length: > 0 } more ? ":" + more : "");
        Process? game = null;
        try { game = Process.Start(start); } catch (Exception e) { await Message("Brawl Recompiled", "The game could not start: " + e.Message); }
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => Dispatcher.UIThread.Post(Gamepads.Resume);
            WindowState = WindowState.Minimized;
        }
        else Gamepads.Resume();
    }

    // ----- Display -----
    Control BuildDisplayPage()
    {
        var (card, body) = NewCard("Display");
        body.Children.Add(Choice("Display mode", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Windowed", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }));
        body.Children.Add(Choice("Resolution", "resolution_multiplier", new[] { ("1080p", "2.25"), ("4K (2160p)", "4.5"), ("Match the screen", "0"), ("1440p", "3"), ("720p", "1.5"), ("480p (original)", "1") }));
        // The game's own 16:9 mode (its widescreen switch) or 4:3; "fill" stretches the 16:9 camera to wider screens.
        body.Children.Add(Choice("Aspect ratio", "aspect_mode", new[] { ("16:9 (the game's own widescreen)", "1"), ("Fill the screen (ultrawide)", "2"), ("Original 4:3", "0") },
            also: value => config.Set("video", "widescreen", value == "0" ? "false" : "true")));
        body.Children.Add(Choice("Frame interpolation", "frame_interpolation_fps", new[] { ("Off (60 FPS)", "0"), ("120 FPS (experimental)", "120"), ("180 FPS (experimental)", "180") }));
        body.Children.Add(Choice("Vsync", "vsync", new[] { ("On (no screen tearing)", "true"), ("Off", "false") }));
        body.Children.Add(Choice("FPS counter", "show_fps", new[] { ("Hidden", "false"), ("Shown", "true") }));
        body.Children.Add(Note("Changes are saved at once and used the next time the game starts. The same settings are in the game's F10 menu. " +
                               "Frame interpolation inserts frames between the game's 60 fps (it needs a display faster than 60 Hz); turn it off if you see artifacts."));
        return card;
    }

    Control Choice(string label, string key, (string label, string value)[] items, string section = "video", Action<string>? also = null)
    {
        var combo = new ComboBox { Width = 300, ItemsSource = items.Select(i => i.label).ToList(), Margin = new Thickness(3, 4, 3, 4) };
        string current = config.Get(section, key) ?? "";
        int index = Array.FindIndex(items, i => i.value.Trim('"') == current || (double.TryParse(i.value, CultureInfo.InvariantCulture, out var a) && double.TryParse(current, CultureInfo.InvariantCulture, out var b) && a == b));
        combo.SelectedIndex = index < 0 ? 0 : index;
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex < 0) return;
            var value = items[combo.SelectedIndex].value;
            config.Set(section, key, value);
            also?.Invoke(value);
            config.Save();
        };
        var row = Row(new TextBlock { Text = label, Width = 200, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, combo);
        row.Margin = new Thickness(0, 4, 0, 4);
        return row;
    }

    // ----- Controls -----
    // Every gamepad is a GameCube controller on its port (runtime hle/input/pad.cpp), laid out automatically for its
    // type. The Gamepad view changes one port's buttons through the game's [controller] expr_<port>_<control>
    // expressions (runtime input_bindings.cpp, Dolphin's input names). The keyboard is set up in the game's own F10 menu.
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

    // A 4-column table: label, button, label, button; six controls per column pair.
    static Grid ControlGrid() => new()
    {
        ColumnDefinitions = new ColumnDefinitions("Auto,Auto,Auto,Auto"), RowDefinitions = new RowDefinitions("Auto,Auto,Auto,Auto,Auto,Auto"),
        Margin = new Thickness(0, 4, 0, 4), HorizontalAlignment = HorizontalAlignment.Left,
    };

    static void Place(Grid grid, Control control, int column, int row)
    {
        Grid.SetColumn(control, column);
        Grid.SetRow(control, row);
        grid.Children.Add(control);
    }

    Control BuildControlsPage()
    {
        var (card, body) = NewCard("Controls");
        var inputCombo = new ComboBox { Width = 460, Margin = new Thickness(3, 4, 3, 4) };
        var inputRow = Row(new TextBlock { Text = "Input", Width = 110, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, inputCombo);
        inputRow.Margin = new Thickness(0, 0, 0, 6);
        body.Children.Add(inputRow);
        var gamepad = new StackPanel();
        var keyboard = new StackPanel();
        body.Children.Add(gamepad);
        body.Children.Add(keyboard);

        // Gamepad: one button per GameCube control; click it, then press the controller input to bind (Escape or a click cancels).
        int style = 0, port = 1;
        uint padId = 0;
        gamepad.Children.Add(Note("This controller is a GameCube controller on its port and is set up automatically: the face buttons by position, " +
                                  "the bumpers L and R, the right trigger Z, the left stick to move and the right stick as the C-stick. " +
                                  "To give a control another button as well, click it and press that button on your controller (Escape cancels); " +
                                  "the automatic button keeps working too."));
        var padTable = ControlGrid();
        gamepad.Children.Add(padTable);
        PaperButton? capturingPad = null;
        var heldAtStart = new HashSet<string>();
        var padTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
        string ExprKey(string control) => $"expr_{port}_{control}";
        void FillPadTable()
        {
            padTable.Children.Clear();
            for (int i = 0; i < GcControls.Length; i++)
            {
                var (key, label) = GcControls[i];
                Place(padTable, new TextBlock { Text = label, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 12, 12, 0) }, (i / 6) * 2, i % 6);
                string? custom = config.Get("controller", ExprKey(key));
                var b = Btn(custom == null ? "Automatic" : PadLabel(style, custom), null, custom == null ? Card : Hover);
                b.Width = 240;
                b.Height = 40;
                b.Margin = new Thickness(0, 4, 18, 4);
                b.Tag = key;
                b.Click += () =>
                {
                    if (capturingPad != null) { EndPadCapture(null); return; }
                    capturingPad = b;
                    b.Text = "Press a button...";
                    b.Back = Orange;
                    b.TextBrush = new SolidColorBrush(Night);
                    heldAtStart = Gamepads.Active(padId);
                    padTimer.Start();
                };
                Place(padTable, b, (i / 6) * 2 + 1, i % 6);
            }
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
        gamepad.Children.Add(Row(Btn("Reset to automatic", () =>
        {
            EndPadCapture(null);
            foreach (var (key, _) in GcControls) config.Remove("controller", ExprKey(key));
            config.Save();
            FillPadTable();
        }, Card)));

        // Keyboard: the game's own keyboard controller.
        keyboard.Children.Add(Note("The keyboard can be a GameCube controller too. Turn it on in the game: press F10, open the Controller tab, " +
                                   "tick \"Keyboard and mouse\" and take the WASD + mouse preset or set each key yourself. It replaces the gamepad on that port."));

        // The input list: connected pads first (in port order), then the keyboard.
        var shown = new List<(uint id, string name, int style)>();
        var names = new List<string>();
        bool filling = false;
        void ShowSelected()
        {
            int i = inputCombo.SelectedIndex;
            bool isPad = i >= 0 && i < shown.Count;
            gamepad.IsVisible = isPad;
            keyboard.IsVisible = !isPad;
            if (isPad) { EndPadCapture(null); style = shown[i].style; padId = shown[i].id; port = i + 1; FillPadTable(); }
        }
        void RefreshInputs()
        {
            var pads = Gamepads.Connected();
            if (pads.Count == 0 && screenshotDir != null) pads.Add((0, "Test controller (screenshot mode)", 0));
            if (pads.SequenceEqual(shown) && names.Count > 0) return;
            uint? selectedId = inputCombo.SelectedIndex >= 0 && inputCombo.SelectedIndex < shown.Count ? shown[inputCombo.SelectedIndex].id : null;
            bool keyboardSelected = names.Count > 0 && inputCombo.SelectedIndex == shown.Count;
            shown.Clear();
            shown.AddRange(pads);
            names.Clear();
            for (int i = 0; i < pads.Count; i++) names.Add($"Port {i + 1}: {pads[i].name}  ({new[] { "Xbox", "PlayStation", "Nintendo" }[pads[i].style]} layout)");
            names.Add(pads.Count == 0 ? "Keyboard  (no controller connected - plug one in and it appears here)" : "Keyboard");
            int keep = selectedId is uint id ? shown.FindIndex(p => p.id == id) : -1;
            filling = true;
            inputCombo.ItemsSource = names.ToList();
            inputCombo.SelectedIndex = keyboardSelected ? shown.Count : keep >= 0 ? keep : 0;
            filling = false;
            ShowSelected();
        }
        inputCombo.SelectionChanged += (_, _) => { if (!filling) ShowSelected(); };
        RefreshInputs();
        var poll = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        poll.Tick += (_, _) => { if (card.IsVisible) RefreshInputs(); };
        poll.Start();
        return card;
    }

    void OnKey(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && cancelPadCapture?.Invoke() == true) e.Handled = true;
    }

    // ----- Game disc (part of the Play page) -----
    // One button sets the disc up (or changes it later); extraction progress shows under it, and the dump
    // instructions only while no disc is set up.
    void AddDiscSetup(StackPanel body)
    {
        var progress = new ProgressBar { Width = 640, Height = 22, Margin = new Thickness(0, 6, 0, 2), IsVisible = false, HorizontalAlignment = HorizontalAlignment.Left };
        var progressText = Note("");
        progressText.IsVisible = false;
        void Say(string text) { progressText.Text = text; progressText.IsVisible = text.Length > 0; }  // no empty gap
        var choose = chooseDisc = Btn("Choose disc image (.iso, .ciso, .wbfs)...", null);
        choose.HorizontalAlignment = HorizontalAlignment.Left;
        choose.Click += async () =>
        {
            var file = await PickFile("Choose your Super Smash Bros. Brawl disc image", "Wii disc images (*.iso, *.ciso, *.wbfs)",
                new[] { "*.iso", "*.ciso", "*.wbfs", "*.ISO", "*.CISO", "*.WBFS" }, null);
            if (file == null) return;
            WiiDisc disc;
            try { disc = WiiDisc.Open(file); }
            catch (Exception ex) { Say("Could not read that image: " + ex.Message); return; }
            using (disc)
            {
                if (disc.Problem() is { Length: > 0 } problem) { Say(problem); return; }
                choose.IsEnabled = false;
                progress.IsVisible = true;
                var staging = discDir + ".new";
                try
                {
                    if (Directory.Exists(staging)) Directory.Delete(staging, true);
                    var report = new Progress<(int done, int total, string name)>(p =>
                    {
                        progress.Maximum = p.total;
                        progress.Value = Math.Min(p.done, p.total);
                        Say($"Extracting {p.done}/{p.total}  {p.name}");
                    });
                    await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                    if (disc.Holes > 0) throw new InvalidDataException("the image is incomplete (blocks missing); dump the disc again.");
                    if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                    Directory.Move(staging, discDir);
                    config.SetString("paths", "dvd_root", "../../Disc/DATA");  // relative to engine/UserData, so the folder can move
                    config.Save();
                    Say("Done. The disc is extracted and ready.");
                }
                catch (Exception ex)
                {
                    Say("Extraction failed: " + ex.Message);
                }
                finally
                {
                    choose.IsEnabled = true;
                    progress.IsVisible = false;
                    RefreshStatus();
                }
            }
        };
        body.Children.Add(choose);
        body.Children.Add(progress);
        body.Children.Add(progressText);
        body.Children.Add(dumpHelp = Note(
            "This port runs your own copy of Super Smash Bros. Brawl (USA, RSBE01). It needs a disc image of the disc you own, " +
            "which the launcher decrypts and extracts once into its Disc folder (about 7 GB; the disc is dual-layer, so give it a few minutes).\n\n" +
            "Making a legal image of your disc (Wii or Wii U consoles can read the disc; PC drives can't):\n" +
            "1. On a Wii with the Homebrew Channel, run CleanRip (or USB Loader GX's install option).\n" +
            "2. Insert Super Smash Bros. Brawl and dump it to an SD card or USB drive. CleanRip writes a .iso " +
            "(it may be split into .part files: join them with  cat game.part* > game.iso ); USB Loader GX writes a .wbfs.\n" +
            "3. Copy the file to this PC and choose it with the button above. .iso, .ciso and .wbfs all work.\n" +
            "The USA release, revision 1, works (the European and Japanese discs do not). " +
            "A Dolphin-extracted folder also works: put it in Disc/DATA next to the launcher."));
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
        playStatus.Foreground = new SolidColorBrush(ok ? Snow : Orange);
        chooseDisc.Text = ok ? "Change disc..." : "Choose disc image (.iso, .ciso, .wbfs)...";
        chooseDisc.Back = ok ? Card : Orange;
        chooseDisc.TextBrush = ok ? SnowBrush : new SolidColorBrush(Night);
        dumpHelp.IsVisible = !ok;
        gameNote.IsVisible = ok;
        if (sdStatus != null) sdStatus.Text = "On the card now: " + SdNames() + ".";
    }

    string ModNames()
    {
        var names = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).Where(d => !File.Exists(Path.Combine(d, "disabled"))).Select(Path.GetFileName).ToArray() : Array.Empty<string>();
        return names.Length == 0 ? "none" : string.Join(", ", names);
    }

    // The builds on the card: its folders holding Gecko codes (RSBE01.GCT), the ones the game can load codes from.
    List<string> SdBuilds() => Directory.Exists(sdDir)
        ? Directory.GetDirectories(sdDir).Where(d => Directory.EnumerateFiles(d).Any(f => Path.GetFileName(f).Equals("RSBE01.GCT", StringComparison.OrdinalIgnoreCase)))
            .Select(d => Path.GetFileName(d)!).OrderBy(n => n, StringComparer.Ordinal).ToList()
        : new List<string>();

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
        body.Children.Add(Subheading("SD card mods (Project+ and the like)"));
        body.Children.Add(Note("The SD card folder is the game's SD card, the way a Wii sees one. Copy a mod's SD card contents into it: for Project+, " +
                               "the Project+, private and apps folders from its Wii download. Its Gecko codes (RSBE01.GCT and gc.txt) load with it, " +
                               "and the game's own SD features (snapshots, replays, custom stages) use the card too. The card is rebuilt when the folder changes."));
        sdStatus = Note("");
        sdStatus.Foreground = SnowBrush;
        body.Children.Add(sdStatus);
        body.Children.Add(Row(Btn("Open SD card folder", () => Open(Directory.CreateDirectory(sdDir).FullName))));
        // Which build's Gecko codes load when several share the card (Config.toml [mods] sd_codes = the folder).
        var codesCombo = new ComboBox { Width = 300, Margin = new Thickness(3, 4, 3, 4) };
        var codesRow = Row(new TextBlock { Text = "Codes to load", Width = 200, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, codesCombo);
        codesRow.Margin = new Thickness(0, 4, 0, 4);
        body.Children.Add(codesRow);
        bool fillingCodes = false;
        var builds = new List<string>();
        void FillCodes()
        {
            fillingCodes = true;
            builds = SdBuilds();
            var items = new List<string> { "The first build on the card" };
            items.AddRange(builds);
            codesCombo.ItemsSource = items;
            int i = builds.IndexOf(config.Get("mods", "sd_codes") ?? "");
            codesCombo.SelectedIndex = i < 0 ? 0 : i + 1;
            fillingCodes = false;
        }
        codesCombo.SelectionChanged += (_, _) =>
        {
            if (fillingCodes || codesCombo.SelectedIndex < 0) return;
            if (codesCombo.SelectedIndex == 0) config.Remove("mods", "sd_codes");
            else config.SetString("mods", "sd_codes", builds[codesCombo.SelectedIndex - 1]);
            config.Save();
        };
        FillCodes();
        body.Children.Add(Subheading("File and texture mods"));
        body.Children.Add(Note("How to install one:\n" +
                               "1. Click Open mods folder.\n" +
                               "2. Put the mod's folder in it (one folder per mod: files/ mirrors the disc, textures/ holds texture replacements).\n" +
                               "3. Click Refresh. Ticked mods load when the game starts; untick one to turn it off."));
        body.Children.Add(Row(Btn("Open mods folder", () => Open(Directory.CreateDirectory(modsDir).FullName)), Btn("Refresh", () => { LoadMods(); FillCodes(); RefreshStatus(); }, Card)));
        modList = new StackPanel { Margin = new Thickness(8, 4, 8, 4) };
        var list = new StackPanel();
        list.Children.Add(new Border
        {
            Child = new TextBlock { Text = "Installed mods", Margin = new Thickness(8, 4, 8, 4) },
            BorderBrush = EdgeBrush, BorderThickness = new Thickness(0, 0, 0, 1),
        });
        list.Children.Add(new ScrollViewer { Content = modList, Height = 180 });
        body.Children.Add(new Border
        {
            Child = list, Width = 660, Background = new SolidColorBrush(Night), BorderBrush = EdgeBrush, BorderThickness = new Thickness(1),
            Margin = new Thickness(0, 6, 0, 6), HorizontalAlignment = HorizontalAlignment.Left,
        });
        LoadMods();
        return card;
    }

    void LoadMods()
    {
        modList.Children.Clear();
        if (!Directory.Exists(modsDir)) return;
        foreach (var dir in Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.Ordinal))
        {
            var box = new CheckBox { Content = Path.GetFileName(dir), IsChecked = !File.Exists(Path.Combine(dir, "disabled")) };
            box.IsCheckedChanged += (_, _) =>
            {
                var marker = Path.Combine(dir, "disabled");
                if (box.IsChecked == true) File.Delete(marker); else File.WriteAllText(marker, "");
                RefreshStatus();
            };
            modList.Children.Add(box);
        }
    }
}

// The launcher's flat button (the Windows launcher's FlatStyle buttons): edge border, bold label, own colour.
sealed class PaperButton : Border
{
    readonly TextBlock label;
    Color back;
    bool hover;
    public Color? HoverColor;
    public event Action? Click;

    public PaperButton(string text, Color back, double fontSize)
    {
        label = new TextBlock { Text = text, FontWeight = FontWeight.Bold, FontSize = fontSize, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center };
        Child = label;
        BorderThickness = new Thickness(2);
        Padding = new Thickness(10, 4, 10, 4);
        Margin = new Thickness(0, 6, 10, 6);
        Cursor = new Cursor(StandardCursorType.Hand);
        HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left;  // sized to the label, like the Windows AutoSize buttons
        Back = back;
        PointerEntered += (_, _) => { hover = true; Paint(); };
        PointerExited += (_, _) => { hover = false; Paint(); };
        PointerPressed += (_, e) =>
        {
            if (!IsEnabled || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            e.Handled = true;
            Click?.Invoke();
        };
        PropertyChanged += (_, e) => { if (e.Property == IsEnabledProperty) Opacity = IsEnabled ? 1 : 0.55; };
    }

    public string Text { get => label.Text ?? ""; set => label.Text = value; }
    public Color Back { get => back; set { back = value; Paint(); } }
    public IBrush? TextBrush { set => label.Foreground = value; }

    // Orange and gold buttons take dark text, the rest light; the nav buttons light up under the pointer.
    void Paint()
    {
        Background = new SolidColorBrush(hover && HoverColor is { } h && back == MainWindow.Card ? h : back);
        BorderBrush = new SolidColorBrush(back == MainWindow.Card ? MainWindow.Edge : back);
        if (label.Foreground == null || label.Foreground == MainWindow.SnowBrush || Equals(label.Foreground, Brushes.Black))
            label.Foreground = back == MainWindow.Orange || back == MainWindow.Gold ? new SolidColorBrush(MainWindow.Night) : MainWindow.SnowBrush;
    }
}
