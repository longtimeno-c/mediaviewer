// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Net.Http;
using System.Text.Json;
using MediaViewer.Interop;
using MediaViewer.Shared;
using MediaViewer.Updater;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// #525 (docs/design/18 "Installing"): one install flow for every add-on of
/// MediaViewer's own: Import, Local search's pieces, Places, Driftwire and any
/// other slot add-on. check (the channel's signed manifest) → download, with
/// bytes → verify (size and SHA-256) → install (the store verifies every file)
/// → attach live, or the restart card when a running copy cannot be replaced
/// (IslandHost.RestartNotice.cs). Each caller keeps only what is its own: its
/// line, its 5 GB rule (Local search), and how it attaches.
/// </summary>
/// <remarks>
/// A failure is kept on the slot (<see cref="AddonSlot.Failure"/>) and drawn by
/// <see cref="InstallFailureRow"/>: the plain sentence and one next step, Retry,
/// or Remove and install again when the copy on disk is itself unusable. The
/// exception's type goes to the log only (#509). The words are
/// Shared/InstallWords.cs, the same on the Mac.
/// Also the background check for add-on updates: once a session's add-ons are
/// read and again every day, only while "Check for updates automatically" is
/// on (docs/design/13), it asks the channels of what is installed, on a worker,
/// and the bar shows "Add-on update available" (<see cref="RefreshAddonUpdates"/>).
/// It never runs before the first pixel and draws nothing per frame.
/// </remarks>
public static partial class IslandHost
{
    private readonly record struct InstallOutcome(InstallFailure? Failure, bool NotPublished, string? Refused)
    {
        public bool Ok => Failure is null && !NotPublished && Refused is null;
    }

    /// <summary>UI. The start every install shares: busy, the bar at "Connecting…", the last failure gone.</summary>
    private static IProgress<AddonPhase> BeginInstall(AddonSlot slot)
    {
        slot.Busy = true;
        slot.Failure = null;
        slot.Phase = new AddonPhase(AddonPhaseKind.Downloading);
        return PhaseReporter(slot);
    }

    /// <summary>UI. The end every install shares; the caller then says what it did.</summary>
    private static void EndInstall(AddonSlot slot, InstallOutcome outcome)
    {
        slot.Busy = false;
        slot.Phase = null;
        slot.Progress = null;
        slot.Failure = outcome.Failure;
        if (outcome.NotPublished) slot.Offer = new AddonOffer(OfferKind.NotPublished, 0);
        RefreshAddonUpdates();
    }

    /// <summary>
    /// Worker. Download, verify and install <paramref name="slot"/>'s channel.
    /// <paramref name="reinstall"/>: Remove and install again; the installed copy
    /// goes first (its data and settings stay). <paramref name="refusal"/>: a
    /// caller's own words for a failure that is a rule, not a fault (Local
    /// search's room), or null.
    /// </summary>
    private static async Task<InstallOutcome> RunInstall(AddonSlot slot, IProgress<AddonPhase> progress, bool reinstall = false,
                                                         Action<JsonElement>? admit = null,
                                                         Func<Exception, string?>? refusal = null)
    {
        try
        {
            if (reinstall)
            {
                try { AddonNative.Remove(slot.Id, keepData: true); }
                catch (MediaViewerException ex) { LogInstall(slot, "remove", ex); }  // installed over instead
            }
            await DownloadAndInstall(slot, admit, progress).ConfigureAwait(false);
            return new InstallOutcome(null, false, null);
        }
        catch (AddonNotPublishedException)
        {
            return new InstallOutcome(null, true, null);
        }
        catch (Exception ex)
        {
            // Anything at all still ends the install, so Busy clears.
            LogInstall(slot, "install", ex);
            if (refusal?.Invoke(ex) is string refused) return new InstallOutcome(null, false, refused);
            return new InstallOutcome(FailureOf(ex), false, null);
        }
    }

    private static InstallFailure FailureOf(Exception ex) => ex switch
    {
        // ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL.
        IOException io when (io.HResult & 0xFFFF) is 0x70 or 0x27 => InstallFailure.NoSpace,
        HttpRequestException or TaskCanceledException or IOException => InstallFailure.Offline,
        InvalidDataException or JsonException or KeyNotFoundException => InstallFailure.Damaged,
        MediaViewerException { Status: MvStatus.Corrupt or MvStatus.UnsupportedFormat or MvStatus.InvalidArg }
            => InstallFailure.NotVerified,
        _ => InstallFailure.Failed,
    };

    // The add-on's id and the exception's type: no message, which can carry a path (rule 6).
    private static void LogInstall(AddonSlot slot, string step, Exception ex)
    {
        string status = ex is MediaViewerException mv ? $" {mv.Status}, reference {mv.CorrelationId}" : "";
        UpdaterLog($"addon {slot.Id}: {step} failed: {ex.GetType().Name}{status}");
    }

