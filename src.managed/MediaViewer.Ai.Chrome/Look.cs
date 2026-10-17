// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Numerics;
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// The base chrome's look, borrowed through IAddonHost2 so Local search reads
/// as part of the app: its colour roles (light, dark and high contrast), its
/// UI font, and its reduce-motion answer. Brushes are shared and recoloured in
/// place when the theme changes, as IslandHost.Theme.cs does.
/// </summary>
internal sealed class Look
{
    private readonly IAddonHost2 _host;
    private readonly Dictionary<AddonColour, SolidColorBrush> _brushes = new();
    private FontFamily? _font;

    public Look(IAddonHost2 host)
    {
        _host = host;
        Refresh();
    }

    public event Action? Changed;

    public SolidColorBrush this[AddonColour role]
    {
        get
        {
            if (!_brushes.TryGetValue(role, out SolidColorBrush? b))
            {
                b = new SolidColorBrush(ToColor(_host.Colour(role)));
                _brushes[role] = b;
            }
            return b;
        }
    }

    public FontFamily Font => _font ??= MakeFont();
    public double FontSize => _host.UiFontSize;
    public bool Motion => _host.AnimationsEnabled;

    /// <summary>A type-in field's look (Shared\FakeInput.cs; a WinUI TextBox
    /// fail-fasts in this host) in these colours, at <paramref name="fontSize"/>.</summary>
    public MediaViewer.Shared.FakeInputLook Input(double fontSize) => new()
    {
        Font = () => Font,
        FontSize = () => fontSize,
        Title = () => this[AddonColour.Title],
        Body = () => this[AddonColour.Body],
        Canvas = () => this[AddonColour.Canvas],
        Hairline = () => this[AddonColour.Hairline],
        Selection = () => LiveTint(AddonColour.Accent, 96),
        SelectionInk = () => this[AddonColour.Title],
    };

    /// <summary>A fresh brush (not shared) for a tinted variant of a role.</summary>
    public SolidColorBrush Tint(AddonColour role, byte alpha)
    {
        Color c = ToColor(_host.Colour(role));
        return new SolidColorBrush(Color.FromArgb(alpha, c.R, c.G, c.B));
    }

    /// <summary>A tinted variant of a role, shared and recoloured in place on a theme
    /// change like the role brushes (for control resources set once).</summary>
    public SolidColorBrush LiveTint(AddonColour role, byte alpha)
    {
        if (!_tints.TryGetValue((role, alpha), out SolidColorBrush? b))
        {
            b = Tint(role, alpha);
            _tints[(role, alpha)] = b;
        }
        return b;
    }

    private readonly Dictionary<(AddonColour, byte), SolidColorBrush> _tints = new();

    public void Refresh()
    {
        foreach ((AddonColour role, SolidColorBrush brush) in _brushes) brush.Color = ToColor(_host.Colour(role));
        foreach (((AddonColour role, byte alpha), SolidColorBrush brush) in _tints)
        {
            Color c = ToColor(_host.Colour(role));
            brush.Color = Color.FromArgb(alpha, c.R, c.G, c.B);
        }
        if (_critical is not null) _critical.Color = CriticalColour();
        Changed?.Invoke();
    }

    private FontFamily MakeFont()
    {
        try { return new FontFamily(_host.UiFontFamily); }
        catch (Exception ex) when (ex is ArgumentException or COMException)
        {
            return new FontFamily("Segoe UI Variable Text");
        }
    }

