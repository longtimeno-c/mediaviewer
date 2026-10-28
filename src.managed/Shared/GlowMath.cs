// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 9's glow (IslandHost.Glow.cs; the Mac's GlowEdge and
// GlowBar in ScreenView.swift use the same numbers): how far the edge has
// turned and where the progress bar's highlight is, at a time in seconds.
namespace MediaViewer.Shared;

public static class GlowMath
{
    /// <summary>Seconds for the edge's colours to go once round.</summary>
    public const double TurnSeconds = 6;

    /// <summary>Seconds for the highlight to cross the bar.</summary>
    public const double SweepSeconds = 2.4;

    /// <summary>Half the highlight's width, as a fraction of the bar.</summary>
    public const double BandHalf = 0.15;

    /// <summary>The edge's angle in degrees, 0 up to 360.</summary>
    public static double Turn(double seconds) => seconds % TurnSeconds / TurnSeconds * 360;

    /// <summary>
    /// The highlight's three stops along the bar (0..1): its leading edge, its
    /// middle and its trailing edge. It starts just off the bar's left end and
    /// leaves just off its right.
    /// </summary>
    public static (double From, double Middle, double To) Band(double seconds)
    {
        double centre = -BandHalf + seconds % SweepSeconds / SweepSeconds * (1 + 2 * BandHalf);
        return (Math.Clamp(centre - BandHalf, 0, 1), Math.Clamp(centre, 0, 1), Math.Clamp(centre + BandHalf, 0, 1));
    }
}
