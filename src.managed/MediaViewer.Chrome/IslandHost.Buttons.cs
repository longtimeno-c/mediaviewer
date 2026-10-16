// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// The chrome's one button builder (#227). Every chrome button is a WinUI
/// <see cref="Button"/> on its own template, made flat by
/// <see cref="FlattenButton"/>'s lightweight resources (a loaded custom template
/// fail-fasts these islands), transparent at rest, and never parks the
/// keyboard on the island when clicked. The named builders are presets over it:
/// <list type="bullet">
/// <item><c>TextButton</c>: the bar's, a label at the UI size, centred.</item>
/// <item><c>EditButton</c>: the panes', compact by default, left-aligned, with
/// an accessible name and a tooltip. <c>MetaButton</c> (IslandHost.Panels.cs)
/// is an EditButton that can start disabled.</item>
/// <item><c>FlatButton</c>: any content, left-aligned, the click wired by the
/// caller (the folder tree, the clip tools).</item>
/// <item><c>SettingsButton</c> (IslandHost.Settings.cs): a TextButton that takes
/// keyboard focus on click.</item>
/// </list>
/// </summary>
public static partial class IslandHost
{
    /// <param name="content">The button's content: <see cref="ButtonLabel"/> for text.</param>
    /// <param name="click">Run through <see cref="Guarded"/>: an exception out of a
    /// XAML event handler is a fail-fast. Null: the caller wires Click itself.</param>
    /// <param name="fontSize">For text content that inherits it; the UI size when null.</param>
    /// <param name="name">The accessible name, when the content does not give one.</param>
    private static Button ChromeButton(UIElement content, Thickness padding, Action? click = null,
                                       HorizontalAlignment align = HorizontalAlignment.Center,
                                       HorizontalAlignment contentAlign = HorizontalAlignment.Center,
                                       double? fontSize = null, bool tabStop = true,
                                       string? name = null, string? tip = null, bool enabled = true)
    {
        var b = new Button
        {
            Content = content,
            Foreground = Brush(Title),
            Background = Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = padding,
            FontFamily = UiFont,
            FontSize = fontSize ?? UiFontSize,
            FontWeight = Microsoft.UI.Text.FontWeights.Normal,
            HorizontalAlignment = align,
            HorizontalContentAlignment = contentAlign,
            VerticalAlignment = VerticalAlignment.Center,
            IsTabStop = tabStop,
            IsEnabled = enabled,
            // Clicking Open / Play / More, or a crop or trim control, must not
            // park keyboard focus on the island, or the canvas's keys wait for
            // the window to be deactivated (the Mac host re-focuses its view).
            AllowFocusOnInteraction = false,
        };
        // WinUI's own template, restyled through lightweight resources only:
        // a custom ControlTemplate that loads in these islands fail-fasts
        // (0xC000027B) once a bar flyout opens or closes (2026-09-27).
        FlattenButton(b);
        if (click is not null) b.Click += (_, _) => Guarded(click);
        if (name is not null) AutomationProperties_SetName(b, name);
        if (tip is not null) ToolTipService.SetToolTip(b, tip);
        return b;
    }

    /// <summary>A button's text, in the chrome's font and title colour.</summary>
    private static TextBlock ButtonLabel(string text, double size, bool trim = false) => new()
    {
        Text = text,
        FontFamily = UiFont,
        FontSize = size,
        FontWeight = Microsoft.UI.Text.FontWeights.Normal,
        Foreground = Brush(Title),
        TextTrimming = trim ? TextTrimming.CharacterEllipsis : TextTrimming.None,
    };

    private static Thickness ButtonPadding => Pad(14, 7, 14, 7);
    private static Thickness CompactButtonPadding => Pad(8, 5, 8, 5);

    private static Button TextButton(string label, Action click) =>
        ChromeButton(ButtonLabel(label, UiFontSize), ButtonPadding, click);

    private static Button EditButton(string label, Action click, bool compact = true, string? tip = null,
                                     bool stretch = false)
    {
        double size = compact ? UiFontSize - 2 : UiFontSize;
        return ChromeButton(ButtonLabel(label, size, trim: true), compact ? CompactButtonPadding : ButtonPadding, click,
                            align: stretch ? HorizontalAlignment.Stretch : HorizontalAlignment.Left,
                            fontSize: size, name: label, tip: tip);
    }

    // No grey disabled fill (a disabled clip tool read as selected) and the
    // chrome's washes, not Windows' stock ones. The caller wires Click.
    private static Button FlatButton(UIElement content, Thickness padding, bool tabStop = true) =>
        ChromeButton(content, padding, align: HorizontalAlignment.Left,
                     contentAlign: HorizontalAlignment.Left, tabStop: tabStop);
}
