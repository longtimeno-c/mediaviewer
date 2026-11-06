// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10's glow (IslandHost.Glow.cs; the Mac's GlowEdge, GlowBar
// and GlowTiming in ScreenView.swift use the same numbers): how far each layer
// of the edge has turned, how far the halo has breathed, how far the bar's
// colours have flowed and where its highlight is, at a time in seconds.
namespace MediaViewer.Shared;

public static class GlowMath
{
    /// <summary>Seconds for the edge's crisp ring to go once round.</summary>
    public const double TurnSeconds = 6;

    /// <summary>Seconds for the soft glow to go once round, the other way.</summary>
    public const double MidTurnSeconds = 9;

    /// <summary>Seconds for the wide halo to go once round.</summary>
    public const double HaloTurnSeconds = 13;

    /// <summary>Seconds for the halo to breathe in and out.</summary>
    public const double BreathSeconds = 2.2;

    /// <summary>Seconds for the bar's colours to flow one bar's length.</summary>
    public const double FlowSeconds = 3;

    /// <summary>Seconds for the highlight to cross the bar.</summary>
    public const double SweepSeconds = 2.4;

    /// <summary>Half the highlight's width, as a fraction of the bar.</summary>
    public const double BandHalf = 0.15;

    /// <summary>The edge's angle in degrees, 0 up to 360.</summary>
    public static double Turn(double seconds) => Turn(seconds, TurnSeconds);

    /// <summary>An angle in degrees, 0 up to 360, for a layer going round once every <paramref name="period"/> seconds.</summary>
    public static double Turn(double seconds, double period) => seconds % period / period * 360;

    /// <summary>The halo's breath: 0 to 1 and back over <see cref="BreathSeconds"/>, eased.</summary>
    public static double Breathe(double seconds) =>
        0.5 - 0.5 * Math.Cos(seconds % BreathSeconds / BreathSeconds * 2 * Math.PI);

    /// <summary>How far the bar's colours have flowed: 0 up to 1 of their repeat.</summary>
    public static double Flow(double seconds) => seconds % FlowSeconds / FlowSeconds;

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
