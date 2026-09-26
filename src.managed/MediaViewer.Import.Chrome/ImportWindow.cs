// SPDX-License-Identifier: GPL-2.0-or-later
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
}

/// <summary>
/// The Import window (plan/18 "The Import window"): sources on the left, the
/// day-grouped grid in the middle, the preset and "Where files go" on the
/// right, one primary button at the bottom. It becomes the progress view
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
    private readonly Button _importButton = new() { Content = "Import", Style = AccentStyle() };
    private readonly StackPanel _presetPanel = new() { Spacing = 8, Padding = new Thickness(12) };
    private readonly ListView _whereFilesGo = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 220 };
    private readonly StackPanel _progressPanel = new() { Spacing = 6, Visibility = Visibility.Collapsed };
    private readonly StackPanel _summaryPanel = new() { Spacing = 6, Visibility = Visibility.Collapsed };
    private readonly InfoBar _banner = new() { IsOpen = false, IsClosable = true };
    private readonly SemaphoreSlim _thumbGate = new(2);

    private IReadOnlyList<string> _marks = Array.Empty<string>();
    private string _root = "";
    private string _volumeId = "";
    private ulong _scan;
    private ulong _plan;
    private ulong _job;
    private JsonObject _preset = new();
    private JsonArray _presets = new();
    private string _destinationForOpen = "";
    private bool _copying;
    private bool _building;

    internal ImportWindow(ImportChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        Title = "Import";
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

        Grid.SetColumnSpan(_banner, 3);
        root.Children.Add(_banner);

        // Sources.
        var left = new StackPanel { Spacing = 8, Padding = new Thickness(12) };
        left.Children.Add(Text("SOURCES", 12));
        _sources.ItemTemplate = Template(
            "<StackPanel Padding=\"4\"><TextBlock Text=\"{Binding Label}\" FontSize=\"15\"/>" +
            "<TextBlock Text=\"{Binding Detail}\" FontSize=\"12\" Opacity=\"0.7\"/></StackPanel>");
        _sources.SelectionChanged += (_, _) =>
        {
            if (!_building && _sources.SelectedItem is SourceVm s) Load(s.Root, s.VolumeId);
        };
        left.Children.Add(_sources);
        var addFolder = new Button { Content = "＋ Folder…" };
        addFolder.Click += async (_, _) => await AddFolderSource();
        left.Children.Add(addFolder);
        var history = new Button { Content = "Imports…" };
        history.Click += (_, _) => ShowHistory();
        left.Children.Add(history);
        var verify = new Button { Content = "Verify a folder…" };
        ToolTipService.SetToolTip(verify, "Re-hash imported files against the library index to find silent corruption.");
        verify.Click += async (_, _) => await VerifyFolder();
        left.Children.Add(verify);
        Grid.SetRow(left, 1);
        root.Children.Add(left);

        // Contents.
        var centre = new Grid();
        centre.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        centre.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        _sourceTitle.Margin = new Thickness(8);
        centre.Children.Add(_sourceTitle);
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
        Grid.SetRow(_grid, 1);
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
        info.Children.Add(_bottomText);
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
        _importButton.Focus(FocusState.Programmatic);
    }

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
                });
            }
            if (current is not null && _sourceItems.All(s => s.Root != current))
            {
                _sourceItems.Insert(0, new SourceVm { Root = current, Label = current, Detail = "", Kind = "folder" });
            }
            SourceVm? pick = _sourceItems.FirstOrDefault(s => s.Root == current) ?? _sourceItems.FirstOrDefault();
            _sources.SelectedItem = pick;
            BuildPresetPanel();
            if (pick is not null && pick.Root != _root) Load(pick.Root, pick.VolumeId);
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
        Windows.Storage.StorageFolder? f = await picker.PickSingleFolderAsync();
        return f?.Path;
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

    private void Load(string root, string volumeId)
    {
        _root = root;
        _volumeId = volumeId;
        _sourceTitle.Text = root + " · reading…";
        _days.Clear();
        _plan = 0;
        try { _scan = _api.Scan(root); }
        catch (MediaViewerException ex) { _sourceTitle.Text = root + " · " + ex.Status; }
        BuildPresetPanel();
    }

    internal void OnScanDone(ulong scan, MvStatus status)
    {
        if (scan != _scan) return;
        if (status != MvStatus.Ok)
        {
            _sourceTitle.Text = _root + " · could not be read";
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
        string label = r.GetProperty("source").GetProperty("label").GetString() ?? _root;
        _sourceTitle.Text = $"{(label.Length > 0 ? label : _root)} · {t.GetProperty("new").GetInt64()} new of {t.GetProperty("units").GetInt64()} · {Format.Bytes(t.GetProperty("bytes").GetInt64())}";
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
        long dups = t.GetProperty("duplicates").GetInt64();
        long eta = t.GetProperty("eta_seconds").GetInt64();
        double rate = t.GetProperty("bytes_per_second").GetDouble();
        _bottomText.Text = $"{selFiles} files · {Format.Bytes(selBytes)} · {dups} duplicates skipped" +
                           (eta >= 0 ? $" · {Format.Eta(eta)} at {Format.Rate(rate)}" : " · time shown after the first import from this device");
        _importButton.Content = $"Import {selUnits}";
        _importButton.IsEnabled = selUnits > 0 && !_copying;
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
                DispatcherQueue.TryEnqueue(() => tile.Thumb = new BitmapImage(new Uri(path)));
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

    // ---- the preset panel ---------------------------------------------------------------

    private string P(string key, string fallback = "") => _preset[key]?.GetValue<string>() ?? fallback;
    private bool B(string key, bool fallback) => _preset[key]?.GetValue<bool>() ?? fallback;

    private void Set(string key, JsonNode? value)
    {
        _preset[key] = value;
        if (!_building) Replan();
    }

    private void BuildPresetPanel()
    {
        bool was = _building;
        _building = true;
        _presetPanel.Children.Clear();
        _presetPanel.Children.Add(Text("PRESET", 12));

        var presetBox = new ComboBox { MinWidth = 260 };
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

        _presetPanel.Children.Add(FolderRow("To", "destination", allowOff: false));
        _presetPanel.Children.Add(FolderRow("Backup", "backup", allowOff: true));

        _presetPanel.Children.Add(Combo("Selection", "selection",
            new[] { ("new", "New since last import"), ("all", "All"), ("marked", "Marked in viewer"), ("date_range", "Date range") }));
        if (P("selection") == "date_range")
        {
            _presetPanel.Children.Add(TextField("From (YYYY-MM-DD)", "range_from"));
            _presetPanel.Children.Add(TextField("To (YYYY-MM-DD)", "range_to"));
        }
        _presetPanel.Children.Add(TypeFilter());
        _presetPanel.Children.Add(Combo("Layout", "layout",
            new[] { ("YYYY/YYYY-MM-DD", "YYYY/YYYY-MM-DD"), ("YYYY/MM/DD", "YYYY/MM/DD"), ("YYYY-MM-DD", "YYYY-MM-DD"),
                    ("card", "Keep card structure"), ("flat", "Flat") }));
        _presetPanel.Children.Add(Toggle("+ camera model", "layout_camera", false));
        _presetPanel.Children.Add(Toggle("+ type folders (RAW / JPEG / Video)", "layout_type", false));
        _presetPanel.Children.Add(Combo("Date", "dates",
            new[] { ("taken", "Date taken, else file time"), ("file_time", "File time only") }));
        _presetPanel.Children.Add(TextField("Rename ({date} {time} {camera} {seq} {original}), empty = off", "rename"));
        _presetPanel.Children.Add(Toggle("Skip duplicates (by content)", "skip_duplicates", true));
        _presetPanel.Children.Add(Combo("Duplicate scope", "scope",
            new[] { ("destination", "This destination"), ("library", "The whole library index") }));
        _presetPanel.Children.Add(Toggle("Full verify (read back from the drive)", "full_verify", true));
        _presetPanel.Children.Add(Toggle("Eject the card when done", "eject_after", true));
        _presetPanel.Children.Add(Toggle("Notify when done", "notify", true));
        _presetPanel.Children.Add(Toggle("Fast (does not wait for the viewer)", "fast", false));
        _presetPanel.Children.Add(Text("Never offered: deleting from or formatting the card, overwriting a file, or any upload.", 11));

        _presetPanel.Children.Add(Text("WHERE FILES GO", 12));
        _presetPanel.Children.Add(_whereFilesGo);

        var name = new TextBox { PlaceholderText = "Preset name", Text = P("name", "Default") };
        var save = new Button { Content = "Save preset" };
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
            var bind = new CheckBox { Content = "Use this preset for this card" };
            var auto = new CheckBox { Content = "Auto-import this card on insert (never deletes)" };
            RoutedEventHandler apply = async (_, _) =>
            {
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
        _building = was;
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
        var box = new ComboBox { Header = label, MinWidth = 260 };
        foreach (var (_, text) in options) box.Items.Add(text);
        string current = P(key, options[0].Value);
        box.SelectedIndex = Math.Max(0, Array.FindIndex(options, o => o.Value == current));
        box.SelectionChanged += (_, _) =>
        {
            if (_building || box.SelectedIndex < 0) return;
            Set(key, options[box.SelectedIndex].Value);
            if (key == "selection") BuildPresetPanel();
        };
        return box;
    }

    private UIElement Toggle(string label, string key, bool fallback)
    {
        var t = new ToggleSwitch { Header = label, IsOn = B(key, fallback) };
        t.Toggled += (_, _) => { if (!_building) Set(key, t.IsOn); };
        return t;
    }

    private UIElement TextField(string label, string key)
    {
        var box = new TextBox { Header = label, Text = P(key) };
        box.LostFocus += (_, _) => { if (box.Text != P(key)) Set(key, box.Text); };
        return box;
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

    private void StartImport()
    {
        if (_copying || _plan == 0) return;
        try
        {
            _job = _api.Start(_plan);
            _chrome.Track(_job, _sourceTitle.Text.Split(" · ")[0]);
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
    }

    internal void OnProgress(ulong job, MvImportProgress p)
    {
        if (job != _job || !_copying) return;
        _progressPanel.Children.Clear();
        for (int d = 0; d < (int)Math.Min(2, p.DestinationCount); ++d)
        {
            ulong verified;
            unsafe { verified = p.BytesVerified[d]; }
            _progressPanel.Children.Add(new ProgressBar
            {
                Maximum = Math.Max(1, p.BytesTotal),
                Value = verified,
                Width = 520,
                HorizontalAlignment = HorizontalAlignment.Left,
            });
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
            var eject = new Button { Content = "Eject (Ctrl+J)" };
            eject.Click += (_, _) => Eject();
            var open = new Button { Content = "Open in viewer" };
            open.Click += (_, _) => { if (_destinationForOpen.Length > 0) _chrome.Host.OpenInViewer(_destinationForOpen); };
            var report = new Button { Content = "Show report" };
            string reportPath = s.GetProperty("report").GetString() ?? "";
            report.IsEnabled = reportPath.Length > 0;
            report.Click += (_, _) => _ = Launcher.LaunchUriAsync(new Uri(reportPath));
            buttons.Children.Add(eject);
            buttons.Children.Add(open);
            buttons.Children.Add(report);
            _summaryPanel.Children.Add(buttons);
            eject.Focus(FocusState.Programmatic);
        }
        _summaryPanel.Visibility = Visibility.Visible;
        if (_scan != 0 && state != MvImportJobState.Cancelled) Load(_root, _volumeId);  // re-plan: what is new now
    }

    private void Eject()
    {
        string root = _root;
        _ = Task.Run(() =>
        {
            string msg;
            try { _api.Eject(root); msg = "Ejected. The card can be removed."; }
            catch (MediaViewerException) { msg = "The card is in use and was not ejected."; }
            DispatcherQueue.TryEnqueue(() => _bottomText.Text = msg);
        });
    }

    private async void ShowHistory()
    {
        string json;
        try { json = await Task.Run(() => _api.HistoryJson()); }
        catch (MediaViewerException) { return; }
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

    // ---- keyboard -----------------------------------------------------------------------

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        bool ctrl = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        bool shift = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        object? focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        if (focused is TextBox) return;
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
