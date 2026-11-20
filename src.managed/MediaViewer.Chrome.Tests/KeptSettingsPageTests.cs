// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A kept entry whose screen is a settings page (the VPN's, docs/plans/vpn-addon.md):
// the tray icon's Open opens Settings at its add-on's page, not a window of its
// own (IslandHost.Notices.cs, src/shell/main.cpp show_tray_menu). The island and
// the host cannot run in a unit test, so the wiring is read as text; the Mac's
// twin is PhoneAccessTests.testAKeptSettingsPageOpensInSettings.
namespace MediaViewer.Chrome.Tests;

public class KeptSettingsPageTests
{
    private static string Code(params string[] parts) => SourceText.StripComments(Repo.Read(parts));

    [Fact]
    public void TheChromeWantsTheAddOnsPage()
    {
        string notices = Code("src.managed", "MediaViewer.Chrome", "IslandHost.Notices.cs");
        Assert.Contains("SlotSettingsScreens.TryGetValue(parts[0], out string? page) && page == parts[1]", notices);
        Assert.Contains("_addonPageWanted = parts[0];", notices);
        Assert.Contains("return 1;", notices);
    }

    [Fact]
    public void TheHostOpensSettingsForIt()
    {
        string host = Code("src", "shell", "chrome_host.cpp");
        Assert.Contains("case 1: return addon_screen_opened::settings_page;", host);
        string main = Code("src", "shell", "main.cpp");
        Assert.Matches(@"addon_screen_opened::settings_page\)\s*\{\s*leave_background\(app\);\s*set_settings_open\(app, true\);", main);
    }
}
