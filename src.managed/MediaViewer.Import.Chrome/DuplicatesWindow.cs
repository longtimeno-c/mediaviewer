// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Windows.Storage.Pickers;
using Windows.System;

namespace MediaViewer.Import.Chrome;

/// <summary>One file of a duplicate group.</summary>
public sealed class DupFileVm : INotifyPropertyChanged
{
    private string _state = "kept";
    private string _reason = "";

    public string Path { get; init; } = "";
    public string Relative { get; init; } = "";
    public long Mtime { get; init; }
    public DupGroupVm Group { get; init; } = null!;

    public string State
    {
        get => _state;
        set { if (_state != value) { _state = value; Changed(); Changed(nameof(Detail)); Changed(nameof(Dim)); } }
    }

    public string Reason
    {
        get => _reason;
        set { if (_reason != value) { _reason = value; Changed(); Changed(nameof(Detail)); } }
    }

    public double Dim => _state == "trashed" ? 0.45 : 1.0;

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
    /// <summary>Copies still on disk; the engine never lets this reach zero.</summary>
    public int Alive => this.Count(f => f.State != "trashed");
}

/// <summary>
/// Find duplicates (PR 54), the Windows twin of DuplicatesView.swift: pick a
/// folder, the engine walks it and every folder under it and groups files
/// with identical bytes (size, then BLAKE3; never the name). Each file can be
/// opened in the viewer, shown in Explorer, or moved to the Recycle Bin. The
/// engine refuses to recycle the last copy in a group, and re-reads the copy
/// it keeps before it moves anything. Keyboard-complete: arrows move through
/// the files, Enter opens one in the viewer, Delete moves it to the Recycle
/// Bin, Ctrl+E shows it in Explorer, Esc closes (a scan carries on).
/// </summary>
internal sealed class DuplicatesWindow : Window
{
    private readonly ImportChrome _chrome;
    private readonly ImportApi _api;
    private readonly DuplicatesApi _dups;

    private readonly TextBlock _folder = Text("", 12, 0.7);
    private readonly TextBlock _headline = Text("", 16);
    private readonly TextBlock _progressText = Text("", 12, 0.7);
    private readonly MediaViewer.Shared.FlatBar _bar;
    private readonly ListView _list = new() { SelectionMode = ListViewSelectionMode.Single, IsDoubleTapEnabled = true };
    private readonly CollectionViewSource _view = new() { IsSourceGrouped = true };
    private readonly ObservableCollection<DupGroupVm> _groups = new();
    private readonly Dictionary<string, DupFileVm> _byPath = new();
    private readonly Button _open = new() { Content = "Open in viewer (Enter)" };
    private readonly Button _reveal = new() { Content = "Show in Explorer (Ctrl+E)" };
    private readonly Button _trash = new() { Content = "Move to Recycle Bin (Delete)" };
    private readonly Button _stop = new() { Content = "Stop" };
    private readonly Button _report = new() { Content = "Show report" };
    private readonly Button _again = new() { Content = "Scan another folder…" };

    private ulong _job;
    private bool _running;
    private bool _canTrash;
    private string _reportPath = "";

