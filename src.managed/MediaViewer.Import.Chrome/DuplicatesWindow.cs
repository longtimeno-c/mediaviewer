// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;
using Windows.System;

namespace MediaViewer.Import.Chrome;

/// <summary>One file of a duplicate group.</summary>
public sealed class DupFileVm : INotifyPropertyChanged
{
    private static readonly HashSet<string> s_photo = new(StringComparer.OrdinalIgnoreCase)
    {
        ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".webp", ".heic", ".heif", ".avif", ".ico",
        ".dng", ".cr2", ".cr3", ".nef", ".arw", ".raf", ".orf", ".rw2", ".pef", ".srw",
    };
    private static readonly HashSet<string> s_video = new(StringComparer.OrdinalIgnoreCase)
    {
        ".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".ts", ".mts", ".m2ts",
    };

    private string _state = "kept";
    private string _reason = "";

    public string Path { get; init; } = "";
    public string Relative { get; init; } = "";
    public long Mtime { get; init; }
    public DupGroupVm Group { get; init; } = null!;

    /// <summary>The file's name, the row's first line.</summary>
    public string Name => System.IO.Path.GetFileName(Relative);

    /// <summary>Where it sits under the scanned folder, the row's second line.</summary>
    public string Folder
    {
        get
        {
            string dir = System.IO.Path.GetDirectoryName(Relative) ?? "";
            return dir.Length == 0 ? "Top of the folder" : dir;
        }
    }

    public string State
    {
        get => _state;
        set
        {
            if (_state == value) return;
            _state = value;
            Changed();
            Changed(nameof(Detail));
            Changed(nameof(Dim));
            Changed(nameof(Glyph));
        }
    }

    public string Reason
    {
        get => _reason;
        set { if (_reason != value) { _reason = value; Changed(); Changed(nameof(Detail)); } }
    }

    public double Dim => _state == "trashed" ? 0.45 : 1.0;

    /// <summary>Segoe Fluent Icons: the state once there is one, else the kind of file.</summary>
    public string Glyph => _state switch
    {
        "trashed" => "",  // Delete
        "queued" => "",   // Sync
        "refused" => "",  // Warning
        _ => s_photo.Contains(System.IO.Path.GetExtension(Relative)) ? ""  // Photo2
           : s_video.Contains(System.IO.Path.GetExtension(Relative)) ? ""  // Video
           : "",  // Page
    };

    public string Detail => _state switch
    {
        "trashed" => "Moved to the Recycle Bin",
        "queued" => "Checking the other copy, then moving to the Recycle Bin…",
        "refused" => _reason switch
        {
            "last_copy" => "Kept: no other copy with the same bytes is left",
            "changed" => "Kept: this file changed since the scan. Scan again.",
            "no_bin" => "Kept: this drive has no Recycle Bin, and nothing is deleted outright",
            _ => "Kept: it could not be moved to the Recycle Bin",
        },
        _ => "Modified " + DateTimeOffset.FromUnixTimeSeconds(Mtime).LocalDateTime.ToString("g"),
    };

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>Files with identical bytes: same size, same BLAKE3.</summary>
public sealed class DupGroupVm : ObservableCollection<DupFileVm>
{
    public string Header { get; init; } = "";
    public string Sub { get; init; } = "";
    public long Size { get; init; }
    /// <summary>Copies still on disk; the engine never lets this reach zero.</summary>
    public int Alive => this.Count(f => f.State != "trashed");
}

/// <summary>
/// Find duplicates (PR 54), the Windows twin of DuplicatesView.swift: pick a
/// folder, the engine walks it and every folder under it and groups files
/// with identical bytes (size, then BLAKE3; never the name). Each file can be
/// opened in the viewer or shown in Explorer, and the copies the person picks
/// (one, or several with Ctrl / Shift) are moved to the Recycle Bin in one go.
/// A pick that would take every copy of a file is refused here, and the
/// engine still refuses to recycle the last copy in a group and re-reads the
/// copy it keeps before it moves anything. Keyboard-complete: arrows move
/// through the files (Shift+arrows / Ctrl+Space pick several), Enter opens one
/// in the viewer, Delete moves the picked copies to the Recycle Bin, Ctrl+E
/// shows one in Explorer, Esc closes (a scan carries on).
/// </summary>
internal sealed class DuplicatesWindow : Window
{
    private const string IconFont = "Segoe Fluent Icons,Segoe MDL2 Assets";

