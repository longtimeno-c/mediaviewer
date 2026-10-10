// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    // Roles, rather than sampled colours, let existing and virtualised controls
    // share brushes. Updating a brush repaints every owner, including an open
    // flyout, without rebuilding a tree or disturbing focus / text entry.
    private enum ChromeColour
    {
        Canvas, PanelBg, Surface, Title, Body, Hairline, Selection,
        // MVTheme.accent: the current item's outline, the focused row.
        Accent,
        TrimAccent, TrimKeep, KeyframeTick, StarOn,
        // FakeInput's selected text: its highlight and the text on it.
        TextSelection, TextSelectionInk,
        // The Mac's FlatButtonStyle: disabled text, and the title colour as a
        // hover / pressed (and selected) wash.
        Disabled, HoverWash, PressedWash,
        // What Windows' own controls (slider, toggle, check box) fill with: a
        // theme's accent, the system accent, or the contrast theme's highlight.
        ControlAccent,
    }

    private static readonly Dictionary<ChromeColour, SolidColorBrush> ThemeBrushes = new();
    private static UISettings? _themeSettings;
    private static AccessibilitySettings? _accessibilitySettings;
    private static DispatcherQueue? _themeDispatcher;
    private static IntPtr _themeWindow;

    private static SolidColorBrush Brush(ChromeColour role) => ThemeBrushes[role];

    private static void InitialiseTheme()
    {
        _themeDispatcher = DispatcherQueue.GetForCurrentThread();
        var settings = new UISettings();
        var accessibility = new AccessibilitySettings();
        _themeSettings = settings;
        _accessibilitySettings = accessibility;
        // docs/design/25: the theme in use, from its cached tokens, before the first
        // brush exists; its add-on is verified on a worker afterwards.
        LoadCachedTheme();
        // Brushes first: every Brush(role) lookup depends on them, so a failed
        // change hook below must still leave a complete palette.
        RefreshTheme();
        // Appearance tracking is best effort. It must never cost the chrome:
        // an exception here escapes EnsureApp and no island attaches.
        TrySubscribe(() => settings.ColorValuesChanged += OnThemeColoursChanged);
        // HighContrastChanged needs a CoreWindow; a Win32-hosted island gets
        // E_NOTFOUND (0x80070490). UISettings.ColorValuesChanged also fires on
        // a contrast-theme switch, and HighContrast is still readable.
        _highContrastHooked = TrySubscribe(() => accessibility.HighContrastChanged += OnHighContrastChanged);
    }

    private static bool _highContrastHooked;

    private static bool TrySubscribe(Action subscribe)
    {
        try
        {
            subscribe();
            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return false;
        }
    }

    // UISettings can call on a worker thread. All XAML brushes and the HWND
    // must be updated on the owning dispatcher, never from this callback.
    private static void OnThemeColoursChanged(UISettings sender, object args) => QueueThemeRefresh();
    private static void OnHighContrastChanged(AccessibilitySettings sender, object args) => QueueThemeRefresh();
    private static void QueueThemeRefresh() => _themeDispatcher?.TryEnqueue(RefreshTheme);

    private static void RefreshTheme()
    {
        if (_themeSettings is null) return; // a queued notification after shutdown
        Color foreground = _themeSettings.GetColorValue(UIColorType.Foreground);
        bool dark = foreground.R * 299 + foreground.G * 587 + foreground.B * 114 > 128000;
        bool contrast = _accessibilitySettings?.HighContrast == true;
        // docs/design/25: a theme from an add-on, unless Windows is in a contrast
        // theme, which outranks it. With one palette the chrome keeps that
        // appearance, so Windows' own controls match it.
        ThemePalette? themed = contrast ? null : ThemedPalette(ref dark);

        Color canvas = dark ? Rgb(33, 35, 42) : Rgb(243, 243, 243);
        Color panel = dark ? Rgb(27, 29, 35) : Rgb(249, 249, 249);
        Color surface = dark ? Rgb(48, 50, 58) : Rgb(255, 255, 255);
        Color title = dark ? Rgb(235, 236, 240) : Rgb(27, 27, 27);
        Color body = dark ? Rgb(177, 181, 191) : Rgb(92, 92, 92);
        Color line = dark ? Rgb(78, 81, 91) : Rgb(195, 197, 202);
        Color disabled = dark ? Rgb(112, 115, 125) : Rgb(160, 161, 166);
        // The washes follow the appearance, not a theme token (docs/design/25,
        // the Mac's Color.primary): white on a dark chrome, black on a light one,
        // at low alpha. `dark` is resolved after a one-palette theme forces it.
        Color washInk = dark ? Rgb(255, 255, 255) : Rgb(0, 0, 0);
        Color accent = _themeSettings.GetColorValue(dark ? UIColorType.AccentLight2 : UIColorType.AccentDark2);
        if (themed is not null)
        {
            // The token table is the Mac's too (MVTheme): one background for
            // the chrome, so the panes take the canvas colour.
            canvas = panel = themed.Canvas;
            surface = themed.Surface;
            title = themed.Title;
            body = themed.Body;
            line = themed.Hairline;
            disabled = themed.Disabled;
            accent = themed.Accent;
        }
        Color selection = Blend(canvas, accent, 0.14);
        Color keep = ColorHelper.FromArgb(70, accent.R, accent.G, accent.B);
        Color textSelection = ColorHelper.FromArgb(96, accent.R, accent.G, accent.B);
        Color textSelectionInk = title;
        Color controlAccent = themed is null
            ? _themeSettings.GetColorValue(dark ? UIColorType.AccentLight1 : UIColorType.Accent)
            : accent;

        if (contrast)
        {
            // GetSysColor returns the user's contrast-theme colours, not a
            // hard-coded black/white approximation. Selection uses an outline
            // so ordinary WindowText remains readable inside selected tiles.
            canvas = panel = surface = selection = SystemColour(5); // COLOR_WINDOW
            title = body = line = accent = SystemColour(8);         // COLOR_WINDOWTEXT
            keep = SystemColour(13);                               // COLOR_HIGHLIGHT
            textSelection = SystemColour(13);                      // COLOR_HIGHLIGHT
            textSelectionInk = SystemColour(14);                   // COLOR_HIGHLIGHTTEXT
            disabled = SystemColour(17);                           // COLOR_GRAYTEXT
            controlAccent = SystemColour(13);                      // COLOR_HIGHLIGHT
        }

        Set(ChromeColour.Canvas, canvas);
        Set(ChromeColour.PanelBg, panel);
        Set(ChromeColour.Surface, surface);
        Set(ChromeColour.Title, title);
        Set(ChromeColour.Body, body);
        Set(ChromeColour.Hairline, line);
        Set(ChromeColour.Selection, selection);
        Set(ChromeColour.Accent, accent);
        Set(ChromeColour.TrimAccent, accent);
        Set(ChromeColour.TrimKeep, keep);
        Set(ChromeColour.KeyframeTick, body);
        Set(ChromeColour.StarOn, accent);
        Set(ChromeColour.TextSelection, textSelection);
        Set(ChromeColour.TextSelectionInk, textSelectionInk);
        Set(ChromeColour.Disabled, disabled);
        // In a contrast theme a wash would be invisible against WindowText;
        // the hover then uses the highlight colour, as Windows' own buttons do.
        Set(ChromeColour.HoverWash, contrast ? SystemColour(13)
            : ColorHelper.FromArgb(WashHoverAlpha, washInk.R, washInk.G, washInk.B));
        Set(ChromeColour.PressedWash, contrast ? SystemColour(13)
            : ColorHelper.FromArgb(WashPressedAlpha, washInk.R, washInk.G, washInk.B));
        Set(ChromeColour.ControlAccent, controlAccent);
        PublishControlAccent();

        // The native welcome/game is outside the XAML tree. Ship the current
        // system window colour to its snapshot on the UI thread, including
        // changes to Windows contrast themes. It never alters the photo canvas.
        uint homeRgb = (uint)(canvas.R << 16 | canvas.G << 8 | canvas.B);
        _onCommand?.Invoke(_context, Command.HomeColour, homeRgb);

        ApplyIslandTheme(themed is null ? ElementTheme.Default
            : dark ? ElementTheme.Dark : ElementTheme.Light);
        _themeTitleDark = dark && !contrast;
        ApplyTitleBarTheme(_themeWindow);
        ApplyTitleBarTheme(_editorThemeWindow);  // PR 30: the Video Editor window

        // Milestone H: an add-on's own window follows (IAddonHost2.ThemeChanged).
        RaiseHostThemeChanged();
    }

    private static bool _themeTitleDark = true;
    private static IntPtr _editorThemeWindow;

    private static void ApplyTitleBarTheme(IntPtr window)
    {
        if (window == IntPtr.Zero) return;
        int useDark = _themeTitleDark ? 1 : 0;
        _ = DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, ref useDark, sizeof(int));
    }

    private static void Set(ChromeColour role, Color colour)
    {
        if (ThemeBrushes.TryGetValue(role, out SolidColorBrush? brush)) brush.Color = colour;
        else ThemeBrushes.Add(role, new SolidColorBrush(colour));
    }

    // Lightweight resource keys of Windows' own controls, set once on the
    // Application so every island and add-on window resolves them (the Mac's
    // .tint(MVTheme.accent) on each chrome root). The brush is live: a theme
    // or appearance change recolours it in place, no re-template. Resource
    // overrides only, never a ControlTemplate (IslandHost.Edit.cs FlattenButton).
    private static readonly string[] ControlAccentKeys =
    {
        "SliderTrackValueFill", "SliderTrackValueFillPointerOver", "SliderTrackValueFillPressed",
        "SliderThumbBackground", "SliderThumbBackgroundPointerOver", "SliderThumbBackgroundPressed",
        "ToggleSwitchFillOn", "ToggleSwitchFillOnPointerOver", "ToggleSwitchFillOnPressed",
        "CheckBoxCheckBackgroundFillChecked", "CheckBoxCheckBackgroundFillCheckedPointerOver",
        "CheckBoxCheckBackgroundFillCheckedPressed",
    };
    private static bool _controlAccentPublished;

    private static void PublishControlAccent()
    {
        if (_controlAccentPublished) return;
        try
        {
            ResourceDictionary? resources = Application.Current?.Resources;
            if (resources is null) return;
            SolidColorBrush accent = Brush(ChromeColour.ControlAccent);
            foreach (string key in ControlAccentKeys) resources[key] = accent;
            _controlAccentPublished = true;
        }
        catch (Exception ex)
        {
            // Best effort, like the appearance hooks: never cost the chrome.
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static Color Rgb(byte r, byte g, byte b) => ColorHelper.FromArgb(255, r, g, b);
    private static Color Blend(Color background, Color foreground, double amount) => Rgb(
        (byte)Math.Round(background.R + (foreground.R - background.R) * amount),
        (byte)Math.Round(background.G + (foreground.G - background.G) * amount),
        (byte)Math.Round(background.B + (foreground.B - background.B) * amount));

    private static Color SystemColour(int index)
    {
        uint colour = GetSysColor(index); // COLORREF is 0x00BBGGRR
        return Rgb((byte)colour, (byte)(colour >> 8), (byte)(colour >> 16));
    }

    private static void ShutdownTheme()
    {
        if (_themeSettings is not null) _themeSettings.ColorValuesChanged -= OnThemeColoursChanged;
        if (_accessibilitySettings is not null && _highContrastHooked)
            _accessibilitySettings.HighContrastChanged -= OnHighContrastChanged;
        _highContrastHooked = false;
        _themeSettings = null;
        _accessibilitySettings = null;
        _themeDispatcher = null;
        _themeWindow = IntPtr.Zero;
        try
        {
            ResourceDictionary? resources = _controlAccentPublished ? Application.Current?.Resources : null;
            if (resources is not null)
                foreach (string key in ControlAccentKeys) resources.Remove(key);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
        _controlAccentPublished = false;
        ThemeBrushes.Clear();
    }

    [DllImport("user32.dll")]
    private static extern uint GetSysColor(int index);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
}
