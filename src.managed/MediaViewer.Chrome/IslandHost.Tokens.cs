// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

// The chrome's shape, size and motion tokens: the Mac's values
// (Theme.swift, CommandBarView.swift FlatButtonStyle), named once here so a
// surface asks for "a card" rather than a literal. Colours stay roles in
// IslandHost.Theme.cs; these never change at run time.
public static partial class IslandHost
{
    // Corner radii. The island windows themselves are rectangular, so these
    // round what sits inside them.
    private const double RadiusButton = 4;
    private const double RadiusFlyout = 6;
    private const double RadiusCard = 8;
    private const double RadiusFloating = 10;
    private const double RadiusSheet = 12;

    // Type ramp around UiFontSize (16, the bar's size).
    private const double TypeCaption = 12;
    private const double TypeSmall = 13;
    private const double TypeBody = 14;
    private const double TypeHeading = 20;

    // Hover / pressed / selected washes: the appearance's text colour at the
    // Mac's primary.opacity 0.06 and 0.11 (FlatButtonStyle).
    private const byte WashHoverAlpha = 15;
    private const byte WashPressedAlpha = 28;

    // Segoe Fluent Icons on Windows 11, MDL2 Assets on 10: the same code points.
    private static FontFamily? _iconFont;
    private static FontFamily IconFont => _iconFont ??= new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets");

    private static class Glyph
    {
        public const string Play = "\uE768";
        public const string Pause = "\uE769";
        public const string Rewind = "\uEB9E";
        public const string FastForward = "\uEB9D";
        public const string More = "\uE712";
        public const string Close = "\uE711";
    }

    // An icon-only bar button in the flat style; the tooltip doubles as its
    // accessible name (the Mac's .help plus VoiceOver label).
    private static Button IconButton(string glyph, string tip, Action click)
    {
        var b = new Button
        {
            Content = new FontIcon { Glyph = glyph, FontFamily = IconFont, FontSize = UiFontSize, Foreground = Brush(Title) },
            Background = Brush(Microsoft.UI.Colors.Transparent),
            Padding = new Thickness(10, 7, 10, 7),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            // As TextButton: a click must not park the keyboard on the island.
            AllowFocusOnInteraction = false,
        };
        FlattenButton(b);
        SetIconTip(b, tip);
        b.Click += (_, _) => Guarded(click);
        return b;
    }

    private static void SetIcon(Button b, string glyph, string tip)
    {
        if (b.Content is FontIcon icon && icon.Glyph != glyph) icon.Glyph = glyph;
        SetIconTip(b, tip);
    }

    private static void SetIconTip(Button b, string tip)
    {
        if (ToolTipService.GetToolTip(b) as string == tip) return;
        ToolTipService.SetToolTip(b, tip);
        AutomationProperties.SetName(b, tip);
    }

    // m:ss, or h:mm:ss from an hour (TransportView.swift clock).
    private static string Clock(long ns)
    {
        long total = Math.Max(0, ns / 1_000_000_000);
        long h = total / 3600, m = total % 3600 / 60, s = total % 60;
        return h > 0 ? $"{h}:{m:00}:{s:00}" : $"{m}:{s:00}";
    }
}
