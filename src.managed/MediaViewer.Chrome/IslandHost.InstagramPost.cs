// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// Instagram Post's row in Settings -> Add-ons (docs/design/30): the install
/// offer while it is not installed. Its window is a screen the add-on posts;
/// the Mac twin is SlotAddonInstallOffer.
/// </summary>
public static partial class IslandHost
{
    private const string InstagramId = "instagram";
    private const string InstagramName = "Instagram Post";
    private const string InstagramDescription =
        "Choose up to 20 photos and clips, set the shape, and export files ready to post.";

    // Its release channel ("mediaviewer-addon-instagram-win-x64").
    private static readonly AddonSlot InstagramSlot = new(InstagramId, InstagramName, null, null);
    private static string _instagramMessage = "";

    private static bool InstagramInstalled => SlotAddonRows.Any(r => r.Id == InstagramId);

    private static void BuildInstagramOffer(StackPanel panel)
    {
        panel.Children.Add(Note(InstagramSlot.Offer.Description.Length > 0 ? InstagramSlot.Offer.Description : InstagramDescription));
        if (_addonsElsewhere)
        {
            panel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (InstagramSlot.Busy && InstagramSlot.Phase is not null)
        {
            panel.Children.Add(ProgressFor(InstagramSlot, 320));
            return;
        }
        if (InstallFailureRow(InstagramSlot, InstagramName, false, () => StartInstagramInstall(),
                              () => StartInstagramInstall(reinstall: true)) is FrameworkElement failed)
        {
            panel.Children.Add(failed);
        }
        else if (InstagramSlot.Offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton(InstallWords.Install(InstagramName, SizeText(InstagramSlot.Offer.ArchiveSize)),
                                            () => StartInstagramInstall());
            install.IsTabStop = true;
            install.HorizontalAlignment = HorizontalAlignment.Left;
            install.IsEnabled = !InstagramSlot.Busy;
            panel.Children.Add(install);
        }
        else
        {
            AddOfferNote(panel, InstagramSlot, InstagramName);
        }
        if (_instagramMessage.Length > 0) panel.Children.Add(Note(_instagramMessage));
    }

    private static void StartInstagramInstall(bool reinstall = false)
    {
        if (InstagramSlot.Busy || !_slotAddonsRead || _addonsElsewhere || _folderSession is not { } session) return;
        _instagramMessage = "";
        IProgress<AddonPhase> progress = BeginInstall(InstagramSlot);
        RefreshSlotSettings();
        _ = Task.Run(async () =>
        {
            InstallOutcome outcome = await RunInstall(InstagramSlot, progress, reinstall).ConfigureAwait(false);
            if (outcome.Ok) StartSlotAddons(session);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                EndInstall(InstagramSlot, outcome);
                _instagramMessage = outcome.Ok ? InstallWords.Installed(InstagramName) : "";
                RefreshSlotSettings();
            });
        });
    }
}
