// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #153: the per-mode describe_commands() snapshot, as the chromes see it.
//
// mv_tests writes and checks tests/data/command_table.snapshot against the live
// table (tests/test_command_contract.cpp). These read the same file from the
// chrome's side: the rows it lists are commands.h ids by those names, and the
// shortcuts the Windows menus print by hand are what the default table binds.
using System.Text.RegularExpressions;

namespace MediaViewer.Chrome.Tests;

public class CommandTableTests
{
    private static readonly string[] Modes =
        { "browse", "video", "slideshow", "island", "loupe", "gallery", "runner", "crop", "trim" };

    [Fact]
    public void SnapshotHasEveryModeOfCommandsH()
    {
        string header = SourceText.StripComments(Repo.Read("src", "shell", "commands.h"));
        var modes = SourceText.CppEnum(header, @"enum\s+class\s+mode\s*:[^{]*")
                              .Where(e => e.Name != "count").OrderBy(e => e.Value).Select(e => e.Name).ToArray();
        Assert.Equal(modes, Modes);
        Assert.Equal(Modes, Snapshot.Rows.Select(r => r.Mode).Distinct().ToArray());
    }

    [Fact]
    public void SnapshotRowsAreCommandsHIdsByTheirNames()
    {
        var byId = Core.CommandId.ToDictionary(kv => kv.Value, kv => kv.Key);
        var failures = new List<string>();
        foreach (SnapshotRow row in Snapshot.Rows)
        {
            if (!byId.TryGetValue(row.Id, out string? idName))
                failures.Add($"[{row.Mode}] {row.Name}: id {row.Id} is not in command_id");
            else if (!Core.CommandNames.TryGetValue(row.Name, out string? named) || named != idName)
                failures.Add($"[{row.Mode}] {row.Name}: id {row.Id} is command_id::{idName}, kCommands says {named ?? "nothing"}");
        }
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }

    [Fact]
    public void WalkingGEnterEscIsBoundInTheModesItCrosses()
    {
        // The smoke walk issue #153 asks of both hosts: G opens the gallery, Enter
        // opens the selected cell, Esc walks out. Headless, the table must say so.
        Assert.Contains(Snapshot.Rows, r => r.Mode == "browse" && r.Keys == "G" && r.Id == Core.CommandId["toggle_gallery"]);
        Assert.Contains(Snapshot.Rows, r => r.Mode == "gallery" && r.Keys == "Enter" &&
                                            r.Id == Core.CommandId["gallery_open_selected"]);
        Assert.Contains(Snapshot.Rows, r => r.Mode == "browse" && r.Keys == "Enter" &&
                                            r.Id == Core.CommandId["edit_workspace"]);
    }

    [Fact]
    public void WindowsMenuShortcutsAreTheDefaultKeys()
    {
        // IslandHost.cs prints a shortcut beside each menu item by hand:
        // Item("Gallery", "G", () => Send(Command.ToggleGallery)). It must be a key
        // the default table binds to that command in browse mode.
        string text = Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.cs");
        var byName = CommandIdTests.CSharpCommandConstants().ToDictionary(c => c.Name, c => c.Value);
        var items = Regex.Matches(text,
            @"\bItem\(""([^""]*)"",\s*""([^""]+)"",\s*\(\)\s*=>\s*\{?\s*Send\(Command\.(\w+)\)").ToList();
        Assert.True(items.Count >= 10, $"found only {items.Count} menu items with a shortcut");
        var failures = new List<string>();
        foreach (Match m in items)
        {
            string label = m.Groups[1].Value, shortcut = m.Groups[2].Value, constant = m.Groups[3].Value;
            int id = byName[constant];
            var bound = Snapshot.Rows.Where(r => r.Id == id && r.Mode == "browse").Select(r => r.Keys).ToList();
            if (!bound.Contains(shortcut))
            {
                failures.Add($"menu \"{label}\" shows {shortcut} for Command.{constant} ({id}); " +
                             $"browse binds {(bound.Count == 0 ? "nothing" : string.Join(" / ", bound))}");
            }
        }
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }
}