    /// <summary>
    /// The failure's sentence and its one next step, under an add-on's buttons;
    /// null when the last install did not fail. <paramref name="broken"/>: the
    /// installed copy did not verify (or would not start).
    /// </summary>
    private static FrameworkElement? InstallFailureRow(AddonSlot slot, string name, bool broken,
                                                       Action retry, Action reinstall)
    {
        if (slot.Failure is not InstallFailure failure || slot.Busy) return null;
        var row = new StackPanel { Spacing = Space(6) };
        TextBlock line = Note(InstallWords.Failure(failure, name));
        line.Foreground = Brush(Title);
        row.Children.Add(line);
        InstallNext next = InstallWords.Next(failure, broken);
        Button go = SettingsButton(InstallWords.NextLabel(next), next == InstallNext.Reinstall ? reinstall : retry);
        go.HorizontalAlignment = HorizontalAlignment.Left;
        if (next == InstallNext.Reinstall) ToolTipService.SetToolTip(go, InstallWords.ReinstallTip);
        AutomationProperties.SetName(go, $"{InstallWords.NextLabel(next)}: {name}");
        row.Children.Add(go);
        return row;
    }

    /// <summary>The channel's line before anything is installed (no Install offered).</summary>
    private static void AddOfferNote(StackPanel into, AddonSlot slot, string name)
    {
        switch (slot.Offer.Kind)
        {
            case OfferKind.NotPublished:
                into.Children.Add(Note(InstallWords.NotPublished(name)));
                break;
            case OfferKind.NeedsNewerApp:
                into.Children.Add(Note(InstallWords.NeedsNewerApp(name)));
                break;
            case OfferKind.Unreachable:
                into.Children.Add(Note(InstallWords.Unreachable));
                Button retry = SettingsButton(InstallWords.Retry, () => ProbeOffer(slot));
                retry.HorizontalAlignment = HorizontalAlignment.Left;
                into.Children.Add(retry);
                break;
            default:
                into.Children.Add(Note(InstallWords.CheckingFor(name)));
                break;
        }
    }

    // ---- updates available ---------------------------------------------------------------

    // The background check's own timer (daily) and whether the first one ran.
    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? _addonUpdateTimer;
    private static bool _addonUpdateCheckStarted;
    private static List<AddonUpdated> _addonUpdates = new();
    // Off the first pixel and the add-ons' own start (they verify their files).
    private static readonly TimeSpan AddonUpdateFirstDelay = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan AddonUpdateInterval = TimeSpan.FromHours(24);

