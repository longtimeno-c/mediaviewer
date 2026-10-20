// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire from the app's side (issues #410, #413, #444; docs/design/28): the
// share menu's first entry, its one-click install, and Settings' row while it is
// not installed. The window itself is the add-on's view tree
// (IslandHost.Screens.cs). Nothing here talks to the network except the plain
// GETs of the add-on's fixed release assets that every add-on install makes,
// and only when the person asks. The Mac twin is Driftwire.swift.
using System.Text.Json;
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    private const string DriftwireId = "driftwire";
    private const string DriftwireDescription =
        "Send photos, video and audio to a MediaViewer nearby, over Wi-Fi or Bluetooth. You type the code the other screen shows, and nothing moves until the other person accepts.";

    // Its release channel ("mediaviewer-addon-driftwire-win-x64"). Not one of
    // AddonSlots: it has no chrome; it fills slots (IslandHost.Slots.cs).
    private static readonly AddonSlot DriftwireSlot = new(DriftwireId, "Driftwire", null, null);

    // #410: each add-on's first window screen (Settings' Open, the share icon).
    private static readonly Dictionary<string, string> SlotWindowScreens = new(StringComparer.Ordinal);

    // The files a share click asked for before Driftwire was there; sent the
    // moment it attaches.
    private static List<string>? _driftwirePending;
    private static string _driftwireMessage = "";
    private static Flyout? _shareFlyout;

    private static bool DriftwireReady =>
        SlotAddonsLoaded.Contains(DriftwireId) && SlotWindowScreens.ContainsKey(DriftwireId);

    private static bool DriftwireInstalled => SlotAddonRows.Any(r => r.Id == DriftwireId);

    /// <summary>UI thread: Driftwire's window with these files as the ones to send.
    /// False when it is not loaded.</summary>
    private static bool OpenDriftwire(List<string> paths)
    {
        if (!DriftwireReady) return false;
        return SlotNative.Invoke(DriftwireId, "send_files", JsonSerializer.Serialize(paths));
    }

    // From ReadSlotContributions: the window screens, and a pending share.
    private static void NoteSlotWindowScreens(JsonElement root)
    {
        SlotWindowScreens.Clear();
        foreach (JsonElement s in JArr(root, "screens"))
        {
            if (JStr(s, "place") != "window") continue;
            string addon = JStr(s, "addon");
            if (addon.Length > 0 && !SlotWindowScreens.ContainsKey(addon)) SlotWindowScreens[addon] = JStr(s, "id");
        }
    }

    private static void AfterSlotContributions()
    {
        if (DriftwireReady && _driftwirePending is { } paths)
        {
            _driftwirePending = null;
            _shareFlyout?.Hide();
            OpenDriftwire(paths);
        }
        RefreshSlotSettings();
    }

    // ---- the share menu's Driftwire entry (#413, #444) ----------------------------------

    /// <summary>The share menu's first entry (IslandHost.Share.cs): these files to
    /// Driftwire, or the one-click install anchored where the menu was.</summary>
    private static void ShareWithDriftwire(List<string> paths, FrameworkElement? anchor, Windows.Foundation.Point? at)
    {
        if (paths.Count == 0) return;
        if (OpenDriftwire(paths))
        {
            // #442: as on the Mac, the grid closes and its range with it. The
            // add-on reads the files on its own thread; nothing here waits.
            if (_galleryVisible) Send(Command.CloseGallery);
            else ClearGalleryRange();
            return;
        }
        if (anchor?.XamlRoot is null) return;
        var panel = new StackPanel { Spacing = 8, MaxWidth = 340 };
        BuildDriftwireOffer(panel, paths);
        _shareFlyout = new Flyout { Content = panel, Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        _shareFlyout.Closed += (_, _) => _shareFlyout = null;
        if (at is { } p)
        {
            _shareFlyout.ShouldConstrainToRootBounds = false;
            _shareFlyout.ShowAt(anchor, new FlyoutShowOptions { Position = p });
        }
        else
        {
            _shareFlyout.ShowAt(anchor);
        }
        if (DriftwireSlot.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(DriftwireSlot);
    }

    // ---- the offer: Settings and the share icon's flyout -----------------------------------

    // `titled`: the name heads the panel (the share icon's flyout); Settings'
    // card carries its own (#442).
    private static void BuildDriftwireOffer(StackPanel panel, List<string>? thenOpen, bool titled = true)
    {
        if (titled)
        {
            TextBlock name = Label("Driftwire");
            name.Foreground = Brush(Title);
            panel.Children.Add(name);
        }
        panel.Children.Add(Note(DriftwireDescription));
        if (_addonsElsewhere)
        {
            // A later window: the first one manages add-ons (docs/design/18 "One host process").
            panel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (DriftwireSlot.Busy && DriftwireSlot.Phase is { } phase)
        {
            var bar = new AddonProgressView(300);
            bar.Show(phase);
            DriftwireSlot.Progress = bar;
            panel.Children.Add(bar.Root);
            return;
        }
        AddonOffer offer = DriftwireSlot.Offer;
        // #525: a failed install says why and offers its next step.
        if (InstallFailureRow(DriftwireSlot, "Driftwire", false, () => StartDriftwireInstall(thenOpen),
                              () => StartDriftwireInstall(thenOpen, reinstall: true)) is FrameworkElement failed)
        {
            panel.Children.Add(failed);
        }
        else if (offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton(InstallWords.Install("Driftwire", SizeText(offer.ArchiveSize)),
                                            () => StartDriftwireInstall(thenOpen));
            install.IsTabStop = true;
            install.HorizontalAlignment = HorizontalAlignment.Left;
            install.IsEnabled = !DriftwireSlot.Busy;
            panel.Children.Add(install);
        }
        else
        {
            AddOfferNote(panel, DriftwireSlot, "Driftwire");
        }
        if (_driftwireMessage.Length > 0) panel.Children.Add(Note(_driftwireMessage));
    }

    private static void RefreshDriftwireOffer()
    {
        RefreshSlotSettings();
        if (_shareFlyout?.Content is StackPanel panel)
        {
            panel.Children.Clear();
            BuildDriftwireOffer(panel, _driftwirePending);
        }
    }

    private static void StartDriftwireInstall(List<string>? thenOpen, bool reinstall = false)
    {
        if (DriftwireSlot.Busy || _addonsElsewhere || _folderSession is not { } session) return;
        _driftwirePending = thenOpen;
        _driftwireMessage = "";
        IProgress<AddonPhase> progress = BeginInstall(DriftwireSlot);
        RefreshDriftwireOffer();
        _ = Task.Run(async () =>
        {
            // #525: the shared flow (IslandHost.InstallFlow.cs), then attached live.
            InstallOutcome outcome = await RunInstall(DriftwireSlot, progress, reinstall).ConfigureAwait(false);
            bool loaded = false;
            if (outcome.Ok)
            {
                try
                {
                    // Re-verifies, loads, and attaches its window; the registry's
                    // listener then reads its contributions (AfterSlotContributions).
                    _ = AddonNative.Load(session, DriftwireId, SlotNative.Interface);
                    loaded = true;
                }
                catch (MediaViewerException ex)
                {
                    LogInstall(DriftwireSlot, "start", ex);
                    outcome = outcome with { Failure = InstallFailure.NotStarted };
                }
            }
            List<SlotAddonRow> rows = ReadSlotAddonRows();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                EndInstall(DriftwireSlot, outcome);
                _driftwireMessage = loaded ? InstallWords.Installed("Driftwire") : "";
                SlotAddonRows.Clear();
                SlotAddonRows.AddRange(rows);
                if (loaded) SlotAddonsLoaded.Add(DriftwireId);
                else _driftwirePending = null;
                EnsureSlotListener();
                RefreshDriftwireOffer();
                AfterSlotContributions();
            });
        });
    }

    // Worker: MediaViewer's own installed add-ons that fill slots (Settings' rows).
    private static List<SlotAddonRow> ReadSlotAddonRows()
    {
        var found = new List<SlotAddonRow>();
        try
        {
            using JsonDocument doc = JsonDocument.Parse(AddonNative.InstalledJson());
            foreach (JsonElement a in doc.RootElement.EnumerateArray())
            {
                string id = JStr(a, "id");
                bool slots = a.TryGetProperty("slots", out JsonElement s) && s.ValueKind == JsonValueKind.True;
                if (!slots || id == "import" || id.StartsWith("ai", StringComparison.Ordinal) || JStr(a, "part_of").Length > 0) continue;
                found.Add(new SlotAddonRow(id, JStr(a, "name"), JStr(a, "version"), JStr(a, "description"), JStr(a, "state"), false));
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        return found;
    }

    private static string SizeText(long bytes) => $"{Math.Max(1, (long)Math.Round(bytes / 1048576.0))} MB";
}
