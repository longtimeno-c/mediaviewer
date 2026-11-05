// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10's glow (IslandHost.Glow.cs; the Mac's GlowEdge and
// GlowTiming in ScreenView.swift use the same numbers): how far each layer of
// the edge has turned and how far the halo has breathed, at a time in seconds.
// The glow-coloured progress bar went on 2026-11-05: every bar is the one
// progress line (ProgressWords.cs, docs/design/20).
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

    /// <summary>The edge's angle in degrees, 0 up to 360.</summary>
    public static double Turn(double seconds) => Turn(seconds, TurnSeconds);

    /// <summary>An angle in degrees, 0 up to 360, for a layer going round once every <paramref name="period"/> seconds.</summary>
    public static double Turn(double seconds, double period) => seconds % period / period * 360;

    /// <summary>The halo's breath: 0 to 1 and back over <see cref="BreathSeconds"/>, eased.</summary>
    public static double Breathe(double seconds) =>
        0.5 - 0.5 * Math.Cos(seconds % BreathSeconds / BreathSeconds * 2 * Math.PI);
}
