// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Hosting;
using Windows.UI;

namespace MediaViewer.Chrome;

/// <summary>
/// The theme an open add-on may put in place of the chrome's own colours
/// (plan/23 "Themes"; the Mac twin is Theme.swift). IslandHost.Theme.cs owns
/// the brushes; this owns which theme is chosen and its tokens.
///
/// The core reads and checks a theme (contrast included) and hands it over as
/// JSON; nothing here reads an add-on. The tokens of the theme in use are kept
/// in theme.json beside settings.ini, so the chrome paints with them at once
/// and never waits for an add-on. The add-on is verified on a worker
/// afterwards, and the chrome returns to its default if it is gone or changed.
///
/// A theme colours the chrome only, never a photo (rule 2).
/// </summary>
public static partial class IslandHost
{
    private sealed record ThemePalette(Color Canvas, Color Surface, Color Title, Color Body,
        Color Disabled, Color Hairline, Color Accent);

    private sealed record ThemeTokens(string Json, string Font, ThemePalette? Dark, ThemePalette? Light);

    private static ThemeTokens? _theme;
    // "addon/theme" of the theme chosen; "" is the default.
    private static string _themeSelection = "";
    // Why the chosen theme is not showing, for Settings; "" when it is.
    private static string _themeNote = "";
    private static ElementTheme _islandTheme = ElementTheme.Default;

    /// <summary>What a new island root asks of Windows' own controls.</summary>
    private static ElementTheme IslandTheme => _islandTheme;

    private static string ThemeFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaViewer", "theme.json");

    private static string ThemeKey(string addon, string theme) => addon + "/" + theme;

    private static ThemePalette? ThemedPalette(ref bool dark)
    {
        if (_theme is null) return null;
        if (_theme.Dark is null) dark = false;
        else if (_theme.Light is null) dark = true;
        return dark ? _theme.Dark : _theme.Light;
    }

    private static Color ParseColour(JsonElement palette, string token)
    {
        // "#rrggbbaa", as the core writes every colour (theme_to_json).
        string text = palette.GetProperty(token).GetString() ?? "";
        if (text.Length != 9 || text[0] != '#') throw new FormatException(token);
        uint v = Convert.ToUInt32(text[1..], 16);
        return ColorHelper.FromArgb((byte)v, (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8));
    }

    private static ThemePalette? ParsePalette(JsonElement theme, string name)
    {
        if (!theme.TryGetProperty(name, out JsonElement p) || p.ValueKind != JsonValueKind.Object) return null;
        return new ThemePalette(ParseColour(p, "canvas"), ParseColour(p, "surface"), ParseColour(p, "title"),
            ParseColour(p, "body"), ParseColour(p, "disabled"), ParseColour(p, "hairline"),
            ParseColour(p, "accent"));
    }

