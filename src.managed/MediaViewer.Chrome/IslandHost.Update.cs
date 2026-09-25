// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Updater;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 8 in-app updater chrome (plan/13 Part 1): a quiet "Update ready —
/// restart" button in the command bar and one Settings row. All network and
/// disk work is on <see cref="UpdateService"/>'s own low-priority thread; this
/// file only renders its status and forwards the click to native, which checks
/// that no clip is playing and builds the restart arguments.
/// </summary>
/// <remarks>
/// Not a command-table row (plan/16): no key, nothing in <c>?</c>. The id
/// <see cref="Command.UpdateRestart"/> is an island notification like Popup.
/// </remarks>
public static partial class IslandHost
{
    private static UpdateService? _updater;
    private static Button? _updateButton;
    private static ToggleSwitch? _autoUpdate;

    private static void StartUpdater()
    {
        if (_updater is not null) return;
        _updater = new UpdateService(() => HasFlag(SettingFlag.UpdateAutoCheck), UpdaterLog);
        _updater.StatusChanged += s =>
        {
            DispatcherQueueControllerTryEnqueue(() => ShowUpdateStatus(s));
        };
        _updater.Start();
    }

    private static void DispatcherQueueControllerTryEnqueue(Action action)
    {
        try { _dispatcher?.DispatcherQueue.TryEnqueue(() => action()); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    // Messages are fixed strings plus versions and exception type names only.
    private static void UpdaterLog(string message)
    {
        System.Diagnostics.Debug.WriteLine(message);
        try { Console.Error.WriteLine(message); } catch (IOException) { }
    }

    private static Button BuildUpdateButton()
    {
        _updateButton = TextButton("Update ready — restart", () => Send(Command.UpdateRestart, 0));
        _updateButton.Visibility = Visibility.Collapsed;
        ToolTipService.SetToolTip(_updateButton,
            "A new version is downloaded. Restart to use it, or it installs when you close MediaViewer.");
        return _updateButton;
    }

    private static void ShowUpdateStatus(UpdateStatus s)
    {
        if (_updateButton is null) return;
        try
        {
            string? text = null;
            string tip = "";
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
            }
            if (text is null && s.RolledBackFrom is not null)
            {
                // Reported once, quietly; clicking restarts nothing (Phase is not Ready).
                text = "Update undone";
                tip = $"Version {s.RolledBackFrom} did not start, so the previous version was restored.";
            }
            if (text is null)
            {
                _updateButton.Visibility = Visibility.Collapsed;
                return;
            }
            SetButtonText(_updateButton, text);
            ToolTipService.SetToolTip(_updateButton, tip);
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
    }

    private static void RefreshUpdateSettingsRow()
    {
        if (_autoUpdate is not null) _autoUpdate.IsOn = HasFlag(SettingFlag.UpdateAutoCheck);
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

