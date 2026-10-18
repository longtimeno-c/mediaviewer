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

    // A tile whose thumbnail has not arrived: the Mac's .quaternary fill, the
    // appearance's text colour at about 0.1 (FilmstripView, GalleryView).
    private const byte PlaceholderAlpha = 26;
    // The gallery's shift-click range over a thumbnail: accent at 0.22.
    private const byte RangeWashAlpha = 56;
    // A folder tile's count capsule: the Mac's .ultraThinMaterial, a surface
    // that lets the cover show through.
    private const byte CapsuleAlpha = 0xC8;

    // Settings buttons are filled (SettingsView.swift SettingsButtonStyle):
    // primary.opacity 0.08 at rest, 0.16 pressed; hover sits between.
    private const byte SettingsFillAlpha = 20;
    private const byte SettingsFillHoverAlpha = 31;
    private const byte SettingsFillPressedAlpha = 41;
    private const double RadiusSettingsButton = 6;

    // Settings type (SettingsView.swift): section titles 16 semibold, notes
    // 13, details and the footer hint 12.
    private const double TypeSection = 16;
    private const double TypeNote = 13;

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
        public const string Volume = "\uE767";
        public const string Mute = "\uE74F";
        public const string Close = "\uE711";
        public const string Rotate = "\uE7AD";      // clockwise; mirrored for left
        public const string FlipArrows = "\uE8AB";  // Switch: left and right arrows
        public const string Previous = "\uE892";
        public const string Next = "\uE893";
    }

    // A flat icon button for the panes (the Mac's iconButton in EditView.swift:
    // compact FlatButtonStyle, .help and an accessibility label). `transform`
    // turns or mirrors the glyph where Segoe has only one direction.
    private static Button PaneIconButton(string glyph, string label, string? key, Action click,
                                         Transform? transform = null)
    {
        var icon = new FontIcon
        {
            Glyph = glyph, FontFamily = IconFont, FontSize = TypeBody, Foreground = Brush(Title),
            RenderTransform = transform, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5),
        };
        var b = new Button
        {
            Content = icon,
            Background = Brush(Microsoft.UI.Colors.Transparent),
            Padding = Pad(8, 6, 8, 6),
            MinWidth = 36,
            VerticalAlignment = VerticalAlignment.Center,
            // As EditButton: the crop, trim and editor keys stay the canvas's.
            AllowFocusOnInteraction = false,
        };
        FlattenButton(b);
        ToolTipService.SetToolTip(b, key is null ? label : $"{label}  {key}");
        AutomationProperties.SetName(b, label);
        if (key is not null) AutomationProperties.SetHelpText(b, "Key " + key);
        b.Click += (_, _) => Guarded(click);
        return b;
    }

    // Activity with no fraction (the Mac's ProgressView spinner): a quarter
    // arc turning once a second. Not ProgressRing, which cannot load in these
    // islands. The animation runs only while Active is set, so a pane left
    // open on a ready view costs nothing (rule: idle is free).
    private sealed class Spinner
    {
        public readonly FrameworkElement Root;
        private readonly Microsoft.UI.Xaml.Media.Animation.Storyboard _turn;
        private bool _active;

        public Spinner(double size)
        {
            var spin = new RotateTransform();
            var arc = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = size, Height = size,
                Stroke = Brush(Body),
                StrokeThickness = 2,
                // A quarter of the circumference, in stroke thicknesses.
                StrokeDashArray = new DoubleCollection { Math.PI * (size - 2) / 4 / 2, 1000 },
                StrokeDashCap = PenLineCap.Round,
                RenderTransform = spin,
                RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5),
            };
            Root = new Grid
            {
                Width = size, Height = size,
                VerticalAlignment = VerticalAlignment.Center,
                Visibility = Visibility.Collapsed,
                Children = { arc },
            };
            AutomationProperties.SetName(Root, "Working");
            var anim = new Microsoft.UI.Xaml.Media.Animation.DoubleAnimation
            {
                From = 0, To = 360,
                Duration = new Duration(TimeSpan.FromSeconds(1)),
                RepeatBehavior = Microsoft.UI.Xaml.Media.Animation.RepeatBehavior.Forever,
            };
            Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTarget(anim, spin);
            Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTargetProperty(anim, "Angle");
            _turn = new Microsoft.UI.Xaml.Media.Animation.Storyboard { Children = { anim } };
            Root.Unloaded += (_, _) => { _turn.Stop(); _active = false; };
        }

        public bool Active
        {
            get => _active;
            set
            {
                if (value == _active) return;
                _active = value;
                Root.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
                if (value) _turn.Begin(); else _turn.Stop();
            }
        }
    }

    // An icon-only bar button in the flat style; the tooltip doubles as its
    // accessible name (the Mac's .help plus VoiceOver label).
    private static Button IconButton(string glyph, string tip, Action click)
    {
        var b = new Button
        {
            Content = new FontIcon { Glyph = glyph, FontFamily = IconFont, FontSize = UiFontSize, Foreground = Brush(Title) },
            Background = Brush(Microsoft.UI.Colors.Transparent),
            Padding = Pad(10, 7, 10, 7),
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

    // A sheet's default button (the Mac's .keyboardShortcut(.defaultAction)):
    // filled with the accent. AccentButtonStyle comes with XamlControlsResources,
    // which these islands do not have, so the fill is per-element lightweight
    // resources over WinUI's own template, never a ControlTemplate.
    private static Button AccentButton(string label, Action click)
    {
        SolidColorBrush fill = Brush(ChromeColour.ControlAccent);
        SolidColorBrush ink = Brush(ChromeColour.OnAccent);
        var b = new Button
        {
            Content = new TextBlock { Text = label, FontFamily = UiFont, FontSize = UiFontSize, Foreground = ink },
            Background = fill,
            Foreground = ink,
            BorderThickness = new Thickness(0),
            Padding = Pad(16, 7, 16, 7),
            CornerRadius = Corner(RadiusButton),
            VerticalAlignment = VerticalAlignment.Center,
            AllowFocusOnInteraction = false,
        };
        b.Resources["ButtonBackground"] = fill;
        b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(fill.Color) { Opacity = 0.88 };
        b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(fill.Color) { Opacity = 0.76 };
        b.Resources["ButtonBackgroundDisabled"] = Brush(ChromeColour.PressedWash);
        foreach (string key in new[] { "ButtonForeground", "ButtonForegroundPointerOver", "ButtonForegroundPressed" })
            b.Resources[key] = ink;
        b.Resources["ButtonForegroundDisabled"] = Brush(ChromeColour.Disabled);
        foreach (string key in new[] { "ButtonBorderBrush", "ButtonBorderBrushPointerOver",
                                       "ButtonBorderBrushPressed", "ButtonBorderBrushDisabled" })
            b.Resources[key] = Brush(Microsoft.UI.Colors.Transparent);
        b.IsEnabledChanged += (_, _) =>
        {
            if (b.Content is TextBlock t) t.Foreground = b.IsEnabled ? ink : Brush(ChromeColour.Disabled);
        };
        b.Click += (_, _) => Guarded(click);
        return b;
    }

    // m:ss, or h:mm:ss from an hour (TransportView.swift clock).
    private static string Clock(long ns)
    {
        long total = Math.Max(0, ns / 1_000_000_000);
        long h = total / 3600, m = total % 3600 / 60, s = total % 60;
        return h > 0 ? $"{h}:{m:00}:{s:00}" : $"{m}:{s:00}";
    }
}
