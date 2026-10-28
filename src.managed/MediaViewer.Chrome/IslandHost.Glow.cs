// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10 (2026-10-28, Enhance): a surface's "glow" and a progress
// bar's "glow" look. The Mac twins are GlowEdge and GlowBar in ScreenView.swift;
// the colours are the same.
//
// Both are the window's own drawing, never the canvas: plain Borders with
// gradient brushes (they load in the islands), stepped on the window's frames
// at 30 fps at most, only while loaded, and never with Windows' animations off.
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    // Violet, pink, amber, teal, blue, and violet again so the turn has no seam.
    private static readonly Windows.UI.Color[] GlowColors =
    {
        Windows.UI.Color.FromArgb(255, 140, 92, 245), Windows.UI.Color.FromArgb(255, 250, 92, 158),
        Windows.UI.Color.FromArgb(255, 255, 158, 64), Windows.UI.Color.FromArgb(255, 64, 199, 217),
        Windows.UI.Color.FromArgb(255, 92, 133, 255), Windows.UI.Color.FromArgb(255, 140, 92, 245),
    };

    private static readonly System.Diagnostics.Stopwatch GlowClock = System.Diagnostics.Stopwatch.StartNew();

    private static GradientStopCollection GlowStops()
    {
        var stops = new GradientStopCollection();
        for (int i = 0; i < GlowColors.Length; ++i)
        {
            stops.Add(new GradientStop { Color = GlowColors[i], Offset = (double)i / (GlowColors.Length - 1) });
        }
        return stops;
    }

    private static bool GlowAnimates()
    {
        try { return new Windows.UI.ViewManagement.UISettings().AnimationsEnabled; }
        catch (Exception) { return false; }
    }

    // Calls `step` with the seconds since start on the window's frames, at most
    // 30 times a second, while `owner` is loaded.
    private static void OnGlowFrames(FrameworkElement owner, Action<double> step)
    {
        double last = -1;
        void Frame(object? sender, object e)
        {
            double now = GlowClock.Elapsed.TotalSeconds;
            if (now - last < 0.030) return;
            last = now;
            step(now);
        }
        bool on = false;
        owner.Loaded += (_, _) =>
        {
            if (on) return;
            on = true;
            CompositionTarget.Rendering += Frame;
        };
        owner.Unloaded += (_, _) =>
        {
            if (!on) return;
            on = false;
            CompositionTarget.Rendering -= Frame;
        };
    }

    /// <summary>
    /// A soft multicolour edge round a surface: a 3-px ring and a wider faint
    /// halo behind it. "moving" turns the colours once every six seconds;
    /// "still" holds them.
    /// </summary>
    private static UIElement WithGlow(UIElement inner, string state)
    {
        if (state is not ("moving" or "still")) return inner;
        var turn = new RotateTransform { CenterX = 0.5, CenterY = 0.5 };
        LinearGradientBrush GlowBrush() => new()
        {
            StartPoint = new Windows.Foundation.Point(0, 0),
            EndPoint = new Windows.Foundation.Point(1, 1),
            GradientStops = GlowStops(),
            RelativeTransform = turn,
        };
        var root = new Grid();
        if (inner is FrameworkElement fe)
        {
            root.HorizontalAlignment = fe.HorizontalAlignment;
            root.VerticalAlignment = fe.VerticalAlignment;
        }
        root.Children.Add(new Border { Background = GlowBrush(), CornerRadius = new CornerRadius(14), Margin = new Thickness(-8), Opacity = 0.28 });
        root.Children.Add(new Border { Background = GlowBrush(), CornerRadius = new CornerRadius(9), Margin = new Thickness(-3) });
        root.Children.Add(inner);
        // The ring and halo are decoration; the surface keeps its own name.
        AutomationProperties.SetAccessibilityView(root.Children[0], Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        AutomationProperties.SetAccessibilityView(root.Children[1], Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        if (state == "moving" && GlowAnimates())
        {
            OnGlowFrames(root, t => turn.Angle = GlowMath.Turn(t));
        }
        return root;
    }

    /// <summary>
    /// A progress bar in the glow's colours: a highlight sweeps along it while
    /// it moves; not started (value &lt; 0), the whole bar shows dimmed.
    /// </summary>
    private static FrameworkElement BuildGlowBar(double v)
    {
        var track = new Grid { Height = 6, MinWidth = 120, Background = Brush(Hairline), CornerRadius = new CornerRadius(3) };  // a pill
        var fill = new Border
        {
            CornerRadius = new CornerRadius(3),
            HorizontalAlignment = HorizontalAlignment.Left,
            Opacity = v < 0 ? 0.55 : 1,
            Background = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0, 0.5),
                EndPoint = new Windows.Foundation.Point(1, 0.5),
                GradientStops = GlowStops(),
            },
        };
        var clear = Windows.UI.Color.FromArgb(0, 255, 255, 255);
        var lit = Windows.UI.Color.FromArgb(128, 255, 255, 255);
        var a = new GradientStop { Color = clear, Offset = 0 };
        var b = new GradientStop { Color = lit, Offset = 0 };
        var c = new GradientStop { Color = clear, Offset = 0 };
        var sheen = new Border
        {
            CornerRadius = new CornerRadius(3),
            HorizontalAlignment = HorizontalAlignment.Left,
            Background = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0, 0.5),
                EndPoint = new Windows.Foundation.Point(1, 0.5),
                GradientStops = { a, b, c },
            },
        };
        track.Children.Add(fill);
        track.Children.Add(sheen);
        track.SizeChanged += (_, _) => fill.Width = sheen.Width = track.ActualWidth * (v < 0 ? 1 : Math.Clamp(v, 0, 1));
        if (GlowAnimates())
        {
            OnGlowFrames(track, t =>
            {
                // A band 30 % of the bar wide, from just off its left end to just off its right.
                (a.Offset, b.Offset, c.Offset) = GlowMath.Band(t);
            });
        }
        AutomationProperties.SetHelpText(track, v < 0 ? "In progress" : $"{Math.Round(v * 100)} percent");
        return track;
    }
}
