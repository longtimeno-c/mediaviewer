// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN add-on is a row of Settings → Add-ons whether or not it is installed,
// and its page offers the install while it is not (IslandHost.Vpn.cs): Phone
// access points people at "Settings → Add-ons → VPN". The island cannot be
// built in a unit test, so its wiring is read as text; the Mac's twin is
// VpnTests.swift.
namespace MediaViewer.Chrome.Tests;

public class VpnPlaceTests
{
    private static string Chrome(string file) =>
        SourceText.StripComments(Repo.Read("src.managed", "MediaViewer.Chrome", file));

    [Fact]
    public void ItIsARowOfTheAddOnsListEvenBeforeItIsInstalled()
    {
        string list = Chrome("IslandHost.AddonList.cs");
        Assert.Contains("list.Add(new(VpnId, \"VPN\"", list);
        Assert.Contains("r.Id != VpnId)", list);  // not listed twice once installed
        Assert.Contains("private const string VpnId = \"vpn\";", Chrome("IslandHost.Vpn.cs"));
    }

    [Fact]
    public void ItsPageOffersTheInstall()
    {
        string slots = Chrome("IslandHost.Slots.cs");
        Assert.Contains("only == VpnId", slots);
        Assert.Contains("BuildVpnOffer(body)", slots);
        Assert.Contains("probe.Add(VpnSlot)", slots);
        Assert.Contains("if (row.Id == VpnId) return VpnSlot;", slots);  // its updates from its own channel
        Assert.Contains("slot == VpnSlot", Chrome("IslandHost.Addons.cs"));  // a probe redraws its page
    }

    [Fact]
    public void ItsChannelIsTheOneTheReleasePublishes()
    {
        // The release refuses to ship without it (tools/package/github-release.py).
        Assert.Contains("'vpn'", Repo.Read("tools", "package", "github-release.py"));
        Assert.Contains("new(VpnId, \"VPN\", null, null)", Chrome("IslandHost.Vpn.cs"));
    }
}
