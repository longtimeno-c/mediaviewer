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
/// status and Pause, Compute, Precision (the one quality scale; the model
/// stays on Auto), the indexed folders, the index
/// size and Clear, the battery threshold, and the separate people opt-in.
/// </summary>
/// <remarks>
/// It lives inside the base chrome's Settings island, where a TextBox
/// fail-fasts (IslandHost.Settings.cs), so there is no text entry here:
/// naming people happens in the people window. Roots and people are read on a
/// worker ([worker-thread]); settings and status are [no-block].
/// </remarks>
internal sealed partial class ManagePanel
{
    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly TextBlock _status;
    private readonly TextBlock _compute;
    private readonly Button _pause;
    private readonly Button _indexAnyway;
    private readonly ComboBox _computeBox;
    // Shown only while a Fast or High from before is stored (no picker now).
    private readonly FrameworkElement _legacyQualityRow;
    private readonly TextBlock _legacyQualityLine;
    private readonly Slider _precision;
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
    // The faces bits and people count the People rows were last drawn for:
    // the piece coming or going, or people being found, redraws them now
    // rather than at the next start (owner report, 2026-09-27).
    private uint _facesFlags = uint.MaxValue;
    private uint _peopleShown = uint.MaxValue;
    private readonly List<MvAiCompute> _computeValues = new();

    private static readonly int[] BatteryValues = { 0, 20, 30, 50, 100 };
    private static readonly string[] BatteryNames =
        { "Never", "Below 20 %", "Below 30 %", "Below 50 %", "Always on battery" };
    private static readonly string[] PrecisionNames = { "Broadest", "Broader", "Balanced", "Stricter", "Strictest" };
    // A stored Fast (1) or High (2), said beside "Use Auto".
    private static readonly string[] LegacyQualityLines =
    {
        "",
        "Set to Fast earlier. Auto chooses the model for this computer and re-indexes in the background if it changes; the current index answers until the new one is ready.",
        "Set to High earlier. Auto chooses the model for this computer and re-indexes in the background if it changes; the current index answers until the new one is ready.",
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
        // Paused on battery: "Index anyway" until the machine is next on AC (never saved).
        _indexAnyway = _look.Button("Index anyway", IndexAnyway);
        _indexAnyway.Visibility = Visibility.Collapsed;
        ToolTipService.SetToolTip(_indexAnyway,
            "Carry on indexing on battery until this PC is next plugged in. The setting below stays as it is.");
        var statusActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        statusActions.Children.Add(_indexAnyway);
        statusActions.Children.Add(_pause);
        Grid.SetColumn(statusActions, 1);
        statusRow.Children.Add(statusActions);
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

        // No "Search quality" picker (owner, 2026-09-28: one scale, not two):
        // the engine's Auto picks the larger model where CUDA runs it quickly,
        // the smaller one on CPU only. A Fast or High chosen before stays as
        // it was, said here with a way back to Auto; nothing rewrites it
        // behind the person's back.
        _legacyQualityLine = _look.Text("", 12);
        _legacyQualityRow = Row("Search model", _legacyQualityLine,
            _look.Button("Use Auto", () => Set("quality", ((uint)MvAiQuality.Auto).ToString())));
        _legacyQualityRow.Visibility = Visibility.Collapsed;
        Root.Children.Add(_legacyQualityRow);

        // Precision (plan/17 "Precision scale"): five steps, the middle the
        // calibrated rule. Each search reads it as it starts: no re-index.
        _precision = new Slider
        {
            Minimum = 0,
            Maximum = 4,
            StepFrequency = 1,
            SnapsTo = SliderSnapsTo.StepValues,
            TickFrequency = 1,
            TickPlacement = TickPlacement.Outside,
            Value = 2,
            Width = 160,
            IsThumbToolTipEnabled = false,
            VerticalAlignment = VerticalAlignment.Center,
        };
        AutomationProperties.SetName(_precision, "Precision");
        _precision.ValueChanged += (_, e) =>
        {
            AutomationProperties.SetItemStatus(_precision, PrecisionNames[(int)Math.Clamp(Math.Round(e.NewValue), 0, 4)]);
            if (_updating) return;
            Set("precision", ((int)Math.Clamp(Math.Round(e.NewValue), 0, 4)).ToString());
            _chrome.PrecisionChanged();
        };
        var precisionControl = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        TextBlock broader = _look.Text("Broader", 12);
        broader.VerticalAlignment = VerticalAlignment.Center;
        TextBlock stricter = _look.Text("Stricter", 12);
        stricter.VerticalAlignment = VerticalAlignment.Center;
        precisionControl.Children.Add(broader);
        precisionControl.Children.Add(_precision);
        precisionControl.Children.Add(stricter);
        Root.Children.Add(Row("Precision",
            "How closely a result must match what you type. Stricter shows only close matches and says “nothing found” rather than a near miss (a plane for “helicopter”); Broader shows more, including looser matches.",
            precisionControl));

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
        // Import and export (plan/17 "Sharing an index"; ManagePanel.Transfer.cs).
        AddTransferSection();

        _batteryBox = Combo();
        foreach (string b in BatteryNames) _batteryBox.Items.Add(b);
        _batteryBox.SelectionChanged += (_, _) =>
        {
            if (_updating || _batteryBox.SelectedIndex < 0) return;
            Set("pause_on_battery_percent", BatteryValues[_batteryBox.SelectedIndex].ToString());
        };
        Root.Children.Add(Row("Pause on battery", "Indexing waits while the battery is below this. Index anyway, beside the status, carries on until you next plug in.", _batteryBox));

        // People: a separate opt-in (PR 24; biometric data, stricter than the frame index).
        Root.Children.Add(Heading("People"));
        _faces = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 48 };
        AutomationProperties.SetName(_faces, "Find people in your photos");
        _faces.Toggled += (_, _) => OnFacesToggled();
        var facesDetail = new StackPanel { Spacing = 2 };
        facesDetail.Children.Add(_look.Text(
            "Face data stays on this computer unless you include People in an index export, and can be deleted at any time.", 12));
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

