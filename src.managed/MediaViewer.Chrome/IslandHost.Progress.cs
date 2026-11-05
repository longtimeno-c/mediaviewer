// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules" (2026-11-05): the chrome's one progress
// language. The bar is JobBar (IslandHost.Clip.cs: 3 DIP, eased forward, a
// calm sweep when there is no fraction); ProgressLine puts the one line under
// it ("Enhancing · 40 % · about 1 min left") with a quiet Cancel at its end,
// in fixed slots so nothing reflows, and carries the outcome in the same slot
// when the work ends. The words are Shared/ProgressWords.cs. The Mac twin is
// ProgressLine.swift.
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    // Each keyed bar's meter across rebuilds (a screen window is rebuilt at
    // every revision), forgotten 30 s after it was last seen.
    private sealed record ProgressEntry(ProgressMeter Meter, double Seen);
    private static readonly Dictionary<string, ProgressEntry> ProgressMemory = new(StringComparer.Ordinal);
    private static readonly System.Diagnostics.Stopwatch ProgressClock = System.Diagnostics.Stopwatch.StartNew();

    private static double ProgressNow => ProgressClock.Elapsed.TotalSeconds;

    private static ProgressMeter SampleProgress(string key, double? fraction, out double lastShown)
    {
        double now = ProgressNow;
        if (ProgressMemory.Count > 64)
        {
            foreach (string stale in ProgressMemory.Where(e => now - e.Value.Seen >= 30).Select(e => e.Key).ToList())
                ProgressMemory.Remove(stale);
        }
        ProgressMeter m = ProgressMemory.TryGetValue(key, out ProgressEntry? e) && now - e.Seen < 30 ? e.Meter : new ProgressMeter();
        lastShown = m.Started ? m.Shown : -1;
        m.Add(fraction, now);
        ProgressMemory[key] = new ProgressEntry(m, now);
        return m;
    }

    private sealed class ProgressLine
    {
        public readonly StackPanel Root;
        private readonly JobBar _bar = new();
        private readonly TextBlock _head;
        private readonly TextBlock _tail;
        private readonly Button? _cancel;
        private readonly Grid _line;
        private DispatcherTimer? _tick;
        private ProgressMeter? _meter;
        private string _verb = "";
        private string? _detail;
        private double? _fraction;
        private bool _timed;

        /// <param name="cancel">The quiet Cancel's label and work, or null for none.</param>
        public ProgressLine((string Label, Action Click)? cancel = null)
        {
            _head = Text("", Body, TypeSize(12), maxLines: 1);
            _head.TextWrapping = TextWrapping.NoWrap;
            _head.TextTrimming = TextTrimming.CharacterEllipsis;
            _tail = Text("", Body, TypeSize(12), maxLines: 1);
            _tail.TextWrapping = TextWrapping.NoWrap;
            // Segoe UI's figures are tabular by default, so the numbers hold still.
            _head.VerticalAlignment = _tail.VerticalAlignment = VerticalAlignment.Center;
            var line = new Grid { ColumnSpacing = 0, MinHeight = 22 };
            _line = line;
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            // The verb shortens before the numbers do (FitHead).
            line.Children.Add(_head);
            Grid.SetColumn(_tail, 1);
            line.Children.Add(_tail);
            if (cancel is { } c)
            {
                _cancel = QuietButton(c.Label, c.Click);
                Grid.SetColumn(_cancel, 3);
                line.Children.Add(_cancel);
            }
            line.SizeChanged += (_, _) => FitHead();
            _tail.SizeChanged += (_, _) => FitHead();
            Root = new StackPanel { Spacing = Space(6) };
            Root.Children.Add(_bar.Root);
            Root.Children.Add(line);
            Root.Unloaded += (_, _) => StopTick();
            Root.Loaded += (_, _) => Words();
        }

        private void FitHead()
        {
            double cancel = _cancel?.ActualWidth ?? 0;
            double room = Math.Max(0, _line.ActualWidth - _tail.ActualWidth - cancel - (cancel > 0 ? Space(8) : 0));
            if (Math.Abs(_head.MaxWidth - room) > 0.5) _head.MaxWidth = room;
        }

        /// <summary>
        /// The bar at <paramref name="fraction"/> (null: unknown) with its line.
        /// With a <paramref name="key"/> the bar never goes back across rebuilds
        /// and a <paramref name="timed"/> line says the time left.
        /// </summary>
        public void Show(string verb, double? fraction, string? detail = null, string? key = null, bool timed = false)
        {
            _verb = verb;
            _detail = detail;
            _timed = timed && key is not null;
            double? shown = fraction;
            if (key is not null)
            {
                _meter = SampleProgress(key, fraction, out double last);
                if (fraction is not null && _meter.Started) shown = _meter.Shown;
                if (last >= 0 && _bar.Root.ActualWidth <= 0) _bar.Seed(last);  // a rebuilt bar starts where the last was drawn
            }
            _fraction = shown;
            _bar.IsIndeterminate = shown is null;
            if (shown is double v) _bar.Value = v;
            Words();
            AutomationProperties.SetName(Root, shown is double a ? $"{verb}, {ProgressWords.Percent(a)}" : verb);
        }

        private void Words()
        {
            double? left = _timed && _fraction is < 1 ? _meter?.SecondsLeft(ProgressNow) : null;
            var (head, tail) = ProgressWords.Parts(_verb, _fraction, _detail, left, _timed);
            if (_head.Text != head) _head.Text = head;
            if (_tail.Text != tail) _tail.Text = tail;
            // Counts down once a second between updates, only while it runs and shows.
            if (_timed && _fraction is < 1 && Root.IsLoaded) StartTick(); else StopTick();
        }

        private void StartTick()
        {
            if (_tick is not null) return;
            _tick = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _tick.Tick += (_, _) => Words();
            _tick.Start();
        }

        private void StopTick()
        {
            _tick?.Stop();
            _tick = null;
        }
    }

    // A quiet text action at the end of a line (Cancel, Try again, Move to
    // Recycle Bin): the body colour, the hover wash, no border.
    private static Button QuietButton(string label, Action click)
    {
        TextBlock t = ButtonLabel(label, TypeSize(12));
        t.Foreground = Brush(Body);
        Button b = ChromeButton(t, Pad(8, 3, 8, 3), click, name: label);
        b.CornerRadius = Corner(5);
        b.VerticalAlignment = VerticalAlignment.Center;
        return b;
    }
}
