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
/// Settings → Local search once the pack is loaded (docs/design/17 PR 23, PR 24):
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
    // Under the folder list: why the last add or folder action did nothing, and
    // the Pictures / Videos folders no root covers yet (2026-10-05).
    private readonly TextBlock _rootError;
    private readonly StackPanel _suggest;
    // OneDrive online-only files (2026-10-05): fetch them to index them.
    private readonly FrameworkElement _cloudRow;
    private readonly ToggleSwitch _cloudSwitch;
    private readonly TextBlock _cloudDetail;
    private bool _cloudProvider;
    // This Windows has no Cloud Files API (before 10 1709): the option is shown, off and greyed.
    private bool _cloudUnsupported;
    // "Search index": the size on disk, the size limit and Clear; the Clear
    // confirmation under it while asked (the Mac's Index section).
    private readonly TextBlock _indexSize;
    private readonly ComboBox _capBox;
    private readonly List<long> _capValues = new();
    private long _capBytes;
    private readonly StackPanel _indexConfirm;
    // The folder whose "Remove…" was pressed: its row asks before the index forgets it.
    private ulong? _confirmRemove;
    // People, as on the Mac: the notices (the off confirmation, "Install
    // People"), Re-analyse, then the grid itself (PeopleGrid.cs).
    private readonly StackPanel _people;
    private readonly FrameworkElement _reanalyseRow;
    private readonly Border _rerunRow;
    private readonly TextBlock _rerunText;
    private readonly MediaViewer.Shared.FlatBar _rerunBar;
    private readonly TextBlock _reanalyseDetail;
    private readonly PeopleGrid _peopleGrid;
    private readonly ToggleSwitch _faces;
    private bool _updating;
    private bool _confirmClear;
    private bool _confirmFacesOff;
    private bool _cudaAvailable;
    private readonly ToggleButton[] _videoIndex = new ToggleButton[3];
    private readonly TextBlock _videoIndexHint;
    // #355: how audio files are indexed, or how to turn them on.
    private readonly TextBlock _audioFilesDetail;
    // #354: Settings "Documents" (doc_index: Off | Words | Words and pages,
    // the last slice D2's page pictures), or how to turn documents on.
    private readonly ToggleButton[] _docIndex = new ToggleButton[3];
    private readonly StackPanel _docIndexTrack;
    private readonly TextBlock _documentsDetail;
    private uint _docIndexValue = 1;
    private bool _pdfAvailable = true;
    private readonly TextBlock _audioLine;
    // Text in pictures (#403): "Read text in photos" (read_text, on by default),
    // greyed with a reason when this Windows has no OCR language.
    private readonly ToggleSwitch _readText;
    private readonly TextBlock _readTextDetail;
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

    public StackPanel Root { get; private set; }

    public ManagePanel(AiChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        Root = new StackPanel { Spacing = 10, Margin = new Thickness(0, 8, 0, 0) };

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
        Root.Children.Add(Stroked(_look.Card(statusRow, 12)));

        // Sections as on the Mac (ManagementView.section): a 15 semibold title
        // over one card with a hairline stroke, its rows inside.
        StackPanel compute = Section("Compute");
        _computeBox = Combo();
        _computeBox.SelectionChanged += (_, _) =>
        {
            if (_updating || _computeBox.SelectedIndex < 0) return;
            Set("compute", ((uint)_computeValues[_computeBox.SelectedIndex]).ToString());
        };
        var computeDetail = new StackPanel { Spacing = 2 };
        computeDetail.Children.Add(_look.Text(
            "CPU is always there underneath; changing it does not re-index.", 12));
        _restart = _look.Text("Restart MediaViewer to use NVIDIA acceleration.", 12, AddonColour.Accent);
        _restart.Visibility = Visibility.Collapsed;
        computeDetail.Children.Add(_restart);
        compute.Children.Add(Row("Where indexing runs", computeDetail, _computeBox));

        // No "Search quality" picker (owner, 2026-09-28: one scale, not two):
        // the engine's Auto picks the larger model where CUDA runs it quickly,
        // the smaller one on CPU only. A Fast or High chosen before stays as
        // it was, said here with a way back to Auto; nothing rewrites it
        // behind the person's back.
        _legacyQualityLine = _look.Text("", 12);
        _legacyQualityRow = Row("Search model", _legacyQualityLine,
            _look.Button("Use Auto", () => Set("quality", ((uint)MvAiQuality.Auto).ToString())));
        _legacyQualityRow.Visibility = Visibility.Collapsed;
        compute.Children.Add(_legacyQualityRow);

        StackPanel search = Section("Search");
        search.Children.Add(Row("Search panel", "Open the search panel. Ctrl+F in the viewer; Ctrl+Shift+F finds similar.",
            _look.Button("Open search", () => _chrome.RunCommand(SearchCommand.Open))));

        // Precision (docs/design/17 "Precision scale"): five steps, the middle the
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
        search.Children.Add(Row("Precision",
            "How closely a result must match what you type. Stricter shows only close matches and says “nothing found” rather than a near miss (a plane for “helicopter”); Broader shows more, including looser matches.",
            precisionControl));

        // Audio (2026-09-27): what videos are indexed for. Sound covers both
        // sounds ("dog barking") and speech, and needs the Audio piece. The
        // search panel's own segmented track (Look.Track / Look.Segment).
        bool hc = Look.HighContrast();
        var segments = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        string[] names = { "Pictures", "Sound", "Both" };
        for (int i = 0; i < 3; ++i)
        {
            MvAiMedia media = (MvAiMedia)(i + 1);
            ToggleButton b = _look.Segment(names[i], () => SetVideoIndex(media), hc);
            AutomationProperties.SetName(b, "Index videos for " + names[i]);
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
        Section("Videos").Children.Add(Row("Index videos for", videoDetail, _look.Track(segments, hc)));
        // #354: documents, by their words (and with D2 their pages' look), while
        // File types "Show documents" is on.
        var docSegments = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        string[] docNames = { "Off", "Words", "Words and pages" };
        for (int i = 0; i < docNames.Length; ++i)
        {
            uint level = (uint)i;
            ToggleButton b = _look.Segment(docNames[i], () => SetDocIndex(level), hc);
            AutomationProperties.SetName(b, "Index documents for " + docNames[i]);
            _docIndex[i] = b;
            docSegments.Children.Add(b);
        }
        _docIndexTrack = new StackPanel();
        _docIndexTrack.Children.Add(_look.Track(docSegments, hc));
        _documentsDetail = _look.Text(DocumentsDetail(_chrome.DocumentFiles, _pdfAvailable), 12);
        Section("Documents").Children.Add(Row("PDF and Word documents", _documentsDetail, _docIndexTrack));
        ShowDocIndex();
        _audioFilesDetail = _look.Text(AudioFilesDetail(_chrome.AudioFiles), 12);
        Section("Audio files").Children.Add(Row("MP3, M4A and M4P", _audioFilesDetail, new StackPanel()));

        _readText = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 48, IsOn = true };
        AutomationProperties.SetName(_readText, "Read text in photos");
        _readText.Toggled += (_, _) =>
        {
            if (_updating) return;
            Set("read_text", _readText.IsOn ? "1" : "0");
        };
        _readTextDetail = _look.Text(ReadTextDetail(true), 12);
        Section("Text in pictures").Children.Add(Row("Read text in photos", _readTextDetail, _readText));

        // Indexed folders: one row each with a hairline between, then Add.
        StackPanel folders = Section("Indexed folders");
        _roots = new StackPanel();
        folders.Children.Add(_roots);
        var add = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        // A folder is always indexed with its subfolders (owner 2026-10-03).
        add.Children.Add(_look.Button("Add a folder…", () => _ = AddFolder()));
        // Pictures and Videos where Windows keeps them (moved to OneDrive or
        // not), offered while no folder covers them: never added unasked.
        _suggest = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        add.Children.Add(_suggest);
        folders.Children.Add(Pad(add));
        _rootError = _look.Text("", 12, AddonColour.Accent);
        _rootError.Margin = new Thickness(12, 0, 12, 10);
        _rootError.Visibility = Visibility.Collapsed;
        folders.Children.Add(_rootError);

        _cloudSwitch = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 48 };
        _cloudSwitch.Toggled += (_, _) =>
        {
            if (_updating) return;
            Set("cloud_files", _cloudSwitch.IsOn ? "1" : "0");
        };
        _cloudDetail = _look.Text("", 12);
        var cloud = new StackPanel();
        cloud.Children.Add(Divider(12));
        cloud.Children.Add(Row("Index online-only OneDrive files", _cloudDetail, _cloudSwitch));
        _cloudRow = cloud;
        _cloudRow.Visibility = Visibility.Collapsed;
        folders.Children.Add(_cloudRow);

        // Index: its size against the limit (index_cap_bytes; over it indexing
        // stops and the status says so, docs/design/17 "Disk"), Clear, battery.
        StackPanel index = Section("Index");
        _capBox = Combo();
        _capBox.MinWidth = 110;
        AutomationProperties.SetName(_capBox, "Index size limit");
        ToolTipService.SetToolTip(_capBox, "Index size limit. Over it, indexing stops until you raise it or remove a folder.");
        _capBox.SelectionChanged += (_, _) =>
        {
            if (_updating || _capBox.SelectedIndex < 0) return;
            Set("index_cap_bytes", _capValues[_capBox.SelectedIndex].ToString(System.Globalization.CultureInfo.InvariantCulture));
        };
        Button clear = _look.Button("Clear index…", () =>
        {
            _confirmClear = true;
            ShowIndexConfirm(focus: true);
        });
        var indexControls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        indexControls.Children.Add(_capBox);
        indexControls.Children.Add(clear);
        _indexSize = _look.Text("", 12);
        FrameworkElement indexRow = Row("Search index", _indexSize, indexControls);
        index.Children.Add(indexRow);
        _indexConfirm = new StackPanel();
        index.Children.Add(_indexConfirm);

        _batteryBox = Combo();
        foreach (string b in BatteryNames) _batteryBox.Items.Add(b);
        _batteryBox.SelectionChanged += (_, _) =>
        {
            if (_updating || _batteryBox.SelectedIndex < 0) return;
            Set("pause_on_battery_percent", BatteryValues[_batteryBox.SelectedIndex].ToString());
        };
        index.Children.Add(Row("Pause on battery", "Indexing waits while the battery is below this. Index anyway, beside the status, carries on until you next plug in.", _batteryBox));

        // Import and export (docs/design/17 "Sharing an index"; ManagePanel.Transfer.cs).
        AddTransferSection();

        // People: a separate opt-in (PR 24; biometric data, stricter than the frame index).
        StackPanel people = Section("People");
        _faces = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 48 };
        AutomationProperties.SetName(_faces, "Find people in your photos");
        _faces.Toggled += (_, _) => OnFacesToggled();
        var facesDetail = new StackPanel { Spacing = 2 };
        facesDetail.Children.Add(_look.Text(
            "Face data stays on this computer unless you include People in an index export, and can be deleted at any time. Off by default.", 12));
        people.Children.Add(Row("Find people in your photos", facesDetail, _faces));
        _people = new StackPanel();
        people.Children.Add(_people);
        // "Re-analyse faces" (docs/design/17 "People model"): every photo and clip
        // again with the pack's face model; the people carry over. While it runs,
        // how far it is (a FlatBar: ProgressBar fail-fasts in this island host).
        _reanalyseDetail = _look.Text("", 12);
        _reanalyseRow = Row("Re-analyse faces", _reanalyseDetail, _look.Button("Re-analyse", Reanalyse));
        _reanalyseRow.Visibility = Visibility.Collapsed;
        people.Children.Add(_reanalyseRow);
        _rerunText = _look.Text("", 12, AddonColour.Title);
        _rerunBar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]);
        var rerun = new StackPanel { Spacing = 6 };
        rerun.Children.Add(_rerunText);
        rerun.Children.Add(_rerunBar.Root);
        _rerunRow = Pad(rerun);
        _rerunRow.Visibility = Visibility.Collapsed;
        people.Children.Add(_rerunRow);
        // The grid, inline as on the Mac (PeopleGrid.cs).
        _peopleGrid = new PeopleGrid(_chrome);
        _peopleGrid.Root.Visibility = Visibility.Collapsed;
        people.Children.Add(_peopleGrid.Root);

        _chrome.StatusChanged += OnStatus;
        RefreshSettings();
        RefreshRoots();
        OnStatus();
        // Settings keeps this panel and re-attaches it each time it opens
        // (IslandHost.LocalSearch): read everything again then, as the Mac's
        // appeared() does, so it never shows what was true when it was built
        // (owner report, 2026-10-05: "0 of 26" long after the folder finished).
        Root.Loaded += (_, _) =>
        {
            RefreshSettings();
            RefreshRoots();
            _chrome.ReadStatus();
        };

        // A later window's reader (AiChrome.ReadOnly): the note and Open search
        // only. The full panel above is still built, detached, so its refreshes
        // land on nothing a person sees.
        if (_chrome.ReadOnly)
        {
            Root = new StackPanel { Spacing = 10, Margin = new Thickness(0, 8, 0, 0) };
            Root.Children.Add(Stroked(_look.Card(_look.Text(AiChrome.ReadOnlyNote, 13), 12)));
            Section("Search").Children.Add(Row("Search panel", "Open the search panel. Ctrl+F in the viewer; Ctrl+Shift+F finds similar.",
                _look.Button("Open search", () => _chrome.RunCommand(SearchCommand.Open))));
        }
    }

    // ---- building blocks -------------------------------------------------------------

    private ComboBox Combo() => new() { FontFamily = _look.Font, FontSize = _look.FontSize - 1, MinWidth = 200 };

    /// <summary>
    /// A titled section (the Mac's ManagementView.section): a 15 semibold
    /// title over one surface card with a hairline stroke. Added to the panel;
    /// returns the card's body for its rows.
    /// </summary>
    private StackPanel Section(string title)
    {
        var body = new StackPanel();
        TextBlock heading = _look.Text(title, 15, AddonColour.Title);
        heading.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        heading.Margin = new Thickness(0, 8, 0, 6);
        AutomationProperties.SetHeadingLevel(heading, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        var section = new StackPanel();
        section.Children.Add(heading);
        section.Children.Add(Stroked(new Border
        {
            Child = body,
            CornerRadius = new CornerRadius(8),
            Background = _look[AddonColour.Surface],
        }));
        Root.Children.Add(section);
        return body;
    }

    private Border Stroked(Border card)
    {
        card.BorderBrush = _look[AddonColour.Hairline];
        card.BorderThickness = new Thickness(1);
        return card;
    }

    /// <summary>A hairline between rows of a section, inset by <paramref name="inset"/>.</summary>
    private Border Divider(double inset = 0) => new()
    {
        Height = 1,
        Margin = new Thickness(inset, 0, inset, 0),
        Background = _look[AddonColour.Hairline],
    };

    /// <summary>Content inside a section, with a row's padding.</summary>
    private static Border Pad(UIElement child) => new() { Child = child, Padding = new Thickness(12) };

    private FrameworkElement Row(string title, string detail, FrameworkElement control) =>
        Row(title, _look.Text(detail, 12), control);

    /// <summary>A row of a section (the Mac's row): a 14 title and its 12 detail, the control at the end.</summary>
    private FrameworkElement Row(string title, FrameworkElement detail, FrameworkElement control)
    {
        var grid = new Grid { ColumnSpacing = 20, Padding = new Thickness(12) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var labels = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Center };
        labels.Children.Add(_look.Text(title, 14, AddonColour.Title));
        labels.Children.Add(detail);
        grid.Children.Add(labels);
        control.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetName(control, title);
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        return grid;
    }

    /// <summary>
    /// "Delete …? [Delete] [Cancel]" under the row that asked (the Mac's
    /// confirmRow): an accent wash, the action in the critical red when it
    /// deletes, and Esc as Cancel so it does not close Settings.
    /// </summary>
    private Grid ConfirmRow(string text, string action, Action perform, Action cancel, bool focus)
    {
        var grid = new Grid { ColumnSpacing = 10, Padding = new Thickness(12), Background = _look.LiveTint(AddonColour.Accent, 20) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock label = _look.Text(text, 12, AddonColour.Title);
        label.VerticalAlignment = VerticalAlignment.Center;
        grid.Children.Add(label);
        Button go = _look.DestructiveButton(action, perform);
        go.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(go, 1);
        grid.Children.Add(go);
        Button no = _look.Button("Cancel", cancel);
        no.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(no, 2);
        grid.Children.Add(no);
        grid.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape) return;
            e.Handled = true;
            cancel();
        };
        // Keyboard focus follows the question, on the safe answer.
        if (focus) no.Loaded += (_, _) => no.Focus(FocusState.Programmatic);
        return grid;
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
            _audioFilesDetail.Text = AudioFilesDetail(_chrome.AudioFiles);
            _docIndexValue = r.TryGetProperty("doc_index", out JsonElement di) ? Math.Min(2u, di.GetUInt32()) : 1;
            _pdfAvailable = !r.TryGetProperty("pdf_available", out JsonElement pa) || pa.ValueKind != JsonValueKind.False;
            ShowDocIndex();
            _cloudProvider = r.TryGetProperty("cloud_provider", out JsonElement cp) && cp.GetString() == "onedrive";
            _cloudUnsupported = r.TryGetProperty("cloud_supported", out JsonElement cs) && cs.ValueKind == JsonValueKind.False;
            _cloudSwitch.IsEnabled = !_cloudUnsupported;
            _cloudSwitch.IsOn = r.TryGetProperty("cloud_files", out JsonElement cf) && cf.ValueKind == JsonValueKind.True;
            bool textAvailable = !r.TryGetProperty("text_available", out JsonElement ta) || ta.ValueKind != JsonValueKind.False;
            _readText.IsEnabled = textAvailable;
            _readText.IsOn = textAvailable && (!r.TryGetProperty("read_text", out JsonElement rt) || rt.ValueKind != JsonValueKind.False);
            _readTextDetail.Text = ReadTextDetail(textAvailable);
            ShowCap(r.TryGetProperty("index_cap_bytes", out JsonElement ic) && ic.TryGetInt64(out long cap)
                ? Math.Max(0, cap) : 8_000_000_000);
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
        ShowCloud(s);
        uint faces = s.Flags & (MvAiStatus.FlagFacesReady | MvAiStatus.FlagFacesOn);
        if (faces != _facesFlags) RefreshPeople();
        else if (s.People != _peopleShown)
        {
            // People found: the grid reads again (not rebuilt, at most twice a second).
            _peopleShown = s.People;
            _peopleGrid.Refresh();
        }
        UpdateRerun(s);
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

    // ---- documents (#354) ------------------------------------------------------------------

    private void ShowDocIndex()
    {
        bool on = _chrome.DocumentFiles;
        _docIndexTrack.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        for (int i = 0; i < _docIndex.Length; ++i) _docIndex[i].IsChecked = _docIndexValue == i;
        _documentsDetail.Text = DocumentsDetail(on, _pdfAvailable);
    }

    private void SetDocIndex(uint level)
    {
        if (level == _docIndexValue)
        {
            ShowDocIndex();
            return;
        }
        _docIndexValue = level;
        ShowDocIndex();
        Set("doc_index", level.ToString());
        RefreshRoots();
    }

    // #354: the Settings line under "Documents". Indexed only while File types
    // "Show documents" is on; then by their words, the text kept on this PC.
    private static string DocumentsDetail(bool on, bool pdf) => !on
        ? "Not searched. Turn on Show documents in Settings → File types to search them."
        : "Words finds PDFs and Word documents by the words in them, and opens one on the page that matched. "
          + "Words and pages also finds pages by what they look like (a receipt, a floor plan, a slide): the first pages of each, "
          + "and any page that is mostly a picture. "
          + "Each folder can differ, from its menu below. An index of documents holds their text on this PC; it never leaves it."
          + (pdf ? "" : " PDFs need the latest Core: update it above. Word documents are read meanwhile.");

    // ---- OneDrive online-only files -----------------------------------------------------

    // Always shown on Windows (owner, 2026-10-07: OneDrive support is a setting
    // you can turn on or off). Off, online-only files are skipped and everything
    // else is searchable as usual: it never makes Local search unavailable. On a
    // Windows without the Cloud Files API it is greyed and says why. The text is
    // kept stable at 4 Hz: only its words change.
    private void ShowCloud(in MvAiStatus s)
    {
        bool show = _cloudProvider || _cloudUnsupported;
        _cloudRow.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (!show) return;
        if (_cloudUnsupported)
        {
            _cloudDetail.Text = "Not available on this version of Windows: it needs Windows 10 version 1709 or later. " +
                "Local search works without it; files kept only in OneDrive are skipped.";
            return;
        }
        string n = s.CloudFilesLeft.ToString("N0", System.Globalization.CultureInfo.CurrentCulture);
        string left = s.CloudFilesLeft == 0 && s.CloudFilesFetched == 0 && !_cloudSwitch.IsOn
            ? "No files in your folders are kept only in OneDrive right now."
            : s.CloudFilesLeft == 0 ? "Every online-only file in your folders is indexed."
            : s.CloudFilesLeft == 1 ? "1 file in your folders is only in OneDrive, so it is not searchable yet."
            : $"{n} files in your folders are only in OneDrive, so they are not searchable yet.";
        string how = " When on, MediaViewer downloads a couple at a time while this PC is plugged in and on an unmetered connection, indexes them, and makes them online-only again." +
            (_cloudSwitch.IsOn ? "" : " Off, they are skipped; everything else is searchable as usual.");
        string? wait = _cloudSwitch.IsOn ? Look.CloudWait(s) : null;
        _cloudDetail.Text = left + how + (wait is null ? "" : " " + wait);
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
    internal void Detach()
    {
        _chrome.StatusChanged -= OnStatus;
        _peopleGrid.Detach();
    }

    // The size line follows the status (4 Hz): only its words change, so the
    // limit box and Clear keep keyboard focus.
    private void RefreshIndexRow(in MvAiStatus s)
    {
        _indexSize.Text = $"{MediaViewer.Shared.ByteSize.Text((long)s.IndexBytes)} on disk" +
                          (_capBytes > 0 ? $" of at most {CapText(_capBytes)}" : "") +
                          ". It is your data: it does not count toward the 5 GB.";
    }

    private void ShowIndexConfirm(bool focus = false)
    {
        _indexConfirm.Children.Clear();
        if (!_confirmClear) return;
        _indexConfirm.Children.Add(ConfirmRow(
            "Clear the whole index? Searching needs indexing again. Thumbnails, your files and your folders list stay.",
            "Clear the index", ClearIndex, () =>
            {
                _confirmClear = false;
                ShowIndexConfirm();
            }, focus));
    }

    // The Mac's choices (ManagementView "Index size limit"), plus the stored
    // value when it is none of them (the engine's 8 GB default, or no limit),
    // so the box never reads blank.
    private static readonly long[] CapChoices =
        { 2_000_000_000, 5_000_000_000, 10_000_000_000, 20_000_000_000, 50_000_000_000 };

    private static string ReadTextDetail(bool available) => available
        ? "Finds photos and video moments by the words in them: a sign, a shirt, a page. Windows reads them on this PC; nothing leaves it."
        : "Windows has no text recognition for your languages, so photos are not searched by the words in them. Add a language with OCR in Settings → Time & language.";

    private static string CapText(long bytes) =>
        bytes <= 0 ? "No limit" : $"{bytes / 1e9:0.#} GB";

    private void ShowCap(long cap)
    {
        _capBytes = cap;
        _capValues.Clear();
        _capValues.AddRange(CapChoices);
        if (!_capValues.Contains(cap)) _capValues.Add(cap);
        // Smallest first, "No limit" last.
        _capValues.Sort((x, y) => (x <= 0 ? long.MaxValue : x).CompareTo(y <= 0 ? long.MaxValue : y));
        _capBox.Items.Clear();
        foreach (long v in _capValues) _capBox.Items.Add(CapText(v));
        _capBox.SelectedIndex = _capValues.IndexOf(cap);
    }

    private void ClearIndex()
    {
        _confirmClear = false;
        ShowIndexConfirm();
        try { _api.ClearIndex(); }
        catch (MediaViewerException) { }
        _chrome.ReadStatus();
    }

    // ---- roots ----------------------------------------------------------------------

    // Held (issue #299): left unfinished last session, it waits for Resume.
    private sealed record RootRow(ulong Id, string Path, bool Recursive, bool Enabled, long Assets, long Done, long Bytes,
                                  MvAiMedia Media, bool Scanning, long CloudOnly, bool Unreadable, long LastScan,
                                  bool Held, long Audio, MvAiDocs Docs, long Documents, long DocumentsDone);

    // #355: the Settings line under "Audio files". Indexed only while File
    // types "Show audio files" is on; then by their tags and cover art.
    private static string AudioFilesDetail(bool on) => on
        ? "Found by their tags (title, artist, album) and cover art."
        : "Not searched. Turn on Show audio files in Settings → File types to search them.";

    // The newest roots read: an older one finishing late never draws over it.
    private int _rootsRead;
    // A row's button was pressed: redraw even if the numbers did not move, so
    // the button it disabled comes back.
    private bool _rootsPressed;

    internal void RefreshRoots()
    {
        AiApi api = _api;
        int read = ++_rootsRead;
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
                            ? (MvAiMedia)m.GetUInt32() : MvAiMedia.Default,
                        r.TryGetProperty("scanning", out JsonElement sc) && sc.ValueKind == JsonValueKind.True,
                        r.TryGetProperty("cloud_only", out JsonElement co) ? co.GetInt64() : 0,
                        r.TryGetProperty("error", out JsonElement er) && er.GetString() == "unreadable",
                        r.TryGetProperty("last_scan", out JsonElement ls) ? ls.GetInt64() : 0,
                        r.TryGetProperty("held", out JsonElement hd) && hd.ValueKind == JsonValueKind.True,
                        r.TryGetProperty("audio", out JsonElement au) ? au.GetInt64() : 0,
                        r.TryGetProperty("docs", out JsonElement ds) && ds.GetUInt32() <= 3
                            ? (MvAiDocs)ds.GetUInt32() : MvAiDocs.Default,
                        r.TryGetProperty("documents", out JsonElement dn) ? dn.GetInt64() : 0,
                        r.TryGetProperty("documents_done", out JsonElement dd) ? dd.GetInt64() : 0));
                }
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException
                                           or InvalidOperationException) { }
            _chrome.Host.Post(() =>
            {
                if (read == _rootsRead) ShowRoots(rows);
            });
        });
    }

    private void ShowRoots(List<RootRow> rows)
    {
        // The engine posts AI_ROOTS as each folder's count moves (~1 s while
        // indexing): unchanged rows are left alone, so a button keeps focus.
        if (!_rootsPressed && _roots.Children.Count > 0 && rows.SequenceEqual(_rootsShown)) return;
        _rootsPressed = false;
        // Export offers these folders; its button waits for one to exist.
        bool had = _rootsShown.Count > 0;
        _rootsShown = rows;
        if (had != (rows.Count > 0) && !_exportOpen) ShowTransfer();
        _roots.Children.Clear();
        ShowSuggestions();
        if (rows.Count == 0)
        {
            _confirmRemove = null;
            _roots.Children.Add(Pad(_look.Text(
                "No folders yet. Open a folder and press Ctrl+F to index it, or add one here.", 12)));
            return;
        }
        if (_confirmRemove is ulong asked && !rows.Any(r => r.Id == asked)) _confirmRemove = null;
        bool focusConfirm = _focusConfirm;
        _focusConfirm = false;
        foreach (RootRow r in rows)
        {
            var grid = new Grid { ColumnSpacing = 12, RowSpacing = 6, Padding = new Thickness(12) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var labels = new StackPanel { Spacing = 2 };
            // A folder icon before the path, as on the Mac.
            var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            head.Children.Add(new FontIcon
            {
                Glyph = GlyphFolder,
                FontFamily = Look.IconFont,
                FontSize = 13,
                Foreground = _look[AddonColour.Body],
                VerticalAlignment = VerticalAlignment.Center,
            });
            TextBlock path = _look.Text(r.Path, 13, AddonColour.Title, wrap: false);
            path.VerticalAlignment = VerticalAlignment.Center;
            ToolTipService.SetToolTip(path, r.Path);
            head.Children.Add(path);
            labels.Children.Add(head);
            // Online-only files wait for the OneDrive fetch: the rest can be up to date.
            long local = Math.Max(0, r.Assets - r.CloudOnly);
            string state = !r.Enabled ? "Paused"
                : r.Unreadable ? "Couldn't read this folder: it may be offline, moved or renamed. Its index is kept."
                : r.Assets == 0 && r.Documents == 0 && r.LastScan != 0 && !r.Scanning ? "No photos or videos found here"
                : r.Assets == 0 && r.Documents > 0 ? $"{r.DocumentsDone:N0} of {r.Documents:N0} documents read"
                : r.Done >= local ? (r.Scanning ? "Checking for changes…" : "Up to date")
                : $"{r.Done:N0} of {local:N0}"
                  + (r.Held ? " · paused since last time" : r.Scanning ? " · checking for changes…" : "");
            if (r.CloudOnly > 0 && !r.Unreadable) state += $" · {r.CloudOnly:N0} only in OneDrive";
            if (r.Audio > 0 && !r.Unreadable) state += r.Audio == 1 ? " · 1 audio file" : $" · {r.Audio:N0} audio files";
            // #354: its documents, counted apart from its photos and videos.
            if (r.Documents > 0 && r.Assets > 0 && !r.Unreadable)
            {
                state += r.Documents == 1 ? " · 1 document" : $" · {r.Documents:N0} documents";
                if (r.DocumentsDone < r.Documents) state += $" ({r.DocumentsDone:N0} read)";
            }
            labels.Children.Add(_look.Text((r.Recursive ? "and subfolders · " : "") + state + " · " + MediaViewer.Shared.ByteSize.Text(r.Bytes), 12,
                r.Unreadable ? AddonColour.Accent : AddonColour.Body));
            // Paused folders' bars are quiet (the Mac tints them secondary).
            var bar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline],
                                                     _look[r.Enabled ? AddonColour.Accent : AddonColour.Body])
            {
                Value = local <= 0 ? 0 : Math.Min(1, (double)r.Done / local),
            };
            bar.Root.Margin = new Thickness(0, 4, 0, 0);
            labels.Children.Add(bar.Root);
            grid.Children.Add(labels);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
            ulong id = r.Id;
            bool enabled = r.Enabled;
            string name = System.IO.Path.GetFileName(r.Path);
            buttons.Children.Add(MediaMenu(id, r.Media));
            if (_chrome.DocumentFiles && _api.HasRootSetDocuments) buttons.Children.Add(DocsMenu(id, r.Docs));
            if (enabled && r.Held)
            {
                // Issue #299: on from its last committed frame, as the launch notice's Resume.
                Button resume = RootButton("Resume", () => _api.SetSetting("resume_root", id.ToString(System.Globalization.CultureInfo.InvariantCulture)));
                ToolTipService.SetToolTip(resume, "Indexing stopped part-way last time. Resume goes on from where it stopped.");
                buttons.Children.Add(resume);
            }
            else
            {
                buttons.Children.Add(RootButton(enabled ? "Pause" : "Resume", () => _api.RootSetEnabled(id, !enabled)));
            }
            Button rescan = RootButton(r.Scanning ? "Checking…" : "Rescan", () => _api.RootRescan(id));
            rescan.IsEnabled = !r.Scanning && enabled;
            buttons.Children.Add(rescan);
            if (_confirmRemove != id)
            {
                // Removing forgets the folder and deletes its rows: ask first (the Mac's "Remove…").
                Button remove = _look.Button("Remove…", () => AskRemove(id));
                ToolTipService.SetToolTip(remove, "Forget this folder and delete its rows from the index. Your files are not touched.");
                buttons.Children.Add(remove);
            }
            foreach (UIElement b in buttons.Children)
            {
                if (b is Button button && button.Content is string label)
                {
                    AutomationProperties.SetName(button, $"{label.TrimEnd('…')} {name}");
                }
            }
            Grid.SetColumn(buttons, 1);
            grid.Children.Add(buttons);
            _roots.Children.Add(grid);
            if (_confirmRemove == id)
            {
                _roots.Children.Add(ConfirmRow($"Remove {name} from the index? Its rows are deleted; your files are not touched.",
                    "Remove from index", () =>
                    {
                        _confirmRemove = null;
                        RootCall(() => _api.RootRemove(id));
                    }, () => AskRemove(null), focusConfirm));
            }
            _roots.Children.Add(Divider());
        }
    }

    // Segoe Fluent Icons "Folder".
    private const string GlyphFolder = "\uE8B7";

    // Set while a "Remove…" redraws the list: focus goes to the question.
    private bool _focusConfirm;

    private void AskRemove(ulong? id)
    {
        _confirmRemove = id;
        _focusConfirm = id is not null;
        _rootsPressed = true;
        ShowRoots(_rootsShown);
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

    private static readonly string[] DocsNames = { "Default", "Off", "Words", "Words and pages" };
    private static readonly string[] DocLevelNames = { "Off", "Words", "Words and pages" };

    // #354: "Documents: Default ▾" — whether this folder's documents are searched
    // by their words, and (D2) their pages' look (a Downloads folder that should
    // not show bank statements).
    private Button DocsMenu(ulong root, MvAiDocs current)
    {
        var shown = (int)current < DocsNames.Length ? current : MvAiDocs.Default;
        var menu = new MenuFlyout();
        for (int i = 0; i < DocsNames.Length; ++i)
        {
            var docs = (MvAiDocs)i;
            var item = new ToggleMenuFlyoutItem
            {
                Text = i == 0 ? $"Default ({DocLevelNames[Math.Min(2u, _docIndexValue)]})" : DocsNames[i],
                IsChecked = docs == shown,
            };
            item.Click += (_, _) => RootCall(() => _api.RootSetDocuments(root, docs));
            menu.Items.Add(item);
        }
        var button = new Button
        {
            Content = "Documents: " + DocsNames[(int)shown] + "  ▾",
            FontFamily = _look.Font,
            FontSize = _look.FontSize - 2,
            Padding = new Thickness(12, 5, 8, 6),
            CornerRadius = new CornerRadius(6),
            Flyout = menu,
        };
        AutomationProperties.SetName(button, "Index this folder's documents for");
        ToolTipService.SetToolTip(button, "Whether this folder's PDFs and Word documents are searched by their words, and by what their pages look like");
        return button;
    }

    // A row button answers at once: it greys out until the row is read again
    // (the engine posts AI_ROOTS for every root change and each scan's start
    // and end), so a click never looks like it did nothing.
    private Button RootButton(string label, Action call)
    {
        Button self = null!;
        self = _look.Button(label, () =>
        {
            self.IsEnabled = false;
            RootCall(call);
        });
        return self;
    }

    private void RootCall(Action call)
    {
        // Said, not swallowed (2026-10-05: a folder add that did nothing looked
        // like indexing that never ran).
        string? error = null;
        try { call(); }
        catch (MediaViewerException ex)
        {
            error = ex.Status switch
            {
                MvStatus.Busy => "An import is running. Try again when it finishes.",
                MvStatus.InvalidArg => "That isn't a folder on this PC.",
                MvStatus.PermissionDenied => "MediaViewer isn't allowed to read that folder.",
                _ => "That didn't work. Try again, or pick the folder another way.",
            };
        }
        ShowRootError(error);
        _rootsPressed = true;
        RefreshRoots();
        _chrome.RefreshCoverage();
        _chrome.ReadStatus();
    }

    private void ShowRootError(string? text)
    {
        _rootError.Text = text ?? "";
        _rootError.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
    }

    private async Task AddFolder()
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
        string path = folder.Path;
        // A library (the Pictures entry in the picker's side bar) has no path of
        // its own: index the folder it saves into, where Windows keeps them.
        if (string.IsNullOrEmpty(path)) path = await LibraryFolder(folder) ?? "";
        if (string.IsNullOrEmpty(path))
        {
            ShowRootError("That is a library, not a folder. Pick the folder itself, for example Pictures under This PC.");
            return;
        }
        RootCall(() => _api.IndexFolder(path, recursive: true));
    }

    private static async Task<string?> LibraryFolder(Windows.Storage.StorageFolder picked)
    {
        foreach (Windows.Storage.KnownLibraryId id in new[]
                 {
                     Windows.Storage.KnownLibraryId.Pictures, Windows.Storage.KnownLibraryId.Videos,
                     Windows.Storage.KnownLibraryId.Documents, Windows.Storage.KnownLibraryId.Music,
                 })
        {
            try
            {
                Windows.Storage.StorageLibrary lib = await Windows.Storage.StorageLibrary.GetLibraryAsync(id);
                if (lib.SaveFolder is { } save && !string.IsNullOrEmpty(save.Path) &&
                    string.Equals(save.DisplayName, picked.DisplayName, StringComparison.CurrentCultureIgnoreCase))
                {
                    return save.Path;
                }
            }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException) { }
        }
        // Fall back on the Known Folder of the same name (Pictures moved to OneDrive included).
        string pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        foreach (string known in new[] { pictures, videos })
        {
            if (known.Length > 0 &&
                string.Equals(System.IO.Path.GetFileName(known), picked.DisplayName, StringComparison.CurrentCultureIgnoreCase))
            {
                return known;
            }
        }
        return null;
    }

    // "Index Pictures" / "Index Videos": the user's own folders, wherever
    // Windows keeps them, while no folder covers them (folder_coverage).
    private void ShowSuggestions()
    {
        _suggest.Children.Clear();
        (string Name, string Path)[] known =
        {
            ("Pictures", Environment.GetFolderPath(Environment.SpecialFolder.MyPictures)),
            ("Videos", Environment.GetFolderPath(Environment.SpecialFolder.MyVideos)),
        };
        foreach ((string name, string dir) in known)
        {
            if (dir.Length == 0 || !System.IO.Directory.Exists(dir)) continue;
            uint covered;
            try { covered = _api.FolderCoverage(dir); }  // [no-block]
            catch (MediaViewerException) { continue; }
            if (covered != 0) continue;
            Button b = _look.Button("Index " + name, () => RootCall(() => _api.IndexFolder(dir, recursive: true)));
            ToolTipService.SetToolTip(b, dir);
            AutomationProperties.SetName(b, $"Index your {name} folder and its subfolders");
            _suggest.Children.Add(b);
        }
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
        MvAiStatus s = _chrome.Status;
        _facesFlags = s.Flags & (MvAiStatus.FlagFacesReady | MvAiStatus.FlagFacesOn);
        _peopleShown = s.People;
        bool ready = _chrome.StatusValid && (s.Flags & MvAiStatus.FlagFacesReady) != 0;
        _peopleGrid.Root.Visibility = _faces.IsOn && !_confirmFacesOff && ready ? Visibility.Visible : Visibility.Collapsed;
        UpdateRerun(s);
        if (_confirmFacesOff)
        {
            // Off deletes every face vector, crop and name: confirm first (the Mac's row).
            _people.Children.Add(ConfirmRow("Turn off people search? Every face, crop and name is deleted now.",
                "Delete face data", () =>
                {
                    _confirmFacesOff = false;
                    try { _api.FacesEnable(false); }
                    catch (MediaViewerException) { }
                    RefreshSettings();
                    _chrome.ReadStatus();
                }, () =>
                {
                    _confirmFacesOff = false;
                    _updating = true;
                    _faces.IsOn = true;
                    _updating = false;
                    RefreshPeople();
                }, focus: true));
            return;
        }
        if (!_faces.IsOn) return;
        if (!ready)
        {
            TextBlock notice = _look.Text(_chrome.Host.IsPieceInstalled("ai-faces")
                ? "People is loading. Faces are found once it is ready."
                : "Install People above to find faces. Until then nothing about faces is computed.", 12);
            notice.Margin = new Thickness(12, 0, 12, 10);
            _people.Children.Add(notice);
            return;
        }
        _peopleGrid.Refresh();
    }

    private void Reanalyse()
    {
        try { _api.PeopleReanalyse(); }
        catch (MediaViewerException)
        {
            _peopleGrid.Note("Faces could not be re-analysed. Try again.");
            return;
        }
        _chrome.RequestStatus();
    }

    /// <summary>"Re-analyse faces": the model in use and a button, or while it runs how far it is.</summary>
    private void UpdateRerun(in MvAiStatus s)
    {
        bool people = _faces.IsOn && !_confirmFacesOff && _chrome.StatusValid && (s.Flags & MvAiStatus.FlagFacesReady) != 0;
        bool settling = (s.Flags & MvAiStatus.FlagPeopleSettling) != 0;
        bool running = (s.Flags & MvAiStatus.FlagPeopleRerun) != 0;
        _reanalyseRow.Visibility = people && !running && !settling ? Visibility.Visible : Visibility.Collapsed;
        _rerunRow.Visibility = people && (running || settling) ? Visibility.Visible : Visibility.Collapsed;
        _rerunBar.IsIndeterminate = settling || s.PeopleScanTotal == 0;
        _rerunBar.Value = s.PeopleScanTotal == 0 ? 0 : (double)s.PeopleScanDone / s.PeopleScanTotal;
        _rerunText.Text = settling ? "Filing faces into people…"
            : $"Re-analysing faces… {s.PeopleScanDone:N0} of {s.PeopleScanTotal:N0}";
        string model = s.PeopleModelText;
        _reanalyseDetail.Text = "Looks at every photo and video again" + (model.Length > 0 ? $" with {model}" : "") +
                                ", then files the faces into the people you have. Names, merges and splits are kept.";
    }

    /// <summary>AI_PEOPLE: the grid reads again (at most twice a second).</summary>
    internal void PeopleChanged() => _peopleGrid.Refresh();

    /// <summary>The viewer opened another folder: People follows it.</summary>
    internal void OnFolderChanged() => _peopleGrid.OnFolderChanged();
}
