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
    public void TheMacUsesTheSameColoursAndTiming()
    {
        string swift = File.ReadAllText(Path.Combine(Repo(), "src.swift", "MediaViewerChrome", "Sources",
                                                     "MediaViewerChrome", "ScreenView.swift"));
        // Seconds per turn.
        Assert.Contains("truncatingRemainder(dividingBy: 6) / 6", swift);
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
