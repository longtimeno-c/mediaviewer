// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text;
using MediaViewer.Shared;
using MediaViewer.Updater;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 8 in-app updater chrome (docs/design/13 Part 1): a quiet "Update ready —
/// restart" button in the command bar and two Settings rows (automatic checks,
/// update channel). All network and
/// disk work is on <see cref="UpdateService"/>'s own low-priority thread; this
/// file only renders its status and forwards the click to native, which checks
/// that no clip is playing and builds the restart arguments.
/// </summary>
/// <remarks>
/// Not a command-table row (docs/design/16): no key, nothing in <c>?</c>. The id
/// <see cref="Command.UpdateRestart"/> is an island notification like Popup.
/// </remarks>
public static partial class IslandHost
{
    private static UpdateService? _updater;
    private static Button? _updateButton;
    // A JobBar, not a ProgressBar: WinUI's has no default style in this island
    // app, and this one sits in the bar, so every launch fail-fasted (0.1.14-0.1.18).
    private static JobBar? _updateProgress;
    // Checking / downloading are status, not an action: smaller than the bar's
    // buttons. "Update ready — restart" keeps the bar's size.
    private const double UpdateStatusFontSize = 12;
    private static ToggleSwitch? _autoUpdate;
    // Issue #220: Settings' "Check now" and what the check it asked for found.
    // 0 none, 1 asked, 2 that check is running.
    private static Button? _checkNow;
    private static TextBlock? _checkNowNote;
    private static int _manualCheck;
    private static ComboBox? _updateChannel;
    // Set when Settings sends a channel change; the check runs once native has
    // stored it and pushed the flags back (ApplySettings), not before.
    private static bool _channelChangePending;
    // The updater's last status, so an add-on restart arriving between two
    // status changes can redraw the bar button.
    private static UpdateStatus? _lastUpdateStatus;
    // Add-on updates that installed beside a running copy and take over only
    // at the next start (Import 0.2.0, Local search 0.3.1). While any is
    // here, the corner card (IslandHost.RestartNotice.cs) and the bar offer a
    // restart, and Settings → Add-ons says "Restart to finish updating".
    private static readonly List<AddonUpdated> _addonRestartFor = new();
    // The bar button's Tag while it shows the add-on updates notice (#525).
    private const int AddonUpdatesArg = 3;
    // #532 (docs/design/13 "Staging and restart"): an app update never cuts a
    // transfer off. A restart waits, the bar saying so, until no add-on reports
    // work under way (busy_post: "a Driftwire transfer") and every other
    // MediaViewer has quit cleanly (Update.exe kills whatever still runs from
    // the install). Only then is Update.exe armed. Non-null while one waits.
    private static string[]? _updateRestartArgs;
    // That restart is the background's own (UpdaterState): it is dropped if the
    // window comes back, so nothing restarts under the person.
    private static bool _updateRestartInBackground;
    // It had to wait: by the time it can go, a clip may play or the view has
    // moved on, so native is asked again (its checks, the view as it is now).
    private static bool _updateRestartWaited;
    // What it waits for, for the bar; empty when it is about to arm.
    private static List<string> _updateWaitingFor = new();
    private static DispatcherQueueTimer? _updateWaitTimer;
    // Native: the viewer window is hidden while an add-on keeps the process.
    private static bool _updaterInBackground;
    // Another MediaViewer is updating: this one quits once idle, applying nothing.
    private static bool _quitForUpdate;
    private static readonly HashSet<uint> UpdateQuitAsked = new();
    private static Button? _importRestartButton;
    private static Button? _localSearchRestartButton;
    private static FrameworkElement? _addonsRestartRow;
    private static TextBlock? _addonsRestartText;

    private static void StartUpdater()
    {
        if (_updater is not null) return;
        _updater = new UpdateService(() => HasFlag(SettingFlag.UpdateAutoCheck),
            () => HasFlag(SettingFlag.UpdatePreview), UpdaterLog);
        _updater.StatusChanged += s =>
        {
            DispatcherQueueControllerTryEnqueue(() =>
            {
                ShowUpdateStatus(s);
                MaybeUpdateInBackground();  // #532: staged while nobody looks
            });
        };
        _updater.Start();
    }

