// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10 (2026-10-28, Enhance): a surface's "glow". The Mac twin
// is GlowEdge in ScreenView.swift; the colours are the same. A progress bar's
// "glow" look is drawn as every bar since 2026-11-05 (ProgressLine, docs/design/20
// "Craft rules": one progress language).
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
    /// A multicolour edge round a surface, after Apple Intelligence's: a crisp
    /// 3-px ring, a soft glow and a wide halo, each turning at its own pace (the
    /// middle one the other way), the halo breathing. "moving" animates it;
    /// "still" holds one quiet ring. The Mac twin is GlowEdge.
    /// </summary>
    private static UIElement WithGlow(UIElement inner, string state)
    {
        if (state is not ("moving" or "still")) return inner;
        bool moving = state == "moving";
        var ringTurn = new RotateTransform { CenterX = 0.5, CenterY = 0.5 };
        var midTurn = new RotateTransform { CenterX = 0.5, CenterY = 0.5 };
        var haloTurn = new RotateTransform { CenterX = 0.5, CenterY = 0.5 };
        LinearGradientBrush GlowBrush(RotateTransform turn) => new()
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
        // No blur on a Border in the islands: wider, fainter layers stand in for it.
        var halo = new Border { Background = GlowBrush(haloTurn), CornerRadius = new CornerRadius(20), Margin = new Thickness(-14), Opacity = moving ? 0.16 : 0.12 };
        var mid = new Border { Background = GlowBrush(midTurn), CornerRadius = new CornerRadius(13), Margin = new Thickness(-7), Opacity = moving ? 0.32 : 0 };
        var ring = new Border { Background = GlowBrush(ringTurn), CornerRadius = new CornerRadius(9), Margin = new Thickness(-3) };
        root.Children.Add(halo);
        root.Children.Add(mid);
        root.Children.Add(ring);
        root.Children.Add(inner);
        // The layers are decoration; the surface keeps its own name.
        foreach (UIElement layer in new UIElement[] { halo, mid, ring })
        {
            AutomationProperties.SetAccessibilityView(layer, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        }
        if (moving && GlowAnimates())
        {
            OnGlowFrames(root, t =>
            {
                ringTurn.Angle = GlowMath.Turn(t);
                midTurn.Angle = -GlowMath.Turn(t, GlowMath.MidTurnSeconds);
                haloTurn.Angle = GlowMath.Turn(t, GlowMath.HaloTurnSeconds);
                double breath = GlowMath.Breathe(t);
                halo.Opacity = 0.12 + 0.14 * breath;
                halo.Margin = new Thickness(-12 - 4 * breath);
            });
        }
        return root;
    }
}