    /// <summary>
    /// UI. What is installed has been read (Import and Local search's states, the
    /// slot add-ons' rows): start the background check, once per process.
    /// </summary>
    private static void StartAddonUpdateCheck()
    {
        if (_addonUpdateCheckStarted || _addonsElsewhere || _dispatcher is null) return;
        if (!_addonStatesRead || !_slotAddonsRead) return;
        _addonUpdateCheckStarted = true;
        bool now = InstallSelfTest();
        try
        {
            _addonUpdateTimer = _dispatcher.DispatcherQueue.CreateTimer();
            _addonUpdateTimer.Interval = now ? TimeSpan.FromSeconds(1) : AddonUpdateFirstDelay;
            _addonUpdateTimer.IsRepeating = false;
            _addonUpdateTimer.Tick += (t, _) => Guarded(() =>
            {
                ProbeInstalledChannels();
                t.Interval = AddonUpdateInterval;
                t.Start();
            });
            _addonUpdateTimer.Start();
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    /// <summary>
    /// UI. Asks the channel of every installed add-on (two small GETs each, on a
    /// worker: ProbeOffer), only while automatic checks are on. Settings opening
    /// asks them all whatever the switch says (the person is looking).
    /// </summary>
    private static void ProbeInstalledChannels()
    {
        if (!HasFlag(SettingFlag.UpdateAutoCheck) || _addonsElsewhere) return;
        foreach (AddonSlot slot in AddonSlots)
        {
            if (slot.State.Installed && !slot.Busy) ProbeOffer(slot);
        }
        foreach (SlotAddonRow row in SlotAddonRows)
        {
            AddonSlot channel = SlotChannelFor(row);
            if (!channel.Busy) ProbeOffer(channel);
        }
    }

    /// <summary>The updates the channels offer for what is installed, by the list's names.</summary>
    private static List<AddonUpdated> AddonUpdatesAvailable()
    {
        var list = new List<AddonUpdated>();
        if (!_addonStatesRead || _addonsElsewhere) return list;
        if (UpdateVersion(ImportSlot) is string import && !ImportSlot.Busy) list.Add(new("Import", import));
        // Local search is one add-on to a person: its newest piece version.
        string? search = AddonSlots.Where(s => (s == AiSlot || s.Parent == AiSlot.Id) && !s.Busy)
                                   .Select(UpdateVersion).Where(v => v is not null)
                                   .OrderByDescending(v => v, Comparer<string?>.Create((a, b) =>
                                       a == b ? 0 : IsNewer(a!, b!) ? 1 : -1))
                                   .FirstOrDefault();
        if (search is not null && AiSlot.State.Installed) list.Add(new("Local search", search));
        foreach (SlotAddonRow row in SlotAddonRows.OrderBy(r => r.Id == PlacesChannel.Id ? 0 : r.Id == DriftwireId ? 1 : 2)
                                                  .ThenBy(r => r.Name, StringComparer.CurrentCultureIgnoreCase))
        {
            AddonSlot channel = SlotChannelFor(row);
            if (!channel.Busy && SlotUpdateVersion(channel, row) is string v) list.Add(new(row.Name, v));
        }
        // An update already installed and waiting on a restart is not offered again.
        list.RemoveAll(u => _addonRestartFor.Any(r => r.Name == u.Name));
        return list;
    }

    /// <summary>UI. Anything an update hangs on changed: the bar's notice follows.</summary>
    private static void RefreshAddonUpdates()
    {
        List<AddonUpdated> now = AddonUpdatesAvailable();
        if (now.SequenceEqual(_addonUpdates)) return;
        _addonUpdates = now;
        ShowUpdateStatus(_lastUpdateStatus ?? new UpdateStatus(UpdatePhase.Inert, null, UpdateUrgency.Normal, null));
    }

    /// <summary>The notice's click: the one add-on's page, or the list when several.</summary>
    private static void OpenAddonUpdates()
    {
        string? page = null;
        if (_addonUpdates.Count == 1)
        {
            string name = _addonUpdates[0].Name;
            page = name switch
            {
                "Import" => ImportPageId,
                "Local search" => LocalSearchPageId,
                _ => SlotAddonRows.FirstOrDefault(r => r.Name == name)?.Id,
            };
        }
        _settingsKeyboard = false;
        if (!_settingsVisible)
        {
            _addonPageWanted = page;
            _addonListWanted = page is null;
            Send(Command.OpenSettings);
            return;
        }
        _showGeneralSettings?.Invoke();
        if (page is not null) OpenAddonPage(page);
        else ShowAddonList();
    }

    /// <summary>
    /// MV_ADDON_INSTALL_SELFTEST (docs/DEVELOPMENT.md), for the notice and the
    /// failure rows without a published update or a broken network:
    /// "places=update:9.9.9,import=fail:offline;now". <c>update:v</c> makes the
    /// channel offer v (until a probe asks it for real), <c>fail:kind</c> sets the
    /// last install's failure (an InstallFailure name), and <c>now</c> runs the
    /// background check in a second rather than 30 s (it still obeys the switch).
    /// Returns whether <c>now</c> was asked for.
    /// </summary>
    private static bool InstallSelfTest()
    {
        string? spec = Environment.GetEnvironmentVariable("MV_ADDON_INSTALL_SELFTEST");
        if (string.IsNullOrWhiteSpace(spec)) return false;
        Environment.SetEnvironmentVariable("MV_ADDON_INSTALL_SELFTEST", null);  // not inherited by a restart
        string[] parts = spec.Split(';', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        foreach (string pair in parts.Length > 0 ? parts[0].Split(',', StringSplitOptions.TrimEntries) : Array.Empty<string>())
        {
            string[] kv = pair.Split('=', 2, StringSplitOptions.TrimEntries);
            if (kv.Length != 2) continue;
            AddonSlot? slot = Array.Find(AddonSlots, s => s.Id == kv[0])
                ?? (kv[0] == PlacesChannel.Id ? PlacesChannel : kv[0] == DriftwireId ? DriftwireSlot
                    : SlotAddonRows.FirstOrDefault(r => r.Id == kv[0]) is { } row ? SlotChannelFor(row) : null);
            if (slot is null) continue;
            if (kv[1].StartsWith("update:", StringComparison.Ordinal))
                slot.Offer = new AddonOffer(OfferKind.Available, 12_000_000, 30_000_000, kv[1][7..]);
            else if (kv[1].StartsWith("fail:", StringComparison.Ordinal) &&
                     Enum.TryParse(kv[1][5..], ignoreCase: true, out InstallFailure f))
                slot.Failure = f;
        }
        RefreshAddonUpdates();
        RefreshAddonRow();
        RefreshSlotSettings();
        RefreshLocalSearch();
        RefreshDriftwireOffer();
        return parts.Contains("now");
    }

    // The notice asked for the list itself, not a page: scrolled into view when Settings shows.
    private static bool _addonListWanted;

    private static void ShowAddonList()
    {
        _addonListWanted = false;
        if (_addonPage.Length > 0) CloseAddonPage();
        try { _addonListPanel?.StartBringIntoView(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }
}
