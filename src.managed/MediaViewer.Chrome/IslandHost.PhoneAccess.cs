// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access from the app's side (docs/plans/phone-access-addon.md,
// owner 2026-11-14): Phone access is an add-on. Installed, its card is the
// add-on's own view tree (its "settings" screen), drawn here by the screens'
// renderer (IslandHost.AddonSettingsScreen.cs opens it while Settings' list
// shows); not installed, this section offers it, once, the first time someone
// opens Settings. Nothing here talks to the network except the plain GETs of
// the add-on's fixed release assets every add-on install makes, and only when
// the person asks. A PC set up before the move is told its phones keep
// working through its server. The Mac twin is PhoneAccess.swift.
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

    private static StackPanel? _phoneAccessPanel;
    private static ContentControl? _phoneAccessScreen;
    private static string _phoneAccessMessage = "";
    private static bool _phoneAccessBefore;  // a library set up before the move (its folder.json)

    private static bool PhoneAccessInstalled => SlotAddonRows.Any(r => r.Id == PhoneAccessId);

    /// <summary>Settings' Phone access section: the add-on's screen, or the offer.</summary>
    private static void AddPhoneAccessSettings(StackPanel view)
    {
        view.Children.Add(SettingsSection("Phone access"));
        _phoneAccessPanel = new StackPanel { Spacing = Space(8) };
        _phoneAccessScreen = new ContentControl
        {
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            IsTabStop = false,
        };
        view.Children.Add(_phoneAccessPanel);
        RefreshPhoneAccess();
    }

    /// <summary>UI thread: Settings shows, or the add-on came or went.</summary>
    private static void RefreshPhoneAccess()
    {
        if (_phoneAccessPanel is null || _phoneAccessScreen is null) return;
        _phoneAccessPanel.Children.Clear();
        if (SlotSettingsScreens.ContainsKey(PhoneAccessId))
        {
            _phoneAccessPanel.Children.Add(_phoneAccessScreen);
            return;
        }
        _phoneAccessPanel.Children.Add(Note(PhoneAccessDescription));
        if (_phoneAccessBefore)
        {
            _phoneAccessPanel.Children.Add(Note(
                "Your paired phones keep working through your server. Install Phone access to add phones, remove them, and keep the copy this PC serves at home."));
        }
        if (_addonsElsewhere)
        {
            _phoneAccessPanel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (PhoneAccessSlot.Busy && PhoneAccessSlot.Phase is { } phase)
        {
            var bar = new AddonProgressView(300);
            bar.Show(phase);
            PhoneAccessSlot.Progress = bar;
            _phoneAccessPanel.Children.Add(bar.Root);
            return;
        }
        if (InstallFailureRow(PhoneAccessSlot, "Phone access", false, () => StartPhoneAccessInstall(),
                              () => StartPhoneAccessInstall(reinstall: true)) is FrameworkElement failed)
        {
            _phoneAccessPanel.Children.Add(failed);
        }
        else if (PhoneAccessSlot.Offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton(InstallWords.Install("Phone access", SizeText(PhoneAccessSlot.Offer.ArchiveSize)),
                                            () => StartPhoneAccessInstall());
            install.IsTabStop = true;
            install.HorizontalAlignment = HorizontalAlignment.Left;
            install.IsEnabled = !PhoneAccessSlot.Busy;
            _phoneAccessPanel.Children.Add(install);
        }
        else
        {
            AddOfferNote(_phoneAccessPanel, PhoneAccessSlot, "Phone access");
        }
        if (_phoneAccessMessage.Length > 0) _phoneAccessPanel.Children.Add(Note(_phoneAccessMessage));
    }

    /// <summary>UI thread, as Settings shows: offered the first time someone opens
    /// it (owner, 2026-11-14), never at launch.</summary>
    private static void OfferPhoneAccessIfWanted()
    {
        if (PhoneAccessInstalled || _addonsElsewhere) return;
        if (PhoneAccessSlot.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable)
        {
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
                    RefreshPhoneAccess();
                });
            });
        }
        RefreshPhoneAccess();
    }

    private static void StartPhoneAccessInstall(bool reinstall = false)
    {
        if (PhoneAccessSlot.Busy || _addonsElsewhere || _folderSession is not { } session) return;
        _phoneAccessMessage = "";
        IProgress<AddonPhase> progress = BeginInstall(PhoneAccessSlot);
        RefreshPhoneAccess();
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
                RefreshPhoneAccess();
            });
        });
    }
}
