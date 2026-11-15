// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone access from the app's side (docs/plans/phone-access-addon.md): an
// add-on with its own page in Settings → Add-ons, like the others. Installed,
// the page draws its "settings" screen, the add-on's own view tree
// (IslandHost.AddonSettingsScreen.cs); not installed, the page offers it.
// Nothing here talks to the network except the plain GETs of the add-on's
// fixed release assets every add-on install makes, and only when the person
// asks. A PC set up before the move is told its phones keep working through
// its server. The Mac twin is PhoneAccess.swift.
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    private const string PhoneAccessId = "phone-access";
    private const string PhoneAccessDescription =
        "Reach your library from your phone, at home and away: through a Synology, through this PC as your server, or through another computer you run. One scan pairs a phone.";

    // Its release channel ("mediaviewer-addon-phone-access-win-x64"). It has no
    // chrome; it fills a settings screen.
    private static readonly AddonSlot PhoneAccessSlot = new(PhoneAccessId, "Phone access", null, null);

    private static string _phoneAccessMessage = "";
    private static bool _phoneAccessBefore;  // a library set up before the move (its folder.json)

    private static bool PhoneAccessInstalled => SlotAddonRows.Any(r => r.Id == PhoneAccessId);

    // docs/plans/self-hosted.md "Tray": the host's Open Phone access settings
    // row opens Settings on its page (src/shell/chrome_host.h kSettingsRevealPhone).
    private const int SettingsRevealPhone = 1;
    private static bool _phoneRevealWanted;

    private static void RevealPhoneSettings()
    {
        _phoneRevealWanted = false;
        OpenAddonPage(PhoneAccessId);
    }

    /// <summary>Its page while it is not installed: the offer.</summary>
    private static void BuildPhoneAccessOffer(StackPanel panel)
    {
        panel.Children.Add(Note(PhoneAccessDescription));
        if (_phoneAccessBefore)
        {
            panel.Children.Add(Note(
                "Your paired phones keep working through your server. Install Phone access to add phones, remove them, and keep the copy this PC serves at home."));
        }
        if (_addonsElsewhere)
        {
            panel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (PhoneAccessSlot.Busy && PhoneAccessSlot.Phase is not null)
        {
            panel.Children.Add(ProgressFor(PhoneAccessSlot, 320));
            return;
        }
        if (InstallFailureRow(PhoneAccessSlot, "Phone access", false, () => StartPhoneAccessInstall(),
                              () => StartPhoneAccessInstall(reinstall: true)) is FrameworkElement failed)
        {
            panel.Children.Add(failed);
        }
        else if (PhoneAccessSlot.Offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton(InstallWords.Install("Phone access", SizeText(PhoneAccessSlot.Offer.ArchiveSize)),
                                            () => StartPhoneAccessInstall());
            install.IsTabStop = true;
            install.HorizontalAlignment = HorizontalAlignment.Left;
            install.IsEnabled = !PhoneAccessSlot.Busy;
            panel.Children.Add(install);
        }
        else
        {
            AddOfferNote(panel, PhoneAccessSlot, "Phone access");
        }
        if (_phoneAccessMessage.Length > 0) panel.Children.Add(Note(_phoneAccessMessage));
    }

    /// <summary>UI thread, as Settings shows: asks its channel, never at launch,
    /// and looks once for a library set up before the move.</summary>
    private static void ProbePhoneAccess()
    {
        if (PhoneAccessInstalled || _addonsElsewhere) return;
        if (PhoneAccessSlot.Offer.Kind is not (OfferKind.Unknown or OfferKind.Unreachable)) return;
        ProbeOffer(PhoneAccessSlot);
        _ = Task.Run(() =>
        {
            // One stat, off the UI thread: the folder the app kept the library in.
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            bool before = File.Exists(Path.Combine(local, "MediaViewer", "phone-access", "folder.json"));
            DispatcherQueueControllerTryEnqueue(() =>
            {
                if (before == _phoneAccessBefore) return;
                _phoneAccessBefore = before;
                RefreshSlotSettings();
            });
        });
    }

    private static void StartPhoneAccessInstall(bool reinstall = false)
    {
        if (PhoneAccessSlot.Busy || _addonsElsewhere || _folderSession is not { } session) return;
        _phoneAccessMessage = "";
        IProgress<AddonPhase> progress = BeginInstall(PhoneAccessSlot);
        RefreshSlotSettings();
        _ = Task.Run(async () =>
        {
            InstallOutcome outcome = await RunInstall(PhoneAccessSlot, progress, reinstall).ConfigureAwait(false);
            bool loaded = false;
            if (outcome.Ok)
            {
                try
                {
                    // Re-verifies, loads, and attaches its screen; the registry's
                    // listener then reads its contributions (AfterSlotContributions).
                    _ = AddonNative.Load(session, PhoneAccessId, SlotNative.Interface);
                    loaded = true;
                }
                catch (MediaViewerException ex)
                {
                    LogInstall(PhoneAccessSlot, "start", ex);
                    outcome = outcome with { Failure = InstallFailure.NotStarted };
                }
            }
            List<SlotAddonRow> rows = ReadSlotAddonRows();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                EndInstall(PhoneAccessSlot, outcome);
                _phoneAccessMessage = loaded ? "" : _phoneAccessMessage;
                SlotAddonRows.Clear();
                SlotAddonRows.AddRange(rows);
                if (loaded) SlotAddonsLoaded.Add(PhoneAccessId);
                EnsureSlotListener();
                AfterSlotContributions();
            });
        });
    }
}
