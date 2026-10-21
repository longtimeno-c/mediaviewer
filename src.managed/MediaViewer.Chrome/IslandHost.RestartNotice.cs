// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Windows.Foundation;

namespace MediaViewer.Chrome;

/// <summary>
/// #510 (docs/design/18 "Updates"): after an add-on update that takes over only
/// at the next start (Import, Local search: a loaded chrome is not replaced
/// under itself), a card in the bottom-right corner says "Import was updated to
/// 1.3.0. Restart MediaViewer to use it." with Later and Restart now. Several
/// updates make one card. Later parks it; Settings → Add-ons keeps "Restart to
/// finish updating" until the next start (IslandHost.Update.cs). Restart now
/// never cuts work off: while an import, an add-on's transfer (busy_post),
/// Local search indexing or an add-on download runs, the card says "Waiting for
/// … to finish" and restarts once it has, or at Restart anyway.
/// </summary>
/// <remarks>
/// The resume notice's place and look (IslandHost.ResumeNotice.cs): a small
/// island of its own over the canvas corner, above the filmstrip, stacked over
/// the resume notice while that is up, with the hint card above both. It does
/// not take focus. It draws nothing into the swapchain and runs nothing per
/// frame: the only timer is the one-second check while Restart now waits.
/// A slot add-on (Places, Driftwire) needs no card: its update reloads it at
/// once (<see cref="ReloadUpdatedSlotAddon"/>), after a transfer it reports
/// has ended. The twin of the Mac's RestartNoticeView.swift.
/// </remarks>
public static partial class IslandHost
{
    private const double RestartNoticeWidth = 340;

    private static DesktopWindowXamlSource? _restartNotice;
    private static Border? _restartNoticeCard;
    private static TextBlock? _restartNoticeTitle;
    private static TextBlock? _restartNoticeBody;
    private static Button? _restartNoticeLater;
    private static Button? _restartNoticeNow;
    private static Button? _restartNoticeCancel;
    private static Button? _restartNoticeAnyway;
    // The card is up (placed in the corner), not parked.
    private static bool _restartNoticeShown;
    // Restart now is waiting for these to finish; empty when it is not.
    private static List<string> _restartWaitingFor = new();
    private static DispatcherQueueTimer? _restartWaitTimer;

    // What Restart now waits for. Import's progress line is up
    // (IAddonHost.SetStatus); the indexing pill is busy (HostSetIndexingPill);
    // an add-on said what it is doing (busy_post, ReadSlotNotices).
    private static bool _importJobRunning;
    private static bool _indexingBusy;
    private static readonly Dictionary<string, string> SlotBusy = new(StringComparer.Ordinal);
    // MV_ADDON_RESTART_SELFTEST's made-up work ("" none): not in SlotBusy, which
    // each notices read replaces.
    private static string _selfTestBusy = "";
    // A slot add-on updated while it was busy: reloaded once it is not.
    private static readonly Dictionary<string, AddonUpdated> PendingSlotReloads = new(StringComparer.Ordinal);

    /// <summary>AddonRestartNeeded: the card, with every update waiting on a restart.</summary>
    private static void ShowRestartNotice()
    {
        if (_addonRestartFor.Count == 0)
        {
            CloseRestartNotice();
            return;
        }
        // The window is not up (or a later window's chrome is going): the bar and
        // Settings still offer the restart.
        if (_themeWindow == IntPtr.Zero || _source?.Content is not FrameworkElement) return;
        try
        {
            if (_restartNotice is null)
            {
                _restartNotice = new DesktopWindowXamlSource();
                _restartNotice.Initialize(Microsoft.UI.Win32Interop.GetWindowIdFromWindow(_themeWindow));
                _restartNotice.TakeFocusRequested += OnTakeFocusRequested;
                // Parked off the client area until it is placed: a default
                // full-client island would flash over the canvas.
                MoveAt(_restartNotice, 0, ClientHeightPx(), 1, 1);
                _restartNoticeCard = BuildRestartNoticeCard();
                // Measured for real once it is in the island's live tree: placed again then.
                _restartNoticeCard.Loaded += (_, _) => Guarded(PlaceCornerCards);
                // Behind the card's rounded corners, the canvas colour (the AI badge's).
                _restartNotice.Content = new Grid { Background = Brush(Canvas), Children = { _restartNoticeCard } };
            }
            _restartNoticeShown = true;
            RenderRestartNotice();
            PlaceCornerCards();
            // Said once as it appears, not only when focused.
            FrameworkElementAutomationPeer.FromElement(_restartNoticeCard!)?.RaiseNotificationEvent(
                AutomationNotificationKind.ActionCompleted, AutomationNotificationProcessing.ImportantMostRecent,
                _restartNoticeTitle?.Text ?? "", "addon-restart");
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            DropRestartNotice();
        }
    }

