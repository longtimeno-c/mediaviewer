// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint from the app's side (issues #410, #413, #444; docs/design/28): the
// share menu's first entry, its one-click install, and Settings' row while it is
// not installed. The window itself is the add-on's view tree
// (IslandHost.Screens.cs). Nothing here talks to the network except the plain
// GETs of the add-on's fixed release assets that every add-on install makes,
// and only when the person asks. The Mac twin is Sharepoint.swift.
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    private const string SharepointId = "sharepoint";
    private const string SharepointDescription =
        "Send photos, video and audio to a MediaViewer nearby, over Wi-Fi or Bluetooth. You type the code the other screen shows, and nothing moves until the other person accepts.";

    // Its release channel ("mediaviewer-addon-sharepoint-win-x64"). Not one of
    // AddonSlots: it has no chrome; it fills slots (IslandHost.Slots.cs).
    private static readonly AddonSlot SharepointSlot = new(SharepointId, "Sharepoint", null, null);

    // #410: each add-on's first window screen (Settings' Open, the share icon).
    private static readonly Dictionary<string, string> SlotWindowScreens = new(StringComparer.Ordinal);

    // The files a share click asked for before Sharepoint was there; sent the
    // moment it attaches.
    private static List<string>? _sharepointPending;
    private static string _sharepointMessage = "";
    private static Flyout? _shareFlyout;

    private static bool SharepointReady =>
        SlotAddonsLoaded.Contains(SharepointId) && SlotWindowScreens.ContainsKey(SharepointId);

    private static bool SharepointInstalled => SlotAddonRows.Any(r => r.Id == SharepointId);

    /// <summary>UI thread: Sharepoint's window with these files as the ones to send.
    /// False when it is not loaded.</summary>
    private static bool OpenSharepoint(List<string> paths)
    {
        if (!SharepointReady) return false;
        return SlotNative.Invoke(SharepointId, "send_files", JsonSerializer.Serialize(paths));
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
        if (SharepointReady && _sharepointPending is { } paths)
        {
            _sharepointPending = null;
            _shareFlyout?.Hide();
            OpenSharepoint(paths);
        }
        RefreshSlotSettings();
    }

    // ---- the share menu's Sharepoint entry (#413, #444) ----------------------------------

    /// <summary>The share menu's first entry (IslandHost.Share.cs): these files to
    /// Sharepoint, or the one-click install anchored where the menu was.</summary>
    private static void ShareWithSharepoint(List<string> paths, FrameworkElement? anchor, Windows.Foundation.Point? at)
    {
        if (paths.Count == 0) return;
        if (OpenSharepoint(paths))
        {
            // #442: as on the Mac, the grid closes and its range with it. The
            // add-on reads the files on its own thread; nothing here waits.
            if (_galleryVisible) Send(Command.CloseGallery);
            else ClearGalleryRange();
            return;
        }
        if (anchor?.XamlRoot is null) return;
        var panel = new StackPanel { Spacing = 8, MaxWidth = 340 };
        BuildSharepointOffer(panel, paths);
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
        if (SharepointSlot.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(SharepointSlot);
    }

    // ---- the offer: Settings and the share icon's flyout -----------------------------------

    // `titled`: the name heads the panel (the share icon's flyout); Settings'
    // card carries its own (#442).
    private static void BuildSharepointOffer(StackPanel panel, List<string>? thenOpen, bool titled = true)
    {
        if (titled)
        {
            TextBlock name = Label("Sharepoint");
            name.Foreground = Brush(Title);
            panel.Children.Add(name);
        }
        panel.Children.Add(Note(SharepointDescription));
        if (_addonsElsewhere)
        {
            // A later window: the first one manages add-ons (docs/design/18 "One host process").
            panel.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (SharepointSlot.Busy && SharepointSlot.Phase is { } phase)
        {
            var bar = new AddonProgressView(300);
            bar.Show(phase);
            SharepointSlot.Progress = bar;
            panel.Children.Add(bar.Root);
            return;
        }
        AddonOffer offer = SharepointSlot.Offer;
        switch (offer.Kind)
        {
            case OfferKind.Available:
            {
                Button install = SettingsButton($"Install Sharepoint, {SizeText(offer.ArchiveSize)}",
                                                () => StartSharepointInstall(thenOpen));
                install.IsTabStop = true;
                install.HorizontalAlignment = HorizontalAlignment.Left;
                install.IsEnabled = !SharepointSlot.Busy;
                panel.Children.Add(install);
                break;
            }
            case OfferKind.NotPublished:
                panel.Children.Add(Note("Sharepoint is not published for download yet. It will be offered here once a release carries it."));
                break;
            case OfferKind.NeedsNewerApp:
                panel.Children.Add(Note("The published Sharepoint needs a newer MediaViewer. Update MediaViewer, then install it."));
                break;
            case OfferKind.Unreachable:
            {
                panel.Children.Add(Note("Could not reach the download server."));
                Button retry = SettingsButton("Try again", () => ProbeOffer(SharepointSlot));
                retry.IsTabStop = true;
                retry.HorizontalAlignment = HorizontalAlignment.Left;
                panel.Children.Add(retry);
                break;
            }
            default:
                panel.Children.Add(Note("Checking for Sharepoint…"));
                break;
        }
        if (_sharepointMessage.Length > 0) panel.Children.Add(Note(_sharepointMessage));
    }

    private static void RefreshSharepointOffer()
    {
        RefreshSlotSettings();
        if (_shareFlyout?.Content is StackPanel panel)
        {
            panel.Children.Clear();
            BuildSharepointOffer(panel, _sharepointPending);
        }
    }

    private static void StartSharepointInstall(List<string>? thenOpen)
    {
        if (SharepointSlot.Busy || _addonsElsewhere || _folderSession is not { } session) return;
        SharepointSlot.Busy = true;
        _sharepointPending = thenOpen;
        _sharepointMessage = "";
        SharepointSlot.Phase = new AddonPhase(AddonPhaseKind.Downloading);
        IProgress<AddonPhase> progress = PhaseReporter(SharepointSlot);
        RefreshSharepointOffer();
        _ = Task.Run(async () =>
        {
            string message = "";
            bool loaded = false;
            try
            {
                await DownloadAndInstall(SharepointSlot, null, progress).ConfigureAwait(false);
                // Re-verifies, loads, and attaches its window; the registry's
                // listener then reads its contributions (AfterSlotContributions).
                _ = AddonNative.Load(session, SharepointId, SlotNative.Interface);
                loaded = true;
            }
            catch (AddonNotPublishedException)
            {
                DispatcherQueueControllerTryEnqueue(() => SharepointSlot.Offer = new AddonOffer(OfferKind.NotPublished, 0));
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or MediaViewerException
                                           or InvalidDataException or TaskCanceledException or JsonException)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = ex is MediaViewerException or InvalidDataException
                    ? "The download did not verify, so nothing was installed."
                    : "Sharepoint could not be downloaded. Check the connection and try again.";
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = "Sharepoint could not be installed.";
            }
            List<SlotAddonRow> rows = ReadSlotAddonRows();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                SharepointSlot.Busy = false;
                SharepointSlot.Phase = null;
                SharepointSlot.Progress = null;
                _sharepointMessage = message;
                SlotAddonRows.Clear();
                SlotAddonRows.AddRange(rows);
                if (loaded) SlotAddonsLoaded.Add(SharepointId);
                else _sharepointPending = null;
                EnsureSlotListener();
                RefreshSharepointOffer();
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
