// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Contribution API 10's glow (IslandHost.Glow.cs; the Mac's GlowEdge in
// ScreenView.swift uses the same numbers): how far the edge has turned at a
// time in seconds. The glow-coloured progress bar went on 2026-11-05: every
// bar is the one progress line (ProgressWords.cs, docs/design/20).
namespace MediaViewer.Shared;

public static class GlowMath
{
    /// <summary>Seconds for the edge's colours to go once round.</summary>
    public const double TurnSeconds = 6;

    /// <summary>The edge's angle in degrees, 0 up to 360.</summary>
    public static double Turn(double seconds) => seconds % TurnSeconds / TurnSeconds * 360;
}
