// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10's glow: the timing the Windows chrome steps by
// (src.managed/Shared/GlowMath.cs), and the Mac twin (ScreenView.swift's
// GlowEdge, GlowBar and GlowPalette) read as text for the same numbers.
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class GlowMathTests
{
    [Fact]
    public void TheEdgeTurnsOnceEverySixSeconds()
    {
        Assert.Equal(0, GlowMath.Turn(0));
        Assert.Equal(180, GlowMath.Turn(3), 6);
        Assert.Equal(0, GlowMath.Turn(6), 6);
        Assert.Equal(60, GlowMath.Turn(13), 6);
    }

    [Fact]
    public void TheLayersTurnAtTheirOwnPaceAndTheHaloBreathes()
    {
        Assert.Equal(180, GlowMath.Turn(4.5, GlowMath.MidTurnSeconds), 6);
        Assert.Equal(0, GlowMath.Turn(13, GlowMath.HaloTurnSeconds), 6);
        Assert.Equal(0, GlowMath.Breathe(0), 6);
        Assert.Equal(1, GlowMath.Breathe(GlowMath.BreathSeconds / 2), 6);
        Assert.Equal(0, GlowMath.Breathe(GlowMath.BreathSeconds), 6);
        Assert.Equal(0.5, GlowMath.Flow(1.5), 6);
        for (double t = 0; t < 10; t += 0.13)
        {
            Assert.InRange(GlowMath.Breathe(t), 0, 1);
            Assert.InRange(GlowMath.Flow(t), 0, 1);
        }
    }

    [Fact]
    public void TheHighlightCrossesTheBarFromOffItsLeftToOffItsRight()
    {
        var start = GlowMath.Band(0);
        Assert.Equal((0, 0, 0), start);  // just off the left end: nothing shows yet
        var middle = GlowMath.Band(GlowMath.SweepSeconds / 2);
        Assert.Equal(0.35, middle.From, 6);
        Assert.Equal(0.5, middle.Middle, 6);
        Assert.Equal(0.65, middle.To, 6);
        var end = GlowMath.Band(GlowMath.SweepSeconds * 0.999);
        Assert.Equal(1, end.Middle, 2);
        for (double t = 0; t < 5; t += 0.07)
        {
            var b = GlowMath.Band(t);
            Assert.True(b.From <= b.Middle && b.Middle <= b.To);
        }
    }

    [Fact]
    public void TheMacUsesTheSameColoursAndTiming()
    {
        string swift = File.ReadAllText(Path.Combine(Repo(), "src.swift", "MediaViewerChrome", "Sources",
                                                     "MediaViewerChrome", "ScreenView.swift"));
        // Seconds per turn of each layer, per breath, per flow and per sweep, and the band's half width.
        Assert.Contains("static let turn = 6.0", swift);
        Assert.Contains("static let midTurn = 9.0", swift);
        Assert.Contains("static let haloTurn = 13.0", swift);
        Assert.Contains("static let breath = 2.2", swift);
        Assert.Contains("static let flow = 3.0", swift);
        Assert.Contains("GlowTiming.phase(t, 2.4)", swift);
        Assert.Contains("let centre = -0.15 + phase * 1.3", swift);
        // The palette: violet, pink, amber, teal, blue (0..255 on Windows, 0..1 on the Mac).
        foreach (string rgb in new[] { "0.55, green: 0.36, blue: 0.96", "0.98, green: 0.36, blue: 0.62",
                                       "1.00, green: 0.62, blue: 0.25", "0.25, green: 0.78, blue: 0.85",
                                       "0.36, green: 0.52, blue: 1.00" })
        {
            Assert.Contains(rgb, swift);
        }
    }

    private static string Repo()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !Directory.Exists(Path.Combine(dir.FullName, "src.swift"))) dir = dir.Parent;
        return dir?.FullName ?? throw new DirectoryNotFoundException("the repository root");
    }
}