    private static Border BuildRestartNoticeCard()
    {
        var stack = new StackPanel { Spacing = Space(8) };
        _restartNoticeTitle = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = UiFontSize + 1,
            Foreground = Brush(Title),
            TextWrapping = TextWrapping.Wrap,
        };
        _restartNoticeBody = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = UiFontSize - 2,
            Foreground = Brush(Body),
            TextWrapping = TextWrapping.Wrap,
        };
        stack.Children.Add(_restartNoticeTitle);
        stack.Children.Add(_restartNoticeBody);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = Space(8),
            HorizontalAlignment = HorizontalAlignment.Right,
        };
        // All four are made once and shown by mode: a button removed from the
        // tree inside its own Click is not safe in an island.
        _restartNoticeLater = TextButton(RestartWords.Later, RestartNoticeLater);
        ToolTipService.SetToolTip(_restartNoticeLater, RestartWords.LaterTip);
        _restartNoticeNow = TextButton(RestartWords.RestartNow, RestartWhenIdle);
        _restartNoticeCancel = TextButton(RestartWords.Cancel, CancelRestartWait);
        ToolTipService.SetToolTip(_restartNoticeCancel, RestartWords.CancelTip);
        _restartNoticeAnyway = TextButton(RestartWords.RestartAnyway, RestartAnyway);
        foreach (Button b in new[] { _restartNoticeLater, _restartNoticeNow, _restartNoticeCancel, _restartNoticeAnyway })
        {
            b.IsTabStop = true;
            b.AllowFocusOnInteraction = true;
            buttons.Children.Add(b);
        }
        stack.Children.Add(buttons);
        return new Border
        {
            Width = RestartNoticeWidth,
            Padding = Pad(14),
            CornerRadius = Corner(8),
            BorderThickness = new Thickness(1),
            BorderBrush = Brush(Hairline),
            Background = Brush(ChromeColour.Surface),
            Child = stack,
        };
    }

    private static bool RestartWaiting => _restartWaitingFor.Count > 0;

    private static void RenderRestartNotice()
    {
        if (_restartNoticeCard is null || _restartNoticeTitle is null || _restartNoticeBody is null) return;
        bool waiting = RestartWaiting;
        string title = waiting ? RestartWords.WaitingTitle(_restartWaitingFor) : RestartWords.Title(_addonRestartFor);
        _restartNoticeTitle.Text = title;
        _restartNoticeBody.Text = waiting ? RestartWords.WaitingBody : RestartWords.Body(_addonRestartFor.Count);
        AutomationProperties.SetName(_restartNoticeCard, title);
        Visibility normal = waiting ? Visibility.Collapsed : Visibility.Visible;
        Visibility wait = waiting ? Visibility.Visible : Visibility.Collapsed;
        if (_restartNoticeLater is not null) _restartNoticeLater.Visibility = normal;
        if (_restartNoticeNow is not null) _restartNoticeNow.Visibility = normal;
        if (_restartNoticeCancel is not null) _restartNoticeCancel.Visibility = wait;
        if (_restartNoticeAnyway is not null)
        {
            _restartNoticeAnyway.Visibility = wait;
            if (waiting) ToolTipService.SetToolTip(_restartNoticeAnyway, RestartWords.AnywayTip(_restartWaitingFor));
        }
    }

    /// <summary>
    /// The resume notice, the restart card and the hint card share the
    /// bottom-right corner, stacked in that order from the bottom (the Mac's
    /// main_mac.mm stacks the same three).
    /// </summary>
    private static void PlaceCornerCards()
    {
        PlaceResumeNotice();
        PlaceRestartNotice();
        PlaceHintNotice();
    }

    // Physical pixels the restart card takes in the corner (with its margin), 0 parked.
    private static int RestartNoticeTakesPx(double scale, int margin) =>
        _restartNoticeShown && _restartNotice is not null && _restartNoticeCard is not null
            ? (int)Math.Ceiling(_restartNoticeCard.DesiredSize.Height * scale) + margin
            : 0;

    private static void PlaceRestartNotice()
    {
        if (_restartNotice is null || _restartNoticeCard is null || !_restartNoticeShown) return;
        if (_themeWindow == IntPtr.Zero || !GetClientRect(_themeWindow, out NativeRect client)) return;
        uint dpi = GetDpiForWindow(_themeWindow);
        double scale = dpi > 0 ? dpi / 96.0 : 1;
        _restartNoticeCard.Measure(new Size(RestartNoticeWidth, double.PositiveInfinity));
        double height = _restartNoticeCard.DesiredSize.Height;
        if (height <= 0) height = 120;  // not measured yet: about two lines and the buttons
        int width = (int)Math.Ceiling(RestartNoticeWidth * scale);
        int tall = (int)Math.Ceiling(height * scale);
        int margin = (int)Math.Round(ResumeNoticeMargin * scale);
        int clientWidth = client.Right - client.Left;
        int bottom = (_filmstripTop > 0 ? _filmstripTop : client.Bottom - client.Top) - margin;
        if (_resumeNotice is not null && _resumeNoticeCard is not null)
        {
            bottom -= (int)Math.Ceiling(_resumeNoticeCard.DesiredSize.Height * scale) + margin;
        }
        MoveAt(_restartNotice, Math.Max(0, clientWidth - width - margin), Math.Max(0, bottom - tall), width, tall);
        // Above the gallery and the panes, as the resume notice is.
        try { _restartNotice.SiteBridge?.MoveInZOrderAtTop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    private static void RestartNoticeLater() => CloseRestartNotice(restoreFocus: true);

    // Parked, not disposed (a later update shows it again). Only a click hands
    // the keyboard back.
    private static void CloseRestartNotice(bool restoreFocus = false)
    {
        StopRestartWait();
        if (!_restartNoticeShown) return;
        _restartNoticeShown = false;
        try { if (_restartNotice is not null) MoveAt(_restartNotice, 0, ClientHeightPx(), 1, 1); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        if (restoreFocus) RestoreCanvasFocus();
        PlaceHintNotice();  // the hint card stacked above it comes down
    }

    /// <summary>Detach (the window is going): no island outlives it.</summary>
    private static void DropRestartNotice()
    {
        StopRestartWait();
        _restartNoticeShown = false;
        _restartNoticeCard = null;
        _restartNoticeTitle = null;
        _restartNoticeBody = null;
        _restartNoticeLater = _restartNoticeNow = _restartNoticeCancel = _restartNoticeAnyway = null;
        if (_restartNotice is not null) _restartNotice.TakeFocusRequested -= OnTakeFocusRequested;
        DisposeSource(ref _restartNotice);
    }

    // ---- Restart now waits for running work --------------------------------------

    /// <summary>
    /// What Restart now would cut off, in words ("an import", "a Driftwire
    /// transfer"). An F7 / F8 copy needs no wait: the file worker finishes it
    /// before the process exits (shell/file_jobs.h), and the new process waits
    /// for that exit (--relaunch-after).
    /// </summary>
    private static List<string> RestartBlockers()
    {
        var what = new List<string>();
        if (_importJobRunning) what.Add(RestartWords.Import);
        foreach (string w in SlotBusy.Values)
        {
            if (!what.Contains(w)) what.Add(w);
        }
        if (_indexingBusy) what.Add(RestartWords.Indexing);
        if (_selfTestBusy.Length > 0 && !what.Contains(_selfTestBusy)) what.Add(_selfTestBusy);
        if (AddonSlots.Any(s => s.Phase is not null) || PlacesChannel.Phase is not null ||
            DriftwireSlot.Phase is not null || SlotChannels.Values.Any(s => s.Phase is not null))
        {
            what.Add(RestartWords.Download);
        }
        return what;
    }

    /// <summary>
    /// Restart now (the card, the bar, Settings): at once when nothing is
    /// running; otherwise the card says what it waits for and restarts when that
    /// has finished. Native still refuses under a playing clip (a beep).
    /// </summary>
    private static void RestartWhenIdle()
    {
        List<string> blockers = RestartBlockers();
        if (blockers.Count == 0)
        {
            StopRestartWait();
            Send(Command.UpdateRestart, 2);
            return;
        }
        _restartWaitingFor = blockers;
        ShowRestartNotice();  // renders the waiting card, wherever the click came from
        if (_dispatcher is null) return;
        _restartWaitTimer ??= _dispatcher.DispatcherQueue.CreateTimer();
        _restartWaitTimer.Interval = TimeSpan.FromSeconds(1);
        _restartWaitTimer.IsRepeating = true;
        _restartWaitTimer.Tick -= OnRestartWaitTick;
        _restartWaitTimer.Tick += OnRestartWaitTick;
        _restartWaitTimer.Start();
    }

    private static void OnRestartWaitTick(DispatcherQueueTimer sender, object args) => Guarded(() =>
    {
        if (!RestartWaiting)
        {
            StopRestartWait();
            return;
        }
        List<string> blockers = RestartBlockers();
        if (blockers.Count == 0)
        {
            StopRestartWait();
            RenderRestartNotice();
            Send(Command.UpdateRestart, 2);
            return;
        }
        if (blockers.SequenceEqual(_restartWaitingFor)) return;
        _restartWaitingFor = blockers;
        RenderRestartNotice();
        PlaceCornerCards();
    });

    private static void StopRestartWait()
    {
        _restartWaitTimer?.Stop();
        _restartWaitingFor = new List<string>();
    }

    private static void CancelRestartWait()
    {
        StopRestartWait();
        RenderRestartNotice();
        PlaceCornerCards();
    }

    private static void RestartAnyway()
    {
        StopRestartWait();
        RenderRestartNotice();
        PlaceCornerCards();
        Send(Command.UpdateRestart, 2);
    }

    // ---- what is running ----------------------------------------------------------

    /// <summary>IAddonHost.SetStatus for Import: a line is up while a job runs.</summary>
    private static void NoteImportJob(bool running) => _importJobRunning = running;

    /// <summary>HostSetIndexingPill: the pill is busy while the pack indexes.</summary>
    private static void NoteIndexingBusy(bool busy) => _indexingBusy = busy;

    /// <summary>
    /// ReadSlotNotices: what each add-on said it is doing (busy_post). An update
    /// that waited on one is reloaded once it is done.
    /// </summary>
    private static void NoteSlotBusy(Dictionary<string, string> now)
    {
        if (now.Count == SlotBusy.Count && now.All(kv => SlotBusy.TryGetValue(kv.Key, out string? w) && w == kv.Value)) return;
        SlotBusy.Clear();
        foreach ((string id, string what) in now) SlotBusy[id] = what;
        bool changed = false;
        foreach ((string id, AddonUpdated u) in PendingSlotReloads.Where(p => !SlotBusy.ContainsKey(p.Key)).ToList())
        {
            PendingSlotReloads.Remove(id);
            ReloadSlotAddonNow(id);
            if (id == PlacesChannel.Id) _placesMessage = $"{u.Name} updated to {u.Version}.";
            else SlotUpdateMessages[id] = $"{u.Name} updated to {u.Version}.";
            changed = true;
        }
        if (changed) RefreshSlotSettings();
    }

    /// <summary>
    /// Attach: the card's verify without publishing an update (screenshots, the
    /// present-loop soak with the card up). MV_ADDON_RESTART_SELFTEST=
    /// "Import=1.3.0,Places=1.1.0" shows it for those; ";busy=a Driftwire
    /// transfer" makes something run, and ";wait" presses Restart now, which
    /// then waits (nothing restarts while the made-up work runs).
    /// </summary>
    private static void RestartNoticeSelfTest()
    {
        string? spec = Environment.GetEnvironmentVariable("MV_ADDON_RESTART_SELFTEST");
        if (string.IsNullOrWhiteSpace(spec) || _addonRestartFor.Count > 0) return;
        // Once: the MediaViewer a Restart now starts again does not inherit it.
        Environment.SetEnvironmentVariable("MV_ADDON_RESTART_SELFTEST", null);
        string[] parts = spec.Split(';', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        foreach (string pair in parts[0].Split(',', StringSplitOptions.TrimEntries))
        {
            string[] kv = pair.Split('=', 2, StringSplitOptions.TrimEntries);
            if (kv.Length == 2 && kv[0].Length > 0 && kv[1].Length > 0) AddonRestartNeeded(kv[0], kv[1]);
        }
        foreach (string p in parts.Skip(1))
        {
            if (p.StartsWith("busy=", StringComparison.Ordinal)) _selfTestBusy = p[5..];
        }
        if (parts.Contains("wait")) RestartWhenIdle();
    }

    // ---- slot add-ons reload live ---------------------------------------------------

    /// <summary>
    /// UI thread, a loaded slot add-on's update has installed: it reloads now
    /// (unload, then the newest verified version loads), with no restart. While
    /// it reports work under way (busy_post: a Driftwire transfer) it reloads
    /// once that has ended. Returns Settings' line for its card.
    /// </summary>
    private static string ReloadUpdatedSlotAddon(string id, string name, string version)
    {
        if (SlotBusy.TryGetValue(id, out string? what))
        {
            PendingSlotReloads[id] = new AddonUpdated(name, version);
            return $"{name} {version} is installed. It takes over when {what} has finished.";
        }
        ReloadSlotAddonNow(id);
        return $"{name} updated to {version}.";
    }

    private static void ReloadSlotAddonNow(string id)
    {
        if (_folderSession is not { } session) return;
        SlotAddonsLoaded.Remove(id);
        // Off the UI thread, as Remove: unloading detaches it, which waits for
        // a call it is in. Its items, windows and commands go, then come back
        // from the new version.
        _ = Task.Run(() =>
        {
            try { AddonNative.Unload(id); }
            catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            StartSlotAddons(session);
        });
    }
}
