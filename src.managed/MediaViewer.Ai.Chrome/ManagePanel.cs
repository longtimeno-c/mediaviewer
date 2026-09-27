// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Windows.Storage.Pickers;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// Settings → Local search once the pack is loaded (plan/17 PR 23, PR 24):
/// status and Pause, Compute, Search quality, the indexed folders, the index
/// size and Clear, the battery threshold, and the separate people opt-in.
/// </summary>
/// <remarks>
/// It lives inside the base chrome's Settings island, where a TextBox
/// fail-fasts (IslandHost.Settings.cs), so there is no text entry here:
/// naming people happens in the people window. Roots and people are read on a
/// worker ([worker-thread]); settings and status are [no-block].
/// </remarks>
internal sealed class ManagePanel
{
    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly TextBlock _status;
    private readonly TextBlock _compute;
    private readonly Button _pause;
    private readonly ComboBox _computeBox;
    private readonly ComboBox _qualityBox;
    private readonly TextBlock _qualityLine;
    private readonly TextBlock _restart;
    private readonly ComboBox _batteryBox;
    private readonly StackPanel _roots;
    private readonly StackPanel _indexRow;
    private readonly StackPanel _people;
    private readonly ToggleSwitch _faces;
    private bool _updating;
    private bool _confirmClear;
    private bool? _indexRowConfirming;
    private TextBlock? _indexSize;
    private bool _confirmFacesOff;
    private bool _cudaAvailable;
    private readonly ToggleButton[] _videoIndex = new ToggleButton[3];
    private readonly TextBlock _videoIndexHint;
    private readonly TextBlock _audioLine;
    private MvAiMedia _videoIndexValue = MvAiMedia.Pictures;
    private bool _audioReady;
    private readonly List<MvAiCompute> _computeValues = new();

    private static readonly int[] BatteryValues = { 0, 20, 30, 50, 100 };
    private static readonly string[] BatteryNames =
        { "Never", "Below 20 %", "Below 30 %", "Below 50 %", "Always on battery" };
    private static readonly string[] QualityLines =
    {
        "Auto — High on a GPU, Fast otherwise",
        "Fast — smaller model, quick on any computer",
        "High — best matches, needs a GPU or Apple silicon to be quick",
    };

    public StackPanel Root { get; }

