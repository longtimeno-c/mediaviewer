// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Phone access card's reading of the pack's JSON and its words
// (src.managed/Shared/PhoneWords.cs), and the Mac twin (PhoneLinkView.swift,
// PhoneAccessView.swift) read as text so the two say the same things.
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class PhoneWordsTests
{
    private const string Link = """
        {"driftwire":true,"busy":"","error":"revoke_waits","claimed":true,"reachable":false,
         "local":"nas.local:7878","public":"photos.example.com:7878","warning":"carrier",
         "devices":[{"name":"Phone","key":"0123456789abcdef"},{"name":"no key"}],
         "folders":[{"id":"1","label":"Catalogue","path":"/volume1/Photos/.mediaviewer"},{"id":"2","label":"Video","path":"/volume1/Video"}],
         "qr":"mediaviewer://pair#x","qr_library":"Photos","qr_fingerprint":"ABCD-EFGH","qr_expires":1700000300,
         "added":["Tristan's iPhone"],"nearby":true,"other_publisher":false,"mismatch":false,"flags_unsaved":false,
         "other_fingerprint":"","asking_name":"","asking_key":"","asking_fingerprint":""}
        """;

    [Fact]
    public void TheLinkStateIsReadAsTheMacReadsIt()
    {
        PhoneLinkState s = PhoneWords.ParseLink(Link)!;
        Assert.True(s.Claimed);
        Assert.False(s.Reachable);
        Assert.Equal("nas.local:7878", s.Local);
        Assert.Single(s.Phones);  // a device without a key is dropped, as compactMap does
        Assert.Equal("Phone", s.Phones[0].Name);
        Assert.Equal(2, s.Folders.Count);
        Assert.True(s.Folders[0].IsCatalogue);
        Assert.False(s.Folders[1].IsCatalogue);
        Assert.Equal(1700000300, s.QrExpires);
        Assert.True(s.Nearby);
        Assert.Equal("Added Tristan's iPhone.", PhoneWords.AddDetail(s));
        Assert.Equal("Key 01234567", PhoneWords.KeyDetail(s.Phones[0].Key));
        Assert.True(s.SameAs(PhoneWords.ParseLink(Link)!));
        Assert.False(s.SameAs(s with { Busy = "pair" }));
        Assert.False(s.SameAs(s with { Phones = Array.Empty<PhoneDevice>() }));
    }

    [Fact]
    public void AnOlderOrBrokenAnswerReadsAsDefaults()
    {
        PhoneLinkState s = PhoneWords.ParseLink("{}")!;
        Assert.True(s.Driftwire);
        Assert.True(s.Reachable);
        Assert.Empty(s.Phones);
        Assert.Null(PhoneWords.ParseLink("not json"));
        Assert.Equal(PhonePublishState.None, PhoneWords.ParsePublish("[]"));
    }

    [Fact]
    public void ThePublishRowSaysWhereItIs()
    {
        var p = PhoneWords.ParsePublish("""{"running":false,"error":"","destination":"\\\\nas\\photos","published":1000,"assets":12345}""");
        Assert.Equal(@"\\nas\photos", p.Destination);
        Assert.Equal("Published 2 hours ago · 12,345 files", PhoneWords.PublishStatus(p, 1000 + 2 * 3600 + 5));
        Assert.Equal("Sending to the NAS", PhoneWords.PublishStatus(p with { Running = true, Phase = "sending" }, 0));
        Assert.Equal("Building the catalogue", PhoneWords.PublishStatus(p with { Running = true }, 0));
        Assert.Equal("Not published yet.", PhoneWords.PublishStatus(PhonePublishState.None, 0));
        Assert.Equal("just now", PhoneWords.Ago(30));
        Assert.Equal("yesterday", PhoneWords.Ago(86400 + 10));
    }

    [Fact]
    public void ACodeCountsDown()
    {
        Assert.Equal("Good for 4:59", PhoneWords.Countdown(1300, 1001));
        Assert.Equal("Good for 0:00", PhoneWords.Countdown(1000, 2000));
    }

    [Theory]
    [InlineData("/volume1/Video/2024", "Video")]
    [InlineData("/volume12/Photos", "Photos")]
    [InlineData("/mnt/tank/Photos", "Photos")]
    [InlineData("/volumeX/Photos", "Photos")]
    [InlineData("/", "")]
    public void ANewFolderIsCalledAfterItsShare(string path, string label) =>
        Assert.Equal(label, PhoneWords.ShareLabel(path));

    // Every reason and progress word the pack can send, as the Mac says it
    // ("this Mac" there is "this PC" here).
    [Fact]
    public void TheWordsAreTheMacs()
    {
        string swift = Repo.Read("src.swift", "AIChrome", "Sources", "AIChrome", "PhoneLinkView.swift") +
                       Repo.Read("src.swift", "AIChrome", "Sources", "AIChrome", "PhoneAccessView.swift");
        var said = new List<string?>();
        foreach (string e in new[] { "refused", "unreachable", "bad_address", "too_many", "no_folder", "not_published",
                                      "no_key_store", "revoke_waits", "no_driftwire" })
            said.Add(PhoneWords.ErrorText(e));
        foreach (string b in new[] { "claim", "pair", "refresh", "revoke", "add_folder", "set_addresses", "takeover_ask" })
            said.Add(PhoneWords.BusyText(b));
        foreach (string w in new[] { "private", "carrier", "unresolved" }) said.Add(PhoneWords.WarningText(w));
        foreach (string e in new[] { "unreachable", "other_library", "no_space", "no_index", "no_folders", "no_key_store", "failed", "cancelled" })
            said.Add(PhoneWords.PublishStatus(PhonePublishState.None with { Error = e }, 0));
        foreach (string? text in said)
        {
            Assert.NotNull(text);
            Assert.Contains("\"" + text!.Replace("this PC", "this Mac") + "\"", swift);
        }
        Assert.Equal("That didn't work.", PhoneWords.ErrorText("something new"));
        Assert.Null(PhoneWords.ErrorText(""));
    }
}