    /// <summary>The core's theme JSON ({"addon","id","name","theme":{...}}), or null.</summary>
    private static ThemeTokens? ParseTheme(string? json)
    {
        if (string.IsNullOrEmpty(json)) return null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement theme = doc.RootElement.GetProperty("theme");
            ThemePalette? dark = ParsePalette(theme, "dark");
            ThemePalette? light = ParsePalette(theme, "light");
            if (dark is null && light is null) return null;
            string font = theme.TryGetProperty("font", out JsonElement f) ? f.GetString() ?? "" : "";
            return new ThemeTokens(json, font, dark, light);
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or FormatException
                                       or InvalidOperationException or OverflowException)
        {
            return null;
        }
    }

    // Chrome start, once: a file of a kilobyte or two beside settings.ini, or
    // nothing there at all. Like the font beside the exe, it is read before
    // the first control is built so nothing is drawn twice; the canvas is
    // already drawing by then and does not wait for the chrome.
    private static void LoadCachedTheme()
    {
        _theme = null;
        _themeSelection = "";
        _themeNote = "";
        try
        {
            if (!File.Exists(ThemeFile)) return;
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(ThemeFile));
            _themeSelection = doc.RootElement.TryGetProperty("selection", out JsonElement s)
                ? s.GetString() ?? "" : "";
            if (_themeSelection.Length == 0) return;
            if (doc.RootElement.TryGetProperty("tokens", out JsonElement t) && t.ValueKind == JsonValueKind.Object)
                _theme = ParseTheme(t.GetRawText());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
        }
        if (_theme is not null && _theme.Font.Length > 0 && FontInstalled(_theme.Font))
        {
            try { _uiFont = new Microsoft.UI.Xaml.Media.FontFamily(_theme.Font); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        }
        if (_themeSelection.Length > 0) VerifyTheme();
    }

    private static void SaveTheme(string selection, string? tokensJson)
    {
        _ = Task.Run(() =>
        {
            try
            {
                if (selection.Length == 0)
                {
                    File.Delete(ThemeFile);
                    return;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(ThemeFile)!);
                string text = "{\"selection\":" + JsonSerializer.Serialize(selection) +
                              (tokensJson is null ? "" : ",\"tokens\":" + tokensJson) + "}";
                string temp = ThemeFile + ".tmp";
                File.WriteAllText(temp, text);
                File.Move(temp, ThemeFile, overwrite: true);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
        });
    }

    /// <summary>Settings' picker. "" returns to the default.</summary>
    private static void ChooseTheme(string key)
    {
        if (key == _themeSelection) return;
        _themeSelection = key;
        _themeNote = "";
        if (key.Length == 0)
        {
            SaveTheme("", null);
            if (_theme is not null)
            {
                _theme = null;
                RefreshTheme();
            }
            RefreshThemeRow();
            return;
        }
        VerifyTheme();
    }

    /// <summary>An add-on was installed, updated or removed.</summary>
    private static void ThemeAddonsChanged()
    {
        if (_themeSelection.Length > 0) VerifyTheme();
    }

    // Reads the theme from its add-on, which re-verifies the add-on: a worker.
    private static void VerifyTheme()
    {
        string chosen = _themeSelection;
        int slash = chosen.IndexOf('/');
        if (slash <= 0) return;
        string addon = chosen[..slash];
        string theme = chosen[(slash + 1)..];
        _ = Task.Run(() =>
        {
            string? json = null;
            try { json = AddonNative.OpenTheme(addon, theme); }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            ThemeTokens? fresh = ParseTheme(json);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                if (_themeSelection != chosen) return;  // chosen again meanwhile
                if (fresh is not null)
                {
                    // Every control holds the face it was built with, so a
                    // typeface (unlike a colour) waits for the next start.
                    _themeNote = fresh.Font.Length > 0 && _theme?.Font != fresh.Font &&
                                 FontInstalled(fresh.Font)
                        ? "The theme's typeface shows the next time MediaViewer starts."
                        : "";
                    SaveTheme(chosen, fresh.Json);
                    if (_theme?.Json != fresh.Json)
                    {
                        _theme = fresh;
                        RefreshTheme();
                    }
                }
                else
                {
                    // Gone, changed on disk, or refused: the default, and say
                    // so. The choice is kept, so installing the add-on again
                    // brings the theme back.
                    _themeNote = "The chosen theme's add-on is missing or did not pass verification, so the default is showing.";
                    SaveTheme(chosen, null);
                    if (_theme is not null)
                    {
                        _theme = null;
                        RefreshTheme();
                    }
                }
                RefreshThemeRow();
            });
        });
    }

    // Windows' own controls (a ComboBox's list, a ToggleSwitch) follow the
    // island's theme, not our brushes: a one-palette theme sets it.
    private static void ApplyIslandTheme(ElementTheme theme)
    {
        _islandTheme = theme;
        foreach (DesktopWindowXamlSource? source in new[]
                 {
                     _source, _filmstrip, _gallery, _transport, _metaPane, _tree, _adjustPane, _jobsPane,
                     _editPane, _editorTimeline, _editorAway,
                 })
        {
            try
            {
                if (source?.Content is FrameworkElement root && root.RequestedTheme != theme)
                    root.RequestedTheme = theme;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
            }
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct LogFont
    {
        public int Height, Width, Escapement, Orientation, Weight;
        public byte Italic, Underline, StrikeOut, FontCharSet, OutPrecision, ClipPrecision, Quality, PitchAndFamily;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string FaceName;
    }

    private delegate int EnumFontProc(IntPtr logFont, IntPtr metrics, uint type, IntPtr param);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    private static extern int EnumFontFamiliesExW(IntPtr hdc, ref LogFont logFont, EnumFontProc callback,
        IntPtr param, uint flags);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr window, IntPtr hdc);

    // A theme names a family; it ships no font file. One that is not on this
    // PC leaves the chrome its own face.
    private static bool FontInstalled(string family)
    {
        if (family.Length == 0 || family.Length > 31) return false;
        IntPtr hdc = GetDC(IntPtr.Zero);
        if (hdc == IntPtr.Zero) return false;
        try
        {
            bool found = false;
            var query = new LogFont { FontCharSet = 1 /* DEFAULT_CHARSET */, FaceName = family };
            EnumFontProc callback = (_, _, _, _) =>
            {
                found = true;
                return 0;  // one is enough
            };
            _ = EnumFontFamiliesExW(hdc, ref query, callback, IntPtr.Zero, 0);
            GC.KeepAlive(callback);
            return found;
        }
        finally
        {
            _ = ReleaseDC(IntPtr.Zero, hdc);
        }
    }
}
