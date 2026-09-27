// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Ai.Chrome;

/// <summary>One remembered root from roots_json, as the gallery bar needs it.</summary>
internal sealed record GalleryRoot(ulong Id, string Path, bool Recursive, bool Enabled, long Assets, long Done,
                                   MvAiMedia Media)
{
    public string Name => System.IO.Path.GetFileName(Path.TrimEnd('\\', '/')) is { Length: > 0 } n ? n : Path;

    /// <summary>
    /// The root that covers <paramref name="folder"/>: the folder itself, or
    /// the deepest recursive root above it. Null when none does (or the JSON
    /// did not parse). Worker: <paramref name="rootsJson"/> is roots_json's answer.
    /// </summary>
    public static GalleryRoot? Covering(string rootsJson, string folder)
    {
        string want = Trim(folder);
        GalleryRoot? best = null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(rootsJson);
            foreach (JsonElement r in doc.RootElement.EnumerateArray())
            {
                string path = r.GetProperty("path").GetString() ?? "";
                string root = Trim(path);
                bool recursive = r.TryGetProperty("recursive", out JsonElement rec) && rec.GetBoolean();
                bool exact = string.Equals(root, want, StringComparison.OrdinalIgnoreCase);
                bool above = recursive && root.Length > 0 && want.Length > root.Length &&
                             want.StartsWith(root, StringComparison.OrdinalIgnoreCase) &&
                             want[root.Length] is '\\' or '/';
                if (!exact && !above) continue;
                // The folder's own root beats one above it; else the deepest.
                if (best is not null && (Trim(best.Path).Length >= root.Length ||
                                         string.Equals(Trim(best.Path), want, StringComparison.OrdinalIgnoreCase)))
                {
                    continue;
                }
                best = new GalleryRoot(
                    r.GetProperty("id").GetUInt64(),
                    path,
                    recursive,
                    !r.TryGetProperty("enabled", out JsonElement en) || en.GetBoolean(),
                    r.TryGetProperty("assets", out JsonElement a) ? a.GetInt64() : 0,
                    r.TryGetProperty("done", out JsonElement d) ? d.GetInt64() : 0,
                    r.TryGetProperty("media", out JsonElement m) && m.GetUInt32() <= 3
                        ? (MvAiMedia)m.GetUInt32() : MvAiMedia.Default);
            }
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException
                                       or FormatException)
        {
            return null;
        }
        return best;
    }

    private static string Trim(string p) => p.TrimEnd('\\', '/');
}

/// <summary>
/// The gallery search bar's index control (plan/17 "Gallery search bar"): one
/// compact button at the bar's right end for the folder on screen. "Index…"
/// when the index does not cover it; "Indexing 120 of 800" with a small ring
/// while it indexes; "Indexed" with a check when it is done. Its menu is the
/// folder's row from Settings → Local search: pause, what videos are indexed
/// for, rescan, remove.
/// </summary>
/// <remarks>
/// Lives in the base gallery's island: no TextBox, no dialog (the remove
/// confirmation is a flyout). Coverage is [no-block]; roots_json is
/// [worker-thread] and goes through Task.Run. It polls at 2 Hz only while the
/// gallery shows a folder that is indexing, and never with the gallery hidden.
/// </remarks>
internal sealed class GalleryControl
{
    private static readonly string[] MediaNames = { "Default", "Pictures", "Sound", "Both" };

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly DispatcherQueueTimer? _poll;
    private readonly ProgressRing _ring;
    private readonly FontIcon _check;
    private readonly TextBlock _text;
    private readonly DropDownButton _button;
    private readonly MenuFlyout _menu;
    private string? _folder;
    private uint _coverage;
    private GalleryRoot? _root;
    private string? _reading;     // the folder a roots read is running for
    private int _readGen;
    private long _lastRead;
    private string _menuKey = "";
    private bool _detached;

    public FrameworkElement Root => _button;

