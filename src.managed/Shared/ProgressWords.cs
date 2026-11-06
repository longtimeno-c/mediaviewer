// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules" (2026-11-05): the one progress language's words
// and arithmetic, apart from any element so the tests can drive it. A run's
// line reads "Enhancing · 40 % · about 1 min left"; a download's "Downloading
// the detail model · 48 of 161 MB · about 25 seconds left". The time left
// follows Driftwire's meter (src/addons/driftwire/eta.h) and its words. The
// Mac twin is ProgressWords.swift, read by the tests for the same constants.
using System.Globalization;

namespace MediaViewer.Shared;

/// <summary>
/// One bar's run: the largest fraction seen (the bar never goes back unless
/// it falls by more than a quarter, a new run) and a smoothed rate: the
/// average since the start for the first 5 s, then a moving average with a
/// 4 s time constant; no estimate before 3 s and 2 % moved.
/// </summary>
public sealed class ProgressMeter
{
    public const double TauS = 4.0;
    public const double WarmS = 5.0;
    public const double ReadyS = 3.0;
    public const double ReadyFraction = 0.02;
    public const double RestartDrop = 0.25;

    private double _startT, _lastT, _startF, _lastF, _rate;

    public bool Started { get; private set; }

    /// <summary>The largest fraction seen this run: what the bar shows.</summary>
    public double Shown { get; private set; }

    /// <summary>The run's fraction at <paramref name="now"/> seconds; null changes nothing.</summary>
    public void Add(double? fraction, double now)
    {
        if (fraction is not double raw || double.IsNaN(raw) || double.IsInfinity(raw)) return;
        double f = Math.Clamp(raw, 0, 1);
        if (!Started || f < Shown - RestartDrop)
        {
            Started = true;
            _startT = _lastT = now;
            _startF = _lastF = f;
            _rate = 0;
            Shown = f;
            return;
        }
        Shown = Math.Max(Shown, f);
        double dt = now - _lastT;
        if (dt < 0.2 || f <= _lastF) return;
        double since = now - _startT;
        if (since <= WarmS)
        {
            _rate = (f - _startF) / since;
        }
        else
        {
            double inst = (f - _lastF) / dt;
            _rate += (1 - Math.Exp(-dt / TauS)) * (inst - _rate);
        }
        _lastT = now;
        _lastF = f;
    }

    public bool Ready => Started && _rate > 0 && _lastT - _startT >= ReadyS && _lastF - _startF >= ReadyFraction;

    /// <summary>Seconds to the end at <paramref name="now"/>, counting down between updates; null before Ready.</summary>
    public double? SecondsLeft(double now)
    {
        if (!Ready) return null;
        return Math.Max(0, (1 - _lastF) / _rate - Math.Max(0, now - _lastT));
    }
}

public static class ProgressWords
{
    /// <summary>"about 4 min left", rounded so it does not flicker (Driftwire's words).</summary>
    public static string TimeLeft(double seconds)
    {
        if (seconds < 10) return "a few seconds left";
        if (seconds < 55) return $"about {(int)Math.Round(seconds / 5, MidpointRounding.AwayFromZero) * 5} seconds left";
        if (seconds < 90) return "about a minute left";
        long minutes = (long)Math.Round(seconds / 60, MidpointRounding.AwayFromZero);
        if (minutes < 60) return $"about {minutes} min left";
        long rounded = (long)Math.Round(seconds / 300, MidpointRounding.AwayFromZero) * 5;
        long h = rounded / 60, m = rounded % 60;
        return $"about {h} h" + (m > 0 ? $" {m} min" : "") + " left";
    }

    /// <summary>Whole, rounded down, so 100 % means done.</summary>
    public static string Percent(double fraction) =>
        $"{(int)Math.Floor(Math.Clamp(fraction, 0, 1) * 100)} %";

    /// <summary>
    /// The line in two: the verb, which may shorten in a narrow pane, and the
    /// numbers after it (" · 40 % · about 1 min left"), which never do.
    /// </summary>
    public static (string Head, string Tail) Parts(string verb, double? fraction, string? detail = null,
                                                   double? left = null, bool timed = false)
    {
        if (fraction is not double f)
        {
            if (!string.IsNullOrEmpty(detail)) return (verb, " · " + detail);
            return (verb.EndsWith('…') ? verb : verb + "…", "");
        }
        var tail = new List<string> { string.IsNullOrEmpty(detail) ? Percent(f) : detail };
        if (f < 1)
        {
            if (left is double l) tail.Add(TimeLeft(l));
            else if (timed) tail.Add("Estimating…");
        }
        return (verb, string.Concat(tail.Select(t => " · " + t)));
    }

    public static string Line(string verb, double? fraction, string? detail = null, double? left = null,
                              bool timed = false)
    {
        var (head, tail) = Parts(verb, fraction, detail, left, timed);
        return head + tail;
    }

    /// <summary>"48 of 161 MB": both in the larger's unit, as a download is read.</summary>
    public static string Amounts(long done, long total)
    {
        if (total <= 0) return $"{Math.Max(0, (long)Math.Round(done / 1e6))} MB";
        if (total >= 1_000_000_000)
        {
            return string.Format(CultureInfo.InvariantCulture, "{0:0.00} of {1:0.00} GB", Math.Max(0, done) / 1e9, total / 1e9);
        }
        return $"{Math.Max(0, (long)Math.Round(done / 1e6))} of {Math.Max(1, (long)Math.Round(total / 1e6))} MB";
    }

    /// <summary>
    /// How a slider says its value beside its label: a whole-number step of 1
    /// or more counts ("3"); any other is how far along its range it sits
    /// ("70 %"), never "1.50". The Mac's SliderWords.
    /// </summary>
    public static string SliderValue(double v, double min, double max, double? step)
    {
        if (step is double s && s >= 1 && s == Math.Round(s) && min == Math.Round(min))
            return ((long)Math.Round(v)).ToString(CultureInfo.InvariantCulture);
        if (max <= min) return "0 %";
        return $"{(long)Math.Round((v - min) / (max - min) * 100, MidpointRounding.AwayFromZero)} %";
    }
}
