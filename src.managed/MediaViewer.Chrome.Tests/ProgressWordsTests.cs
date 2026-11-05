// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules": the one progress line's words and meter
// (src.managed/Shared/ProgressWords.cs), and the Mac twin
// (ProgressWords.swift) read as text for the same constants and words.
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class ProgressWordsTests
{
    [Fact]
    public void TheTimeLeftIsRoundedAsDriftwireSaysIt()
    {
        Assert.Equal("a few seconds left", ProgressWords.TimeLeft(4));
        Assert.Equal("about 25 seconds left", ProgressWords.TimeLeft(23));
        Assert.Equal("about a minute left", ProgressWords.TimeLeft(70));
        Assert.Equal("about 4 min left", ProgressWords.TimeLeft(4 * 60 + 10));
        Assert.Equal("about 59 min left", ProgressWords.TimeLeft(59 * 60));
        Assert.Equal("about 1 h 20 min left", ProgressWords.TimeLeft(3600 + 22 * 60));
        Assert.Equal("about 2 h left", ProgressWords.TimeLeft(2 * 3600 + 60));
    }

    [Fact]
    public void ALineIsTheVerbThenItsNumbers()
    {
        Assert.Equal("Enhancing · 40 % · about 4 min left", ProgressWords.Line("Enhancing", 0.4, left: 250));
        Assert.Equal("Enhancing · 40 % · Estimating…", ProgressWords.Line("Enhancing", 0.4, timed: true));
        Assert.Equal("Enhancing · 40 %", ProgressWords.Line("Enhancing", 0.4));
        Assert.Equal("Enhancing…", ProgressWords.Line("Enhancing", null));
        Assert.Equal("Checking…", ProgressWords.Line("Checking…", null));
        Assert.Equal("Downloading · 48 of 161 MB · about 25 seconds left",
                     ProgressWords.Line("Downloading", 0.3, ProgressWords.Amounts(48_000_000, 161_000_000), 24));
        // Done: no time left, and 99.9 % is not yet 100 %.
        Assert.Equal("Enhancing · 100 %", ProgressWords.Line("Enhancing", 1, timed: true));
        Assert.Equal("99 %", ProgressWords.Percent(0.999));
        var (head, tail) = ProgressWords.Parts("Downloading the detail model", 0.5, left: 30);
        Assert.Equal("Downloading the detail model", head);
        Assert.Equal(" · 50 % · about 30 seconds left", tail);
    }

    [Fact]
    public void SizesReadInTheLargersUnit()
    {
        Assert.Equal("48 of 161 MB", ProgressWords.Amounts(48_000_000, 161_000_000));
        Assert.Equal("0.41 of 1.08 GB", ProgressWords.Amounts(412_000_000, 1_080_000_000));
    }

    [Fact]
    public void ASliderSaysAPercentageOfItsRangeOrACount()
    {
        Assert.Equal("33 %", ProgressWords.SliderValue(1.5, 1, 2.5, 0.1));  // Restyle's Keep the photo
        Assert.Equal("70 %", ProgressWords.SliderValue(0.7, 0, 1, 0.05));
        Assert.Equal("100 %", ProgressWords.SliderValue(1, 0, 1, null));
        Assert.Equal("3", ProgressWords.SliderValue(3, 1, 8, 1));
    }

    [Fact]
    public void TheMeterWaitsThenCountsDownAndTheBarNeverGoesBack()
    {
        var m = new ProgressMeter();
        double t = 0;
        m.Add(0, t);
        for (; t < 2; t += 0.5) m.Add(t * 0.01, t);
        Assert.Null(m.SecondsLeft(t));  // not before 3 s
        for (; t < 40; t += 0.5) m.Add(t * 0.01, t);  // 1 % a second
        double? left = m.SecondsLeft(t);
        Assert.NotNull(left);
        Assert.InRange(left!.Value, 55, 65);  // about 60 s at 1 % a second from 39.5 %
        Assert.True(m.SecondsLeft(t + 5) < left);  // counts down between updates
        double shown = m.Shown;
        m.Add(shown - 0.05, t + 1);  // a small step back is held
        Assert.Equal(shown, m.Shown);
        m.Add(0.01, t + 2);  // a fall of more than a quarter: a new run
        Assert.Equal(0.01, m.Shown, 6);
        Assert.Null(m.SecondsLeft(t + 2));
    }

    [Fact]
    public void TheMacUsesTheSameMeterAndWords()
    {
        string swift = File.ReadAllText(Path.Combine(Repo(), "src.swift", "MediaViewerChrome", "Sources",
                                                     "MediaViewerChrome", "ProgressWords.swift"));
        Assert.Contains("static let tauS = 4.0", swift);
        Assert.Contains("static let warmS = 5.0", swift);
        Assert.Contains("static let readyS = 3.0", swift);
        Assert.Contains("static let readyFraction = 0.02", swift);
        Assert.Contains("static let restartDrop = 0.25", swift);
        foreach (string words in new[] { "a few seconds left", " seconds left", "about a minute left", " min left",
                                         "Estimating…", " · " })
        {
            Assert.Contains(words, swift);
        }
    }

    private static string Repo()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !Directory.Exists(Path.Combine(dir.FullName, "src.swift"))) dir = dir.Parent;
        return dir?.FullName ?? throw new DirectoryNotFoundException("the repository root");
    }
}