    public GalleryControl(AiChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;

        // Determinate: it moves with the count and is still between counts,
        // so a reduced-motion session sees no spinner.
        _ring = new ProgressRing
        {
            Width = 14,
            Height = 14,
            MinWidth = 14,
            MinHeight = 14,
            IsIndeterminate = false,
            Foreground = _look[AddonColour.Accent],
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        _check = new FontIcon
        {
            Glyph = "\uE73E",
            FontSize = 12,
            Foreground = _look[AddonColour.Accent],
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        _text = _look.Text("Index…", _look.FontSize - 3, AddonColour.Body, wrap: false);
        _text.VerticalAlignment = VerticalAlignment.Center;
        _text.MaxWidth = 240;
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        row.Children.Add(_ring);
        row.Children.Add(_check);
        row.Children.Add(_text);
        _menu = new MenuFlyout();
        _button = new DropDownButton
        {
            Content = row,
            FontFamily = _look.Font,
            FontSize = _look.FontSize - 3,
            Padding = new Thickness(10, 4, 8, 5),
            CornerRadius = new CornerRadius(6),
            MinHeight = 0,
            VerticalAlignment = VerticalAlignment.Center,
            Flyout = _menu,
        };
        ToolTipService.SetToolTip(_button, "Local search for this folder. Indexing runs in the background while you keep browsing.");

        _poll = _button.DispatcherQueue?.CreateTimer();
        if (_poll is not null)
        {
            _poll.Interval = TimeSpan.FromMilliseconds(500);  // 2 Hz, only while indexing and shown
            _poll.IsRepeating = true;
            _poll.Tick += (_, _) => Refresh();
        }
        _chrome.StatusChanged += OnStatus;
        Show();
    }

    internal void Detach()
    {
        _detached = true;
        _poll?.Stop();
        _chrome.StatusChanged -= OnStatus;
    }

    /// <summary>Null: the gallery is hidden; stop asking.</summary>
    internal void SetFolder(string? folder)
    {
        if (string.Equals(folder, _folder, StringComparison.OrdinalIgnoreCase) && folder is not null)
        {
            Refresh();
            return;
        }
        _folder = folder;
        _root = null;
        _coverage = 0;
        ++_readGen;
        _reading = null;
        if (folder is null)
        {
            _poll?.Stop();
            return;
        }
        Refresh();
    }

    // Status moves while a folder indexes; the per-folder count follows at <= 2 Hz.
    private void OnStatus()
    {
        if (_detached || _folder is null || (_poll?.IsRunning ?? false)) return;
        if (Environment.TickCount64 - _lastRead >= 500) Refresh();
    }

    /// <summary>Coverage now ([no-block]); the covering root's counts from a worker.</summary>
    internal void Refresh()
    {
        if (_detached) return;
        if (_folder is not string folder)
        {
            _poll?.Stop();
            return;
        }
        _lastRead = Environment.TickCount64;
        try { _coverage = _api.FolderCoverage(folder); }
        catch (MediaViewerException) { _coverage = 0; }
        if (_coverage == 0)
        {
            _root = null;
            Show();
            return;
        }
        Show();
        if (_reading is not null && string.Equals(_reading, folder, StringComparison.OrdinalIgnoreCase)) return;
        _reading = folder;
        int gen = _readGen;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            GalleryRoot? root = null;
            try { root = GalleryRoot.Covering(api.RootsJson(), folder); }  // [worker-thread]
            catch (MediaViewerException) { }
            _chrome.Host.Post(() =>
            {
                if (_detached || gen != _readGen) return;
                _reading = null;
                _root = root;
                Show();
            });
        });
    }

    // Loaded, not only installed (as Settings' "Index videos for").
    private bool AudioAvailable =>
        _chrome.StatusValid && (_chrome.Status.Flags & MvAiStatus.FlagAudioReady) != 0;

    private bool OnBattery => _chrome.StatusValid && Look.OnBattery(_chrome.Status);

    private void Show()
    {
        if (_detached) return;
        GalleryRoot? r = _root;
        bool indexing = _coverage == 1;
        bool paused = r is not null && !r.Enabled;
        bool battery = indexing && !paused && OnBattery;
        string text;
        if (_coverage == 0) text = "Index…";
        else if (paused) text = r!.Assets > 0 ? $"Paused · {r.Done:N0} of {r.Assets:N0}" : "Paused";
        else if (battery) text = r is not null && r.Assets > 0 ? $"Paused on battery · {r.Done:N0} of {r.Assets:N0}" : "Paused on battery";
        else if (indexing) text = r is not null && r.Assets > 0 ? $"Indexing {r.Done:N0} of {r.Assets:N0}" : "Indexing…";
        else text = "Indexed";
        _text.Text = text;
        _check.Visibility = _coverage == 2 && !paused ? Visibility.Visible : Visibility.Collapsed;
        _ring.Visibility = indexing && !paused ? Visibility.Visible : Visibility.Collapsed;
        if (r is not null && r.Assets > 0)
        {
            _ring.Maximum = r.Assets;
            _ring.Value = Math.Min(r.Done, r.Assets);
        }
        AutomationProperties.SetName(_button, $"Local search for this folder: {text}");

        // Poll only while the numbers move and the gallery is up.
        bool poll = _folder is not null && indexing && !paused;
        if (poll && _poll is not null && !_poll.IsRunning) _poll.Start();
        else if (!poll) _poll?.Stop();

        string key = $"{_folder}|{_coverage}|{r?.Id}|{r?.Enabled}|{r?.Media}|{AudioAvailable}|{battery}";
        if (key == _menuKey) return;
        _menuKey = key;
        FillMenu();
    }

