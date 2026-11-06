// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10's glow: the timing the Windows chrome steps by
// (src.managed/Shared/GlowMath.cs), and the Mac twin (ScreenView.swift's
// GlowEdge and GlowPalette) read as text for the same numbers.
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
        for (double t = 0; t < 10; t += 0.13)
        {
            Assert.InRange(GlowMath.Breathe(t), 0, 1);
        }
    }

    [Fact]
    public void TheMacUsesTheSameColoursAndTiming()
    {
        string swift = File.ReadAllText(Path.Combine(Repo(), "src.swift", "MediaViewerChrome", "Sources",
                                                     "MediaViewerChrome", "ScreenView.swift"));
        // Seconds per turn of each layer and per breath.
        Assert.Contains("static let turn = 6.0", swift);
        Assert.Contains("static let midTurn = 9.0", swift);
        Assert.Contains("static let haloTurn = 13.0", swift);
        Assert.Contains("static let breath = 2.2", swift);
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