    internal DuplicatesWindow(ImportChrome chrome, DuplicatesApi dups)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _dups = dups;
        _bar = new MediaViewer.Shared.FlatBar(Banner.Neutral, Banner.Accent);
        Title = "Find Duplicates";
        AppWindow.Resize(new Windows.Graphics.SizeInt32(980, 700));
        Content = BuildLayout();
        _view.Source = _groups;
        _list.ItemsSource = _view.View;
        UpdateButtons();
    }

    internal ulong Job => _job;
    internal string Headline => _headline.Text;

    private static TextBlock Text(string s, double size = 14, double opacity = 1) =>
        new() { Text = s, FontSize = size, Opacity = opacity, TextWrapping = TextWrapping.Wrap };

    private static DataTemplate Template(string body) => (DataTemplate)XamlReader.Load(
        "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\">" + body + "</DataTemplate>");

    private UIElement BuildLayout()
    {
        var root = (Grid)XamlReader.Load(
            """
            <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                  Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" Padding="16" RowSpacing="8"/>
            """);
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var top = new StackPanel { Spacing = 4 };
        top.Children.Add(Text("Find Duplicates", 20));
        top.Children.Add(_folder);
        top.Children.Add(_headline);
        _bar.Root.Width = 520;
        _bar.Root.HorizontalAlignment = HorizontalAlignment.Left;
        top.Children.Add(_bar.Root);
        top.Children.Add(_progressText);
        root.Children.Add(top);

        _list.ItemTemplate = Template(
            "<StackPanel Padding=\"4\" Opacity=\"{Binding Dim}\">" +
            "<TextBlock Text=\"{Binding Relative}\" TextTrimming=\"CharacterEllipsis\"/>" +
            "<TextBlock Text=\"{Binding Detail}\" FontSize=\"12\" Opacity=\"0.7\"/></StackPanel>");
        _list.GroupStyle.Add(new GroupStyle
        {
            HeaderTemplate = Template("<TextBlock Text=\"{Binding Header}\" FontSize=\"15\"/>"),
        });
        _list.SelectionChanged += (_, _) => UpdateButtons();
        _list.DoubleTapped += (_, _) => OpenSelected();
        _list.KeyDown += OnListKeyDown;
        Grid.SetRow(_list, 1);
        root.Children.Add(_list);

        var bottom = new StackPanel { Spacing = 8 };
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _open.Click += (_, _) => OpenSelected();
        _reveal.Click += (_, _) => RevealSelected();
        _trash.Click += (_, _) => TrashSelected();
        _stop.Click += (_, _) => { try { _api.Cancel(_job); } catch (MediaViewerException) { } };
        _report.Click += (_, _) => { if (_reportPath.Length > 0) _ = Launcher.LaunchUriAsync(new Uri(_reportPath)); };
        _again.Click += async (_, _) => await ChooseAndStart();
        foreach (Button b in new[] { _open, _reveal, _trash, _stop, _report, _again }) actions.Children.Add(b);
        bottom.Children.Add(actions);
        bottom.Children.Add(Text(
            "Originals stay put. Move to Recycle Bin can be undone from the Recycle Bin; the last copy of a file is always kept.",
            12, 0.7));
        Grid.SetRow(bottom, 2);
        root.Children.Add(bottom);

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
        _reportPath = "";
        _folder.Text = dir;
        _headline.Text = "";
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
        _progressText.Text = $"Comparing {p.UnitsDone} of {p.UnitsTotal} files that share a size · {Format.Rate(p.BytesPerSecond)}";
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
        _progressText.Text = "";
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
        string headline =
            s.GetProperty("walk_failed").GetBoolean() ? "This folder could not be read." :
            groups == 0 ? $"No duplicates among {files} files." :
            $"{dupes} duplicate {(dupes == 1 ? "file" : "files")} in {groups} {(groups == 1 ? "group" : "groups")} · " +
            $"{Format.Bytes(wasted)} could be freed";
        if (s.GetProperty("cancelled").GetBoolean()) headline += " (stopped early: not every file was compared)";
        if (!_canTrash && groups > 0) headline += ". Moving to the Recycle Bin needs a newer MediaViewer.";
        _headline.Text = headline;
        try { _reportPath = await Task.Run(() => _api.ReportPath(job)); }
        catch (MediaViewerException) { _reportPath = ""; }
        UpdateButtons();
    }

    private void Build(JsonElement s, string prefix)
    {
        _groups.Clear();
        _byPath.Clear();
        foreach (JsonElement g in s.GetProperty("groups").EnumerateArray())
        {
            JsonElement members = g.GetProperty("files");
            var group = new DupGroupVm
            {
                Header = $"{members.GetArrayLength()} identical files · {Format.Bytes(g.GetProperty("size").GetInt64())} each",
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

    private DupFileVm? Selected => _list.SelectedItem as DupFileVm;

    private bool CanTrash(DupFileVm f) =>
        _canTrash && f.State is not ("trashed" or "queued") && f.Group.Alive > 1;

    private void UpdateButtons()
    {
        DupFileVm? f = Selected;
        bool present = f is not null && f.State != "trashed";
        _open.IsEnabled = present;
        _reveal.IsEnabled = present;
        _trash.IsEnabled = f is not null && !_running && CanTrash(f);
        ToolTipService.SetToolTip(_trash, f is not null && f.Group.Alive <= 1
            ? "The last copy is always kept."
            : "Moves this copy to the Recycle Bin; another copy stays.");
        _stop.Visibility = _running ? Visibility.Visible : Visibility.Collapsed;
        _report.Visibility = _running ? Visibility.Collapsed : Visibility.Visible;
        _report.IsEnabled = _reportPath.Length > 0;
        _again.Visibility = _running ? Visibility.Collapsed : Visibility.Visible;
    }

    private void OpenSelected()
    {
        if (Selected is { } f && f.State != "trashed") _chrome.Host.OpenInViewer(f.Path);
    }

    private void RevealSelected()
    {
        if (Selected is not { } f || f.State == "trashed") return;
        string path = f.Path;
        // Off the UI thread: starting a process can stall (rule 1).
        _ = Task.Run(() =>
        {
            try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = false }); }
            catch (Exception ex) when (ex is Win32Exception or InvalidOperationException) { }
        });
    }

    private void TrashSelected()
    {
        if (Selected is not { } f || _running || !CanTrash(f)) return;
        MvStatus s = _dups.Trash(_job, f.Path);
        if (s == MvStatus.Ok)
        {
            f.State = "queued";
            f.Reason = "";
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
                OpenSelected();
                e.Handled = true;
                break;
            case VirtualKey.Delete:
                TrashSelected();
                e.Handled = true;
                break;
            case VirtualKey.E when ctrl:
                RevealSelected();
                e.Handled = true;
                break;
        }
    }

    /// <summary>The Import window's translucent brushes, so both windows read alike.</summary>
    private static class Banner
    {
        internal static readonly Microsoft.UI.Xaml.Media.SolidColorBrush Neutral =
            new(Microsoft.UI.ColorHelper.FromArgb(0x33, 0x80, 0x80, 0x80));
        internal static readonly Microsoft.UI.Xaml.Media.SolidColorBrush Accent = new(
            new Windows.UI.ViewManagement.UISettings().GetColorValue(Windows.UI.ViewManagement.UIColorType.Accent));
    }
}
