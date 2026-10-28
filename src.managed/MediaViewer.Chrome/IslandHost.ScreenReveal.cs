// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// "reveal" on a screen stack (contribution API 9, 2026-10-28; Driftwire's
/// transfer card). The Mac twin is RevealLook in ScreenView.swift.
/// </summary>
/// <remarks>
/// When a stack's token first shows or changes, its scroller glides it into
/// view (a send started from a code typed at the foot of the page appears in
/// a card far above), an accent outline lights on it and fades, and a stack
/// that is new rises into place. With animations off the scroll jumps and
/// nothing moves; the outline shows and goes. A window's content is rebuilt
/// at every revision (ten a second while bytes move), so what is under way is
/// kept by node id here and picked up by each new build, and a rebuild in the
/// middle of the glide carries on toward the same place rather than stopping.
/// </remarks>
public static partial class IslandHost
{
    private readonly record struct RevealKey(ScreenAt At, string Id);

    private sealed class RevealGlow(double start, bool first)
    {
        public double Start { get; } = start;
        public bool First { get; } = first;
        public Border? Ring { get; set; }
        public FrameworkElement? Card { get; set; }
        public TranslateTransform? Lift { get; set; }
    }

    // The token each stack showed last, by window and node id.
    private static readonly Dictionary<RevealKey, string> RevealSeen = new();
    private static readonly Dictionary<RevealKey, RevealGlow> RevealGlows = new();
    // The reveal ids the window being built holds (null for other builds).
    private static HashSet<string>? _revealBuilt;
    private static readonly System.Diagnostics.Stopwatch RevealClock = System.Diagnostics.Stopwatch.StartNew();
    private static bool _revealTicking;
    private static double _revealLastStep = -1;

    private const double RevealRise = 0.35, RevealFadeIn = 0.25, RevealFadeOut = 1.4;

    // Where a scroller is gliding to, so a rebuild before it gets there keeps going.
    private sealed class RevealAim(double offset, double until)
    {
        public double Offset { get; } = offset;
        public double Until { get; } = until;
    }

    private static readonly ConditionalWeakTable<ScrollViewer, RevealAim> RevealAims = new();

    private static double RevealHold(bool first) => first ? 1.0 : 0.8;

