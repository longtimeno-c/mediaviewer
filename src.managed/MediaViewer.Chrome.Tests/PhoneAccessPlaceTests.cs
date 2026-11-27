// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone access is a row of Settings → Add-ons with its settings (or its offer)
// on its own page, like every other add-on, not a section of its own in
// Settings (IslandHost.PhoneAccess.cs). The island cannot be built in a unit
// test, so its wiring is read as text; the Mac's twin is PhoneAccessTests.swift.
namespace MediaViewer.Chrome.Tests;

public class PhoneAccessPlaceTests
{
    private static string Chrome(string file) =>
        SourceText.StripComments(Repo.Read("src.managed", "MediaViewer.Chrome", file));

    [Fact]
    public void ItIsARowOfTheAddOnsList()
    {
        string list = Chrome("IslandHost.AddonList.cs");
        Assert.Contains("list.Add(new(PhoneAccessId, \"Phone access\"", list);
        Assert.Contains("r.Id != PhoneAccessId)", list);  // not listed twice
    }

    [Fact]
    public void ItsSettingsAndOfferAreOnItsPage()
    {
        string screens = Chrome("IslandHost.AddonSettingsScreen.cs");
        Assert.DoesNotContain("PhoneAccessId", screens);  // drawn on its page, as every add-on's
        string slots = Chrome("IslandHost.Slots.cs");
        Assert.Contains("only == PhoneAccessId", slots);
        Assert.Contains("BuildPhoneAccessOffer(body)", slots);
        Assert.DoesNotContain("Its settings are in Settings", slots);
    }

    [Fact]
    public void SettingsHasNoSectionOfItsOwn()
    {
        Assert.DoesNotContain("AddPhoneAccessSettings", Chrome("IslandHost.Settings.cs"));
        Assert.DoesNotContain("SettingsSection(\"Phone access\")", Chrome("IslandHost.PhoneAccess.cs"));
        // The tray's Open Phone access settings opens its page.
        Assert.Contains("OpenAddonPage(PhoneAccessId)", Chrome("IslandHost.PhoneAccess.cs"));
    }
}