            bool legacyQuality = quality is (uint)MvAiQuality.Fast or (uint)MvAiQuality.High;
            _legacyQualityRow.Visibility = legacyQuality ? Visibility.Visible : Visibility.Collapsed;
            if (legacyQuality) _legacyQualityLine.Text = LegacyQualityLines[quality];
            uint precision = r.TryGetProperty("precision", out JsonElement pr) ? pr.GetUInt32() : 2;
            _precision.Value = Math.Min(4u, precision);
            AutomationProperties.SetItemStatus(_precision, PrecisionNames[(int)Math.Min(4u, precision)]);
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
        _indexAnyway.Visibility = Look.OnBattery(s) ? Visibility.Visible : Visibility.Collapsed;
        // "Sound: 12 of 40 clips · Speech: 8 of 40", while there is sound work.
        bool audio = (s.Flags & MvAiStatus.FlagAudioReady) != 0;
        _audioLine.Visibility = audio && (s.SoundTotal > 0 || s.SpeechTotal > 0) ? Visibility.Visible : Visibility.Collapsed;
        _audioLine.Text = $"Sound: {s.SoundDone:N0} of {s.SoundTotal:N0} clips · Speech: {s.SpeechDone:N0} of {s.SpeechTotal:N0}";
        if (audio != _audioReady)
        {
            _audioReady = audio;
            ShowVideoIndex();
            // Each folder's "Videos:" menu too: built with the readiness of its
            // day, it kept Sound and Both off until the folder list happened to
            // rebuild (the Mac's per-folder menu follows audioReady at once).
            RefreshRoots();
        }
        RefreshIndexRow(s);
        uint faces = s.Flags & (MvAiStatus.FlagFacesReady | MvAiStatus.FlagFacesOn);
        if (faces != _facesFlags || s.People != _peopleShown) RefreshPeople();
        PollTransfer();
    }

    // ---- index videos for --------------------------------------------------------------

    // Loaded, not only installed: between an install and the pack picking the
    // piece up there are no sound models to index with (owner report,
    // 2026-09-27: Sound / Both chosen before the piece was ready).
    private bool AudioAvailable => _audioReady;

    private void ShowVideoIndex()
    {
        bool audio = AudioAvailable;
        for (int i = 0; i < 3; ++i)
        {
            var media = (MvAiMedia)(i + 1);
            _videoIndex[i].IsEnabled = media == MvAiMedia.Pictures || audio;
            _videoIndex[i].IsChecked = media == _videoIndexValue;
        }
        // Installed but not yet picked up by the pack: say so, not "install".
        _videoIndexHint.Text = audio ? ""
            : _chrome.Host.IsPieceInstalled("ai-audio")
                ? "Audio is loading. Sound and Both turn on when it is ready."
                : "Install Audio above to index sounds and speech.";
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

    private void IndexAnyway()
    {
        try { _api.IndexAnyway(); }
        catch (MediaViewerException) { }
        _chrome.ReadStatus();
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
        // Export offers these folders; its button waits for one to exist.
        bool had = _rootsShown.Count > 0;
        _rootsShown = rows;
        if (had != (rows.Count > 0) && !_exportOpen) ShowTransfer();
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
            var bar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent])
            {
                Value = r.Assets <= 0 ? 0 : Math.Min(1, (double)r.Done / r.Assets),
            };
            bar.Root.Margin = new Thickness(0, 4, 0, 0);
            labels.Children.Add(bar.Root);
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
    // A Button with a Flyout, not a DropDownButton: that control has no default
    // style in this island host and fail-fasts when it enters the tree.
    private Button MediaMenu(ulong root, MvAiMedia current)
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
        var button = new Button
        {
            Content = "Videos: " + MediaNames[(int)current] + "  ▾",
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
        MvAiStatus s = _chrome.Status;
        _facesFlags = s.Flags & (MvAiStatus.FlagFacesReady | MvAiStatus.FlagFacesOn);
        _peopleShown = s.People;
        if (!_faces.IsOn) return;
        bool ready = _chrome.StatusValid && (s.Flags & MvAiStatus.FlagFacesReady) != 0;
        if (!ready && !_chrome.Host.IsPieceInstalled("ai-faces"))
        {
            _people.Children.Add(_look.Text("Install People in the list above to find people.", 13));
            return;
        }
        string found = s.People == 0 ? "No people found yet." : s.People == 1 ? "1 person found." : $"{s.People:N0} people found.";
        _people.Children.Add(Row("People", found + " Name them, merge the same person found twice, and split them in the people window.",
            _look.Button("People…", _chrome.OpenPeople)));
    }
}