    private static void DispatcherQueueControllerTryEnqueue(Action action)
    {
        // The catch inside the callback matters more than the one outside: an
        // exception escaping a dispatcher callback takes the process down.
        try
        {
            _dispatcher?.DispatcherQueue.TryEnqueue(() =>
            {
                try { action(); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            });
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    // Messages are fixed strings plus versions and exception type names only.
    private static void UpdaterLog(string message)
    {
        System.Diagnostics.Debug.WriteLine(message);
        try { Console.Error.WriteLine(message); } catch (IOException) { }
    }

    private static StackPanel BuildUpdateButton()
    {
        // Tag 2: the button is offering an add-on restart (RequestAddonRestart).
        // Tag 3: it is #525's "Add-on update available", which opens Settings → Add-ons.
        _updateButton = TextButton("Update ready — restart", () =>
        {
            int arg = _updateButton?.Tag is int t ? t : 0;
            if (arg == AddonUpdatesArg) OpenAddonUpdates();
            else Send(Command.UpdateRestart, arg);
        });
        _updateButton.Visibility = Visibility.Collapsed;
        // Not clickable, it is a status line (the Mac's plain Body text), not a
        // dimmed button: undo FlattenButton's disabled dim, which runs first.
        Button status = _updateButton;
        void Undim() { if (!status.IsEnabled) status.Opacity = 1; }
        status.IsEnabledChanged += (_, _) => Undim();
        status.Loaded += (_, _) => Undim();
        ToolTipService.SetToolTip(_updateButton,
            "A new version is downloaded. Restart to use it, or it installs when you close MediaViewer.");
        _updateProgress = new JobBar { Visibility = Visibility.Collapsed };
        _updateProgress.Root.Width = 90;
        _updateProgress.Root.VerticalAlignment = VerticalAlignment.Center;
        _updateProgress.Root.Margin = Pad(0, 0, 10, 0);
        AutomationProperties.SetName(_updateProgress.Root, "Update download progress");
        var group = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        group.Children.Add(_updateButton);
        group.Children.Add(_updateProgress.Root);
        return group;
    }

    /// <summary>
    /// An add-on update installed beside a running copy: it takes over at the
    /// next start, so the bar and Settings offer a restart (owner, 2026-10-05:
    /// "prompt an app restart"). Never a forced restart (docs/design/13).
    /// </summary>
    private static void AddonRestartNeeded(string name, string version)
    {
        var what = new AddonUpdated(name, version);
        // A second update of the same add-on replaces its line: one card, one line each.
        _addonRestartFor.RemoveAll(u => u.Name == name);
        _addonRestartFor.Add(what);
        RefreshRestartPrompts();
        RefreshAddonUpdates();  // #525: no longer "available", it waits on the restart
        ShowRestartNotice();  // #510: the corner card
    }

    private static string AddonRestartText() => RestartWords.Tooltip(_addonRestartFor);

    /// <summary>A Settings row's "Restart now", hidden until an add-on update waits on a restart.</summary>
    private static Button AddonRestartButton()
    {
        Button b = SettingsButton("Restart now", RequestAddonRestart);
        b.HorizontalAlignment = HorizontalAlignment.Left;
        b.Visibility = _addonRestartFor.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        return b;
    }

    /// <summary>
    /// Native checks no clip is playing, then restarts onto the same view:
    /// through Update.exe when an app update is staged too (it applies both),
    /// else by starting MediaViewer again once this process has exited.
    /// </summary>
    private static void RequestAddonRestart() => RestartWhenIdle();

    /// <summary>
    /// #510: Settings → Add-ons' quiet marker while an update waits on a
    /// restart (the card's Later leaves it): "Restart to finish updating" with
    /// the add-ons and their versions, and Restart now. Hidden otherwise.
    /// </summary>
    private static FrameworkElement BuildAddonsRestartRow()
    {
        _addonsRestartText = Note(RestartWords.Marker(_addonRestartFor));
        _addonsRestartText.Foreground = Brush(Title);
        _addonsRestartText.VerticalAlignment = VerticalAlignment.Center;
        Button restart = SettingsButton("Restart now", RequestAddonRestart, accent: true);
        ToolTipService.SetToolTip(restart, "MediaViewer opens again on this folder and file. Anything still running finishes first.");
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(12) };
        row.Children.Add(_addonsRestartText);
        row.Children.Add(restart);
        row.Visibility = _addonRestartFor.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        _addonsRestartRow = row;
        return row;
    }

    private static void RefreshRestartPrompts()
    {
        Visibility v = _addonRestartFor.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (_importRestartButton is not null) _importRestartButton.Visibility = v;
        if (_localSearchRestartButton is not null) _localSearchRestartButton.Visibility = v;
        if (_addonsRestartRow is not null) _addonsRestartRow.Visibility = v;
        if (_addonsRestartText is not null) _addonsRestartText.Text = RestartWords.Marker(_addonRestartFor);
        RefreshAddonList();  // #510: "Restart to finish updating" on the rows
        ShowUpdateStatus(_lastUpdateStatus ?? _updater?.Status
                         ?? new UpdateStatus(UpdatePhase.Inert, null, UpdateUrgency.Normal, null));
    }

    private static void ShowCheckNowNote(UpdateStatus s)
    {
        if (_checkNow is null || _checkNowNote is null) return;
        if (_manualCheck == 1 && s.Phase == UpdatePhase.Checking) _manualCheck = 2;
        bool busy = _manualCheck == 1 || s.Phase is UpdatePhase.Checking or UpdatePhase.Downloading;
        _checkNow.IsEnabled = s.Phase != UpdatePhase.Inert && !busy;
        string? note = s.Phase switch
        {
            UpdatePhase.Inert => "Updates come with the installed app",
            UpdatePhase.Checking => "Checking…",
            UpdatePhase.Downloading => "Downloading…",
            UpdatePhase.Ready => "Update ready — restart from the bar",
            _ when _manualCheck == 1 => "Checking…",
            // The answer to the check Settings asked for; any other change
            // leaves the last answer up.
            UpdatePhase.Failed when _manualCheck == 2 => "The update could not be downloaded",
            _ when _manualCheck == 2 => _updater?.LastCheckFailed == true
                ? "Could not reach GitHub"
                : "MediaViewer is up to date",
            _ => null,
        };
        if (note is not null) _checkNowNote.Text = note;
        if (_manualCheck == 2 && s.Phase is not (UpdatePhase.Checking or UpdatePhase.Downloading))
            _manualCheck = 0;
    }

    private static void ShowUpdateStatus(UpdateStatus s)
    {
        _lastUpdateStatus = s;
        ShowCheckNowNote(s);
        if (_updateButton is null) return;
        try
        {
            string? text = null;
            string tip = "";
            bool clickable = false;
            int restartArg = 0;
            if (_updateWaitingFor.Count > 0 && (_updateRestartArgs is not null || _quitForUpdate))
            {
                // #532: quiet status, nothing to click; it restarts by itself.
                text = RestartWords.UpdateWaiting;
                tip = RestartWords.UpdateWaitingTip(_updateWaitingFor);
            }
            else if (s.Phase == UpdatePhase.Ready)
            {
                text = s.Urgency == UpdateUrgency.Normal ? "Update ready — restart" : "Important update — restart";
                tip = s.Urgency switch
                {
                    UpdateUrgency.CurrentBlocklisted =>
                        $"This version was withdrawn. Version {s.Version} is downloaded; restart to use it.",
                    UpdateUrgency.BelowMinimum =>
                        $"Version {s.Version} is downloaded and fixes a problem in this version. Restart to use it.",
                    _ => $"Version {s.Version} is downloaded. Restart to use it, or it installs when you close MediaViewer.",
                };
                // The restart loads the newest add-ons too.
                if (_addonRestartFor.Count > 0) tip += " " + AddonRestartText();
                clickable = true;
            }
            else if (_addonRestartFor.Count > 0)
            {
                // An add-on update waits on a restart: an action, so it wins
                // over a quiet check or download of the app.
                text = "Add-on updated — restart";
                tip = AddonRestartText();
                clickable = true;
                restartArg = 2;
            }
            // Checking/Downloading are quiet, non-clickable states: same command-bar
            // spot, no popup, nothing to click yet (docs/design/13 "never interrupt").
            // Without these the button just stays hidden for the whole check +
            // download, which reads as "nothing is happening" — most noticeable
            // right after switching the update channel in Settings.
            else if (s.Phase == UpdatePhase.Checking)
            {
                text = "Checking for updates…";
                tip = "Asking GitHub for a newer version.";
            }
            else if (s.Phase == UpdatePhase.Downloading)
            {
                text = s.Version is null ? "Downloading update…" : $"Downloading update {s.Version}…";
                tip = "Downloading in the background at low priority. This does not interrupt anything.";
            }
            if (text is null && s.RolledBackFrom is not null)
            {
                // Reported once, quietly; clicking restarts nothing (Phase is not Ready).
                text = "Update undone";
                tip = $"Version {s.RolledBackFrom} did not start, so the previous version was restored.";
            }
            if (text is null && _addonUpdates.Count > 0)
            {
                // #525: the quietest of them, and only when nothing else is said.
                text = InstallWords.UpdatesBadge(_addonUpdates.Count);
                tip = InstallWords.UpdatesTip(_addonUpdates);
                clickable = true;
                restartArg = AddonUpdatesArg;
            }
            bool downloading = text is not null && s.Phase == UpdatePhase.Downloading;
            if (_updateProgress is not null)
            {
                // Determinate once Velopack has reported a percent.
                _updateProgress.IsIndeterminate = s.Percent is null;
                _updateProgress.Value = (s.Percent ?? 0) / 100.0;
                _updateProgress.Visibility = downloading ? Visibility.Visible : Visibility.Collapsed;
            }
            if (text is null)
            {
                _updateButton.Visibility = Visibility.Collapsed;
                return;
            }
            SetButtonText(_updateButton, text);
            if (_updateButton.Content is TextBlock label)
            {
                label.FontSize = clickable ? UiFontSize : UpdateStatusFontSize;
                // The add-on updates notice is clickable but quiet: body colour.
                label.Foreground = Brush(clickable && restartArg != AddonUpdatesArg ? Title : Body);
            }
            ToolTipService.SetToolTip(_updateButton, tip);
            _updateButton.IsEnabled = clickable;
            _updateButton.Tag = restartArg;
            _updateButton.Visibility = Visibility.Visible;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static void AddUpdateSettingsRow(StackPanel view)
    {
        _autoUpdate = SettingsToggle("Check for updates automatically", SettingFlag.UpdateAutoCheck);
        _autoUpdate.Toggled += (_, _) =>
        {
            if (!_updatingSettingsUi && _autoUpdate.IsOn) _updater?.Poke();
        };
        ToolTipService.SetToolTip(_autoUpdate,
            "Asks GitHub for a newer version at launch and every 6 hours. The request reveals your IP address and app version, nothing about your files.");
        view.Children.Add(SettingsRow("Automatic update checks", "Ask GitHub for new versions at launch and every six hours.", _autoUpdate));

        // Issue #220: the Mac's "Check for Updates…". One check, now, whether
        // or not automatic checks are on; the bar shows it as any check.
        _checkNow = SettingsButton("Check now", () =>
        {
            if (_updater is null) return;
            _manualCheck = 1;
            _updater.CheckNow();
            ShowCheckNowNote(_lastUpdateStatus ?? _updater.Status);
        });
        ToolTipService.SetToolTip(_checkNow,
            "Asks GitHub for a newer version now. The request reveals your IP address and app version, nothing about your files.");
        _checkNowNote = Label("");
        _checkNowNote.FontSize = 12;
        _checkNowNote.VerticalAlignment = VerticalAlignment.Center;
        var checkNow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(12) };
        checkNow.Children.Add(_checkNowNote);
        checkNow.Children.Add(_checkNow);
        ShowCheckNowNote(_lastUpdateStatus ?? _updater?.Status
                         ?? new UpdateStatus(UpdatePhase.Inert, null, UpdateUrgency.Normal, null));
        view.Children.Add(SettingsRow("Check for updates", "Ask GitHub for a new version now.", checkNow));

        _updateChannel = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Width = 180,
        };
        _updateChannel.Items.Add("Stable");
        _updateChannel.Items.Add("Preview");
        _updateChannel.SelectedIndex = HasFlag(SettingFlag.UpdatePreview) ? 1 : 0;
        AutomationProperties.SetName(_updateChannel, "Update channel");
        _updateChannel.SelectionChanged += (_, _) =>
        {
            if (_updatingSettingsUi || _updateChannel.SelectedIndex < 0) return;
            _channelChangePending = true;
            SetFlag(SettingFlag.UpdatePreview, _updateChannel.SelectedIndex == 1);
        };
        view.Children.Add(SettingsRow("Update channel",
            "Preview gets signed test builds before they become stable. Switching back to Stable keeps this version until a newer stable one is released.",
            _updateChannel));
    }

    private static void RefreshUpdateSettingsRow()
    {
        if (_autoUpdate is not null) _autoUpdate.IsOn = HasFlag(SettingFlag.UpdateAutoCheck);
        if (_updateChannel is not null) _updateChannel.SelectedIndex = HasFlag(SettingFlag.UpdatePreview) ? 1 : 0;
    }

    /// <summary>Native pushed the flags: a channel change from Settings is stored, so check now.</summary>
    private static void UpdateChannelApplied()
    {
        if (!_channelChangePending) return;
        _channelChangePending = false;
        _updater?.Poke();
    }

    /// <summary>
    /// Native, after it checked no clip is playing. In: NUL-separated UTF-16
    /// restart arguments. Returns 0 when the apply is being armed (then posts
    /// UpdateRestart(1) so native closes), 1 when nothing is staged. #532: the
    /// arming waits until no transfer runs and the other MediaViewers have quit.
    /// </summary>
    public static int UpdateRestart(IntPtr arg, int sizeBytes)
    {
        try
        {
            UpdateService? updater = _updater;
            if (updater is null || updater.Status.Phase != UpdatePhase.Ready) return 1;
            string[] args = arg == IntPtr.Zero || sizeBytes < 2
                ? Array.Empty<string>()
                : (Marshal.PtrToStringUni(arg, sizeBytes / 2) ?? "").Split('\0', StringSplitOptions.RemoveEmptyEntries);
            if (_quitForUpdate) return 0;  // another MediaViewer applies it; this one is quitting
            BeginUpdateRestart(args, inBackground: false);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>Native exit path, window gone: apply a staged update after exit, no restart.</summary>
    public static int UpdaterExit(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            UpdateService? updater = _updater;
            if (updater is null) return 0;
            updater.Stop();
            // #532: quitting for another MediaViewer's update, which applies it:
            // a second Update.exe from here would kill that one.
            if (!_quitForUpdate) updater.ApplyOnExit();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ---- #532: never under a transfer -------------------------------------------

    /// <summary>
    /// Native (src/shell/chrome_host.h updater_state), two int32: the state (0
    /// the window is on screen, 1 it is hidden while an add-on keeps the process
    /// (#452), 2 another MediaViewer is applying an update and asks this one to
    /// quit) and, for 2, that process's pid.
    /// </summary>
    public static int UpdaterState(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            int state = Marshal.ReadInt32(arg);
            uint from = unchecked((uint)Marshal.ReadInt32(arg, 4));
            switch (state)
            {
                case 0:
                    _updaterInBackground = false;
                    // The person is back: their bar offers the restart again.
                    if (_updateRestartInBackground) StopUpdateWait();
                    break;
                case 1:
                    _updaterInBackground = true;
                    MaybeUpdateInBackground();
                    break;
                case 2:
                    // Both asked at once (each restart clicked in its own window):
                    // the lower pid applies, the other quits for it.
                    if (_updateRestartArgs is not null && UpdateQuitAsked.Count > 0 && Environment.ProcessId < from) break;
                    UpdaterLog("updater: another MediaViewer is updating; quitting once idle");
                    _quitForUpdate = true;
                    _updateRestartArgs = null;  // its update restarts the app, not this one
                    UpdateWaitTick();
                    break;
                default:
                    return unchecked((int)0x80070057);
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>
    /// Hidden in the background with an update staged and no add-on window open:
    /// restart into the background (--background) to install it, so a machine
    /// that only receives still gets updates. Waits on a transfer like any restart.
    /// </summary>
    private static void MaybeUpdateInBackground()
    {
        if (!_updaterInBackground || _quitForUpdate || _updateRestartArgs is not null) return;
        if (_updater?.Status.Phase != UpdatePhase.Ready) return;
        // A sign-in start no add-on has kept yet (it may quit at the grace).
        if (!_noticeKeeping) return;
        if (ScreenWindows.Count > 0) return;  // the person is in Driftwire's window; its close asks again
        UpdaterLog("updater: update staged while in the background; restarting into the background");
        BeginUpdateRestart(new[] { "--background" }, inBackground: true);
    }

    private static void BeginUpdateRestart(string[] args, bool inBackground)
    {
        _updateRestartArgs = args;
        _updateRestartInBackground = inBackground;
        _updateRestartWaited = false;
        UpdateWaitTick();
    }

    /// <summary>What an app update's restart waits for: what add-ons say they are doing.</summary>
    private static List<string> UpdateBlockers()
    {
        var what = new List<string>();
        foreach (string w in SlotBusy.Values)
        {
            if (!what.Contains(w)) what.Add(w);
        }
        return what;
    }

    // UI thread: at once, then each second while something runs. The timer runs
    // only while a restart waits; an idle viewer has none.
    private static void UpdateWaitTick()
    {
        if (_updateRestartArgs is null && !_quitForUpdate)
        {
            StopUpdateWait();
            return;
        }
        List<string> waiting = UpdateBlockers();
        // The others are asked only once this process is idle, so a person's
        // other windows do not close while this one still waits on its own transfer.
        if (waiting.Count == 0 && !_quitForUpdate && AskOtherViewersToQuit() > 0) waiting.Add(RestartWords.OtherWindow);
        if (waiting.Count > 0)
        {
            _updateRestartWaited = true;
            if (!waiting.SequenceEqual(_updateWaitingFor))
            {
                _updateWaitingFor = waiting;
                UpdaterLog("updater: restart waits on running work");
                ShowUpdateStatus(_lastUpdateStatus ?? _updater?.Status
                                 ?? new UpdateStatus(UpdatePhase.Inert, null, UpdateUrgency.Normal, null));
            }
            if (_dispatcher is null) return;
            if (_updateWaitTimer is null)
            {
                _updateWaitTimer = _dispatcher.DispatcherQueue.CreateTimer();
                _updateWaitTimer.Interval = TimeSpan.FromSeconds(1);
                _updateWaitTimer.IsRepeating = true;
                _updateWaitTimer.Tick += (_, _) => Guarded(UpdateWaitTick);
            }
            if (!_updateWaitTimer.IsRunning) _updateWaitTimer.Start();
            return;
        }
        if (_quitForUpdate)
        {
            StopUpdateWait();
            // The armed-update close (native chrome_cmd_update_restart 1): quit
            // for real, never kept in the background.
            Send(Command.UpdateRestart, 1);
            return;
        }
        string[] args = _updateRestartArgs!;
        bool askAgain = _updateRestartWaited && !_updateRestartInBackground;
        UpdateService? updater = _updater;
        StopUpdateWait();
        if (updater is null) return;
        if (askAgain)
        {
            // Native refuses under a playing clip (a beep: the bar offers the
            // restart again) and hands back the arguments for the view now.
            Send(Command.UpdateRestart, 0);
            return;
        }
        // Starting Update.exe is a process launch: not on the UI thread.
        _ = Task.Run(() =>
        {
            bool armed = updater.RequestRestart(args);
            DispatcherQueueControllerTryEnqueue(() => { if (armed) Send(Command.UpdateRestart, 1); });
        });
    }

    private static void StopUpdateWait()
    {
        _updateWaitTimer?.Stop();
        bool shown = _updateWaitingFor.Count > 0;
        _updateRestartArgs = null;
        _updateRestartInBackground = false;
        _updateRestartWaited = false;
        _updateWaitingFor = new List<string>();
        if (shown)
        {
            ShowUpdateStatus(_lastUpdateStatus ?? _updater?.Status
                             ?? new UpdateStatus(UpdatePhase.Inert, null, UpdateUrgency.Normal, null));
        }
    }

    // src/shell/main.cpp kWindowClass and kMsgUpdateQuit.
    private const string ViewerWindowClass = "MediaViewer.PresentLab";
    private const uint MsgUpdateQuit = 0x8000 + 0x82;

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lparam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")]
    private static extern int UpdateGetClassName(IntPtr hwnd, StringBuilder name, int max);

    [DllImport("user32.dll", EntryPoint = "GetWindowThreadProcessId")]
    private static extern uint UpdateWindowProcess(IntPtr hwnd, out uint pid);

    [DllImport("kernel32.dll", EntryPoint = "OpenProcess")]
    private static extern IntPtr UpdateOpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "QueryFullProcessImageNameW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateProcessImage(IntPtr process, uint flags, StringBuilder name, ref int size);

    [DllImport("kernel32.dll", EntryPoint = "CloseHandle")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateCloseHandle(IntPtr handle);

    // pid -> it runs from this install (what Update.exe would kill); asked once per pid.
    private static readonly Dictionary<uint, bool> UpdateSameInstall = new();

    private static bool RunsFromThisInstall(uint pid)
    {
        if (UpdateSameInstall.TryGetValue(pid, out bool known)) return known;
        bool same = false;
        string? current = Path.GetDirectoryName(Environment.ProcessPath);
        string? root = current is null ? null : Path.GetDirectoryName(current);
        const uint QueryLimitedInformation = 0x1000;
        IntPtr process = root is null ? IntPtr.Zero : UpdateOpenProcess(QueryLimitedInformation, false, pid);
        if (process != IntPtr.Zero)
        {
            var image = new StringBuilder(1024);
            int size = image.Capacity;
            if (UpdateProcessImage(process, 0, image, ref size))
            {
                same = image.ToString(0, size).StartsWith(root + Path.DirectorySeparatorChar,
                                                          StringComparison.OrdinalIgnoreCase);
            }
            UpdateCloseHandle(process);
        }
        UpdateSameInstall[pid] = same;
        return same;
    }

    /// <summary>
    /// Every other MediaViewer (each window is its own process, the background
    /// one included) is asked once to quit cleanly; each quits when its own
    /// transfers are done. Returns how many are still running. A window
    /// enumeration, once a second and only while an update waits.
    /// </summary>
    private static int AskOtherViewersToQuit()
    {
        uint self = (uint)Environment.ProcessId;
        int running = 0;
        var name = new StringBuilder(64);
        EnumWindows((hwnd, _) =>
        {
            name.Clear();
            if (UpdateGetClassName(hwnd, name, name.Capacity) <= 0 || name.ToString() != ViewerWindowClass) return true;
            if (UpdateWindowProcess(hwnd, out uint pid) == 0 || pid == self) return true;
            if (!RunsFromThisInstall(pid)) return true;  // a dev build beside it: not Update.exe's
            ++running;
            if (UpdateQuitAsked.Add(pid)) NoticePostMessage(hwnd, MsgUpdateQuit, (IntPtr)self, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        if (running > 0) UpdaterLog($"updater: waiting for {running} other MediaViewer process(es) to quit");
        return running;
    }
}
