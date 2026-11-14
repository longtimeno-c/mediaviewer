// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "This computer" → Drives (docs/plans/self-host-drives.md §6): Shared/PhoneDrives.cs, the words
// PhoneWords.cs adds for it, and the Mac twin (PhoneDrivesModel.swift, PhoneDrivesView.swift) read
// as text so the two cards do not drift.
using System.Text.Json;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class PhoneDrivesTests
{
    private const string Listing = """
        {"os":"windows","pool_ok":true,"not_a_backup":"RAID is not a backup.","disks":[
         {"id":"2","name":"Untitled (2)","model":"WD","size":2000000000000,"used":5,"folders":2,
          "facts":"Untitled (2) · 2 TB · WD","system":false,"internal":false,"image":true,
          "volumes":[{"label":"Untitled","size":2000000000000,"used":5,"folders":2,"mounted":true}]},
         {"id":"0","name":"Windows","model":"NVMe","size":1,"used":1,"folders":1,"facts":"x","system":true,
          "internal":true,"image":false,"refused":"it holds this computer's system","volumes":[]}],
         "sets":[{"id":"{abc}","name":"Photos","kind":"mirror","state":"degraded","percent":0,"size":2,
          "line":"Photos (mirror): degraded","members":[{"id":"3","status":"OK","missing":false},
          {"id":"","status":"Lost Communication","missing":true}]}]}
        """;

    private const string Choices = """
        {"not_a_backup":"RAID is not a backup.","options":[
         {"choice":"separate","title":"Separate","offered":true,"recommended":false,"warning":false,"space":4,"space_words":"a","dead_words":"b","speed_words":"c"},
         {"choice":"mirror","title":"Mirror (RAID 1)","offered":true,"recommended":true,"warning":false,"space":2,"space_words":"2 TB","dead_words":"Nothing is lost.","speed_words":"d"},
         {"choice":"span","title":"Span (JBOD)","offered":true,"recommended":false,"warning":false,"space":4,"space_words":"","dead_words":"","speed_words":""},
         {"choice":"stripe","title":"Stripe (RAID 0)","offered":true,"recommended":false,"warning":true,"space":4,"space_words":"","dead_words":"","speed_words":""},
         {"choice":"parity","title":"Parity (RAID 5)","offered":false,"why_not":"Needs 3 or more drives.","recommended":false,"warning":false,"space":0,"space_words":"","dead_words":"","speed_words":""}]}
        """;

    [Fact]
    public void TheLinkStateCarriesTheDrivesAsText()
    {
        PhoneLinkState? a = PhoneWords.ParseLink("{\"drives\":" + Listing + ",\"drives_choices\":" + Choices + "}");
        PhoneLinkState? b = PhoneWords.ParseLink("{\"drives\":" + Listing + ",\"drives_choices\":" + Choices + "}");
        Assert.NotNull(a);
        Assert.True(a!.SameAs(b!));  // compared by value: no redraw while nothing changed
        Assert.False(a.SameAs(PhoneWords.ParseLink("{\"drives\":" + Listing + "}")!));
        Assert.Equal("", PhoneWords.ParseLink("{}")!.DrivesJson);
        Assert.NotNull(PhoneDrives.Listing(a.DrivesJson));
        Assert.NotNull(PhoneDrives.Choices(a.DrivesChoicesJson));
    }

    [Fact]
    public void TheListingReads()
    {
        DriveListing l = PhoneDrives.Listing(Listing)!;
        Assert.Equal("windows", l.Os);
        Assert.True(l.PoolOk);
        Assert.Equal(new[] { "2", "0" }, l.Disks.Select(d => d.Id));
        Assert.True(l.Disks[0].Volumes[0].Mounted);
        Assert.Equal("it holds this computer's system", l.Disks[1].Refused);
        Assert.Equal("Photos (mirror): degraded", l.Sets[0].Line);
        Assert.True(l.Sets[0].CanTakeADrive);
        Assert.False((l.Sets[0] with { Kind = "stripe" }).CanTakeADrive);
        Assert.False((l.Sets[0] with { State = "healthy" }).CanTakeADrive);
        Assert.Equal(new[] { "2" }, PhoneDrives.Replacements(l.Sets[0], l).Select(d => d.Id));
        Assert.Null(PhoneDrives.Listing(""));
        Assert.Null(PhoneDrives.Listing("[1]"));
    }

    [Fact]
    public void TheFiveChoicesRead()
    {
        DriveChoices c = PhoneDrives.Choices(Choices)!;
        Assert.Equal("RAID is not a backup.", c.NotABackup);
        Assert.Equal(new[] { "separate", "mirror", "span", "stripe", "parity" }, c.Options.Select(o => o.Choice));
        Assert.True(c.Options[1].Recommended);
        Assert.True(c.Options[3].Warning && c.Options[3].Offered);  // warned, still offered
        Assert.False(c.Options[4].Offered);
        Assert.Equal("Needs 3 or more drives.", c.Options[4].WhyNot);
        Assert.False(c.Options[0].Erases);
        Assert.True(c.Options[1].Erases);
    }

    [Fact]
    public void EveryTypedNameMustMatchItsDrive()
    {
        var disks = new[] { new DriveDisk { Id = "2", Name = "Untitled (2)" }, new DriveDisk { Id = "3", Name = "Photos B" } };
        Assert.True(PhoneDrives.NamesMatch(new[] { "Untitled (2)", "  Photos B\n" }, disks));
        Assert.False(PhoneDrives.NamesMatch(new[] { "Untitled (2)", "photos b" }, disks));
        Assert.False(PhoneDrives.NamesMatch(new[] { "Photos B", "Untitled (2)" }, disks));
        Assert.False(PhoneDrives.NamesMatch(new[] { "Untitled (2)" }, disks));
        Assert.False(PhoneDrives.NamesMatch(Array.Empty<string>(), Array.Empty<DriveDisk>()));
        Assert.False(PhoneDrives.NamesMatch(new[] { "" }, new[] { new DriveDisk { Id = "9" } }));
    }

    [Theory]
    [InlineData("Photos", true)]
    [InlineData("My Photos_2-b", true)]
    [InlineData("aaaaaaaaaaaaaaaaaaaaaaaaaaa", true)]
    [InlineData("aaaaaaaaaaaaaaaaaaaaaaaaaaaa", false)]
    [InlineData("", false)]
    [InlineData(" Photos", false)]
    [InlineData("Photos/2", false)]
    [InlineData("Fotós", false)]
    public void TheCombinedDrivesName(string label, bool good) => Assert.Equal(good, PhoneDrives.GoodLabel(label));

    [Fact]
    public void TheRequests()
    {
        var disks = new[] { new DriveDisk { Id = "2", Name = "A" }, new DriveDisk { Id = "3", Name = "B" } };
        using (JsonDocument m = JsonDocument.Parse(PhoneDrives.CombineRequest("mirror", disks, "Photos ", new[] { " A", "B" })))
        {
            JsonElement r = m.RootElement;
            Assert.Equal("host_drives_combine", r.GetProperty("do").GetString());
            Assert.Equal(new[] { "2", "3" }, r.GetProperty("ids").EnumerateArray().Select(e => e.GetString()));
            Assert.Equal("Photos", r.GetProperty("label").GetString());
            Assert.Equal(new[] { "A", "B" }, r.GetProperty("names").EnumerateArray().Select(e => e.GetString()));
        }
        using (JsonDocument sep = JsonDocument.Parse(PhoneDrives.CombineRequest("separate", disks, "x", Array.Empty<string>())))
        {
            Assert.False(sep.RootElement.TryGetProperty("label", out _));
            Assert.False(sep.RootElement.TryGetProperty("names", out _));
        }
        using (JsonDocument rep = JsonDocument.Parse(PhoneDrives.ReplaceRequest("Photos", "4", " New ")))
        {
            Assert.Equal("host_drives_replace", rep.RootElement.GetProperty("do").GetString());
            Assert.Equal("Photos", rep.RootElement.GetProperty("set").GetString());
            Assert.Equal("4", rep.RootElement.GetProperty("ids")[0].GetString());
            Assert.Equal("New", rep.RootElement.GetProperty("name").GetString());
        }
        Assert.Equal("{\"do\":\"host_drives_choices\",\"ids\":[\"2\",\"3\"]}", PhoneDrives.ChoicesRequest(new[] { "2", "3" }));
    }

    // Every new error and progress word, as the Mac says it ("this Mac" there is "this PC" here).
    [Fact]
    public void TheDrivesWordsAreTheMacs()
    {
        string swift = Repo.Read("src.swift", "MediaViewerChrome", "Sources", "MediaViewerChrome", "PhoneAccessView.swift");
        var said = new List<string?> { PhoneWords.ErrorText("host_drives"), PhoneWords.ErrorText("host_drives_failed") };
        foreach (string b in new[] { "host_drives", "host_drives_choices", "host_drives_combine", "host_drives_replace" })
            said.Add(PhoneWords.BusyText(b));
        foreach (string? text in said)
        {
            Assert.NotNull(text);
            Assert.Contains("\"" + text!.Replace("this PC", "this Mac") + "\"", swift);
        }
        Assert.Equal("The drives weren't changed: disk4 is gone",
                     PhoneWords.ErrorText(new PhoneLinkState { Error = "host_drives_failed", HostSaid = "disk4 is gone" }, _ => ""));
        Assert.Contains("\"The drives weren't changed: \\(state.hostSaid)\"", swift);
        // The Mac card says the same things on its cards and rows.
        string view = Repo.Read("src.swift", "MediaViewerChrome", "Sources", "MediaViewerChrome", "PhoneDrivesView.swift");
        string mine = Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.PhoneDrives.cs");
        foreach (string words in new[] { "Use two or more drives…", "\"If one dies: \"", "For photos", "Risky",
                                         "Name of the combined drive", "Erase and combine", "Replace a drive…", "Erase and replace",
                                         "A drive to take its place", "Share each drive as it is", "\\nCan't be erased: " })
        {
            Assert.Contains(words, view);
            Assert.Contains(words, mine);
        }
    }
}
