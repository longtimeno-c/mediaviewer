// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/16 "Drag and drop into folders": the island's question to native
// (src.managed/Shared/DropWords.cs), how it reads the answer, and that its words
// and wire values are shell/file_drop.h's. What a drop becomes is tested in C++
// (tests/test_file_drop.cpp); main.cpp's parser reads the payload built here.
using System.Text.RegularExpressions;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class DropWordsTests
{
    [Fact]
    public void FlagsMatchTheMac()
    {
        Assert.Equal(1, DropWords.Flags(fromApp: true, ctrl: false, shift: false));
        Assert.Equal(3, DropWords.Flags(fromApp: true, ctrl: true, shift: false));
        Assert.Equal(4, DropWords.Flags(fromApp: false, ctrl: false, shift: true));
        Assert.Equal(0, DropWords.Flags(false, false, false));
    }

    [Fact]
    public void ThePayloadIsFlagsFolderThenPaths()
    {
        Assert.Equal("1\nC:\\Photos\\Trip\nC:\\Photos\\a.jpg\nC:\\Photos\\b.jpg",
                     DropWords.Payload(1, @"C:\Photos\Trip", new[] { @"C:\Photos\a.jpg", @"C:\Photos\b.jpg" }));
        // An outside drag hovering: its files are read on drop, the folder is judged alone.
        Assert.Equal("0\nC:\\Photos", DropWords.Payload(0, @"C:\Photos", Array.Empty<string>()));
    }

    [Fact]
    public void TheAnswerUnpacksVerbAndRefusal()
    {
        Assert.Equal(new DropWords.Verdict(DropWords.Verb.Move, DropWords.Refusal.None), DropWords.Read(2));
        Assert.Equal(new DropWords.Verdict(DropWords.Verb.Copy, DropWords.Refusal.None), DropWords.Read(1));
        DropWords.Verdict remote = DropWords.Read(3 << 8);
        Assert.False(remote.Allowed);
        Assert.True(remote.RefusedVisibly);
        Assert.Equal(DropWords.Refusal.RemoteTarget, remote.Refusal);
        Assert.False(DropWords.Read(1 << 8).RefusedVisibly);  // no files: nothing to say
        Assert.False(DropWords.Read(0).Allowed);
        Assert.False(DropWords.Read(9).Allowed);  // a newer host's value is no drop, never a copy
    }

    [Fact]
    public void TheCaptionSaysWhatHappens()
    {
        var move = new DropWords.Verdict(DropWords.Verb.Move, DropWords.Refusal.None);
        Assert.Equal("Move 3 items to Trip", DropWords.Caption(move, 3, "Trip"));
        Assert.Equal("Move 1 item to Trip", DropWords.Caption(move, 1, "Trip"));
        var copy = new DropWords.Verdict(DropWords.Verb.Copy, DropWords.Refusal.None);
        Assert.Equal("Copy to Trip", DropWords.Caption(copy, 0, "Trip"));
        Assert.Equal("Can't put a folder inside itself",
                     DropWords.Caption(DropWords.Read(4 << 8), 2, "Trip"));
    }

    [Fact]
    public void OneClickSelectsTwoOpen()
    {
        Assert.False(DropWords.OpensTile(1));
        Assert.True(DropWords.OpensTile(2));
    }

    [Fact]
    public void BrowseBitsAreUpBackForward()
    {
        Assert.Equal((true, false, false), DropWords.Moves(1));
        Assert.Equal((false, true, true), DropWords.Moves(6));
    }

    // The words and numbers are file_drop.h's: read the header as text.
    [Fact]
    public void RefusalsMatchFileDropH()
    {
        string header = Repo.Read("src", "shell", "file_drop.h");
        string enumBody = SourceText.Block(header, @"enum\s+class\s+drop_refusal\s*:\s*std::uint8_t");
        var values = Regex.Matches(enumBody, @"(\w+)\s*=\s*(\d+)")
            .ToDictionary(m => m.Groups[1].Value, m => int.Parse(m.Groups[2].Value));
        Assert.Equal((int)DropWords.Refusal.ReadOnlyTarget, values["read_only_target"]);
        Assert.Equal((int)DropWords.Refusal.RemoteTarget, values["remote_target"]);
        Assert.Equal((int)DropWords.Refusal.IntoItself, values["into_itself"]);
        Assert.Equal((int)DropWords.Refusal.AlreadyThere, values["already_there"]);
        Assert.Equal((int)DropWords.Refusal.RemoteSource, values["remote_source"]);
        foreach ((string name, DropWords.Refusal r) in new[]
                 {
                     ("read_only_target", DropWords.Refusal.ReadOnlyTarget),
                     ("remote_target", DropWords.Refusal.RemoteTarget),
                     ("into_itself", DropWords.Refusal.IntoItself),
                     ("already_there", DropWords.Refusal.AlreadyThere),
                     ("remote_source", DropWords.Refusal.RemoteSource),
                 })
        {
            Match m = Regex.Match(header, @"case drop_refusal::" + name + @":\s*return ""([^""]*)""");
            Assert.True(m.Success, name);
            Assert.Equal(m.Groups[1].Value, DropWords.RefusalText(r));
        }
    }

    // main.cpp answers on the same WM_COPYDATA ids the island sends.
    [Fact]
    public void TheCopyDataIdsMatchNative()
    {
        string main = Repo.Read("src", "shell", "main.cpp");
        Assert.Contains("0x4D560002ul", main);
        Assert.Contains("0x4D560003ul", main);
        Assert.Equal(0x4D560002, DropWords.DropCopyData);
        Assert.Equal(0x4D560003, DropWords.VerdictCopyData);
    }
}
