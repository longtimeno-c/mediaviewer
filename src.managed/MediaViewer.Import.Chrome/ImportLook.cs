// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace MediaViewer.Import.Chrome;

/// <summary>
/// The host's look for Import's windows (issue #204; docs/design/25 PR 56): its
/// colour roles, its UI face and the theme's shape, borrowed through
/// <see cref="IAddonHost2"/> and <see cref="IAddonThemeShape"/> the way
/// MediaViewer.Ai.Chrome's Look does (this assembly cannot reference that one).
/// Brushes are made once and recoloured in place when the host's theme
/// changes, so nothing is rebuilt. An older host, without IAddonHost2, gets the
/// system colours and Segoe UI these windows always had.
/// </summary>
internal sealed class ImportLook
{
    private readonly IAddonHost2? _host;
    private readonly IAddonThemeShape? _shape;
    private FontFamily? _font;

    /// <summary>The look of the attached chrome; the system one until Attach.</summary>
    internal static ImportLook Current { get; private set; } = new(null);

    internal static void Attach(IAddonHost? host)
    {
        Current.Detach();
        Current = new ImportLook(host);
    }

    private ImportLook(IAddonHost? host)
    {
        _host = host as IAddonHost2;
        _shape = host as IAddonThemeShape;
        Fill();
        if (_host is not null) _host.ThemeChanged += OnHostThemeChanged;
    }

    /// <summary>Stops following the host (the chrome is shutting down).</summary>
    internal void Detach()
    {
        if (_host is not null) _host.ThemeChanged -= OnHostThemeChanged;
    }

    /// <summary>True when the colours are the host's tokens, not the system's.</summary>
    internal bool FromHost => _host is not null;

    /// <summary>Raised on the UI thread after the brushes took the host's new colours.</summary>
    internal event Action? Changed;

    // Translucent greys and the system accent without a host, as before.
    internal SolidColorBrush Neutral { get; } = new();     // a bar's track, the banner
    internal SolidColorBrush CardFill { get; } = new();
    internal SolidColorBrush CardStroke { get; } = new();
    internal SolidColorBrush Accent { get; } = new();
    internal SolidColorBrush Ink { get; } = new();         // text
    internal SolidColorBrush Muted { get; } = new();       // a field's placeholder
    internal SolidColorBrush Highlight { get; } = new();   // selected text
    internal SolidColorBrush Canvas { get; } = new();      // the window, with a host
    private readonly SolidColorBrush _clear = new(Colors.Transparent);

    private void OnHostThemeChanged()
    {
        Fill();
        Changed?.Invoke();
    }

    private void Fill()
    {
        if (_host is null)
        {
            var system = new Windows.UI.ViewManagement.UISettings();
            Color accent = system.GetColorValue(Windows.UI.ViewManagement.UIColorType.Accent);
            Neutral.Color = ColorHelper.FromArgb(0x33, 0x80, 0x80, 0x80);
            CardFill.Color = ColorHelper.FromArgb(0x14, 0x80, 0x80, 0x80);
            CardStroke.Color = ColorHelper.FromArgb(0x29, 0x80, 0x80, 0x80);
            Accent.Color = accent;
            Ink.Color = system.GetColorValue(Windows.UI.ViewManagement.UIColorType.Foreground);
            Muted.Color = ColorHelper.FromArgb(0x99, 0x80, 0x80, 0x80);
            Highlight.Color = ColorHelper.FromArgb(0x60, accent.R, accent.G, accent.B);
            return;
        }
        Color a = Role(AddonColour.Accent);
        Neutral.Color = Role(AddonColour.Hairline);
        CardFill.Color = Role(AddonColour.Surface);
        CardStroke.Color = Role(AddonColour.Hairline);
        Accent.Color = a;
        Ink.Color = Role(AddonColour.Title);
        Muted.Color = Role(AddonColour.Body);
        Highlight.Color = ColorHelper.FromArgb(0x60, a.R, a.G, a.B);
        Canvas.Color = Role(AddonColour.Canvas);
    }

    private Color Role(AddonColour role)
    {
        uint argb = _host!.Colour(role);
        return ColorHelper.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);
    }

    /// <summary>What Windows' own controls should draw as: the canvas's lightness
    /// with a host (a theme may hold one appearance), the system's without.</summary>
    internal ElementTheme Theme
    {
        get
        {
            if (_host is null) return ElementTheme.Default;
            Color c = Canvas.Color;
            return c.R * 299 + c.G * 587 + c.B * 114 < 128000 ? ElementTheme.Dark : ElementTheme.Light;
        }
    }

    /// <summary>The host's UI face; Segoe UI without a host.</summary>
    internal FontFamily Font => _font ??= MakeFont();

    private FontFamily MakeFont()
    {
        if (_host is not null)
        {
            try { return new FontFamily(_host.UiFontFamily); }
            catch (Exception ex) when (ex is ArgumentException or COMException) { }
        }
        return new FontFamily("Segoe UI");
    }

    // ---- the theme's shape (1 without IAddonThemeShape: today's literals) ----------

    internal double TypeSize(double literal) =>
        _shape is null || _shape.TypeScale == 1 ? literal : Math.Round(literal * _shape.TypeScale);

    internal double Space(double literal) =>
        _shape is null || _shape.Density == 1 ? literal : Math.Round(literal * _shape.Density);

    internal Thickness Pad(double uniform) => new(Space(uniform));

    internal Thickness Pad(double left, double top, double right, double bottom) =>
        new(Space(left), Space(top), Space(right), Space(bottom));

    internal CornerRadius Corner(double literal) =>
        new(_shape is null ? literal : literal * _shape.RadiusScale);

    /// <summary>The window's type-in fields (Shared\FakeInput.cs).</summary>
    internal MediaViewer.Shared.FakeInputLook InputLook => _inputLook ??= new()
    {
        Font = () => Font,
        FontSize = () => TypeSize(14),
        Title = () => Ink,
        Body = () => Muted,
        Canvas = () => _clear,
        Hairline = () => Neutral,
        Selection = () => Highlight,
        SelectionInk = () => Ink,
    };

    private MediaViewer.Shared.FakeInputLook? _inputLook;

    /// <summary>
    /// The window's root: <paramref name="layout"/> in the host's face and text
    /// colour, which its text inherits, filled with the canvas token and told
    /// which appearance to draw Windows' controls in. Without a host it is the
    /// layout as it was (its ThemeResource background, the system's theme).
    /// </summary>
    internal UIElement Root(Grid layout, Window window)
    {
        if (_host is null) return layout;
        layout.Background = Canvas;
        var root = new UserControl
        {
            Content = layout,
            FontFamily = Font,
            FontSize = TypeSize(14),
            Foreground = Ink,
            RequestedTheme = Theme,
        };
        void Follow() => root.RequestedTheme = Theme;
        Changed += Follow;
        window.Closed += (_, _) => Changed -= Follow;
        return root;
    }
}