    private readonly ImportChrome _chrome;
    private readonly ImportApi _api;
    private readonly DuplicatesApi _dups;

    private readonly TextBlock _folder = Secondary("", 12);
    private readonly TextBlock _headline = new() { FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _progressText = Secondary("", 12);
    private readonly TextBlock _selectionText = Secondary("", 12);
    private readonly MediaViewer.Shared.FlatBar _bar;
    private readonly ListView _list = new()
    {
        // Explorer's picking: click one, Ctrl+click to add, Shift+click for a run.
        SelectionMode = ListViewSelectionMode.Extended,
        IsDoubleTapEnabled = true,
        Padding = new Thickness(0, 4, 0, 8),
    };
    private readonly CollectionViewSource _view = new() { IsSourceGrouped = true };
    private readonly ObservableCollection<DupGroupVm> _groups = new();
    private readonly Dictionary<string, DupFileVm> _byPath = new();
    private readonly Button _open = IconButton("", "Open in viewer");
    private readonly Button _reveal = IconButton("", "Show in Explorer");
    private readonly Button _trash = IconButton("", "Move to Recycle Bin");
    private readonly Button _clear = IconButton("", "Clear selection");
    private readonly Button _stop = IconButton("", "Stop");
    private readonly Button _report = IconButton("", "Show report");
    private readonly Button _again = IconButton("", "Scan another folder…");

    private ulong _job;
    private bool _running;
    private bool _canTrash;
    private string _reportPath = "";
    private DupFileVm? _focused;

    internal DuplicatesWindow(ImportChrome chrome, DuplicatesApi dups)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _dups = dups;
        _bar = new MediaViewer.Shared.FlatBar(Banner.Neutral, Banner.Accent);
        Title = "Find Duplicates";
        MediaViewer.Shared.AppIcon.Apply(this);
        bool mica = MicaController.IsSupported();
        if (mica)
        {
            try { SystemBackdrop = new MicaBackdrop(); }
            catch (Exception ex) { Debug.WriteLine(ex.Message); mica = false; }
        }
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1040, 740));
        Content = BuildLayout(mica);
        _view.Source = _groups;
        _list.ItemsSource = _view.View;
        UpdateButtons();
    }

    internal ulong Job => _job;
    internal string Headline => _headline.Text;

    /// <summary>Secondary text, its colour a live ThemeResource so it follows light / dark.</summary>
    private static TextBlock Secondary(string s, double size)
    {
        var t = (TextBlock)XamlReader.Load(
            """
            <TextBlock xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                       TextWrapping="Wrap" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
            """);
        t.Text = s;
        t.FontSize = size;
        return t;
    }

    private static Button IconButton(string glyph, string label)
    {
        var b = new Button();
        SetButton(b, glyph, label);
        return b;
    }

    private static void SetButton(Button b, string glyph, string label)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        row.Children.Add(new TextBlock
        {
            Text = glyph,
            FontFamily = new FontFamily(IconFont),
            FontSize = 14,
            VerticalAlignment = VerticalAlignment.Center,
        });
        row.Children.Add(new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center });
        b.Content = row;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, label);
    }

    private static DataTemplate Template(string body) => (DataTemplate)XamlReader.Load(
        "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\">" + body + "</DataTemplate>");

    /// <summary>A Windows 11 card: the card fill with its hairline stroke.</summary>
    private static Border Card(UIElement child, Thickness padding)
    {
        var b = (Border)XamlReader.Load(
            """
            <Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                    BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}"
                    BorderThickness="1" CornerRadius="8"/>
            """);
        b.Child = child;
        b.Padding = padding;
        return b;
    }

    private UIElement BuildLayout(bool mica)
    {
        // Keep the ThemeResource expression on the live root so Windows updates
        // it when appearance changes; transparent over Mica, which is the page.
        var root = (Grid)XamlReader.Load(mica
            ? """<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Padding="24,16,24,16" RowSpacing="12"/>"""
            : """
              <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" Padding="24,16,24,16" RowSpacing="12"/>
              """);
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // title
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // summary card
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // selection bar
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });  // list
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // footnote

        // Title: the tool's mark, its name, the folder, and a new scan on the right.
        var title = new Grid { ColumnSpacing = 12 };
        title.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        title.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        title.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var mark = new Border
        {
            Width = 40,
            Height = 40,
            CornerRadius = new CornerRadius(8),
            Background = Banner.Accent,
            VerticalAlignment = VerticalAlignment.Center,
            Child = new TextBlock
            {
                Text = "",  // Copy
                FontFamily = new FontFamily(IconFont),
                FontSize = 20,
                Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            },
        };
        title.Children.Add(mark);
        var names = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        names.Children.Add(new TextBlock
        {
            Text = "Find Duplicates",
            FontSize = 24,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        });
        _folder.TextWrapping = TextWrapping.NoWrap;
        _folder.TextTrimming = TextTrimming.CharacterEllipsis;
        names.Children.Add(_folder);
        Grid.SetColumn(names, 1);
        title.Children.Add(names);
        _again.VerticalAlignment = VerticalAlignment.Center;
        _again.Click += async (_, _) => await ChooseAndStart();
        Grid.SetColumn(_again, 2);
        title.Children.Add(_again);
        root.Children.Add(title);

        // Summary: what the scan found (or how far it is), with Stop / the report.
        var summary = new Grid { ColumnSpacing = 12 };
        summary.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        summary.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var lines = new StackPanel { Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        lines.Children.Add(_headline);
        lines.Children.Add(_bar.Root);
        lines.Children.Add(_progressText);
        summary.Children.Add(lines);
        var side = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        _stop.Click += (_, _) => { try { _api.Cancel(_job); } catch (MediaViewerException) { } };
        _report.Click += (_, _) => { if (_reportPath.Length > 0) _ = Launcher.LaunchUriAsync(new Uri(_reportPath)); };
        side.Children.Add(_stop);
        side.Children.Add(_report);
        Grid.SetColumn(side, 1);
        summary.Children.Add(side);
        Border summaryCard = Card(summary, new Thickness(16, 14, 16, 14));
        Grid.SetRow(summaryCard, 1);
        root.Children.Add(summaryCard);

        // Selection bar: how much is picked, and what to do with it.
        var bar = new Grid { ColumnSpacing = 8 };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _selectionText.VerticalAlignment = VerticalAlignment.Center;
        bar.Children.Add(_selectionText);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _open.Click += (_, _) => OpenFocused();
        _reveal.Click += (_, _) => RevealFocused();
        _clear.Click += (_, _) => _list.SelectedItems.Clear();
        _trash.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        _trash.Click += (_, _) => TrashSelection();
        ToolTipService.SetToolTip(_open, "Enter");
        ToolTipService.SetToolTip(_reveal, "Ctrl+E");
        foreach (Button b in new[] { _open, _reveal, _clear, _trash }) actions.Children.Add(b);
        Grid.SetColumn(actions, 1);
        bar.Children.Add(actions);
        Grid.SetRow(bar, 2);
        root.Children.Add(bar);

        // The groups.
        _list.ItemTemplate = Template(
            """
            <Grid Padding="4,8,4,8" ColumnSpacing="14" Opacity="{Binding Dim}">
              <Grid.ColumnDefinitions>
                <ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/>
              </Grid.ColumnDefinitions>
              <TextBlock Text="{Binding Glyph}" FontFamily="Segoe Fluent Icons,Segoe MDL2 Assets" FontSize="20"
                         VerticalAlignment="Center" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
              <StackPanel Grid.Column="1" VerticalAlignment="Center" Spacing="1">
                <TextBlock Text="{Binding Name}" TextTrimming="CharacterEllipsis"/>
                <TextBlock Text="{Binding Folder}" FontSize="12" TextTrimming="CharacterEllipsis"
                           Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
              </StackPanel>
              <TextBlock Grid.Column="2" Text="{Binding Detail}" FontSize="12" MaxWidth="320" TextWrapping="Wrap"
                         TextAlignment="Right" VerticalAlignment="Center" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
            </Grid>
            """);
        _list.GroupStyle.Add(new GroupStyle
        {
            HeaderTemplate = Template(
                """
                <StackPanel Orientation="Horizontal" Spacing="10">
                  <TextBlock Text="{Binding Header}" FontSize="14" FontWeight="SemiBold" VerticalAlignment="Center"/>
                  <TextBlock Text="{Binding Sub}" FontSize="12" VerticalAlignment="Center"
                             Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
                </StackPanel>
                """),
        });
        _list.SelectionChanged += (_, e) =>
        {
            if (e.AddedItems.Count > 0 && e.AddedItems[^1] is DupFileVm added) _focused = added;
            UpdateButtons();
        };
        _list.DoubleTapped += (_, e) =>
        {
            if ((e.OriginalSource as FrameworkElement)?.DataContext is DupFileVm f) _focused = f;
            OpenFocused();
        };
        _list.KeyDown += OnListKeyDown;
        Border listCard = Card(_list, new Thickness(4, 0, 4, 0));
        Grid.SetRow(listCard, 3);
        root.Children.Add(listCard);

        TextBlock foot = Secondary(
            "Originals stay put. Moved files can be restored from the Recycle Bin, and the last copy of a file is always kept.",
            12);
        Grid.SetRow(foot, 4);
        root.Children.Add(foot);

        root.KeyDown += (_, e) =>
        {
            if (e.Key == VirtualKey.Escape)
            {
                Close();  // a running scan carries on; the command bar shows it
                e.Handled = true;
            }
        };
        return root;
    }

    // ---- starting -----------------------------------------------------------------

    internal async Task ChooseAndStart()
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        string? dir;
        try { dir = (await picker.PickSingleFolderAsync())?.Path; }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            dir = null;
        }
        if (dir is null) return;
        Start(dir);
    }

    private void Start(string dir)
    {
        try { _job = _dups.Find(dir); }
        catch (MediaViewerException ex)
        {
            _headline.Text = "Could not start: " + ex.Status;
            return;
        }
        _chrome.Track(_job, "duplicates");
        _running = true;
        _groups.Clear();
        _byPath.Clear();
        _focused = null;
        _reportPath = "";
        _folder.Text = dir;
        _headline.Text = "Looking for duplicates…";
        _progressText.Text = "Looking through " + dir + "…";
        _bar.IsIndeterminate = true;
        _bar.Root.Visibility = Visibility.Visible;
        UpdateButtons();
        Activate();
    }

    internal void OnProgress(ulong job, MvImportProgress p)
    {
        if (job != _job || !_running) return;
        if (p.BytesTotal == 0 && p.UnitsDone == 0)
        {
            _bar.IsIndeterminate = true;
            _progressText.Text = $"Looking through {p.UnitsTotal} files…";
            return;
        }
        _bar.IsIndeterminate = false;
        _bar.Value = p.UnitsTotal == 0 ? 0 : (double)p.UnitsDone / p.UnitsTotal;
        string rate = Format.Rate(p.BytesPerSecond);
        _progressText.Text = $"Comparing {p.UnitsDone} of {p.UnitsTotal} files that share a size" +
                             (rate.Length > 0 ? " · " + rate : "");
    }

    // ---- results ------------------------------------------------------------------

    /// <summary>The scan finished, or a Recycle Bin request settled: re-read the summary.</summary>
    internal async Task Refresh(ulong job)
    {
        if (job != _job) return;
        string json;
        try { json = await Task.Run(() => _api.SummaryJson(job)); }
        catch (MediaViewerException) { return; }
        if (job != _job) return;
        bool first = _running || _groups.Count == 0;
        _running = false;
        _bar.Root.Visibility = Visibility.Collapsed;
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement s = doc.RootElement;
        _canTrash = s.TryGetProperty("can_trash", out JsonElement ct) && ct.GetBoolean();
        string root = s.GetProperty("folder").GetString() ?? "";
        string prefix = root.EndsWith('\\') || root.EndsWith('/') ? root : root + "\\";

        if (first) Build(s, prefix);
        else Update(s);

        long wasted = s.GetProperty("wasted_bytes").GetInt64();
        long dupes = s.GetProperty("duplicate_files").GetInt64();
        long files = s.GetProperty("files").GetInt64();
        int groups = s.GetProperty("groups").GetArrayLength();
        bool failed = s.GetProperty("walk_failed").GetBoolean();
        _headline.Text =
            failed ? "This folder could not be read." :
            groups == 0 ? $"No duplicates among {files} files." :
            $"{dupes} duplicate {(dupes == 1 ? "file" : "files")} in {groups} {(groups == 1 ? "group" : "groups")} · " +
            $"{Format.Bytes(wasted)} could be freed";
        var notes = new List<string>();
        if (s.GetProperty("cancelled").GetBoolean()) notes.Add("Stopped early: not every file was compared.");
        if (!_canTrash && groups > 0) notes.Add("Moving to the Recycle Bin needs a newer MediaViewer.");
        if (!failed && groups > 0 && notes.Count == 0)
        {
            notes.Add($"Compared {files} files by content. Pick the copies you don't need, then move them to the Recycle Bin.");
        }
        _progressText.Text = string.Join(" ", notes);
        try { _reportPath = await Task.Run(() => _api.ReportPath(job)); }
        catch (MediaViewerException) { _reportPath = ""; }
        UpdateButtons();
    }

    private void Build(JsonElement s, string prefix)
    {
        _groups.Clear();
        _byPath.Clear();
        _focused = null;
        foreach (JsonElement g in s.GetProperty("groups").EnumerateArray())
        {
            JsonElement members = g.GetProperty("files");
            int n = members.GetArrayLength();
            long size = g.GetProperty("size").GetInt64();
            var group = new DupGroupVm
            {
                Header = $"{n} identical files",
                Sub = $"{Format.Bytes(size)} each · {Format.Bytes(size * (n - 1))} could be freed",
                Size = size,
            };
            foreach (JsonElement f in members.EnumerateArray())
            {
                string path = f.GetProperty("path").GetString() ?? "";
                var vm = new DupFileVm
                {
                    Path = path,
                    Relative = path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? path[prefix.Length..] : path,
                    Mtime = f.GetProperty("mtime").GetInt64(),
                    Group = group,
                    State = f.GetProperty("state").GetString() ?? "kept",
                    Reason = f.GetProperty("reason").GetString() ?? "",
                };
                group.Add(vm);
                _byPath[path] = vm;
            }
            _groups.Add(group);
        }
        if (_groups.Count > 0 && _groups[0].Count > 0) _list.SelectedItem = _groups[0][0];
        _list.Focus(FocusState.Programmatic);
    }

    /// <summary>In place, so the list keeps its scroll position and selection.</summary>
    private void Update(JsonElement s)
    {
        foreach (JsonElement g in s.GetProperty("groups").EnumerateArray())
        {
            foreach (JsonElement f in g.GetProperty("files").EnumerateArray())
            {
                if (!_byPath.TryGetValue(f.GetProperty("path").GetString() ?? "", out DupFileVm? vm)) continue;
                vm.State = f.GetProperty("state").GetString() ?? "kept";
                vm.Reason = f.GetProperty("reason").GetString() ?? "";
            }
        }
    }

    // ---- actions ------------------------------------------------------------------

    private List<DupFileVm> Picked => _list.SelectedItems.OfType<DupFileVm>().ToList();

    /// <summary>The file Open and Show in Explorer act on: the last one picked.</summary>
    private DupFileVm? Focused
    {
        get
        {
            List<DupFileVm> picked = Picked;
            return _focused is not null && picked.Contains(_focused) ? _focused : picked.LastOrDefault();
        }
    }

    private bool CanTrash(DupFileVm f) =>
        _canTrash && f.State is not ("trashed" or "queued") && f.Group.Alive > 1;

    /// <summary>
    /// The picked copies that can go to the bin, and whether the pick would
    /// take every copy of some file: then nothing is sent, so which copy
    /// survives is never down to the order the engine ran the requests in.
    /// </summary>
    private (List<DupFileVm> Files, bool TakesEveryCopy) Batch()
    {
        List<DupFileVm> files = Picked.Where(CanTrash).ToList();
        bool every = files.GroupBy(f => f.Group).Any(g =>
            g.Key.Count(f => f.State is not ("trashed" or "queued")) - g.Count() < 1);
        return (files, every);
    }

    private void UpdateButtons()
    {
        DupFileVm? f = Focused;
        bool present = f is not null && f.State != "trashed";
        _open.IsEnabled = present;
        _reveal.IsEnabled = present;

        List<DupFileVm> picked = Picked;
        (List<DupFileVm> batch, bool every) = Batch();
        _trash.IsEnabled = !_running && batch.Count > 0 && !every;
        SetButton(_trash, "", batch.Count > 1 ? $"Move {batch.Count} to Recycle Bin" : "Move to Recycle Bin");
        ToolTipService.SetToolTip(_trash,
            every ? "Every copy of a file is picked. Leave one unpicked to keep it."
            : picked.Count > 0 && batch.Count == 0 && picked.Any(p => p.Group.Alive <= 1) ? "The last copy is always kept."
            : "Delete. Each copy goes only while another identical copy stays.");
        _clear.Visibility = picked.Count > 1 ? Visibility.Visible : Visibility.Collapsed;

        long bytes = batch.Sum(p => p.Group.Size);
        _selectionText.Text =
            _groups.Count == 0 ? "" :
            every ? $"{picked.Count} picked, including every copy of a file. Leave one copy of each file unpicked." :
            picked.Count > 1 ? $"{picked.Count} picked · {Format.Bytes(bytes)} to free. Ctrl+click adds or removes one, Shift+click picks a run." :
            "Ctrl+click or Shift+click to pick several copies, then move them to the Recycle Bin together.";

        _stop.Visibility = _running ? Visibility.Visible : Visibility.Collapsed;
        _report.Visibility = _running ? Visibility.Collapsed : Visibility.Visible;
        _report.IsEnabled = _reportPath.Length > 0;
        _again.IsEnabled = !_running;
    }

    private void OpenFocused()
    {
        if (Focused is { } f && f.State != "trashed") _chrome.Host.OpenInViewer(f.Path);
    }

    private void RevealFocused()
    {
        if (Focused is not { } f || f.State == "trashed") return;
        string path = f.Path;
        // Off the UI thread: starting a process can stall (rule 1).
        _ = Task.Run(() =>
        {
            try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = false }); }
            catch (Exception ex) when (ex is Win32Exception or InvalidOperationException) { }
        });
    }

    /// <summary>
    /// Queues each picked copy. No-block: every call only queues; the engine
    /// checks and moves them one at a time on its own thread, and a refusal
    /// shows on that file's row.
    /// </summary>
    private void TrashSelection()
    {
        if (_running) return;
        (List<DupFileVm> batch, bool every) = Batch();
        if (batch.Count == 0 || every) return;
        foreach (DupFileVm f in batch)
        {
            if (_dups.Trash(_job, f.Path) != MvStatus.Ok) continue;
            f.State = "queued";
            f.Reason = "";
            _list.SelectedItems.Remove(f);
        }
        UpdateButtons();
    }

    private void OnListKeyDown(object sender, KeyRoutedEventArgs e)
    {
        bool ctrl = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        switch (e.Key)
        {
            case VirtualKey.Enter:
                OpenFocused();
                e.Handled = true;
                break;
            case VirtualKey.Delete:
                TrashSelection();
                e.Handled = true;
                break;
            case VirtualKey.E when ctrl:
                RevealFocused();
                e.Handled = true;
                break;
        }
    }

    /// <summary>The Import window's translucent brushes, so both windows read alike.</summary>
    private static class Banner
    {
        internal static readonly SolidColorBrush Neutral =
            new(Microsoft.UI.ColorHelper.FromArgb(0x33, 0x80, 0x80, 0x80));
        internal static readonly SolidColorBrush Accent = new(
            new Windows.UI.ViewManagement.UISettings().GetColorValue(Windows.UI.ViewManagement.UIColorType.Accent));
    }
}