    private void FillMenu()
    {
        _menu.Items.Clear();
        if (_folder is not string folder) return;
        if (_coverage == 0)
        {
            _menu.Items.Add(Item("Index this folder", () => Index(folder, false)));
            _menu.Items.Add(Item("Index this folder and subfolders", () => Index(folder, true)));
            return;
        }
        GalleryRoot? r = _root;
        if (r is null)
        {
            // Covered, but its root is not read yet (or sits under another name).
            _menu.Items.Add(Item("Local search settings…", () => _chrome.Host.ShowSettings()));
            return;
        }
        bool own = string.Equals(r.Path.TrimEnd('\\', '/'), folder.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
        if (!own)
        {
            // What the items below act on: the whole indexed tree.
            _menu.Items.Add(new MenuFlyoutItem { Text = $"Indexed as part of “{r.Name}”", IsEnabled = false });
            _menu.Items.Add(new MenuFlyoutSeparator());
        }
        ulong id = r.Id;
        if (_coverage == 1 && r.Enabled && OnBattery)
        {
            // Until this PC is next plugged in; never saved.
            _menu.Items.Add(Item("Index anyway, on battery", () =>
            {
                try { _api.IndexAnyway(); }
                catch (MediaViewerException) { }
                _chrome.ReadStatus();
            }));
        }
        if (_coverage == 1 || !r.Enabled)
        {
            bool enabled = r.Enabled;
            _menu.Items.Add(Item(enabled ? "Pause indexing this folder" : "Resume indexing this folder",
                                 () => RootCall(() => _api.RootSetEnabled(id, !enabled))));
        }
        var videos = new MenuFlyoutSubItem { Text = "Videos: " + MediaNames[(int)r.Media] };
        bool audio = AudioAvailable;
        for (int i = 0; i < MediaNames.Length; ++i)
        {
            var media = (MvAiMedia)i;
            var choice = new ToggleMenuFlyoutItem
            {
                Text = MediaNames[i],
                IsChecked = media == r.Media,
                // Sound needs the Audio piece; Default may resolve to it via the setting.
                IsEnabled = media is MvAiMedia.Default or MvAiMedia.Pictures || audio,
            };
            choice.Click += (_, _) => RootCall(() => _api.RootSetMedia(id, media));
            videos.Items.Add(choice);
        }
        if (!audio)
        {
            videos.Items.Add(new MenuFlyoutSeparator());
            videos.Items.Add(new MenuFlyoutItem { Text = "Sound needs Audio (Settings → Local search)", IsEnabled = false });
        }
        _menu.Items.Add(videos);
        _menu.Items.Add(Item("Rescan", () => RootCall(() => _api.RootRescan(id))));
        _menu.Items.Add(new MenuFlyoutSeparator());
        string name = r.Name;
        _menu.Items.Add(Item("Remove from index…", () => ConfirmRemove(id, name)));
    }

    private static MenuFlyoutItem Item(string text, Action click)
    {
        var item = new MenuFlyoutItem { Text = text };
        item.Click += (_, _) => click();
        return item;
    }

    // The chrome refreshes this control once the folder is added.
    private void Index(string folder, bool recursive) => _chrome.IndexGalleryFolder(folder, recursive);

    private void RootCall(Action call)
    {
        try { call(); }
        catch (MediaViewerException) { }
        _chrome.RefreshCoverage();
        _chrome.ReadStatus();
        ++_readGen;
        _reading = null;
        Refresh();
    }

    // Not a dialog: a flyout under the button, after the menu has closed.
    private void ConfirmRemove(ulong id, string name)
    {
        var box = new StackPanel { Spacing = 10, MaxWidth = 340 };
        box.Children.Add(_look.Text($"Remove “{name}” from the index?", null, AddonColour.Title));
        box.Children.Add(_look.Text(
            "Its search data is deleted. Your photos and videos are untouched.", _look.FontSize - 3));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var confirm = new Flyout { Content = box };
        buttons.Children.Add(_look.Button("Remove", () =>
        {
            confirm.Hide();
            RootCall(() => _api.RootRemove(id));
        }, accent: true));
        buttons.Children.Add(_look.Button("Cancel", () => confirm.Hide()));
        box.Children.Add(buttons);
        _button.DispatcherQueue?.TryEnqueue(() =>
        {
            if (_detached || _button.XamlRoot is null) return;
            confirm.ShowAt(_button);
        });
    }
}