    private static Color ToColor(uint argb) =>
        Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);

    // ---- building blocks ----------------------------------------------------------

    public TextBlock Text(string s, double? size = null, AddonColour role = AddonColour.Body, bool wrap = true) => new()
    {
        Text = s,
        FontFamily = Font,
        FontSize = size ?? FontSize,
        Foreground = this[role],
        TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap,
        TextTrimming = wrap ? TextTrimming.None : TextTrimming.CharacterEllipsis,
    };

    public Button Button(string label, Action click, bool accent = false)
    {
        var b = new Button
        {
            Content = label,
            FontFamily = Font,
            FontSize = FontSize - 2,
            Padding = new Thickness(12, 5, 12, 6),
            CornerRadius = new CornerRadius(6),
        };
        if (accent && Application.Current?.Resources.TryGetValue("AccentButtonStyle", out object? style) == true &&
            style is Style s)
        {
            b.Style = s;
        }
        b.Click += (_, _) => click();
        return b;
    }

    public Border Card(UIElement child, double padding = 14) => new()
    {
        Child = child,
        Padding = new Thickness(padding),
        CornerRadius = new CornerRadius(8),
        Background = this[AddonColour.Surface],
    };

    /// <summary>
    /// A button for an action that deletes something (the Mac's
    /// <c>role: .destructive</c>): the label in the system's critical red, kept
    /// through pointer-over and pressed. High contrast keeps the system's colours.
    /// </summary>
    public Button DestructiveButton(string label, Action click)
    {
        Button b = Button(label, click);
        if (HighContrast()) return b;
        SolidColorBrush red = Critical;
        b.Foreground = red;
        b.Resources["ButtonForeground"] = red;
        b.Resources["ButtonForegroundPointerOver"] = red;
        b.Resources["ButtonForegroundPressed"] = red;
        return b;
    }

    /// <summary>
    /// The system's critical red (Fluent SystemFillColorCritical: #C42B1C on a
    /// light theme, #FF99A4 on a dark one), as the Mac uses systemRed rather than
    /// a theme colour. Shared and recoloured on a theme change.
    /// </summary>
    public SolidColorBrush Critical => _critical ??= new SolidColorBrush(CriticalColour());

    private SolidColorBrush? _critical;

    private Color CriticalColour()
    {
        Color c = ToColor(_host.Colour(AddonColour.Canvas));
        bool dark = 0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B < 128;
        return dark ? Color.FromArgb(255, 0xFF, 0x99, 0xA4) : Color.FromArgb(255, 0xC4, 0x2B, 0x1C);
    }

    /// <summary>Whether Windows is in a contrast theme: custom fills give way to the system's.</summary>
    public static bool HighContrast()
    {
        try { return new Windows.UI.ViewManagement.AccessibilitySettings().HighContrast; }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return false;
        }
    }

    /// <summary>Segoe Fluent Icons, with Segoe MDL2 Assets on Windows 10.</summary>
    public static FontFamily IconFont => _iconFont ??= new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets");

    private static FontFamily? _iconFont;

    /// <summary>The recessed track a single-choice group sits in (a segmented control).</summary>
    public Border Track(UIElement segments, bool hc) => new()
    {
        Child = segments,
        Padding = new Thickness(2),
        CornerRadius = new CornerRadius(7),
        Background = hc ? new SolidColorBrush(Microsoft.UI.Colors.Transparent) : LiveTint(AddonColour.Body, 22),
        BorderBrush = this[AddonColour.Hairline],
        BorderThickness = new Thickness(1),
    };

    /// <summary>
    /// One segment of a track: quiet until chosen, then raised on the surface
    /// colour with the title colour and a hairline. Lightweight styling (the
    /// template's own resource keys), so pointer, pressed and the system focus
    /// visual stay Fluent; high contrast keeps the system's toggle look.
    /// </summary>
    public Microsoft.UI.Xaml.Controls.Primitives.ToggleButton Segment(string label, Action click, bool hc, double fontSize = 13)
    {
        var b = new Microsoft.UI.Xaml.Controls.Primitives.ToggleButton
        {
            Content = label,
            FontFamily = Font,
            FontSize = fontSize,
            Padding = new Thickness(10, 2, 10, 3),
            MinHeight = 0,
            MinWidth = 0,
            Height = 24,
            CornerRadius = new CornerRadius(5),
            BorderThickness = new Thickness(1),
        };
        if (!hc)
        {
            var clear = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            ToggleBrushes(b, "Background", clear, LiveTint(AddonColour.Body, 26), LiveTint(AddonColour.Body, 40),
                          this[AddonColour.Surface], this[AddonColour.Surface], LiveTint(AddonColour.Surface, 210));
            ToggleBrushes(b, "Foreground", this[AddonColour.Body], this[AddonColour.Title], this[AddonColour.Title],
                          this[AddonColour.Title], this[AddonColour.Title], this[AddonColour.Title]);
            ToggleBrushes(b, "BorderBrush", clear, clear, clear,
                          this[AddonColour.Hairline], this[AddonColour.Hairline], this[AddonColour.Hairline]);
        }
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, label);
        b.Click += (_, _) => click();
        return b;
    }

    /// <summary>ToggleButton lightweight styling: rest, pointer over, pressed, then the checked three.</summary>
    public static void ToggleBrushes(Microsoft.UI.Xaml.Controls.Primitives.ToggleButton b, string part, Brush rest, Brush over,
                                     Brush pressed, Brush on, Brush onOver, Brush onPressed)
    {
        b.Resources[$"ToggleButton{part}"] = rest;
        b.Resources[$"ToggleButton{part}PointerOver"] = over;
        b.Resources[$"ToggleButton{part}Pressed"] = pressed;
        b.Resources[$"ToggleButton{part}Checked"] = on;
        b.Resources[$"ToggleButton{part}CheckedPointerOver"] = onOver;
        b.Resources[$"ToggleButton{part}CheckedPressed"] = onPressed;
    }

    // ---- motion (docs/design/17 brief: quick, physical, never blocking input) ------------

    /// <summary>Panel in: opacity 0→1 and scale 0.96→1, ~220 ms; a fade alone under reduce motion.</summary>
    public void PanelIn(UIElement e)
    {
        e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) };
        e.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(220) };
        e.Opacity = 0;
        // The first show has no size yet to scale about: that one only fades.
        e.Scale = Motion && CentreScale(e) ? new Vector3(0.96f, 0.96f, 1) : Vector3.One;
        e.DispatcherQueue.TryEnqueue(() =>
        {
            e.Opacity = 1;
            e.Scale = Vector3.One;
        });
    }

    /// <summary>Panel out, ~140 ms, then <paramref name="done"/>.</summary>
    public void PanelOut(UIElement e, Action done)
    {
        e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(140) };
        e.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(140) };
        e.Opacity = 0;
        if (Motion && CentreScale(e)) e.Scale = new Vector3(0.98f, 0.98f, 1);
        var timer = e.DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(150);
        timer.IsRepeating = false;
        timer.Tick += (t, _) =>
        {
            _fading.Remove(t);
            done();
        };
        // Held until it fires: a timer only a local refers to can be collected
        // mid-fade, and then the panel is left faded out but never hidden.
        _fading.Add(timer);
        timer.Start();
    }

    private static readonly HashSet<Microsoft.UI.Dispatching.DispatcherQueueTimer> _fading = new();

    private static bool CentreScale(UIElement e)
    {
        if (e is not FrameworkElement fe || fe.ActualWidth <= 0) return false;
        e.CenterPoint = new Vector3((float)fe.ActualWidth / 2, (float)fe.ActualHeight / 2, 0);
        return true;
    }

    // ---- words -----------------------------------------------------------------------

    /// <summary>The status line, shared by the footer, the pill and Settings.</summary>
    public static string StatusLine(in MvAiStatus s)
    {
        if ((s.Flags & MvAiStatus.FlagNoModels) != 0) return "The search models are missing. Reinstall local search in Settings.";
        if ((s.Flags & MvAiStatus.FlagIndexFull) != 0) return "Index is full — raise the cap or remove a folder";
        string n(ulong v) => v.ToString("N0", CultureInfo.CurrentCulture);
        switch (s.State)
        {
            case MvAiState.Loading:
                // It opens the models only between the viewer's busy spells.
                // "The first time takes a few minutes" only when the pack says
                // this load is that first compile (a Mac's Core ML; no Windows
                // provider sets it), never on every start (2026-09-27).
                if (s.YieldReason != MvAiYield.None) return "Getting ready when the viewer is idle";
                return (s.Flags & MvAiStatus.FlagFirstCompile) != 0
                    ? "Preparing the search model for this computer — the first time takes a few minutes"
                    : "Getting ready…";
            case MvAiState.Error:
                return "Local search could not load its model.";
            case MvAiState.Paused:
                return "Paused";
            case MvAiState.Yielding:
                return s.YieldReason switch
                {
                    MvAiYield.Viewer => "Paused while a video plays",
                    MvAiYield.Battery => "Paused on battery",
                    MvAiYield.Frames => "Paused while the viewer is busy",
                    _ => "Waiting for the viewer",
                };
            case MvAiState.Indexing:
                if (s.MigrateTotal > 0)
                {
                    return $"Upgrading search quality · {n(s.MigrateDone)} of {n(s.MigrateTotal)}" + Eta(s);
                }
                if (s.CloudFetch == MvAiCloudFetch.Downloading)
                {
                    return $"Downloading from OneDrive ({(int)Math.Round(s.CloudFetchProgress * 100)}%) · {n(s.CloudFilesLeft)} left to index";
                }
                // After the pictures, the pack reads a clip's own text, then the
                // text in pictures (before sound and speech, which have their own line).
                bool picturesDone = s.AssetsDone + s.AssetsFailed + s.AssetsUnavailable + s.CloudFilesLeft >= s.AssetsTotal;
                // #354: documents' words come straight after the pictures.
                if (picturesDone && s.DocsDone < s.DocsTotal)
                {
                    return $"Reading documents {n(s.DocsDone)} of {n(s.DocsTotal)}";
                }
                // #354 D2: then their pages' pictures (Words and pages).
                if (picturesDone && s.PagesDone < s.PagesTotal)
                {
                    return $"Looking at document pages {n(s.PagesDone)} of {n(s.PagesTotal)}";
                }
                // #354 D3: then their passages by meaning (the Documents piece).
                if (picturesDone && s.MeaningDone < s.MeaningTotal)
                {
                    return $"Understanding documents {n(s.MeaningDone)} of {n(s.MeaningTotal)}";
                }
                if (picturesDone && s.CuesDone < s.CuesTotal)
                {
                    return $"Reading chapters and subtitles {n(s.CuesDone)} of {n(s.CuesTotal)} clips";
                }
                if (picturesDone && s.TextDone < s.TextTotal)
                {
                    return $"Reading text in pictures {n(s.TextDone)} of {n(s.TextTotal)}";
                }
                // #354 D4: last, the pages of scanned documents, read by the OS.
                if (picturesDone && s.ScansDone < s.ScansTotal)
                {
                    return $"Reading scanned pages {n(s.ScansDone)} of {n(s.ScansTotal)} documents";
                }
                // Online-only files wait for the fetch: they are not "of" the work in hand.
                return $"Indexing {n(s.AssetsDone)} of {n(s.AssetsTotal - Math.Min(s.AssetsTotal, s.CloudFilesLeft))}" + Eta(s);
            default:
                string done = s.FramesIndexed == 0 && s.DocsTotal == 0 ? "Nothing indexed yet" : $"Up to date · {n(s.FramesIndexed)} moments";
                // #354: the documents read for their words, and any skipped behind a password.
                if (s.DocsTotal > 0) done += s.DocsTotal == 1 ? " · 1 document" : $" · {n(s.DocsTotal)} documents";
                if (s.DocsLocked > 0) done += s.DocsLocked == 1 ? " · 1 protected document skipped" : $" · {n(s.DocsLocked)} protected documents skipped";
                return s.CloudFilesLeft > 0 ? $"{done} · {n(s.CloudFilesLeft)} only in OneDrive" : done;
        }
    }

    /// <summary>Why the OneDrive fetch is waiting, for Settings; null when it is not.</summary>
    public static string? CloudWait(in MvAiStatus s) => s.CloudFetch switch
    {
        MvAiCloudFetch.WaitNetwork => "Waiting for an unmetered connection.",
        MvAiCloudFetch.WaitPower => "Waiting until this PC is plugged in.",
        MvAiCloudFetch.LowDisk => "Waiting: the drive has less than 10 GB free.",
        MvAiCloudFetch.Paused => "Waiting: indexing is paused.",
        MvAiCloudFetch.RetryLater => "OneDrive did not send some files. They are tried again next time MediaViewer starts.",
        _ => null,
    };

    /// <summary>" · about 6–9 min", from completed work only (docs/design/17: no hard-coded claim).</summary>
    private static string Eta(in MvAiStatus s)
    {
        if (s.EtaLowSeconds < 0 || s.EtaHighSeconds <= 0) return "";
        double lo = s.EtaLowSeconds, hi = Math.Max(s.EtaHighSeconds, s.EtaLowSeconds);
        if (hi < 90) return " · about a minute";
        if (hi < 90 * 60)
        {
            long a = (long)Math.Max(1, Math.Round(lo / 60)), b = (long)Math.Max(1, Math.Round(hi / 60));
            return a == b ? $" · about {a} min" : $" · about {a}–{b} min";
        }
        long h0 = (long)Math.Max(1, Math.Round(lo / 3600)), h1 = (long)Math.Max(1, Math.Round(hi / 3600));
        return h0 == h1 ? $" · about {h0} h" : $" · about {h0}–{h1} h";
    }

    /// <summary>The compute badge: where inference runs.</summary>
    public static string ComputeBadge(in MvAiStatus s) => s.Backend switch
    {
        MvAiBackend.Cuda => "GPU · CUDA",
        MvAiBackend.OpenVino => "GPU · OpenVINO",
        MvAiBackend.CoreMl => "Neural Engine",
        _ => "CPU",
    };

    /// <summary>Why the chosen provider fell back, or null.</summary>
    public static string? FallbackReason(in MvAiStatus s) => s.ProviderFault switch
    {
        0 => null,
        1 => "GPU acceleration is not in this install — using CPU",
        2 => "CUDA 13 or cuDNN 9 not found — using CPU",
        3 => "The GPU provider failed — using CPU",
        4 => "GPU results did not match the CPU's — using CPU",
        5 => "The GPU was slower than the CPU here — using CPU",
        _ => "Using CPU",
    };

    /// <summary>Whether the command-bar pill shows: work in hand, not idle.</summary>
    // Not while loading: the pill is about indexing, and appearing over a
    // viewer that is busy (the load waits for it) would cost it a frame.
    public static bool PillVisible(in MvAiStatus s) =>
        s.State is MvAiState.Indexing or MvAiState.Yielding ||
        (s.State == MvAiState.Paused && s.AssetsDone < s.AssetsTotal);

    /// <summary>Indexing waits on battery: "Index anyway" can override it for now.</summary>
    public static bool OnBattery(in MvAiStatus s) =>
        s.State == MvAiState.Yielding && s.YieldReason == MvAiYield.Battery;

    // Work is running: the ring spins. A load waiting for the viewer is still.
    public static bool Busy(in MvAiStatus s) =>
        s.State == MvAiState.Indexing || (s.State == MvAiState.Loading && s.YieldReason == MvAiYield.None);

    public static string Moment(long ms)
    {
        long sec = Math.Max(0, ms) / 1000;
        return sec >= 3600 ? $"{sec / 3600}:{sec / 60 % 60:00}:{sec % 60:00}" : $"{sec / 60}:{sec % 60:00}";
    }
}

