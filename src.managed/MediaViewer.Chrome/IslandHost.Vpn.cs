// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN add-on from the app's side (docs/plans/vpn-addon.md): a row of
// Settings → Add-ons whose page offers it with one click while it is not
// installed. Once installed it is a slot add-on like any other: its page draws
// its own "settings" screen. Nothing here talks to the network except the plain
// GETs of the add-on's fixed release assets that every add-on install makes,
// and only when the person asks. The Mac twin is the VPN offer in SlotItems.swift.
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    private const string VpnId = "vpn";
    private const string VpnDescription =
        "Your own VPN, with no VPN company: away from home, send this computer's traffic through your home computer or your server, and use this computer or your server as your laptop's way out.";

    // Its release channel ("mediaviewer-addon-vpn-win-x64"). It has no chrome;
    // it fills a settings screen.
    private static readonly AddonSlot VpnSlot = new(VpnId, "VPN", null, null);
    private static string _vpnMessage = "";

    private static bool VpnInstalled => SlotAddonRows.Any(r => r.Id == VpnId);

    /// <summary>Its page while it is not installed: the offer.</summary>
    private static void BuildVpnOffer(StackPanel panel)
    {
        panel.Children.Add(Note(VpnSlot.Offer.Description.Length > 0 ? VpnSlot.Offer.Description : VpnDescription));
        if (_addonsElsewhere)
        {
            panel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (VpnSlot.Busy && VpnSlot.Phase is not null)
        {
            panel.Children.Add(ProgressFor(VpnSlot, 320));
            return;
        }
        if (InstallFailureRow(VpnSlot, "VPN", false, () => StartVpnInstall(),
                              () => StartVpnInstall(reinstall: true)) is FrameworkElement failed)
        {
            panel.Children.Add(failed);
        }
        else if (VpnSlot.Offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton(InstallWords.Install("VPN", SizeText(VpnSlot.Offer.ArchiveSize)),
                                            () => StartVpnInstall());
            install.IsTabStop = true;
            install.HorizontalAlignment = HorizontalAlignment.Left;
            install.IsEnabled = !VpnSlot.Busy;
            panel.Children.Add(install);
        }
        else
        {
            AddOfferNote(panel, VpnSlot, "VPN");
        }
        if (_vpnMessage.Length > 0) panel.Children.Add(Note(_vpnMessage));
    }

    private static void StartVpnInstall(bool reinstall = false)
    {
        if (VpnSlot.Busy || !_slotAddonsRead || _addonsElsewhere || _folderSession is not { } session) return;
        _vpnMessage = "";
        IProgress<AddonPhase> progress = BeginInstall(VpnSlot);
        RefreshSlotSettings();
        _ = Task.Run(async () =>
        {
            // The shared flow (IslandHost.InstallFlow.cs), then loaded with the
            // other slot add-ons, as Enhance is.
            InstallOutcome outcome = await RunInstall(VpnSlot, progress, reinstall).ConfigureAwait(false);
            if (outcome.Ok) StartSlotAddons(session);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                EndInstall(VpnSlot, outcome);
                _vpnMessage = outcome.Ok ? InstallWords.Installed("VPN") : "";
                RefreshSlotSettings();
            });
        });
    }
}