    public ManagePanel(AiChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        Root = new StackPanel { Spacing = 8, Margin = new Thickness(0, 8, 0, 0) };

        // Status, compute, Pause / Resume.
        _status = _look.Text("", null, AddonColour.Title);
        _compute = _look.Text("", 12);
        _pause = _look.Button("Pause indexing", TogglePause);
        var statusText = new StackPanel { Spacing = 2 };
        statusText.Children.Add(_status);
        statusText.Children.Add(_compute);
        _audioLine = _look.Text("", 12);
        _audioLine.Visibility = Visibility.Collapsed;
        statusText.Children.Add(_audioLine);
        var statusRow = new Grid { ColumnSpacing = 16 };
        statusRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        statusRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        statusRow.Children.Add(statusText);
        Grid.SetColumn(_pause, 1);
        _pause.VerticalAlignment = VerticalAlignment.Center;
        statusRow.Children.Add(_pause);
        Root.Children.Add(_look.Card(statusRow));
        Root.Children.Add(Row("Search", "Open the search panel. Ctrl+F in the viewer; Ctrl+Shift+F finds similar.",
            _look.Button("Open search", () => _chrome.RunCommand(SearchCommand.Open))));

        _computeBox = Combo();
        _computeBox.SelectionChanged += (_, _) =>
        {
            if (_updating || _computeBox.SelectedIndex < 0) return;
            Set("compute", ((uint)_computeValues[_computeBox.SelectedIndex]).ToString());
        };
        var computeDetail = new StackPanel { Spacing = 2 };
        computeDetail.Children.Add(_look.Text(
            "Where indexing runs. CPU is always there underneath; changing it does not re-index.", 12));
        _restart = _look.Text("Restart MediaViewer to use NVIDIA acceleration.", 12, AddonColour.Accent);
        _restart.Visibility = Visibility.Collapsed;
        computeDetail.Children.Add(_restart);
        Root.Children.Add(Row("Compute", computeDetail, _computeBox));

        _qualityBox = Combo();
        foreach (string q in new[] { "Auto", "Fast", "High" }) _qualityBox.Items.Add(q);
        _qualityLine = _look.Text("", 12);
        _qualityBox.SelectionChanged += (_, _) =>
        {
            if (_qualityBox.SelectedIndex >= 0) _qualityLine.Text = QualityLines[_qualityBox.SelectedIndex];
            if (_updating || _qualityBox.SelectedIndex < 0) return;
            Set("quality", _qualityBox.SelectedIndex.ToString());
        };
        var qualityDetail = new StackPanel { Spacing = 2 };
        qualityDetail.Children.Add(_qualityLine);
        qualityDetail.Children.Add(_look.Text(
            "Changing it re-indexes in the background; the current index answers until the new one is ready.", 12));
        Root.Children.Add(Row("Search quality", qualityDetail, _qualityBox));

        // Audio (2026-09-27): what videos are indexed for. Sound covers both
        // sounds ("dog barking") and speech, and needs the Audio piece.
        var segments = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        string[] names = { "Pictures", "Sound", "Both" };
        for (int i = 0; i < 3; ++i)
        {
            MvAiMedia media = (MvAiMedia)(i + 1);
            var b = new ToggleButton
            {
                Content = names[i],
                FontFamily = _look.Font,
                FontSize = _look.FontSize - 2,
                Padding = new Thickness(14, 5, 14, 6),
                CornerRadius = i == 0 ? new CornerRadius(6, 0, 0, 6) : i == 2 ? new CornerRadius(0, 6, 6, 0) : new CornerRadius(0),
            };
            AutomationProperties.SetName(b, "Index videos for " + names[i]);
            b.Click += (_, _) => SetVideoIndex(media);
            int at = i;
            b.KeyDown += (_, e) =>
            {
                // Arrows walk the segments, like a radio group; Tab leaves it.
                int step = e.Key == Windows.System.VirtualKey.Right ? 1 : e.Key == Windows.System.VirtualKey.Left ? -1 : 0;
                if (step == 0) return;
                for (int n = at + step; n >= 0 && n < 3; n += step)
                {
                    if (!_videoIndex[n].IsEnabled) continue;
                    _videoIndex[n].Focus(FocusState.Keyboard);
                    SetVideoIndex((MvAiMedia)(n + 1));
                    break;
                }
                e.Handled = true;
            };
            _videoIndex[i] = b;
            segments.Children.Add(b);
        }
        _videoIndexHint = _look.Text("", 12);
        var videoDetail = new StackPanel { Spacing = 2 };
        videoDetail.Children.Add(_look.Text(
            "Pictures finds what a clip shows; Sound finds what it sounds like and what is said. Photos are always pictures.", 12));
        videoDetail.Children.Add(_videoIndexHint);
        Root.Children.Add(Row("Index videos for", videoDetail, segments));

        // Indexed folders.
        Root.Children.Add(Heading("Indexed folders"));
        _roots = new StackPanel { Spacing = 6 };
        Root.Children.Add(_roots);
        var add = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        add.Children.Add(_look.Button("Add a folder…", () => _ = AddFolder(false)));
        add.Children.Add(_look.Button("Add a folder and its subfolders…", () => _ = AddFolder(true)));
        Root.Children.Add(add);

        _indexRow = new StackPanel { Spacing = 8 };
        Root.Children.Add(_indexRow);

        _batteryBox = Combo();
        foreach (string b in BatteryNames) _batteryBox.Items.Add(b);
        _batteryBox.SelectionChanged += (_, _) =>
        {
            if (_updating || _batteryBox.SelectedIndex < 0) return;
            Set("pause_on_battery_percent", BatteryValues[_batteryBox.SelectedIndex].ToString());
        };
        Root.Children.Add(Row("Pause on battery", "Indexing waits while the battery is below this.", _batteryBox));

        // People: a separate opt-in (PR 24; biometric data, stricter than the frame index).
        Root.Children.Add(Heading("People"));
        _faces = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 48 };
        AutomationProperties.SetName(_faces, "Find people in your photos");
        _faces.Toggled += (_, _) => OnFacesToggled();
        var facesDetail = new StackPanel { Spacing = 2 };
        facesDetail.Children.Add(_look.Text(
            "Face data stays on this computer, is never shared, and can be deleted at any time.", 12));
        Root.Children.Add(Row("Find people in your photos", facesDetail, _faces));
        _people = new StackPanel { Spacing = 8 };
        Root.Children.Add(_people);

