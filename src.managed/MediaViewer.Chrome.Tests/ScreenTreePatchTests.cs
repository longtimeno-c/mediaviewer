// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// src.managed/Shared/ScreenTreePatch.cs: a transfer's progress updates its
// window in place; anything else builds it again.
using System.Text.Json;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class ScreenTreePatchTests
{
    // Driftwire's receive card as tree.cpp writes it, at one moment.
    private static JsonElement Screen(long revision, string moved, double value, string extra = "")
    {
        string json = $$$"""
            {"addon":"driftwire","id":"main","place":"window","title":"Driftwire","revision":{{{revision}}},
             "tree":{"type":"stack","id":"root","children":[
               {"type":"stack","id":"incoming","look":"card","reveal":"moving","children":[
                 {"type":"link","id":"in_path","via":"wifi","state":"moving","value":{{{value.ToString(System.Globalization.CultureInfo.InvariantCulture)}}}},
                 {"type":"text","id":"in_title","text":"Receiving from Ann","style":"heading"},
                 {"type":"text","id":"in_moved","text":"{{{moved}}}","style":"secondary"},
                 {"type":"button","id":"in_cancel","label":"Cancel","style":"destructive"}{{{extra}}}]}]}}
            """;
        return JsonDocument.Parse(json).RootElement.Clone();
    }

    [Fact]
    public void ProgressMovingIsPatchedInPlace()
    {
        var changes = ScreenTreePatch.Changes(Screen(1, "1 of 9 files", 0.1), Screen(2, "2 of 9 files", 0.25));
        Assert.NotNull(changes);
        Assert.Equal(2, changes!.Count);
        Assert.Contains(new ScreenTreePatch.Change("in_path", "link", "", 0.25), changes);
        Assert.Contains(new ScreenTreePatch.Change("in_moved", "text", "2 of 9 files", 0), changes);
    }

    [Fact]
    public void OnlyTheRevisionMovingChangesNothing()
    {
        Assert.Empty(ScreenTreePatch.Changes(Screen(1, "a", 0.5), Screen(2, "a", 0.5))!);
    }

    [Fact]
    public void ANodeAddedOrAnythingButWordsAndProgressRebuilds()
    {
        JsonElement before = Screen(1, "a", 0.5);
        // A card appears (the receive stopped: "Unfinished transfers" below it).
        Assert.Null(ScreenTreePatch.Changes(before,
            Screen(2, "a", 0.5, """,{"type":"text","id":"unfinished_title","text":"Unfinished transfers"}""")));
        // A label that is not a text node's words: a button's.
        JsonElement relabelled = JsonDocument.Parse(before.GetRawText().Replace("\"Cancel\"", "\"Stop\"")).RootElement;
        Assert.Null(ScreenTreePatch.Changes(before, relabelled));
        // A style.
        JsonElement restyled = JsonDocument.Parse(before.GetRawText().Replace("\"heading\"", "\"title\"")).RootElement;
        Assert.Null(ScreenTreePatch.Changes(before, restyled));
    }

    [Fact]
    public void ATextWithNoIdOrARepeatedIdRebuilds()
    {
        JsonElement Doc(string a, string b) => JsonDocument.Parse(
            $$$"""{"revision":1,"tree":{"type":"stack","children":[{"type":"text","text":"{{{a}}}"},{"type":"text","id":"x","text":"{{{b}}}"},{"type":"text","id":"x","text":"same"}]}}""").RootElement;
        Assert.Null(ScreenTreePatch.Changes(Doc("1", "s"), Doc("2", "s")));  // no id
        Assert.Null(ScreenTreePatch.Changes(Doc("1", "s"), Doc("1", "t")));  // "x" twice
        Assert.Empty(ScreenTreePatch.Changes(Doc("1", "s"), Doc("1", "s"))!);
    }
}