    /// <summary>The stack as built, wrapped for its outline when it carries a token.</summary>
    private static UIElement ScreenRevealed(UIElement built, System.Text.Json.JsonElement n, ScreenAt at)
    {
        string token = JStr(n, "reveal");
        string id = JStr(n, "id");
        if (token.Length == 0 || id.Length == 0 || built is not FrameworkElement card) return built;
        var key = new RevealKey(at, id);
        _revealBuilt?.Add(id);
        bool first = !RevealSeen.TryGetValue(key, out string? was);
        bool moved = first || was != token;
        RevealSeen[key] = token;

        var ring = new Border
        {
            BorderBrush = Brush(ChromeColour.TrimAccent),
            BorderThickness = new Thickness(2),
            CornerRadius = Corner(JStr(n, "look") == "card" ? 10 : 6),
            IsHitTestVisible = false,
            Opacity = 0,
        };
        var lift = new TranslateTransform();
        card.RenderTransform = lift;
        var host = new Grid();
        host.Children.Add(card);
        host.Children.Add(ring);

        double now = RevealClock.Elapsed.TotalSeconds;
        if (moved)
        {
            RevealGlows[key] = new RevealGlow(now, first);
            void Loaded(object sender, RoutedEventArgs e)
            {
                host.Loaded -= Loaded;
                // After the rebuild has put its kept place back (RestoreScreenPlace, on Loaded).
                if (host.DispatcherQueue is not { } queue ||
                    !queue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () => ScrollToReveal(host)))
                {
                    ScrollToReveal(host);
                }
            }
            host.Loaded += Loaded;
        }
        if (RevealGlows.TryGetValue(key, out RevealGlow? glow))
        {
            glow.Ring = ring;
            glow.Card = card;
            glow.Lift = lift;
            if (!StepReveal(glow, now)) RevealGlows.Remove(key);
            else StartRevealTicking();
        }
        return host;
    }

    // Once a window's tree is built: a stack gone from it shows as new when it comes back.
    private static void PruneReveals(ScreenAt at, HashSet<string> built)
    {
        foreach (RevealKey k in RevealSeen.Keys.Where(k => k.At == at && !built.Contains(k.Id)).ToList())
        {
            RevealSeen.Remove(k);
            RevealGlows.Remove(k);
        }
    }

    // A window closed: its stacks are new when it opens again.
    private static void ForgetReveals(ScreenAt at)
    {
        foreach (RevealKey k in RevealSeen.Keys.Where(k => k.At == at).ToList()) RevealSeen.Remove(k);
        foreach (RevealKey k in RevealGlows.Keys.Where(k => k.At == at).ToList()) RevealGlows.Remove(k);
    }

    private static void ScrollToReveal(FrameworkElement target)
    {
        try
        {
            ScrollViewer? sv = null;
            for (DependencyObject? up = VisualTreeHelper.GetParent(target); up is not null; up = VisualTreeHelper.GetParent(up))
            {
                if (up is ScrollViewer s && s.VerticalScrollBarVisibility != ScrollBarVisibility.Disabled) { sv = s; break; }
            }
            if (sv is null || target.XamlRoot is null) return;
            sv.UpdateLayout();
            // A margin above the stack, so it does not land flush with the window's edge.
            double y = target.TransformToVisual(sv).TransformPoint(new Windows.Foundation.Point(0, 0)).Y;
            double to = Math.Clamp(sv.VerticalOffset + y - Space(14), 0, sv.ScrollableHeight);
            if (Math.Abs(to - sv.VerticalOffset) < 1) return;
            bool animate = HostAnimationsEnabled();
            if (animate) RevealAims.AddOrUpdate(sv, new RevealAim(to, RevealClock.Elapsed.TotalSeconds + 1.0));
            sv.ChangeView(null, to, null, disableAnimation: !animate);
        }
        catch (Exception ex)
        {
            // Cosmetic: the card is still there, only not scrolled to.
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
    }

    // NoteScreenPlace: where a scroller is still gliding to, if it is.
    private static double? RevealAimOf(ScrollViewer sv) =>
        RevealAims.TryGetValue(sv, out RevealAim? aim) && aim.Until > RevealClock.Elapsed.TotalSeconds &&
        Math.Abs(aim.Offset - sv.VerticalOffset) >= 1
            ? aim.Offset : null;

    // RestoreScreenPlace: the new scroller goes on from where the old one was.
    private static void ContinueRevealAim(ScrollViewer sv, double offset)
    {
        RevealAims.AddOrUpdate(sv, new RevealAim(offset, RevealClock.Elapsed.TotalSeconds + 1.0));
        sv.ChangeView(null, offset, null, disableAnimation: false);
    }

    private static void StartRevealTicking()
    {
        if (_revealTicking) return;
        _revealTicking = true;
        CompositionTarget.Rendering += RevealTick;
    }

    // On the window's frames, at most 30 fps, as the link picture's dots.
    private static void RevealTick(object? sender, object e)
    {
        double now = RevealClock.Elapsed.TotalSeconds;
        if (now - _revealLastStep < 0.030) return;
        _revealLastStep = now;
        foreach ((RevealKey key, RevealGlow glow) in RevealGlows.ToList())
        {
            if (!StepReveal(glow, now)) RevealGlows.Remove(key);
        }
        if (RevealGlows.Count > 0) return;
        CompositionTarget.Rendering -= RevealTick;
        _revealTicking = false;
    }

    // The outline's and the rise's look at `now`; false once both are over.
    private static bool StepReveal(RevealGlow g, double now)
    {
        double t = now - g.Start;
        bool animate = HostAnimationsEnabled();
        double hold = RevealHold(g.First);
        double ring;
        if (!animate) ring = t < hold + RevealFadeOut / 2 ? 1 : 0;
        else if (t < RevealFadeIn) ring = EaseOut(t / RevealFadeIn);
        else if (t < hold) ring = 1;
        else ring = 1 - EaseOut(Math.Min(1, (t - hold) / RevealFadeOut));
        if (g.Ring is { } r) r.Opacity = ring;

        double rise = !g.First || !animate ? 1 : EaseOut(Math.Min(1, t / RevealRise));
        if (g.Card is { } c) c.Opacity = rise;
        if (g.Lift is { } lift) lift.Y = 14 * (1 - rise);
        return t < hold + RevealFadeOut;
    }

    private static double EaseOut(double p) => 1 - Math.Pow(1 - p, 3);
}