/// <summary>Win32 bits the add-on's own windows need: owner, placement, corners.</summary>
internal static class Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left, Top, Right, Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    public static extern IntPtr SetWindowLongPtr(IntPtr hwnd, int index, IntPtr value);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetPropW(IntPtr hwnd, string name, IntPtr data);

    [DllImport("dwmapi.dll")]
    public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    public const int GwlpHwndParent = -8;
    public const int DwmwaWindowCornerPreference = 33;
    public const int DwmwcpRound = 2;

    /// <summary>
    /// Marks an add-on window so the native key router leaves its keys alone
    /// (main.cpp handle_app_key): typing "n" in the query is text, not "next
    /// matching moment".
    /// </summary>
    public const string AddonWindowProp = "MediaViewer.AddonWindow";

    /// <summary>Owned by the main window (stays above it, never in Alt+Tab on its
    /// own), marked for the router, rounded corners on Windows 11.</summary>
    public static void Adopt(IntPtr hwnd, IntPtr owner)
    {
        if (owner != IntPtr.Zero) SetWindowLongPtr(hwnd, GwlpHwndParent, owner);
        SetPropW(hwnd, AddonWindowProp, 1);
        int round = DwmwcpRound;
        _ = DwmSetWindowAttribute(hwnd, DwmwaWindowCornerPreference, ref round, sizeof(int));
    }

    /// <summary>A rectangle of <paramref name="dipW"/>×<paramref name="dipH"/>
    /// centred over <paramref name="owner"/> (a third of the way down), in pixels.</summary>
    public static Windows.Graphics.RectInt32 CentreOver(IntPtr owner, double dipW, double dipH)
    {
        double scale = 1;
        Rect r = new() { Left = 100, Top = 100, Right = 1300, Bottom = 900 };
        if (owner != IntPtr.Zero && GetWindowRect(owner, out Rect got))
        {
            r = got;
            uint dpi = GetDpiForWindow(owner);
            if (dpi > 0) scale = dpi / 96.0;
        }
        int ow = r.Right - r.Left, oh = r.Bottom - r.Top;
        int w = (int)Math.Min(dipW * scale, Math.Max(480 * scale, ow * 0.92));
        int h = (int)Math.Min(dipH * scale, Math.Max(360 * scale, oh * 0.86));
        int x = r.Left + (ow - w) / 2;
        int y = r.Top + Math.Max(0, (oh - h) / 3);
        return new Windows.Graphics.RectInt32(x, y, w, h);
    }
}