        _chrome.StatusChanged += OnStatus;
        RefreshSettings();
        RefreshRoots();
        OnStatus();
    }

    // ---- building blocks -------------------------------------------------------------

    private ComboBox Combo() => new() { FontFamily = _look.Font, FontSize = _look.FontSize - 1, MinWidth = 200 };

    private TextBlock Heading(string text)
    {
        TextBlock t = _look.Text(text, _look.FontSize + 1, AddonColour.Title);
        t.Margin = new Thickness(0, 12, 0, 2);
        return t;
    }

    private FrameworkElement Row(string title, string detail, FrameworkElement control) =>
        Row(title, _look.Text(detail, 12), control);

    private FrameworkElement Row(string title, FrameworkElement detail, FrameworkElement control)
    {
        var grid = new Grid { ColumnSpacing = 24 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var labels = new StackPanel { Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        labels.Children.Add(_look.Text(title, null, AddonColour.Title));
        labels.Children.Add(detail);
        grid.Children.Add(labels);
        control.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetName(control, title);
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        return _look.Card(grid);
    }

    private void Set(string key, string valueJson)
    {
        try { _api.SetSetting(key, valueJson); }
        catch (MediaViewerException) { }
        RefreshSettings();
        _chrome.ReadStatus();
    }

    // ---- settings --------------------------------------------------------------------

    internal void RefreshSettings()
    {
        string json;
        try { json = _api.SettingsJson(); }  // [no-block]
        catch (MediaViewerException) { return; }
        _updating = true;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement r = doc.RootElement;
            uint compute = r.TryGetProperty("compute", out JsonElement c) ? c.GetUInt32() : 0;
            uint quality = r.TryGetProperty("quality", out JsonElement q) ? q.GetUInt32() : 0;
            int battery = r.TryGetProperty("pause_on_battery_percent", out JsonElement b) ? b.GetInt32() : 30;
            bool faces = r.TryGetProperty("faces", out JsonElement f) && f.GetBoolean();
            // ai-cuda carries its own ORT build: it takes effect at the next start.
            _restart.Visibility = r.TryGetProperty("restart_needed", out JsonElement rn) && rn.ValueKind == JsonValueKind.True
                ? Visibility.Visible : Visibility.Collapsed;
            _cudaAvailable = r.TryGetProperty("available", out JsonElement a) &&
                             a.TryGetProperty("cuda", out JsonElement cu) && cu.GetBoolean();

            _computeValues.Clear();
            _computeBox.Items.Clear();
            _computeValues.Add(MvAiCompute.Auto);
            _computeBox.Items.Add("Auto");
            if (_cudaAvailable || compute == (uint)MvAiCompute.Cuda)
            {
                _computeValues.Add(MvAiCompute.Cuda);
                _computeBox.Items.Add("NVIDIA CUDA");
            }
            _computeValues.Add(MvAiCompute.CpuOnly);
            _computeBox.Items.Add("CPU only");
            _computeBox.SelectedIndex = Math.Max(0, _computeValues.IndexOf((MvAiCompute)compute));

            _qualityBox.SelectedIndex = (int)Math.Min(2, quality);
            _qualityLine.Text = QualityLines[_qualityBox.SelectedIndex];
            int bi = Array.IndexOf(BatteryValues, battery);
            if (bi < 0) bi = Array.FindIndex(BatteryValues, v => v >= battery);
            _batteryBox.SelectedIndex = bi < 0 ? 0 : bi;
            if (!_confirmFacesOff) _faces.IsOn = faces;
            uint videoIndex = r.TryGetProperty("video_index", out JsonElement vi) ? vi.GetUInt32() : 0;
            _videoIndexValue = videoIndex is >= 1 and <= 3 ? (MvAiMedia)videoIndex : MvAiMedia.Pictures;
            _audioReady = r.TryGetProperty("audio_ready", out JsonElement ar) && ar.ValueKind == JsonValueKind.True;
            ShowVideoIndex();
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException or FormatException) { }
        finally
        {
            _updating = false;
        }
        RefreshPeople();
    }

    private void OnStatus()
    {
        if (!_chrome.StatusValid)
        {
            _status.Text = "Local search is not running.";
            _compute.Text = "";
            return;
        }
        MvAiStatus s = _chrome.Status;
        _status.Text = Look.StatusLine(s);
        string? why = Look.FallbackReason(s);
        string model = s.ModelText.Length > 0 ? " · " + s.ModelText : "";
        _compute.Text = (why ?? $"Running on {Look.ComputeBadge(s)}") + model;
        _pause.Content = s.State == MvAiState.Paused ? "Resume indexing" : "Pause indexing";
        // "Sound: 12 of 40 clips · Speech: 8 of 40", while there is sound work.
        bool audio = (s.Flags & MvAiStatus.FlagAudioReady) != 0;
        _audioLine.Visibility = audio && (s.SoundTotal > 0 || s.SpeechTotal > 0) ? Visibility.Visible : Visibility.Collapsed;
        _audioLine.Text = $"Sound: {s.SoundDone:N0} of {s.SoundTotal:N0} clips · Speech: {s.SpeechDone:N0} of {s.SpeechTotal:N0}";
        if (audio != _audioReady)
        {
            _audioReady = audio;
            ShowVideoIndex();
        }
        RefreshIndexRow(s);
    }

    // ---- index videos for --------------------------------------------------------------

    private bool AudioAvailable => _audioReady || _chrome.Host.IsPieceInstalled("ai-audio");

    private void ShowVideoIndex()
    {
        bool audio = AudioAvailable;
        for (int i = 0; i < 3; ++i)
        {
            var media = (MvAiMedia)(i + 1);
            _videoIndex[i].IsEnabled = media == MvAiMedia.Pictures || audio;
            _videoIndex[i].IsChecked = media == _videoIndexValue;
        }
        _videoIndexHint.Text = audio ? "" : "Install Audio above to index sounds and speech.";
        _videoIndexHint.Visibility = audio ? Visibility.Collapsed : Visibility.Visible;
    }

    private void SetVideoIndex(MvAiMedia media)
    {
        if (media != MvAiMedia.Pictures && !AudioAvailable)
        {
            ShowVideoIndex();
            return;
        }
        _videoIndexValue = media;
        ShowVideoIndex();
        Set("video_index", ((uint)media).ToString());
    }

    private void TogglePause()
    {
        bool paused = _chrome.StatusValid && _chrome.Status.State == MvAiState.Paused;
        try { _api.Pause(!paused); }
        catch (MediaViewerException) { }
        _chrome.ReadStatus();
    }

    // ---- index size and Clear -------------------------------------------------------

    /// <summary>Stops following status; Settings built a newer panel.</summary>
    internal void Detach() => _chrome.StatusChanged -= OnStatus;

    // Rebuilt only when it changes shape: the status arrives at 4 Hz, and a
    // rebuilt button would drop keyboard focus.
    private void RefreshIndexRow(in MvAiStatus s)
    {
        string size = $"{Look.Size((long)s.IndexBytes)} on disk. It is your data: it does not count toward the 3 GB.";
        if (_indexRowConfirming == _confirmClear)
        {
            if (_indexSize is not null) _indexSize.Text = size;
            return;
        }
        _indexRowConfirming = _confirmClear;
        _indexRow.Children.Clear();
        _indexSize = null;
        if (_confirmClear)
        {
            var box = new StackPanel { Spacing = 8 };
            box.Children.Add(_look.Text(
                "Clear the whole index? Searching needs indexing again. Thumbnails, your files and your folders list stay.", null,
                AddonColour.Title));
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            Button cancel = _look.Button("Cancel", () =>
            {
                _confirmClear = false;
                OnStatus();
            });
            buttons.Children.Add(_look.Button("Clear the index", ClearIndex));
            buttons.Children.Add(cancel);
            box.Children.Add(buttons);
            _indexRow.Children.Add(_look.Card(box));
            return;
        }
        Button clear = _look.Button("Clear index…", () =>
        {
            _confirmClear = true;
            OnStatus();
        });
        _indexSize = _look.Text(size, 12);
        _indexRow.Children.Add(Row("Index size", _indexSize, clear));
    }

    private void ClearIndex()
    {
        _confirmClear = false;
        try { _api.ClearIndex(); }
        catch (MediaViewerException) { }
        _chrome.ReadStatus();
    }

    // ---- roots ----------------------------------------------------------------------

    private sealed record RootRow(ulong Id, string Path, bool Recursive, bool Enabled, long Assets, long Done, long Bytes,
                                  MvAiMedia Media);

    internal void RefreshRoots()
    {
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            var rows = new List<RootRow>();
            try
            {
                using JsonDocument doc = JsonDocument.Parse(api.RootsJson());  // [worker-thread]
                foreach (JsonElement r in doc.RootElement.EnumerateArray())
                {
                    rows.Add(new RootRow(
                        r.GetProperty("id").GetUInt64(),
                        r.GetProperty("path").GetString() ?? "",
                        r.TryGetProperty("recursive", out JsonElement rec) && rec.GetBoolean(),
                        !r.TryGetProperty("enabled", out JsonElement en) || en.GetBoolean(),
                        r.TryGetProperty("assets", out JsonElement a) ? a.GetInt64() : 0,
                        r.TryGetProperty("done", out JsonElement d) ? d.GetInt64() : 0,
                        r.TryGetProperty("bytes", out JsonElement b) ? b.GetInt64() : 0,
                        r.TryGetProperty("media", out JsonElement m) && m.GetUInt32() <= 3
                            ? (MvAiMedia)m.GetUInt32() : MvAiMedia.Default));
                }
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException
                                           or InvalidOperationException) { }
            _chrome.Host.Post(() => ShowRoots(rows));
        });
    }

    private void ShowRoots(List<RootRow> rows)
    {
        _roots.Children.Clear();
        if (rows.Count == 0)
        {
            _roots.Children.Add(_look.Text(
                "No folders yet. Open a folder and press Ctrl+F to index it, or add one here.", 13));
            return;
        }
        foreach (RootRow r in rows)
        {
            var grid = new Grid { ColumnSpacing = 12, RowSpacing = 6 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var labels = new StackPanel { Spacing = 2 };
            TextBlock path = _look.Text(r.Path, 14, AddonColour.Title, wrap: false);
            ToolTipService.SetToolTip(path, r.Path);
            labels.Children.Add(path);
            string state = !r.Enabled ? "Paused" : r.Done >= r.Assets ? "Up to date" : $"{r.Done:N0} of {r.Assets:N0}";
            labels.Children.Add(_look.Text((r.Recursive ? "and subfolders · " : "") + state + " · " + Look.Size(r.Bytes), 12));
            var bar = new ProgressBar
            {
                Maximum = Math.Max(1, r.Assets),
                Value = Math.Min(r.Done, Math.Max(1, r.Assets)),
                Foreground = _look[AddonColour.Accent],
                Margin = new Thickness(0, 4, 0, 0),
            };
            labels.Children.Add(bar);
            grid.Children.Add(labels);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
            ulong id = r.Id;
            bool enabled = r.Enabled;
            buttons.Children.Add(MediaMenu(id, r.Media));
            buttons.Children.Add(_look.Button(enabled ? "Pause" : "Resume", () => RootCall(() => _api.RootSetEnabled(id, !enabled))));
            buttons.Children.Add(_look.Button("Rescan", () => RootCall(() => _api.RootRescan(id))));
            buttons.Children.Add(_look.Button("Remove", () => RootCall(() => _api.RootRemove(id))));
            foreach (UIElement b in buttons.Children)
            {
                if (b is Button button) AutomationProperties.SetName(button, $"{button.Content} {System.IO.Path.GetFileName(r.Path)}");
            }
            Grid.SetColumn(buttons, 1);
            grid.Children.Add(buttons);
            _roots.Children.Add(_look.Card(grid, 12));
        }
    }

    private static readonly string[] MediaNames = { "Default", "Pictures", "Sound", "Both" };

    // "Videos: Default ▾" — this folder's own choice, or the setting's.
    private DropDownButton MediaMenu(ulong root, MvAiMedia current)
    {
        var menu = new MenuFlyout();
        for (int i = 0; i < 4; ++i)
        {
            var media = (MvAiMedia)i;
            var item = new ToggleMenuFlyoutItem
            {
                Text = i == 0 ? $"Default ({MediaNames[(int)_videoIndexValue]})" : MediaNames[i],
                IsChecked = media == current,
                // Sound needs the Audio piece; Default may resolve to it via the setting.
                IsEnabled = media is MvAiMedia.Default or MvAiMedia.Pictures || AudioAvailable,
            };
            item.Click += (_, _) => RootCall(() => _api.RootSetMedia(root, media));
            menu.Items.Add(item);
        }
        var button = new DropDownButton
        {
            Content = "Videos: " + MediaNames[(int)current],
            FontFamily = _look.Font,
            FontSize = _look.FontSize - 2,
            Padding = new Thickness(12, 5, 8, 6),
            CornerRadius = new CornerRadius(6),
            Flyout = menu,
        };
        AutomationProperties.SetName(button, "Index this folder's videos for");
        ToolTipService.SetToolTip(button, "What this folder's videos are indexed for: pictures, sound and speech, or both");
        return button;
    }

    private void RootCall(Action call)
    {
        try { call(); }
        catch (MediaViewerException) { }
        RefreshRoots();
        _chrome.RefreshCoverage();
        _chrome.ReadStatus();
    }

    private async Task AddFolder(bool recursive)
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, _chrome.Host.MainWindow);
        Windows.Storage.StorageFolder? folder;
        try { folder = await picker.PickSingleFolderAsync(); }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return;
        }
        if (folder is null) return;
        RootCall(() => _api.IndexFolder(folder.Path, recursive));
    }

    // ---- people -----------------------------------------------------------------------

    private void OnFacesToggled()
    {
        if (_updating) return;
        if (_faces.IsOn)
        {
            _confirmFacesOff = false;
            try { _api.FacesEnable(true); }
            catch (MediaViewerException) { }
            RefreshSettings();
            _chrome.ReadStatus();
            return;
        }
        // Off deletes every face vector, crop and name: confirm first.
        _confirmFacesOff = true;
        RefreshPeople();
    }

    internal void RefreshPeople()
    {
        _people.Children.Clear();
        if (_confirmFacesOff)
        {
            var box = new StackPanel { Spacing = 8 };
            box.Children.Add(_look.Text(
                "Turn off people and delete all face data? Names and corrections go too. Photos and the search index stay.",
                null, AddonColour.Title));
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            buttons.Children.Add(_look.Button("Delete face data", () =>
            {
                _confirmFacesOff = false;
                try { _api.FacesEnable(false); }
                catch (MediaViewerException) { }
                RefreshSettings();
                _chrome.ReadStatus();
            }));
            buttons.Children.Add(_look.Button("Keep it on", () =>
            {
                _confirmFacesOff = false;
                _updating = true;
                _faces.IsOn = true;
                _updating = false;
                RefreshPeople();
            }));
            box.Children.Add(buttons);
            _people.Children.Add(_look.Card(box));
            return;
        }
        if (!_faces.IsOn) return;
        MvAiStatus s = _chrome.Status;
        bool ready = _chrome.StatusValid && (s.Flags & MvAiStatus.FlagFacesReady) != 0;
        if (!ready && !_chrome.Host.IsPieceInstalled("ai-faces"))
        {
            _people.Children.Add(_look.Text("Install People in the list above to find people.", 13));
            return;
        }
        string found = s.People == 0 ? "No people found yet." : s.People == 1 ? "1 person found." : $"{s.People:N0} people found.";
        _people.Children.Add(Row("People", found + " Name them, merge and split them in the people window.",
            _look.Button("People…", _chrome.OpenPeople)));
    }
}
