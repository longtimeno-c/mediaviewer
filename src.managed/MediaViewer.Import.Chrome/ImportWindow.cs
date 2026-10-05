// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Pickers;
using Windows.System;

namespace MediaViewer.Import.Chrome;

/// <summary>A tile: one unit (a file, a RAW+JPEG pair, a Live Photo).</summary>
public sealed class TileVm : INotifyPropertyChanged
{
    private bool _selected;
    private ImageSource? _thumb;

    public int Index { get; init; }
    public string Name { get; init; } = "";
    public string Badge { get; init; } = "";
    public string State { get; init; } = "new";
    public string Path { get; init; } = "";
    public string Tip { get; init; } = "";
    public double Dim => State is "duplicate" or "imported" or "filtered" ? 0.4 : 1.0;
    public bool ThumbRequested { get; set; }

    public bool Selected
    {
        get => _selected;
        set { if (_selected != value) { _selected = value; Changed(); Changed(nameof(Check)); } }
    }

    public string Check => _selected ? "☑" : "☐";

    public ImageSource? Thumb
    {
        get => _thumb;
        set { _thumb = value; Changed(); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>A day in the grid, with its own checkbox (Shift+Space).</summary>
public sealed class DayGroup : ObservableCollection<TileVm>
{
    public string Day { get; init; } = "";
    public string Header { get; set; } = "";
}

public sealed class SourceVm
{
    public string Root { get; init; } = "";
    public string Label { get; init; } = "";
    public string Detail { get; init; } = "";
    public string VolumeId { get; init; } = "";
    public string Kind { get; init; } = "";
    /// <summary>Whether this source can ever be ejected (issue #41/#42): a
    /// card or a USB/network drive, never an ordinary folder or a fixed disk.</summary>
    public bool Removable { get; init; }
}

/// <summary>
/// The Import window (docs/design/18 "The Import window"): sources on the left, the
/// day-grouped grid in the middle, three plain steps on the right (where to, what
/// to import, how to organise) with the rest of the preset under More options,
/// one primary button at the bottom. It becomes the progress view
/// while copying and the summary after. Keyboard-complete:
/// Enter imports (or opens the focused tile in the viewer), Ctrl+Enter
/// imports from anywhere, Space toggles a file (pauses / resumes while
/// copying), Shift+Space a day, Ctrl+Tab / Ctrl+Shift+Tab the next / previous
/// source, Ctrl+J ejects after the summary, Esc closes (the import carries on).
/// </summary>
internal sealed class ImportWindow : Window
{
    private readonly ImportChrome _chrome;
    private readonly ImportApi _api;

    private readonly ListView _sources = new() { SelectionMode = ListViewSelectionMode.Single };
    private readonly ObservableCollection<SourceVm> _sourceItems = new();
    private readonly GridView _grid = new() { SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = true };
    private readonly CollectionViewSource _groups = new() { IsSourceGrouped = true };
    private readonly ObservableCollection<DayGroup> _days = new();
    private readonly TextBlock _sourceTitle = Text("", 18);
    private readonly TextBlock _bottomText = Text("");
    /// <summary>The bottom bar's quieter line: what is skipped, how long it takes.</summary>
    private readonly TextBlock _bottomDetail = new() { FontSize = 12, Opacity = 0.7, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _sourceCounts = new() { FontSize = 15, Opacity = 0.7, VerticalAlignment = VerticalAlignment.Center };
    private readonly StackPanel _selectButtons = new() { Orientation = Orientation.Horizontal, Spacing = 6, Visibility = Visibility.Collapsed };
    private readonly Border _allDoneStrip = new() { Padding = new Thickness(8), Margin = new Thickness(8, 0, 8, 4), CornerRadius = new CornerRadius(6), Visibility = Visibility.Collapsed };
    private readonly StackPanel _emptyPanel = new() { Spacing = 10, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _noSources = new() { Text = "Insert a memory card or a drive, or add a folder.", FontSize = 13, Opacity = 0.7, TextWrapping = TextWrapping.Wrap };
    private long _planUnits, _planNew;
    /// <summary>"reading", "failed" or "" (planned): what the empty grid says.</summary>
    private string _scanState = "";
    private bool _showMore;
    private readonly Button _importButton = new() { Content = "Import", Style = AccentStyle() };
    private readonly StackPanel _presetPanel = new() { Spacing = 8, Padding = new Thickness(12) };
    private readonly ListView _whereFilesGo = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 220 };
    private readonly StackPanel _progressPanel = new() { Spacing = 6, Visibility = Visibility.Collapsed };
    private readonly StackPanel _summaryPanel = new() { Spacing = 6, Visibility = Visibility.Collapsed };
    private readonly Banner _banner = new();
    private readonly SemaphoreSlim _thumbGate = new(2);
    private readonly HyperlinkButton _whyLink = new() { Content = "Why?", FontSize = 12, Visibility = Visibility.Collapsed, Padding = new Thickness(0) };

    private IReadOnlyList<string> _marks = Array.Empty<string>();
    private string _root = "";
    private string _volumeId = "";
    private bool _removable;
    private ulong _scan;
    private ulong _plan;
    private ulong _job;
    private JsonObject _preset = new();
    private JsonArray _presets = new();
    private string _destinationForOpen = "";
    private bool _copying;
    private bool _building;
    private long _confirmUnits, _confirmFiles, _confirmBytes;

    internal ImportWindow(ImportChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        Title = "Import";
        MediaViewer.Shared.AppIcon.Apply(this);
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1280, 760));
        Content = BuildLayout();
        _groups.Source = _days;
        _grid.ItemsSource = _groups.View;
        _sources.ItemsSource = _sourceItems;
    }

    // ---- layout ------------------------------------------------------------------

    private static TextBlock Text(string s, double size = 14) =>
        new() { Text = s, FontSize = size, TextWrapping = TextWrapping.Wrap };

    private static Style AccentStyle() =>
        (Style)Application.Current.Resources["AccentButtonStyle"];

    private static DataTemplate Template(string body) => (DataTemplate)XamlReader.Load(
        "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\">" + body + "</DataTemplate>");

    private UIElement BuildLayout()
    {
        // Keep the ThemeResource expression on the live root so Windows updates
        // it, along with the native controls, when appearance changes.
        var root = (Grid)XamlReader.Load(
            """
            <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                  Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>
            """);
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(240) });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(320) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        Grid.SetColumnSpan(_banner.Root, 3);
        root.Children.Add(_banner.Root);

        // Sources.
        var left = new StackPanel { Spacing = 8, Padding = new Thickness(12) };
        left.Children.Add(new TextBlock { Text = "Import from", FontSize = 15, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        left.Children.Add(_noSources);
        _sources.ItemTemplate = Template(
            "<StackPanel Padding=\"4\"><TextBlock Text=\"{Binding Label}\" FontSize=\"15\"/>" +
            "<TextBlock Text=\"{Binding Detail}\" FontSize=\"12\" Opacity=\"0.7\"/></StackPanel>");
        _sources.SelectionChanged += (_, _) =>
        {
            if (!_building && _sources.SelectedItem is SourceVm s) Load(s.Root, s.VolumeId, s.Removable);
        };
        left.Children.Add(_sources);
        var addFolder = new Button { Content = "+ Add a folder…" };
        addFolder.Click += async (_, _) => await AddFolderSource();
        left.Children.Add(addFolder);
        // The occasional tools, in one menu rather than four buttons.
        var tools = new MenuFlyout();
        var history = new MenuFlyoutItem { Text = "Past imports…" };
        history.Click += (_, _) => ShowHistory();
        tools.Items.Add(history);
        var verify = new MenuFlyoutItem { Text = "Check a folder for damaged files…" };
        ToolTipService.SetToolTip(verify, "Re-hash imported files against the library index to find silent corruption.");
        verify.Click += async (_, _) => await VerifyFolder();
        tools.Items.Add(verify);
        if (_chrome.HasDuplicates)
        {
            var dups = new MenuFlyoutItem { Text = "Find duplicates…" };
            ToolTipService.SetToolTip(dups,
                "Compare every file in a folder and its subfolders by content, and move extra copies to the Recycle Bin.");
            dups.Click += async (_, _) => await _chrome.OpenDuplicates();
            tools.Items.Add(dups);
        }
        tools.Items.Add(new MenuFlyoutSeparator());
        var help = new MenuFlyoutItem { Text = "About Import" };
        help.Click += (_, _) => _ = ShowExplainer();
        tools.Items.Add(help);
        left.Children.Add(new Button { Content = "Tools ▾", Flyout = tools });
        Grid.SetRow(left, 1);
        root.Children.Add(left);

        // Contents.
        var centre = new Grid();
        centre.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        centre.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        centre.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var titleRow = new Grid { Margin = new Thickness(8), ColumnSpacing = 10 };
        titleRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        titleRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        titleRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _sourceTitle.TextWrapping = TextWrapping.NoWrap;
        _sourceTitle.TextTrimming = TextTrimming.CharacterEllipsis;
        titleRow.Children.Add(_sourceTitle);
        Grid.SetColumn(_sourceCounts, 1);
        titleRow.Children.Add(_sourceCounts);
        var selectAll = new Button { Content = "Select all" };
        selectAll.Click += (_, _) => SelectAll(true);
        var selectNone = new Button { Content = "Select none" };
        selectNone.Click += (_, _) => SelectAll(false);
        _selectButtons.Children.Add(selectAll);
        _selectButtons.Children.Add(selectNone);
        Grid.SetColumn(_selectButtons, 2);
        titleRow.Children.Add(_selectButtons);
        centre.Children.Add(titleRow);
        _allDoneStrip.Background = new SolidColorBrush(ColorHelper.FromArgb(0x30, 0x34, 0xC7, 0x59));
        _allDoneStrip.Child = Text("✓  Everything here has already been imported. Click a file to import it again.", 13);
        Grid.SetRow(_allDoneStrip, 1);
        centre.Children.Add(_allDoneStrip);
        Grid.SetRow(_emptyPanel, 2);
        centre.Children.Add(_emptyPanel);
        _grid.ItemTemplate = Template(
            "<Grid Width=\"128\" Height=\"128\" Opacity=\"{Binding Dim}\">" +
            "<Image Source=\"{Binding Thumb}\" Stretch=\"UniformToFill\"/>" +
            "<TextBlock Text=\"{Binding Check}\" FontSize=\"18\" Margin=\"4\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\"/>" +
            "<TextBlock Text=\"{Binding Badge}\" FontSize=\"11\" Margin=\"4\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Top\"/>" +
            "<TextBlock Text=\"{Binding Name}\" FontSize=\"11\" Margin=\"4\" VerticalAlignment=\"Bottom\" TextTrimming=\"CharacterEllipsis\"/>" +
            "</Grid>");
        var header = new GroupStyle
        {
            HeaderTemplate = Template("<TextBlock Text=\"{Binding Header}\" FontSize=\"15\"/>"),
        };
        _grid.GroupStyle.Add(header);
        _grid.ItemClick += (_, e) => { if (e.ClickedItem is TileVm t && !_copying) Toggle(t); };
        _grid.ContainerContentChanging += (_, e) =>
        {
            if (e.Item is TileVm t && !t.ThumbRequested) RequestThumb(t);
        };
        Grid.SetRow(_grid, 2);
        centre.Children.Add(_grid);
        Grid.SetRow(centre, 1);
        Grid.SetColumn(centre, 1);
        root.Children.Add(centre);

        // Preset.
        var right = new ScrollViewer { Content = _presetPanel };
        Grid.SetRow(right, 1);
        Grid.SetColumn(right, 2);
        root.Children.Add(right);

        // Bottom bar.
        var bottom = new Grid { Padding = new Thickness(12), ColumnSpacing = 12 };
        bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var info = new StackPanel { Spacing = 6 };
        var bottomRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        bottomRow.Children.Add(_bottomText);
        bottomRow.Children.Add(_whyLink);
        info.Children.Add(bottomRow);
        info.Children.Add(_bottomDetail);
        info.Children.Add(_progressPanel);
        info.Children.Add(_summaryPanel);
        bottom.Children.Add(info);
        _importButton.Click += (_, _) => StartImport();
        Grid.SetColumn(_importButton, 1);
        bottom.Children.Add(_importButton);
        Grid.SetRow(bottom, 2);
        Grid.SetColumnSpan(bottom, 3);
        root.Children.Add(bottom);

        root.KeyDown += OnKeyDown;
        return root;
    }

    // ---- showing and sources --------------------------------------------------------

    internal void Show(string? sourceRoot, IReadOnlyList<string> marks)
    {
        _marks = marks;
        Activate();
        _ = RefreshSources(sourceRoot);
        _ = CheckUnfinished();
        UpdateGridState();
        _importButton.Focus(FocusState.Programmatic);
        ShowFirstUseExplainerIfNeeded();
    }

    // ---- first-use explainer and contextual help (issue #41) --------------------------

    private static readonly (string Key, string Title, string Body)[] ExplainerSections =
    {
        ("overview", "What Import does",
            "Import copies photos and video from a card or folder into your library. It never " +
            "edits, deletes or overwrites anything at the source — files there are only read " +
            "(an original is never modified, on the card or off it)."),
        ("sources", "Cards vs. folders",
            "A card, USB drive or network share appears on the left with its free space and how " +
            "many files on it are new. A folder you add with “Add a folder…” is scanned and copied the " +
            "same way, but — because it is not removable media — Import never offers to eject it."),
        ("filters", "New / All / Marked / date range, and duplicates",
            "“New since last import” skips anything this source has given you before, tracked per " +
            "card even after it is unplugged and replugged. “All” considers everything, “Marked” " +
            "only what you starred in the viewer, and a date range limits by the date each file was " +
            "taken. Before copying, every file's content is checked against what is already at the " +
            "destination (or the whole library, with the wider duplicate scope); an exact match is " +
            "skipped, never overwritten or duplicated."),
        ("destination", "Destination and name preview",
            "The folder list under “How to organise” and the grid show exactly which folder — and, if renaming is turned " +
            "on, which file name — each file will get before you click Import. Nothing is copied " +
            "until you start the import."),
        ("verify", "Verified copies",
            "Every copy is hashed and checked against the source as it is written; full verify " +
            "also reads the copy back from the drive. A copy that does not match is reported as " +
            "failed rather than left silently short."),
        ("pause", "Pause and resume",
            "Space pauses or resumes a running import. If it is interrupted, files already " +
            "verified are kept and are not copied again when you resume."),
        ("eject", "Why Eject is (or isn't) offered",
            "Eject only appears for a card, USB drive or network share — never for an ordinary " +
            "folder or your main disk, which cannot be ejected at all. It is also refused while a " +
            "copy or scan is still reading from that source; if eject fails, the message says why " +
            "(still busy, already gone, not removable, or Windows refused it)."),
    };

    private UIElement HelpLine(string label, string section)
    {
        var link = new HyperlinkButton { Content = Text(label, 11), Padding = new Thickness(0, 2, 0, 2) };
        link.Click += (_, _) => _ = ShowExplainer(section);
        return link;
    }

    private void ShowFirstUseExplainerIfNeeded()
    {
        bool seen = false;
        try
        {
            var values = Windows.Storage.ApplicationData.Current.LocalSettings.Values;
            seen = values.TryGetValue("import_explainer_seen", out object? v) && v is bool b && b;
        }
        catch { /* no local settings (e.g. under test) — show once per session only */ }
        if (!seen) _ = ShowExplainer(firstUse: true);
    }

    private async Task ShowExplainer(string? scrollTo = null, bool firstUse = false)
    {
        var body = new StackPanel { Spacing = 18, Padding = new Thickness(4) };
        var marks = new Dictionary<string, FrameworkElement>();
        foreach ((string key, string title, string text) in ExplainerSections)
        {
            var section = new StackPanel { Spacing = 4 };
            section.Children.Add(Text(title, 15));
            section.Children.Add(Text(text, 13));
            marks[key] = section;
            body.Children.Add(section);
        }
        if (firstUse)
        {
            try { Windows.Storage.ApplicationData.Current.LocalSettings.Values["import_explainer_seen"] = true; }
            catch { }
        }
        var dialog = new ContentDialog
        {
            Title = "About Import",
            Content = new ScrollViewer { Content = body, MaxHeight = 480, MaxWidth = 440 },
            CloseButtonText = firstUse ? "Got it" : "Close",
            XamlRoot = Content.XamlRoot,
        };
        if (scrollTo is not null && marks.TryGetValue(scrollTo, out FrameworkElement? target))
        {
            dialog.Opened += (_, _) => target.StartBringIntoView();
        }
        await dialog.ShowAsync();
    }

    private void AddWhyLink(string section)
    {
        _whyLink.Visibility = Visibility.Visible;
        _whyLink.Click -= WhyLinkClicked;  // avoid stacking handlers across calls
        _currentWhySection = section;
        _whyLink.Click += WhyLinkClicked;
    }

    private string _currentWhySection = "eject";
    private void WhyLinkClicked(object sender, RoutedEventArgs e) => _ = ShowExplainer(_currentWhySection);

    internal void OnSourcesChanged() => _ = RefreshSources(null);

    private async Task RefreshSources(string? select)
    {
        string json;
        string presets;
        try
        {
            (json, presets) = await Task.Run(() => (_api.SourcesJson(), _api.PresetsJson()));
        }
        catch (MediaViewerException) { return; }
        _building = true;
        try
        {
            using (JsonDocument p = JsonDocument.Parse(presets))
            {
                JsonObject all = JsonNode.Parse(presets)!.AsObject();
                _presets = all["presets"]!.AsArray();
                string last = all["last"]?.GetValue<string>() ?? "";
                JsonNode? chosen = _presets.FirstOrDefault(n => n?["name"]?.GetValue<string>() == last) ?? _presets.FirstOrDefault();
                if (_preset.Count == 0 && chosen is not null) _preset = chosen.DeepClone().AsObject();
                if (string.IsNullOrEmpty(_preset["destination"]?.GetValue<string>()))
                {
                    string lastDest = all["last_destination"]?.GetValue<string>() ?? "";
                    _preset["destination"] = lastDest.Length > 0 ? lastDest : all["default_destination"]?.GetValue<string>() ?? "";
                }
            }
            string? current = select ?? (_root.Length > 0 ? _root : null);
            _sourceItems.Clear();
            using JsonDocument doc = JsonDocument.Parse(json);
            foreach (JsonElement s in doc.RootElement.EnumerateArray())
            {
                long total = s.GetProperty("total_bytes").GetInt64();
                long fresh = s.GetProperty("new_count").GetInt64();
                string kind = s.GetProperty("kind").GetString() ?? "";
                string detail = (total > 0 ? Format.Bytes(total) + " · " : "") +
                                (fresh >= 0 ? $"{fresh} new" : kind == "network" ? "network (slower)" : "");
                _sourceItems.Add(new SourceVm
                {
                    Root = s.GetProperty("root").GetString() ?? "",
                    Label = s.GetProperty("label").GetString() is { Length: > 0 } l ? l : s.GetProperty("root").GetString() ?? "",
                    Detail = detail,
                    VolumeId = s.GetProperty("volume_id").GetString() ?? "",
                    Kind = kind,
                    Removable = s.TryGetProperty("removable", out JsonElement rem) && rem.GetBoolean(),
                });
            }
            if (current is not null && _sourceItems.All(s => s.Root != current))
            {
                _sourceItems.Insert(0, new SourceVm { Root = current, Label = current, Detail = "", Kind = "folder", Removable = false });
            }
            _noSources.Visibility = _sourceItems.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            SourceVm? pick = _sourceItems.FirstOrDefault(s => s.Root == current) ?? _sourceItems.FirstOrDefault();
            _sources.SelectedItem = pick;
            BuildPresetPanel();
            if (pick is not null && pick.Root != _root) Load(pick.Root, pick.VolumeId, pick.Removable);
        }
        finally
        {
            _building = false;
        }
    }

    private async Task AddFolderSource()
    {
        string? dir = await PickFolder();
        if (dir is null) return;
        await Task.Run(() => { try { _api.AddFolderSource(dir); } catch (MediaViewerException) { } });
        await RefreshSources(dir);
    }

    private async Task<string?> PickFolder()
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        // Every caller is an async void click handler: a picker failure that
        // escaped here would take the process down. As ManagePanel.AddFolder.
        try
        {
            Windows.Storage.StorageFolder? f = await picker.PickSingleFolderAsync();
            return f?.Path;
        }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private async Task CheckUnfinished()
    {
        string json;
        try { json = await Task.Run(() => _api.UnfinishedJson()); }
        catch (MediaViewerException) { return; }
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement first = doc.RootElement.EnumerateArray().FirstOrDefault();
        if (first.ValueKind != JsonValueKind.Object) return;
        ulong job = first.GetProperty("job").GetUInt64();
        string label = first.GetProperty("label").GetString() ?? first.GetProperty("source").GetString() ?? "";
        long pending = first.GetProperty("pending").GetInt64();
        _banner.Title = "An import was interrupted";
        _banner.Message = $"{label}: {pending} files not copied yet. Verified files are kept and will not be copied again.";
        var resume = new Button { Content = "Resume" };
        resume.Click += (_, _) =>
        {
            try
            {
                _api.Resume(job);
                _chrome.Track(job, label);
                _job = job;
                SetCopying(true);
            }
            catch (MediaViewerException ex) { _banner.Message = "Could not resume: " + ex.Status; }
            _banner.IsOpen = false;
        };
        _banner.ActionButton = resume;
        _banner.IsOpen = true;
    }

    // ---- scan and plan ----------------------------------------------------------------

    private void Load(string root, string volumeId, bool removable)
    {
        _root = root;
        _volumeId = volumeId;
        _removable = removable;
        _sourceTitle.Text = _sourceItems.FirstOrDefault(s => s.Root == root)?.Label ?? root;
        _scanState = "reading";
        _planUnits = _planNew = 0;
        _days.Clear();
        _plan = 0;
        try { _scan = _api.Scan(root); }
        catch (MediaViewerException) { _scanState = "failed"; }
        BuildPresetPanel();
        UpdateGridState();
    }

    internal void OnScanDone(ulong scan, MvStatus status)
    {
        if (scan != _scan) return;
        if (status != MvStatus.Ok)
        {
            _scanState = "failed";
            UpdateGridState();
            return;
        }
        Replan();
    }

    private void Replan()
    {
        if (_scan == 0) return;
        string? marks = _preset["selection"]?.GetValue<string>() == "marked"
            ? new JsonArray(_marks.Select(m => (JsonNode?)JsonValue.Create(m)).ToArray()).ToJsonString()
            : null;
        try { _plan = _api.Plan(_scan, _preset.ToJsonString(), marks); }
        catch (MediaViewerException ex) { _bottomText.Text = "Settings not accepted: " + ex.Status; }
    }

    internal void OnPlanReady(ulong plan, MvStatus status)
    {
        if (plan != _plan) return;
        if (status != MvStatus.Ok)
        {
            _bottomText.Text = "Could not plan this import: " + status;
            return;
        }
        string json;
        try { json = _api.PlanJson(plan); }
        catch (MediaViewerException) { return; }
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement r = doc.RootElement;
        JsonElement t = r.GetProperty("totals");
        JsonElement sourceInfo = r.GetProperty("source");
        string label = sourceInfo.GetProperty("label").GetString() ?? _root;
        if (sourceInfo.TryGetProperty("removable", out JsonElement srcRemovable) && srcRemovable.GetBoolean() != _removable)
        {
            _removable = srcRemovable.GetBoolean();
            BuildPresetPanel();
        }
        _sourceTitle.Text = label.Length > 0 ? label : _root;
        _scanState = "";
        _planUnits = t.GetProperty("units").GetInt64();
        _planNew = t.GetProperty("new").GetInt64();
        _destinationForOpen = r.GetProperty("destination").GetString() ?? "";

        // Tiles: rebuilt when the unit list changed, else only re-checked.
        var units = r.GetProperty("units").EnumerateArray().ToArray();
        int existing = _days.Sum(d => d.Count);
        if (existing != units.Length)
        {
            _days.Clear();
            var byDay = new Dictionary<string, DayGroup>();
            foreach (JsonElement u in units)
            {
                string day = u.GetProperty("day").GetString() ?? "";
                if (!byDay.TryGetValue(day, out DayGroup? g))
                {
                    g = new DayGroup { Day = day };
                    byDay[day] = g;
                    _days.Add(g);
                }
                string kind = u.GetProperty("kind").GetString() ?? "";
                string type = u.GetProperty("type").GetString() ?? "";
                string state = u.GetProperty("state").GetString() ?? "new";
                string matched = u.GetProperty("matched").GetString() ?? "";
                g.Add(new TileVm
                {
                    Index = u.GetProperty("i").GetInt32(),
                    Name = u.GetProperty("sources")[0].GetString() ?? "",
                    Badge = kind == "raw_jpeg" ? "RAW+JPEG" : kind == "live_photo" ? "LIVE" : type == "video" ? "▶" : "",
                    State = state,
                    Path = u.GetProperty("path").GetString() ?? "",
                    Tip = state == "duplicate" ? "Already in library: " + matched : state == "imported" ? "Imported from this card before" : "",
                    Selected = u.GetProperty("selected").GetBoolean(),
                });
            }
        }
        else
        {
            var tiles = _days.SelectMany(d => d).ToDictionary(x => x.Index);
            foreach (JsonElement u in units)
            {
                if (tiles.TryGetValue(u.GetProperty("i").GetInt32(), out TileVm? tile))
                {
                    tile.Selected = u.GetProperty("selected").GetBoolean();
                }
            }
        }
        var dayRows = r.GetProperty("days").EnumerateArray().ToDictionary(d => d.GetProperty("day").GetString() ?? "");
        foreach (DayGroup g in _days)
        {
            if (!dayRows.TryGetValue(g.Day, out JsonElement d)) continue;
            long n = d.GetProperty("units").GetInt64();
            long fresh = d.GetProperty("new").GetInt64();
            long sel = d.GetProperty("selected").GetInt64();
            string check = sel == 0 ? "☐" : sel == n ? "☑" : "◧";
            g.Header = $"{check} {DayLabel(g.Day)} · {n}" + (fresh == n ? " (all new)" : fresh == 0 ? " (already imported)" : $" ({fresh} new)");
        }
        _groups.Source = null;
        _groups.Source = _days;

        _whereFilesGo.Items.Clear();
        foreach (JsonElement f in r.GetProperty("folders").EnumerateArray())
        {
            string folder = f.GetProperty("folder").GetString() is { Length: > 0 } s ? s : "(destination)";
            _whereFilesGo.Items.Add($"{folder}   {f.GetProperty("units").GetInt64()}");
        }

        long selUnits = t.GetProperty("selected_units").GetInt64();
        long selFiles = t.GetProperty("selected_files").GetInt64();
        long selBytes = t.GetProperty("selected_bytes").GetInt64();
        _confirmUnits = selUnits;
        _confirmFiles = selFiles;
        _confirmBytes = selBytes;
        long dups = t.GetProperty("duplicates").GetInt64();
        long eta = t.GetProperty("eta_seconds").GetInt64();
        double rate = t.GetProperty("bytes_per_second").GetDouble();
        _bottomText.Text = selUnits == 0
            ? "Nothing selected"
            : $"{selUnits} item{(selUnits == 1 ? "" : "s")} selected · {Format.Bytes(selBytes)}";
        var detail = new List<string>();
        if (dups > 0) detail.Add($"{dups} already in your library will be skipped");
        if (eta >= 0 && selUnits > 0) detail.Add($"{Format.Eta(eta)} at {Format.Rate(rate)}");
        _bottomDetail.Text = string.Join(" · ", detail);
        UpdateImportButton(selUnits);
        UpdateGridState();
    }

    private static string DayLabel(string day) =>
        DateTime.TryParse(day, System.Globalization.CultureInfo.InvariantCulture,
            System.Globalization.DateTimeStyles.None, out DateTime d)
            ? d.ToString("ddd d MMM yyyy", System.Globalization.CultureInfo.CurrentCulture)
            : day;

    private void RequestThumb(TileVm tile)
    {
        tile.ThumbRequested = true;
        ulong plan = _plan;
        _ = Task.Run(async () =>
        {
            await _thumbGate.WaitAsync();
            try
            {
                string? path = ImportChrome.Try(() => _api.Thumbnail(plan, (uint)tile.Index));
                if (path is null) return;
                DispatcherQueue.TryEnqueue(() =>
                {
                    // Nothing may escape a dispatcher callback (a crash), as SearchWindow's thumbs.
                    try { tile.Thumb = new BitmapImage(new Uri(path)); }
                    catch (Exception ex) when (ex is UriFormatException or ArgumentException) { }
                });
            }
            finally
            {
                _thumbGate.Release();
            }
        });
    }

    private void Toggle(TileVm tile)
    {
        try { _api.Select(_plan, tile.Index, null, !tile.Selected); }
        catch (MediaViewerException) { }
    }

    private void ToggleDay(TileVm tile)
    {
        DayGroup? g = _days.FirstOrDefault(d => d.Contains(tile));
        if (g is null) return;
        bool any = g.Any(t => t.Selected);
        try { _api.Select(_plan, -1, g.Day, !any); }
        catch (MediaViewerException) { }
    }

    /// <summary>Everything on (or off) at once: unit -1 with no day (mediaviewer_import.h).</summary>
    private void SelectAll(bool on)
    {
        if (_copying || _plan == 0) return;
        try { _api.Select(_plan, -1, null, on); }
        catch (MediaViewerException) { }
    }

    /// <summary>The grid, or a sentence saying why it is empty; the counts and the
    /// "already imported" strip above it.</summary>
    private void UpdateGridState()
    {
        bool hasTiles = _days.Count > 0;
        bool onlyNew = P("selection", "new") == "new";
        _grid.Visibility = hasTiles ? Visibility.Visible : Visibility.Collapsed;
        _sourceCounts.Text = _planUnits > 0 ? $"{_planNew} new of {_planUnits}" : "";
        _selectButtons.Visibility = hasTiles && !_copying ? Visibility.Visible : Visibility.Collapsed;
        _allDoneStrip.Visibility = hasTiles && !_copying && _planNew == 0 && onlyNew ? Visibility.Visible : Visibility.Collapsed;
        _emptyPanel.Children.Clear();
        _emptyPanel.Visibility = hasTiles ? Visibility.Collapsed : Visibility.Visible;
        if (hasTiles) return;
        void Line(string text) => _emptyPanel.Children.Add(new TextBlock
        {
            Text = text, FontSize = 15, TextWrapping = TextWrapping.Wrap,
            HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center,
        });
        if (_root.Length == 0)
        {
            Line("Insert a memory card, or choose a folder to import from.");
            var choose = new Button { Content = "Choose a folder…", HorizontalAlignment = HorizontalAlignment.Center };
            choose.Click += async (_, _) => await AddFolderSource();
            _emptyPanel.Children.Add(choose);
        }
        else if (_scanState == "reading")
        {
            Line("Looking for photos and videos…");
        }
        else if (_scanState == "failed")
        {
            Line("This source could not be read. Check that it is still connected.");
        }
        else if (_planUnits > 0 && onlyNew)
        {
            Line("✓ Everything here has already been imported.");
            var all = new Button { Content = $"Show all {_planUnits} files", HorizontalAlignment = HorizontalAlignment.Center };
            all.Click += (_, _) => { Set("selection", "all"); BuildPresetPanel(); };
            _emptyPanel.Children.Add(all);
        }
        else
        {
            Line("No photos or videos match these settings.");
        }
    }

    /// <summary>"Import N", or, with no destination yet, the step that comes first.</summary>
    private void UpdateImportButton(long selUnits)
    {
        bool noDestination = P("destination").Length == 0;
        _importButton.Content = noDestination ? "Choose where to import…" : $"Import {selUnits}";
        _importButton.IsEnabled = !_copying && (noDestination || selUnits > 0);
    }

    // ---- the preset panel ---------------------------------------------------------------

    private string P(string key, string fallback = "") => _preset[key]?.GetValue<string>() ?? fallback;
    private bool B(string key, bool fallback) => _preset[key]?.GetValue<bool>() ?? fallback;

    private void Set(string key, JsonNode? value)
    {
        _preset[key] = value;
        if (!_building) Replan();
    }

    /// <summary>Three plain questions (where to, what to import, how to organise),
    /// the backup and eject switches, and everything else under More options.</summary>
    private void BuildPresetPanel()
    {
        bool was = _building;
        _building = true;
        _presetPanel.Children.Clear();
        _presetPanel.Spacing = 12;

        if (_presets.Count > 1)
        {
            var presetBox = new ComboBox { Header = "Saved settings", MinWidth = 260 };
            foreach (JsonNode? n in _presets) presetBox.Items.Add(n?["name"]?.GetValue<string>() ?? "");
            presetBox.SelectedItem = P("name", "Default");
            presetBox.SelectionChanged += (_, _) =>
            {
                if (_building) return;
                JsonNode? chosen = _presets.FirstOrDefault(n => n?["name"]?.GetValue<string>() == presetBox.SelectedItem as string);
                if (chosen is null) return;
                _preset = chosen.DeepClone().AsObject();
                BuildPresetPanel();
                Replan();
            };
            _presetPanel.Children.Add(presetBox);
        }

        _presetPanel.Children.Add(Heading("Where to"));
        _presetPanel.Children.Add(DestinationRow());

        _presetPanel.Children.Add(Heading("What to import"));
        _presetPanel.Children.Add(Combo("", "selection",
            new[] { ("new", "New since the last import"), ("all", "Everything"), ("marked", "Only what I marked in the viewer"), ("date_range", "Taken between two dates") }));
        if (P("selection") == "date_range")
        {
            _presetPanel.Children.Add(TextField("From (YYYY-MM-DD)", "range_from"));
            _presetPanel.Children.Add(TextField("To (YYYY-MM-DD)", "range_to"));
        }

        _presetPanel.Children.Add(Heading("How to organise"));
        _presetPanel.Children.Add(Combo("", "layout",
            new[] { ("YYYY/YYYY-MM-DD", "Year, then day"), ("YYYY/MM/DD", "Year, month, then day"), ("YYYY-MM-DD", "A folder per day"),
                    ("card", "Same folders as the card"), ("flat", "All in one folder") }));
        ToolTipService.SetToolTip(_whereFilesGo, "Where the selected files will go. Nothing is copied until you click Import.");
        _presetPanel.Children.Add(_whereFilesGo);

        var backup = new ToggleSwitch { Header = "Also copy to a backup drive", IsOn = P("backup").Length > 0 };
        backup.Toggled += async (_, _) =>
        {
            if (_building) return;
            if (backup.IsOn)
            {
                string? dir = await PickFolder();
                if (dir is not null) Set("backup", dir);
            }
            else
            {
                Set("backup", "");
            }
            BuildPresetPanel();
        };
        _presetPanel.Children.Add(backup);
        if (P("backup").Length > 0) _presetPanel.Children.Add(Text(P("backup"), 12));
        // Eject only ever means something for a card or a USB/network drive
        // (issue #41/#42): an ordinary folder has nothing to eject.
        if (_removable) _presetPanel.Children.Add(Toggle("Eject the card when done", "eject_after", true));

        var more = new Button { Content = (_showMore ? "▾ " : "▸ ") + "More options" };
        more.Click += (_, _) => { _showMore = !_showMore; BuildPresetPanel(); };
        _presetPanel.Children.Add(more);
        if (_showMore) AddMoreOptions();

        _presetPanel.Children.Add(Text("Import only copies. Nothing on the card is deleted, changed or overwritten, and every copy is checked.", 11));
        _building = was;
    }

    private static TextBlock Heading(string s) =>
        new() { Text = s, FontSize = 15, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };

    private UIElement DestinationRow()
    {
        string dest = P("destination");
        if (dest.Length == 0)
        {
            var choose = new Button { Content = "Choose a folder…", Style = AccentStyle(), HorizontalAlignment = HorizontalAlignment.Stretch };
            choose.Click += async (_, _) => await ChooseDestination();
            return choose;
        }
        var row = new Grid { ColumnSpacing = 8 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var names = new StackPanel();
        names.Children.Add(new TextBlock { Text = System.IO.Path.GetFileName(dest.TrimEnd('\\', '/')) is { Length: > 0 } leaf ? leaf : dest, FontSize = 14 });
        names.Children.Add(new TextBlock { Text = dest, FontSize = 11, Opacity = 0.7, TextTrimming = TextTrimming.CharacterEllipsis });
        ToolTipService.SetToolTip(names, dest);
        row.Children.Add(names);
        var change = new Button { Content = "Change…" };
        change.Click += async (_, _) => await ChooseDestination();
        Grid.SetColumn(change, 1);
        row.Children.Add(change);
        return row;
    }

    private async Task ChooseDestination()
    {
        string? dir = await PickFolder();
        if (dir is null) return;
        Set("destination", dir);
        BuildPresetPanel();
        UpdateImportButton(_confirmUnits);
    }

    /// <summary>Everything a first import does not need to touch.</summary>
    private void AddMoreOptions()
    {
        _presetPanel.Children.Add(Text("File types", 12));
        _presetPanel.Children.Add(TypeFilter());
        _presetPanel.Children.Add(Toggle("Add a folder per camera", "layout_camera", false));
        _presetPanel.Children.Add(Toggle("Add RAW / JPEG / Video folders", "layout_type", false));
        _presetPanel.Children.Add(Combo("Date from", "dates",
            new[] { ("taken", "When it was taken"), ("file_time", "The file's date") }));
        _presetPanel.Children.Add(TextField("Rename files, e.g. {date}_{seq} (empty keeps names)", "rename"));
        _presetPanel.Children.Add(Toggle("Skip files already in the library", "skip_duplicates", true));
        if (B("skip_duplicates", true))
        {
            _presetPanel.Children.Add(Combo("Look for them in", "scope",
                new[] { ("destination", "This folder"), ("library", "The whole library") }));
        }
        _presetPanel.Children.Add(Toggle("Read every copy back to check it", "full_verify", true));
        _presetPanel.Children.Add(Toggle("Notify me when done", "notify", true));
        _presetPanel.Children.Add(Toggle("Copy at full speed (the viewer may lag)", "fast", false));
        _presetPanel.Children.Add(HelpLine("How duplicates, verifying and the folder preview work", "filters"));

        _presetPanel.Children.Add(Text("Saved settings", 12));
        var name = new MediaViewer.Shared.FakeInput(Banner.InputLook, "Name");
        name.SetText(P("name", "Default"));
        var save = new Button { Content = "Save" };
        save.Click += async (_, _) =>
        {
            _preset["name"] = name.Text.Trim().Length > 0 ? name.Text.Trim() : "Default";
            string json = _preset.ToJsonString();
            await Task.Run(() => { try { _api.SavePreset(json); } catch (MediaViewerException) { } });
            await RefreshSources(_root);
        };
        _presetPanel.Children.Add(name);
        _presetPanel.Children.Add(save);

        if (_volumeId.Length > 0)
        {
            var bind = new CheckBox { Content = "Always use these settings for this card" };
            var auto = new CheckBox { Content = "Import this card as soon as it is inserted", IsEnabled = false };
            RoutedEventHandler apply = async (_, _) =>
            {
                auto.IsEnabled = bind.IsChecked == true;
                string vol = _volumeId;
                string preset = bind.IsChecked == true ? P("name", "Default") : "";
                bool on = auto.IsChecked == true && bind.IsChecked == true;
                await Task.Run(() => { try { _api.BindCard(vol, preset, on); } catch (MediaViewerException) { } });
            };
            bind.Click += apply;
            auto.Click += apply;
            _presetPanel.Children.Add(bind);
            _presetPanel.Children.Add(auto);
        }
    }

    private UIElement FolderRow(string label, string key, bool allowOff)
    {
        var row = new StackPanel { Spacing = 2 };
        string value = P(key);
        row.Children.Add(Text($"{label}: {(value.Length > 0 ? value : "Off")}", 13));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        var choose = new Button { Content = "Choose…" };
        choose.Click += async (_, _) =>
        {
            string? dir = await PickFolder();
            if (dir is null) return;
            Set(key, dir);
            BuildPresetPanel();
        };
        buttons.Children.Add(choose);
        if (allowOff && value.Length > 0)
        {
            var off = new Button { Content = "Off" };
            off.Click += (_, _) => { Set(key, ""); BuildPresetPanel(); };
            buttons.Children.Add(off);
        }
        row.Children.Add(buttons);
        return row;
    }

    private UIElement Combo(string label, string key, (string Value, string Text)[] options)
    {
        var box = new ComboBox { MinWidth = 260 };
        if (label.Length > 0) box.Header = label;
        foreach (var (_, text) in options) box.Items.Add(text);
        string current = P(key, options[0].Value);
        box.SelectedIndex = Math.Max(0, Array.FindIndex(options, o => o.Value == current));
        box.SelectionChanged += (_, _) =>
        {
            if (_building || box.SelectedIndex < 0) return;
            Set(key, options[box.SelectedIndex].Value);
            if (key is "selection" or "skip_duplicates") BuildPresetPanel();
        };
        return box;
    }

    private UIElement Toggle(string label, string key, bool fallback)
    {
        var t = new ToggleSwitch { Header = label, IsOn = B(key, fallback) };
        t.Toggled += (_, _) =>
        {
            if (_building) return;
            Set(key, t.IsOn);
            if (key == "skip_duplicates") BuildPresetPanel();
        };
        return t;
    }

    private UIElement TextField(string label, string key)
    {
        // Not a TextBox, which fail-fasts in this host (Shared\FakeInput.cs).
        var box = new MediaViewer.Shared.FakeInput(Banner.InputLook, "");
        box.SetText(P(key));
        box.LostFocus += (_, _) => { if (box.Text != P(key)) Set(key, box.Text); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, label);
        var field = new StackPanel { Spacing = 4 };
        field.Children.Add(new TextBlock { Text = label });
        field.Children.Add(box);
        return field;
    }

    private UIElement TypeFilter()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        JsonArray types = _preset["types"] as JsonArray ?? new JsonArray("raw", "jpeg", "heic", "video", "other");
        foreach (string t in new[] { "raw", "jpeg", "heic", "video", "other" })
        {
            var box = new CheckBox { Content = t.ToUpperInvariant(), IsChecked = types.Any(n => n?.GetValue<string>() == t), MinWidth = 0 };
            box.Click += (_, _) =>
            {
                var next = new JsonArray();
                foreach (CheckBox c in row.Children.OfType<CheckBox>())
                {
                    if (c.IsChecked == true) next.Add(((string)c.Content).ToLowerInvariant());
                }
                Set("types", next);
            };
            row.Children.Add(box);
        }
        return row;
    }

    // ---- copying, progress, summary ------------------------------------------------------

    private void StartImport() => _ = StartImportAsync();

    /// <summary>A clear count-and-destination confirmation before anything is
    /// copied (issue #41), so the last thing a person sees before Import
    /// actually starts is exactly what will happen and where it will go.</summary>
    private async Task StartImportAsync()
    {
        if (_copying) return;
        // No destination yet: choosing one is the next step, not a confirmation
        // that says "(not set)".
        if (P("destination").Length == 0) { await ChooseDestination(); return; }
        if (_plan == 0) return;
        var dialog = new ContentDialog
        {
            Title = "Import these files?",
            Content = Text(
                $"{_confirmUnits} item" + (_confirmUnits == 1 ? "" : "s") +
                $" ({_confirmFiles} file" + (_confirmFiles == 1 ? "" : "s") + $", {Format.Bytes(_confirmBytes)})\n" +
                $"From: {_root}\n" +
                $"To: {(_destinationForOpen.Length > 0 ? _destinationForOpen : "(not set)")}\n\n" +
                "Originals are never modified or deleted; every copy is verified.", 13),
            PrimaryButtonText = "Import",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = Content.XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        try
        {
            _job = _api.Start(_plan);
            _chrome.Track(_job, _sourceTitle.Text);
            SetCopying(true);
        }
        catch (MediaViewerException ex)
        {
            _bottomText.Text = "Import could not start: " + ex.Status;
        }
    }

    private void SetCopying(bool on)
    {
        _copying = on;
        _importButton.IsEnabled = !on;
        _grid.IsEnabled = !on;
        _progressPanel.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        if (on) _summaryPanel.Visibility = Visibility.Collapsed;
        _bottomDetail.Visibility = on ? Visibility.Collapsed : Visibility.Visible;
        UpdateGridState();
    }

    internal void OnProgress(ulong job, MvImportProgress p)
    {
        if (job != _job || !_copying) return;
        _progressPanel.Children.Clear();
        for (int d = 0; d < (int)Math.Min(2, p.DestinationCount); ++d)
        {
            ulong verified;
            unsafe { verified = p.BytesVerified[d]; }
            // A FlatBar, not a ProgressBar: that control has no default style in
            // this island host and fail-fasts when it enters the tree.
            var bar = new MediaViewer.Shared.FlatBar(Banner.Neutral, Banner.Accent)
            {
                Value = p.BytesTotal == 0 ? 0 : (double)verified / p.BytesTotal,
            };
            bar.Root.Width = 520;
            bar.Root.HorizontalAlignment = HorizontalAlignment.Left;
            _progressPanel.Children.Add(bar.Root);
        }
        string state = p.State == MvImportJobState.Paused ? "Paused" : "Copying";
        _progressPanel.Children.Add(Text(
            $"{state} {p.CurrentNameText} · {p.UnitsDone} of {p.UnitsTotal} verified · {Format.Rate(p.BytesPerSecond)} {Format.Eta(p.EtaSeconds)}", 13));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var pause = new Button { Content = p.State == MvImportJobState.Paused ? "Resume (Space)" : "Pause (Space)" };
        pause.Click += (_, _) => PauseResume();
        var cancel = new Button { Content = "Cancel" };
        cancel.Click += (_, _) => { try { _api.Cancel(_job); } catch (MediaViewerException) { } };
        buttons.Children.Add(pause);
        buttons.Children.Add(cancel);
        _progressPanel.Children.Add(buttons);
    }

    private void PauseResume()
    {
        try
        {
            MvImportProgress p = _api.Progress(_job);
            _api.Pause(_job, p.State != MvImportJobState.Paused);
        }
        catch (MediaViewerException) { }
    }

    internal void OnJobDone(ulong job, MvImportJobState state)
    {
        if (job != _job) return;
        SetCopying(false);
        string json;
        try { json = _api.SummaryJson(job); }
        catch (MediaViewerException) { return; }
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement s = doc.RootElement;
        _summaryPanel.Children.Clear();
        if (s.TryGetProperty("kind", out JsonElement kind) && kind.GetString() == "verify")
        {
            long ok = s.GetProperty("ok").GetInt64();
            var problems = s.GetProperty("problems").EnumerateArray().ToArray();
            _summaryPanel.Children.Add(Text($"Verified {ok} files: " + (problems.Length == 0 ? "all intact." : $"{problems.Length} problems."), 15));
            foreach (JsonElement p in problems.Take(200))
            {
                _summaryPanel.Children.Add(Text($"{p.GetProperty("problem").GetString()}: {p.GetProperty("path").GetString()}", 12));
            }
        }
        else
        {
            JsonElement copied = s.GetProperty("copied");
            var skipped = s.GetProperty("skipped").EnumerateArray().ToArray();
            var failed = s.GetProperty("failed").EnumerateArray().ToArray();
            bool sourceRemovable = s.TryGetProperty("source_removable", out JsonElement sr) && sr.GetBoolean();
            _summaryPanel.Children.Add(Text(
                $"{copied.GetProperty("files").GetInt64()} copied · {skipped.Length} skipped · {failed.Length} failed" +
                (s.GetProperty("ejected").GetBoolean() ? " · card ejected" : ""), 15));
            var list = new ListView { MaxHeight = 160, SelectionMode = ListViewSelectionMode.None };
            foreach (JsonElement f in failed) list.Items.Add($"Failed: {f.GetProperty("name").GetString()} — {f.GetProperty("reason").GetString()}");
            foreach (JsonElement k in skipped.Take(500)) list.Items.Add($"Skipped: {k.GetProperty("name").GetString()} — matches {k.GetProperty("matched").GetString()}");
            _summaryPanel.Children.Add(list);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            if (failed.Length > 0)
            {
                var retry = new Button { Content = "Retry failed" };
                retry.Click += (_, _) =>
                {
                    try
                    {
                        _job = _api.RetryFailed(job);
                        _chrome.Track(_job, "retry");
                        SetCopying(true);
                    }
                    catch (MediaViewerException) { }
                };
                buttons.Children.Add(retry);
            }
            // Eject is only ever offered where it could work (issue #41/#42):
            // never for an ordinary folder or a fixed disk.
            Button? eject = null;
            if (sourceRemovable)
            {
                eject = new Button { Content = "Eject (Ctrl+J)" };
                eject.Click += (_, _) => Eject();
                buttons.Children.Add(eject);
            }
            var open = new Button { Content = "Open in viewer" };
            open.Click += (_, _) => { if (_destinationForOpen.Length > 0) _chrome.Host.OpenInViewer(_destinationForOpen); };
            var report = new Button { Content = "Show report" };
            string reportPath = s.GetProperty("report").GetString() ?? "";
            report.IsEnabled = reportPath.Length > 0;
            report.Click += (_, _) => _ = Launcher.LaunchUriAsync(new Uri(reportPath));
            buttons.Children.Add(open);
            buttons.Children.Add(report);
            _summaryPanel.Children.Add(buttons);
            (eject ?? open).Focus(FocusState.Programmatic);
        }
        _summaryPanel.Visibility = Visibility.Visible;
        if (_scan != 0 && state != MvImportJobState.Cancelled) Load(_root, _volumeId, _removable);  // re-plan: what is new now
    }

    /// <summary>A short, specific reason for each eject failure category
    /// (issue #42), instead of one "in use" bucket for everything.</summary>
    private static string EjectFailureMessage(MvStatus status) => status switch
    {
        MvStatus.Busy => "The card is still in use (a copy or scan is reading from it) and was not ejected.",
        MvStatus.NotRemovable => "This is not a removable card or drive, so there is nothing to eject.",
        MvStatus.PermissionDenied => "Windows would not let this app eject the card. Try Explorer's own Eject.",
        MvStatus.NotFound => "The card is already gone — it looks like it was already removed.",
        MvStatus.Timeout => "Ejecting the card took too long and was given up on. It may still be safe to remove.",
        _ => "The card could not be ejected.",
    };

    private void Eject()
    {
        string root = _root;
        _ = Task.Run(() =>
        {
            string msg;
            bool showWhy = false;
            try { _api.Eject(root); msg = "Ejected. The card can be removed."; }
            catch (MediaViewerException ex) { msg = EjectFailureMessage(ex.Status); showWhy = true; }
            DispatcherQueue.TryEnqueue(() =>
            {
                _bottomText.Text = msg;
                if (showWhy) AddWhyLink("eject");
                else _whyLink.Visibility = Visibility.Collapsed;
            });
        });
    }

    private async void ShowHistory()
    {
        string json;
        try { json = await Task.Run(() => _api.HistoryJson()); }
        catch (MediaViewerException) { return; }
        // async void: nothing may escape. A malformed history row or a second
        // dialog already open (ShowAsync throws) just shows nothing.
        try { await ShowHistoryDialog(json); }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException
                                       or System.Runtime.InteropServices.COMException)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private async Task ShowHistoryDialog(string json)
    {
        var list = new ListView { SelectionMode = ListViewSelectionMode.None, MaxHeight = 480 };
        using (JsonDocument doc = JsonDocument.Parse(json))
        {
            foreach (JsonElement j in doc.RootElement.EnumerateArray())
            {
                DateTime when = DateTimeOffset.FromUnixTimeSeconds(j.GetProperty("created").GetInt64()).LocalDateTime;
                string files = "";
                if (j.GetProperty("summary").ValueKind == JsonValueKind.Object &&
                    j.GetProperty("summary").TryGetProperty("copied", out JsonElement c))
                {
                    files = $" · {c.GetProperty("files").GetInt64()} files";
                }
                list.Items.Add($"{when:g} · {j.GetProperty("kind").GetString()} · {j.GetProperty("label").GetString()} {j.GetProperty("source").GetString()} · {j.GetProperty("state").GetString()}{files}");
            }
        }
        var dialog = new ContentDialog
        {
            Title = "Imports",
            Content = list,
            CloseButtonText = "Close",
            XamlRoot = Content.XamlRoot,
        };
        await dialog.ShowAsync();
    }

    private async Task VerifyFolder()
    {
        string? dir = await PickFolder();
        if (dir is null) return;
        try
        {
            _job = _api.VerifyFolder(dir);
            _chrome.Track(_job, "verify " + dir);
            SetCopying(true);
        }
        catch (MediaViewerException ex) { _bottomText.Text = "Could not verify: " + ex.Status; }
    }

    // ---- banner -------------------------------------------------------------------------

    /// <summary>
    /// The "import was interrupted" banner: title, message, one action, close.
    /// Not an InfoBar: that control has no default style in this island host
    /// and fail-fasts (0xC000027B) the moment it enters the tree, which this
    /// one does when the window is built.
    /// </summary>
    private sealed class Banner
    {
        // Translucent, so they read on the light and the dark theme alike.
        internal static readonly SolidColorBrush Neutral = new(ColorHelper.FromArgb(0x33, 0x80, 0x80, 0x80));
        internal static readonly SolidColorBrush Accent = new(
            new Windows.UI.ViewManagement.UISettings().GetColorValue(Windows.UI.ViewManagement.UIColorType.Accent));
        private static readonly SolidColorBrush Ink = new(
            new Windows.UI.ViewManagement.UISettings().GetColorValue(Windows.UI.ViewManagement.UIColorType.Foreground));
        private static readonly SolidColorBrush Muted = new(ColorHelper.FromArgb(0x99, 0x80, 0x80, 0x80));
        private static readonly SolidColorBrush Clear = new(Microsoft.UI.Colors.Transparent);
        private static readonly SolidColorBrush Highlight = new(ColorHelper.FromArgb(0x60, Accent.Color.R, Accent.Color.G, Accent.Color.B));
        private static readonly FontFamily InputFont = new("Segoe UI");

        // The window's type-in fields (Shared\FakeInput.cs): system text and
        // accent colours, as the rest of this window's default controls.
        internal static readonly MediaViewer.Shared.FakeInputLook InputLook = new()
        {
            Font = () => InputFont,
            FontSize = () => 14,
            Title = () => Ink,
            Body = () => Muted,
            Canvas = () => Clear,
            Hairline = () => Neutral,
            Selection = () => Highlight,
            SelectionInk = () => Ink,
        };

        public readonly Border Root;
        private readonly TextBlock _title = new() { FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        private readonly TextBlock _message = new() { TextWrapping = TextWrapping.Wrap };
        private readonly ContentControl _action = new() { VerticalAlignment = VerticalAlignment.Center };

        public Banner()
        {
            var text = new StackPanel { Spacing = 2 };
            text.Children.Add(_title);
            text.Children.Add(_message);
            var close = new Button { Content = "✕", Padding = new Thickness(8, 4, 8, 4), VerticalAlignment = VerticalAlignment.Center };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(close, "Close");
            close.Click += (_, _) => IsOpen = false;
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(text);
            Grid.SetColumn(_action, 1);
            row.Children.Add(_action);
            Grid.SetColumn(close, 2);
            row.Children.Add(close);
            Root = new Border
            {
                Child = row,
                Background = Neutral,
                BorderBrush = Accent,
                BorderThickness = new Thickness(0, 0, 0, 2),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(14, 10, 10, 10),
                Margin = new Thickness(0, 0, 0, 8),
                Visibility = Visibility.Collapsed,
            };
        }

        public string Title { set => _title.Text = value; }
        public string Message { set => _message.Text = value; }
        public Button? ActionButton { set => _action.Content = value; }

        public bool IsOpen
        {
            set => Root.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    // ---- keyboard -----------------------------------------------------------------------

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        bool ctrl = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        bool shift = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        object? focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        if (focused is MediaViewer.Shared.FakeInput) return;
        TileVm? tile = (focused as GridViewItem)?.Content as TileVm;
        switch (e.Key)
        {
            case VirtualKey.Enter when ctrl:
                StartImport();
                e.Handled = true;
                break;
            case VirtualKey.Enter when tile is not null:
                _chrome.Host.OpenInViewer(tile.Path);  // cull before copying
                e.Handled = true;
                break;
            case VirtualKey.Space when _copying:
                PauseResume();
                e.Handled = true;
                break;
            case VirtualKey.Space when tile is not null:
                if (shift) ToggleDay(tile); else Toggle(tile);
                e.Handled = true;
                break;
            case VirtualKey.Tab when ctrl:
                if (_sourceItems.Count > 0)
                {
                    int i = _sources.SelectedIndex;
                    _sources.SelectedIndex = ((i < 0 ? 0 : i) + (shift ? -1 : 1) + _sourceItems.Count) % _sourceItems.Count;
                }
                e.Handled = true;
                break;
            case VirtualKey.J when ctrl:
                Eject();
                e.Handled = true;
                break;
            case VirtualKey.Escape:
                Close();  // the job keeps running; the command bar shows it
                e.Handled = true;
                break;
        }
    }
}
