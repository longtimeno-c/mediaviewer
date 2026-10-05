// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Updater;
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
    private static ComboBox? _updateChannel;
    // Set when Settings sends a channel change; the check runs once native has
    // stored it and pushed the flags back (ApplySettings), not before.
    private static bool _channelChangePending;
    // The updater's last status, so an add-on restart arriving between two
    // status changes can redraw the bar button.
    private static UpdateStatus? _lastUpdateStatus;
    // Add-on updates that installed beside a running copy and take over only
    // at the next start ("Import 0.2.0", "Local search 0.3.1"). While any is
    // here, the bar offers a restart and each Settings row a Restart now.
    private static readonly List<string> _addonRestartFor = new();
    private static Button? _importRestartButton;
    private static Button? _localSearchRestartButton;

    private static void StartUpdater()
    {
        if (_updater is not null) return;
        _updater = new UpdateService(() => HasFlag(SettingFlag.UpdateAutoCheck),
            () => HasFlag(SettingFlag.UpdatePreview), UpdaterLog);
        _updater.StatusChanged += s =>
        {
            DispatcherQueueControllerTryEnqueue(() => ShowUpdateStatus(s));
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
        _updateButton = TextButton("Update ready — restart",
            () => Send(Command.UpdateRestart, _updateButton?.Tag is int arg ? arg : 0));
        _updateButton.Visibility = Visibility.Collapsed;
        ToolTipService.SetToolTip(_updateButton,
            "A new version is downloaded. Restart to use it, or it installs when you close MediaViewer.");
        _updateProgress = new JobBar { Visibility = Visibility.Collapsed };
        _updateProgress.Root.Width = 90;
        _updateProgress.Root.VerticalAlignment = VerticalAlignment.Center;
        _updateProgress.Root.Margin = new Thickness(0, 0, 10, 0);
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
    private static void AddonRestartNeeded(string what)
    {
        if (!_addonRestartFor.Contains(what)) _addonRestartFor.Add(what);
        RefreshRestartPrompts();
    }

    private static string AddonRestartText() =>
        $"{string.Join(" and ", _addonRestartFor)} {(_addonRestartFor.Count == 1 ? "is" : "are")} installed. Restart MediaViewer to use {(_addonRestartFor.Count == 1 ? "it" : "them")}.";

    /// <summary>A Settings row's "Restart now", hidden until an add-on update waits on a restart.</summary>
    private static Button AddonRestartButton()
    {
        Button b = SettingsButton("Restart now", RequestAddonRestart);
        b.Visibility = _addonRestartFor.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        return b;
    }

    /// <summary>
    /// Native checks no clip is playing, then restarts onto the same view:
    /// through Update.exe when an app update is staged too (it applies both),
    /// else by starting MediaViewer again once this process has exited.
    /// </summary>
    private static void RequestAddonRestart() => Send(Command.UpdateRestart, 2);

    private static void RefreshRestartPrompts()
    {
        Visibility v = _addonRestartFor.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (_importRestartButton is not null) _importRestartButton.Visibility = v;
        if (_localSearchRestartButton is not null) _localSearchRestartButton.Visibility = v;
        ShowUpdateStatus(_lastUpdateStatus ?? _updater?.Status
                         ?? new UpdateStatus(UpdatePhase.Inert, null, UpdateUrgency.Normal, null));
    }

    private static void ShowUpdateStatus(UpdateStatus s)
    {
        _lastUpdateStatus = s;
        if (_updateButton is null) return;
        try
        {
            string? text = null;
            string tip = "";
            bool clickable = false;
            int restartArg = 0;
            if (s.Phase == UpdatePhase.Ready)
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
                label.FontSize = clickable ? UiFontSize : UpdateStatusFontSize;
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
    /// UpdateRestart(1) so native closes), 1 when nothing is staged.
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
            // Starting Update.exe is a process launch: not on the UI thread.
            _ = Task.Run(() =>
            {
                bool armed = updater.RequestRestart(args);
                DispatcherQueueControllerTryEnqueue(() => { if (armed) Send(Command.UpdateRestart, 1); });
            });
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
            updater.ApplyOnExit();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }
}

