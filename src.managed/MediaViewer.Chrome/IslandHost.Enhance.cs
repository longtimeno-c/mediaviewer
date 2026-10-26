// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance from the app's side (#297): Settings' row and its one-click install
// while it is not installed. Once installed it is a slot add-on like any other:
// its sheet and its settings page (where each kind's model is downloaded) are
// its own view trees (IslandHost.Screens.cs, IslandHost.AddonList.cs). Nothing
// here talks to the network except the plain GETs of the add-on's fixed
// release assets that every add-on install makes, and only when the person
// asks. The Mac twin is SlotAddonInstallOffer in SlotItems.swift.
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    private const string EnhanceId = "enhance";
    private const string EnhanceDescription =
        "Writes a restored copy of the photo, clip or audio file on screen. Each kind's model is downloaded only when you ask for it.";

    // Its release channel ("mediaviewer-addon-enhance-win-x64"). Not one of
    // AddonSlots: it has no chrome; it fills slots (IslandHost.Slots.cs).
    private static readonly AddonSlot EnhanceSlot = new(EnhanceId, "Enhance", null, null);
    private static string _enhanceMessage = "";

    private static bool EnhanceInstalled => SlotAddonRows.Any(r => r.Id == EnhanceId);

    private static void BuildEnhanceOffer(StackPanel panel)
    {
        panel.Children.Add(Note(EnhanceSlot.Offer.Description.Length > 0 ? EnhanceSlot.Offer.Description : EnhanceDescription));
        if (_addonsElsewhere)
        {
            // A later window: the first one manages add-ons (docs/design/18 "One host process").
            panel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (EnhanceSlot.Busy && EnhanceSlot.Phase is not null)
        {
            panel.Children.Add(ProgressFor(EnhanceSlot, 320));
            return;
        }
        // #525: a failed install says why and offers its next step.
        if (InstallFailureRow(EnhanceSlot, "Enhance", false, () => StartEnhanceInstall(),
                              () => StartEnhanceInstall(reinstall: true)) is FrameworkElement failed)
        {
            panel.Children.Add(failed);
        }
        else if (EnhanceSlot.Offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton(InstallWords.Install("Enhance", SizeText(EnhanceSlot.Offer.ArchiveSize)),
                                            () => StartEnhanceInstall());
            install.IsTabStop = true;
            install.HorizontalAlignment = HorizontalAlignment.Left;
            install.IsEnabled = !EnhanceSlot.Busy;
            panel.Children.Add(install);
        }
        else
        {
            AddOfferNote(panel, EnhanceSlot, "Enhance");
        }
        if (_enhanceMessage.Length > 0) panel.Children.Add(Note(_enhanceMessage));
    }

    private static void StartEnhanceInstall(bool reinstall = false)
    {
        if (EnhanceSlot.Busy || !_slotAddonsRead || _addonsElsewhere || _folderSession is not { } session) return;
        _enhanceMessage = "";
        IProgress<AddonPhase> progress = BeginInstall(EnhanceSlot);
        RefreshSlotSettings();
        _ = Task.Run(async () =>
        {
            // #525: the shared flow (IslandHost.InstallFlow.cs), then loaded with
            // the other slot add-ons, as Places is.
            InstallOutcome outcome = await RunInstall(EnhanceSlot, progress, reinstall).ConfigureAwait(false);
            if (outcome.Ok) StartSlotAddons(session);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                EndInstall(EnhanceSlot, outcome);
                _enhanceMessage = outcome.Ok ? InstallWords.Installed("Enhance") : "";
                RefreshSlotSettings();
            });
        });
    }
}
