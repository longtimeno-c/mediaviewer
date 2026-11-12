// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access: the reading of the app's state JSON and the card's
// words (src.managed/Shared/PhoneWords.cs), and the Mac twin
// (MediaViewerChrome's PhoneAccessView.swift) read as text so the two say the
// same things.
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
    public void TheSearchRowSaysWhatThePhoneSearches()
    {
        PhoneLinkState with = PhoneWords.ParseLink("""
            {"folder":"\\\\nas\\photos","library_name":"photos","local_search":true,
             "catalogue":{"running":false,"error":"","published":1000,"assets":12345}}
            """)!;
        Assert.Equal(@"\\nas\photos", with.Folder);
        Assert.Equal("Smart search is on. Published 2 hours ago · 12,345 files", PhoneWords.SearchDetail(with, 1000 + 2 * 3600 + 5));
        Assert.Equal("Sending to the NAS",
                     PhoneWords.SearchDetail(with with { Catalogue = with.Catalogue! with { Running = true, Phase = "sending" } }, 0));
        Assert.Equal("Smart search starts once the catalogue is published.",
                     PhoneWords.SearchDetail(with with { Catalogue = PhonePublishState.None }, 0));
        // No Local search: the phone searches as the desktop does without it.
        PhoneLinkState without = PhoneWords.ParseLink("""{"folder":"/x","local_search":false}""")!;
        Assert.StartsWith("Your phone searches file names and tags.", PhoneWords.SearchDetail(without, 0));
        Assert.True(PhoneWords.ParseLink("""{"secondary":true}""")!.Secondary);
        Assert.True(PhoneWords.ParseLink("""{"starting":true}""")!.Starting);
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
        string swift = Repo.Read("src.swift", "MediaViewerChrome", "Sources", "MediaViewerChrome", "PhoneAccessView.swift");
        var said = new List<string?>();
        foreach (string e in new[] { "refused", "unreachable", "bad_address", "too_many", "no_folder", "not_published",
                                      "other_library", "no_key_store", "revoke_waits", "no_driftwire", "no_local_search",
                                      "needs_unlock", "passphrase", "passphrase_short", "no_passphrase", "slow_down",
                                      "wrong_service", "nas_too_old" })
            said.Add(PhoneWords.ErrorText(e));
        foreach (string b in new[] { "claim", "pair", "refresh", "revoke", "add_folder", "set_addresses", "folder", "takeover_ask",
                                      "unlock", "set_passphrase", "grant" })
            said.Add(PhoneWords.BusyText(b));
        foreach (string w in new[] { "private", "carrier", "unresolved" }) said.Add(PhoneWords.WarningText(w));
        var loaded = new PhoneLinkState { LocalSearch = true };
        foreach (string e in new[] { "unreachable", "other_library", "no_space", "no_index", "no_folders", "no_key_store", "failed", "cancelled" })
            said.Add(PhoneWords.SearchDetail(loaded with { Catalogue = PhonePublishState.None with { Error = e } }, 0));
        said.Add(PhoneWords.SearchDetail(new PhoneLinkState(), 0));
        said.Add(PhoneWords.FolderDetail(new PhoneLinkState()));
        foreach (string p in new[] { "set", "none", "broken", "" })
            said.Add(PhoneWords.PassphraseDetail(new PhoneLinkState { NasPassphrase = p }));
        said.Add(PhoneWords.Unlimited);
        foreach ((string label, long _) in PhoneWords.Durations) said.Add(label);
        // Built from parts on the Mac: the parts, as Swift writes them.
        Assert.Contains("Too many tries. Try again at ", swift);
        Assert.Contains("Claimed. Update the NAS service to protect it with a passphrase.", swift);
        Assert.Contains("Guest code · access for ", swift);
        foreach (string? text in said)
        {
            Assert.NotNull(text);
            Assert.Contains("\"" + text!.Replace("this PC", "this Mac") + "\"", swift);
        }
        Assert.Equal("That didn't work.", PhoneWords.ErrorText("something new"));
        Assert.Null(PhoneWords.ErrorText(""));
    }

    [Fact]
    public void ThePassphraseAndTimedAccessAreRead()
    {
        PhoneLinkState s = PhoneWords.ParseLink("""
            {"claimed":true,"nas_passphrase":"set","nas_features":["passphrase"],"unlocked_s":61,
             "locked_until":0,"qr_for_s":86400,"error":"",
             "devices":[{"name":"Ann","key":"0123456789abcdef","limited":true,"expires":2000,"expired":false,"guest":true},
                        {"name":"Bea","key":"fedcba9876543210","limited":false,"expires":0}]}
            """)!;
        Assert.True(s.NasHasFeatures);
        Assert.Equal("Unlocked · 2 min", PhoneWords.PassphraseDetail(s));
        Assert.Equal("Locked", PhoneWords.PassphraseDetail(s with { UnlockedSeconds = 0 }));
        Assert.Equal("Guest code · access for 24 hours from when it is scanned", PhoneWords.GuestCodeLine(s.QrForSeconds));
        Assert.Equal("Guest code · access for 7 days from when it is scanned", PhoneWords.GuestCodeLine(7 * 86400));
        Assert.Equal("Guest · Until Fri", PhoneWords.PhoneDetail(s.Phones[0], 1000, _ => "Fri"));
        Assert.Equal("Guest · Expired 1 minute ago", PhoneWords.PhoneDetail(s.Phones[0], 2060, _ => "Fri"));
        Assert.Equal("Key fedcba98", PhoneWords.PhoneDetail(s.Phones[1], 1000, _ => "Fri"));
        Assert.False(PhoneWords.ParseLink("""{"nas_features":[]}""")!.NasHasFeatures);
        // The lockout's line has its end; a claim on a NAS too old for a passphrase still says it went through.
        Assert.StartsWith("Too many tries. Try again at 14:20,",
                          PhoneWords.ErrorText(s with { Error = "locked_out", LockedUntil = 5 }, _ => "14:20"));
        Assert.Equal("Claimed. Update the NAS service to protect it with a passphrase.",
                     PhoneWords.ErrorText(s with { Error = "nas_too_old", NasPassphrase = "" }, _ => ""));
        // The unlock counts down each second; only a new minute on the card is a change.
        Assert.True(s.SameAs(s with { UnlockedSeconds = 70 }));
        Assert.False(s.SameAs(s with { UnlockedSeconds = 1 }));
        Assert.False(s.SameAs(s with { UnlockedSeconds = 0 }));
    }
}
