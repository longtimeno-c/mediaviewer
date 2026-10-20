// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #525: an add-on install says the same on both platforms. The C# words
// (src.managed/Shared/InstallWords.cs) are run here; the Swift copy
// (InstallWords.swift) is read as text: every C# string is one of its strings,
// and its failure kinds are the C# enum's, in order.
using System.Text.RegularExpressions;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class InstallWordsTests
{
    [Fact]
    public void AFailureSaysWhatHappenedInPlainWords()
    {
        Assert.Equal("Places could not be downloaded. Check the connection, then retry.",
                     InstallWords.Failure(InstallFailure.Offline, "Places"));
        Assert.Equal("The download of Import was damaged, so nothing was installed.",
                     InstallWords.Failure(InstallFailure.Damaged, "Import"));
        Assert.Equal("Local search did not pass verification, so nothing was installed.",
                     InstallWords.Failure(InstallFailure.NotVerified, "Local search"));
        Assert.Equal("There is not enough free space to install Driftwire.",
                     InstallWords.Failure(InstallFailure.NoSpace, "Driftwire"));
        Assert.Equal("Import is installed but could not start.", InstallWords.Failure(InstallFailure.NotStarted, "Import"));
        Assert.Equal("Import could not be installed.", InstallWords.Failure(InstallFailure.Failed, "Import"));
    }

    [Fact]
    public void NoFailureNamesAnExceptionType()
    {
        foreach (InstallFailure f in Enum.GetValues<InstallFailure>())
        {
            string text = InstallWords.Failure(f, "Import");
            Assert.DoesNotMatch(new Regex(@"Exception|Error\b|\(\w+\)"), text);
        }
    }

    [Fact]
    public void TheNextStepIsRetryUnlessTheCopyIsBroken()
    {
        Assert.Equal(InstallNext.Retry, InstallWords.Next(InstallFailure.Offline, broken: false));
        // A working copy whose update failed keeps running: never removed.
        Assert.Equal(InstallNext.Retry, InstallWords.Next(InstallFailure.NotVerified, broken: false));
        Assert.Equal(InstallNext.Reinstall, InstallWords.Next(InstallFailure.Offline, broken: true));
        Assert.Equal(InstallNext.Reinstall, InstallWords.Next(InstallFailure.NotStarted, broken: false));
        Assert.Equal("Retry", InstallWords.NextLabel(InstallNext.Retry));
        Assert.Equal("Remove and install again", InstallWords.NextLabel(InstallNext.Reinstall));
    }

    [Fact]
    public void TheStepsAndButtons()
    {
        Assert.Equal("412 MB of 1.08 GB", InstallWords.Downloaded("412 MB", "1.08 GB"));
        Assert.Equal("Downloading Import…", InstallWords.Downloading("Import", null));
        Assert.Equal("Downloading Places 1.1.0…", InstallWords.Downloading("Places", "1.1.0"));
        Assert.Equal("Install Places, 12.0 MB", InstallWords.Install("Places", "12.0 MB"));
        Assert.Equal("Update to 1.1.0, 12.0 MB", InstallWords.UpdateTo("1.1.0", "12.0 MB"));
        Assert.Equal("Places updated to 1.1.0.", InstallWords.Updated("Places", "1.1.0"));
    }

    [Fact]
    public void TheUpdatesNotice()
    {
        var one = new List<AddonUpdated> { new("Places", "1.1.0") };
        var two = new List<AddonUpdated> { new("Import", "1.3.0"), new("Places", "1.1.0") };
        Assert.Equal("Add-on update available", InstallWords.UpdatesBadge(1));
        Assert.Equal("2 add-on updates available", InstallWords.UpdatesBadge(2));
        Assert.Equal("Places 1.1.0 is available. Open Settings → Add-ons to update it.", InstallWords.UpdatesTip(one));
        Assert.Equal("Import 1.3.0 and Places 1.1.0 are available. Open Settings → Add-ons to update them.",
                     InstallWords.UpdatesTip(two));
        Assert.Equal("", InstallWords.UpdatesTip(new List<AddonUpdated>()));
    }

    [Fact]
    public void TheMacSaysTheSame() =>
        RestartWordsTests.AssertMacHas("InstallWords.cs", "InstallWords", "InstallWords.swift");

    [Fact]
    public void TheMacHasTheSameFailures()
    {
        string swift = SourceText.StripComments(
            Repo.Read("src.swift", "MediaViewerChrome", "Sources", "MediaViewerChrome", "InstallWords.swift"));
        foreach ((string name, Type type) in new[] { ("InstallFailure", typeof(InstallFailure)), ("InstallNext", typeof(InstallNext)) })
        {
            string body = SourceText.Block(swift, $@"enum\s+{name}\b[^{{]*");
            List<string> mac = Regex.Matches(body, @"\bcase\s+([^\n]+)")
                                    .SelectMany(m => m.Groups[1].Value.Split(','))
                                    .Select(c => c.Trim()).Where(c => c.Length > 0).ToList();
            List<string> win = Enum.GetNames(type).Select(n => char.ToLowerInvariant(n[0]) + n[1..]).ToList();
            Assert.Equal(win, mac);
        }
    }

    [Fact]
    public void WindowsShowsNoExceptionTypeOnScreen()
    {
        // #509: a type name in the add-on load failure's sentence, gone; the log keeps it.
        string addons = Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.Addons.cs");
        Assert.DoesNotContain("could not start ({ex.GetType().Name})", addons);
    }
}
