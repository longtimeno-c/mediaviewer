// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// src.managed/Shared/SpringLatch.cs: one rest with a drag opens a folder once, however many
// times the drag passed over it first (review item 5: one Tick handler was added per pass).
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class SpringLatchTests
{
    [Fact]
    public void PassesThenOneRestOpensOnce()
    {
        int opened = 0;
        var latch = new SpringLatch(() => opened++);
        for (int i = 0; i < 5; i++)
        {
            latch.Arm();     // DragEnter
            latch.Disarm();  // DragLeave before the timer
        }
        latch.Arm();
        latch.Fire();        // the timer's one tick
        latch.Fire();        // a late tick
        Assert.Equal(1, opened);
    }

    [Fact]
    public void ALeftOrDroppedTargetNeverOpens()
    {
        int opened = 0;
        var latch = new SpringLatch(() => opened++);
        latch.Arm();
        latch.Disarm();
        latch.Fire();
        Assert.Equal(0, opened);
        Assert.False(latch.Armed);
    }

    // The island makes the timer and subscribes its handler once per target, not per DragEnter.
    [Fact]
    public void TheIslandSubscribesTheTickOnce()
    {
        string text = Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.DragDrop.cs");
        string body = SourceText.Block(text, @"target\.DragEnter\s*\+=");
        // The only Tick subscription is where the timer is made, once.
        string made = SourceText.Block(body, @"if\s*\(spring is null\)");
        Assert.Contains("Tick +=", made);
        Assert.Single(System.Text.RegularExpressions.Regex.Matches(body, @"Tick\s*\+="));
    }
}
