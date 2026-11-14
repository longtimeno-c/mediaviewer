// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Windows.Storage.Pickers;
using Windows.System;

namespace MediaViewer.Import.Chrome;

/// <summary>One line of the report: a file, and what it is or where its bytes are.</summary>
public sealed class CheckRowVm
{
    public string Path { get; init; } = "";
    public string Title { get; init; } = "";
    public string Detail { get; init; } = "";
}

/// <summary>
/// Check library (docs/plans/check-library.md), the Windows twin of
/// CheckLibraryView.swift: pick a folder to check and a library folder, and
/// the engine says which of the folder's files are already in the library by
/// content (size, then BLAKE3; never the name), wherever they are and whatever
/// they are called, which are not, and which duplicates the library already
/// holds. The library's hashes are kept in import.db, so a second check reads
/// no unchanged file. Report only: nothing is copied, moved or deleted.
/// Keyboard: Enter on a row opens it in the viewer, Ctrl+E shows it in
/// Explorer, Esc closes (a check carries on).
/// </summary>
internal sealed class CheckLibraryWindow : Window
{
    private static ImportLook Look => ImportLook.Current;

    private readonly ImportChrome _chrome;
    private readonly ImportApi _api;
    private readonly CheckLibraryApi _check;

    private readonly TextBlock _source = Secondary("No folder chosen", 12);
    private readonly TextBlock _library = Secondary("No library chosen", 12);
    private readonly ComboBox _scope = new() { MinWidth = 240 };
    private readonly Button _start = new() { Content = "Check" };
    private readonly Button _stop = new() { Content = "Stop" };
    private readonly Button _report = new() { Content = "Show report" };
    private readonly TextBlock _headline = new() { FontSize = Look.TypeSize(16), FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _progressText = Secondary("", 12);
    private readonly MediaViewer.Shared.FlatBar _bar;
    private readonly ListView _list = new() { SelectionMode = ListViewSelectionMode.Single, IsDoubleTapEnabled = true };
    private readonly ObservableCollection<CheckRowVm> _rows = new();

    private string _sourcePath = "";
    private string _libraryPath = "";
    private string _reportPath = "";
    private ulong _job;
    private bool _running;

    internal CheckLibraryWindow(ImportChrome chrome, CheckLibraryApi check, string defaultLibrary)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _check = check;
        _bar = new MediaViewer.Shared.FlatBar(ImportLook.Current.Neutral, ImportLook.Current.Accent);
        Title = "Check a Folder Against the Library";
        MediaViewer.Shared.AppIcon.Apply(this);
        bool mica = !ImportLook.Current.FromHost && MicaController.IsSupported();
        if (mica)
        {
            try { SystemBackdrop = new Microsoft.UI.Xaml.Media.MicaBackdrop(); }
            catch (Exception ex) { Debug.WriteLine(ex.Message); mica = false; }
        }
        AppWindow.Resize(new Windows.Graphics.SizeInt32(960, 700));
        if (defaultLibrary.Length > 0) SetLibrary(defaultLibrary);
        Content = ImportLook.Current.Root(BuildLayout(mica), this);
        UpdateButtons();
    }

    internal ulong Job => _job;
    internal string Headline => _headline.Text;

    private static TextBlock Secondary(string s, double size) =>
        new() { Text = s, FontSize = Look.TypeSize(size), Opacity = 0.7, TextWrapping = TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis };

    private Grid BuildLayout(bool mica)
    {
        var root = (Grid)XamlReader.Load(mica
            ? """<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"/>"""
            : """
              <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>
              """);
        root.Padding = Look.Pad(24, 16, 24, 16);
        root.RowSpacing = Look.Space(12);
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // title
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // choices
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // summary
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });  // rows

        var title = new StackPanel { Spacing = Look.Space(2) };
        title.Children.Add(new TextBlock
        {
            Text = "Check a Folder Against the Library",
            FontSize = Look.TypeSize(24),
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        });
        title.Children.Add(new TextBlock
        {
            Text = "Finds the folder's files that are already in the library by their content, even renamed or in other folders. Nothing is copied, moved or deleted.",
            FontSize = Look.TypeSize(12),
            Opacity = 0.7,
            TextWrapping = TextWrapping.Wrap,
        });
        root.Children.Add(title);

        var choices = new Grid { ColumnSpacing = Look.Space(12), RowSpacing = Look.Space(8) };
        choices.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        choices.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        choices.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        AddChoice(choices, 0, "Folder to check", _source, async () => { if (await Pick() is { } d) { _sourcePath = d; _source.Text = d; UpdateButtons(); } });
        AddChoice(choices, 1, "Library", _library, async () => { if (await Pick() is { } d) { SetLibrary(d); UpdateButtons(); } });
        _scope.Items.Add("This library folder and its subfolders");
        _scope.Items.Add("Every library Import knows");
        _scope.SelectedIndex = 0;
        var scopeLabel = new TextBlock { Text = "Look in", VerticalAlignment = VerticalAlignment.Center };
        Grid.SetRow(scopeLabel, 2);
        Grid.SetRow(_scope, 2);
        Grid.SetColumn(_scope, 1);
        choices.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        choices.Children.Add(scopeLabel);
        choices.Children.Add(_scope);
        if (ImportWindow.AccentStyle() is Style accent) _start.Style = accent;
        _start.Click += (_, _) => Start();
        _start.KeyboardAccelerators.Add(new KeyboardAccelerator { Key = VirtualKey.Enter, Modifiers = VirtualKeyModifiers.Control });
        Grid.SetRow(_start, 2);
        Grid.SetColumn(_start, 2);
        choices.Children.Add(_start);
        Grid.SetRow(choices, 1);
        root.Children.Add(choices);

        var summary = new Grid { ColumnSpacing = Look.Space(12) };
        summary.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        summary.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var lines = new StackPanel { Spacing = Look.Space(6) };
        lines.Children.Add(_headline);
        lines.Children.Add(_bar.Root);
        lines.Children.Add(_progressText);
        summary.Children.Add(lines);
        var side = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Look.Space(8), VerticalAlignment = VerticalAlignment.Center };
        _stop.Click += (_, _) => { try { _api.Cancel(_job); } catch (MediaViewerException) { } };
        _report.Click += (_, _) => { if (_reportPath.Length > 0) _ = Launcher.LaunchUriAsync(new Uri(_reportPath)); };
        side.Children.Add(_stop);
        side.Children.Add(_report);
        Grid.SetColumn(side, 1);
        summary.Children.Add(side);
        _bar.Root.Visibility = Visibility.Collapsed;
        Grid.SetRow(summary, 2);
        root.Children.Add(summary);

        _list.ItemTemplate = (DataTemplate)XamlReader.Load(
            "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\">" +
            "<StackPanel Padding=\"0,4,0,4\"><TextBlock Text=\"{Binding Title}\" TextTrimming=\"CharacterEllipsis\"/>" +
            "<TextBlock Text=\"{Binding Detail}\" Opacity=\"0.7\" FontSize=\"12\" TextTrimming=\"CharacterEllipsis\"/></StackPanel>" +
            "</DataTemplate>");
        _list.ItemsSource = _rows;
        _list.DoubleTapped += (_, _) => OpenSelected();
        _list.KeyDown += (_, e) =>
        {
            if (e.Key == VirtualKey.Enter) { OpenSelected(); e.Handled = true; }
        };
        var reveal = new KeyboardAccelerator { Key = VirtualKey.E, Modifiers = VirtualKeyModifiers.Control };
        reveal.Invoked += (_, e) => { RevealSelected(); e.Handled = true; };
        _list.KeyboardAccelerators.Add(reveal);
        Grid.SetRow(_list, 3);
        root.Children.Add(_list);
        return root;
    }

    private static void AddChoice(Grid grid, int row, string label, TextBlock value, Func<Task> choose)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var l = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
        value.VerticalAlignment = VerticalAlignment.Center;
        var b = new Button { Content = "Choose…" };
        b.Click += async (_, _) => await choose();
        Grid.SetRow(l, row);
        Grid.SetRow(value, row);
        Grid.SetColumn(value, 1);
        Grid.SetRow(b, row);
        Grid.SetColumn(b, 2);
        grid.Children.Add(l);
        grid.Children.Add(value);
        grid.Children.Add(b);
    }

    private void SetLibrary(string dir)
    {
        _libraryPath = dir;
        _library.Text = dir;
    }

    private async Task<string?> Pick()
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        try { return (await picker.PickSingleFolderAsync())?.Path; }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private void UpdateButtons()
    {
        _start.IsEnabled = !_running && _sourcePath.Length > 0 && _libraryPath.Length > 0 &&
                           !string.Equals(_sourcePath, _libraryPath, StringComparison.OrdinalIgnoreCase);
        _stop.Visibility = _running ? Visibility.Visible : Visibility.Collapsed;
        _report.Visibility = !_running && _reportPath.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        _scope.IsEnabled = !_running;
    }

    private void Start()
    {
        if (!_start.IsEnabled) return;
        string request = JsonSerializer.Serialize(new Dictionary<string, string>
        {
            ["destination"] = _libraryPath,
            ["source_folder"] = _sourcePath,
            ["scope"] = _scope.SelectedIndex == 1 ? "library" : "destination",
        });
        try { _job = _check.Check(request); }
        catch (MediaViewerException ex)
        {
            _headline.Text = "Could not start: " + ex.Status;
            return;
        }
        _chrome.Track(_job, "check");
        _running = true;
        _rows.Clear();
        _reportPath = "";
        _headline.Text = "Checking…";
        _progressText.Text = "Looking through the library…";
        _bar.IsIndeterminate = true;
        _bar.Root.Visibility = Visibility.Visible;
        UpdateButtons();
    }

    internal void OnProgress(ulong job, MvImportProgress p)
    {
        if (job != _job || !_running) return;
        if (p.DestinationCount == 0)  // the check's phase (mediaviewer_import.h)
        {
            _progressText.Text = $"Looking through the library · {p.UnitsDone:N0} files";
            _bar.IsIndeterminate = true;
            return;
        }
        _progressText.Text = p.DestinationCount == 1 ? "Reading library files whose size matches" : "Reading the folder's files";
        _bar.IsIndeterminate = p.BytesTotal == 0;
        if (p.BytesTotal > 0) _bar.Value = Math.Min(1.0, (double)p.BytesRead / p.BytesTotal);
    }

    internal async Task Refresh(ulong job)
    {
        if (job != _job) return;
        string json;
        try { json = await Task.Run(() => _api.SummaryJson(job)); }
        catch (MediaViewerException) { return; }
        if (job != _job) return;
        _running = false;
        _bar.Root.Visibility = Visibility.Collapsed;
        _progressText.Text = "";
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement s = doc.RootElement;
        _rows.Clear();
        if (!s.GetProperty("reachable").GetBoolean())
        {
            _headline.Text = "The library folder could not be reached. Nothing was checked.";
        }
        else
        {
            long found = s.GetProperty("found_count").GetInt64();
            long missing = s.GetProperty("not_found_count").GetInt64();
            long bytes = s.GetProperty("not_found_bytes").GetInt64();
            long groups = s.GetProperty("duplicate_groups").GetInt64();
            _headline.Text = $"{found:N0} already in the library · {missing:N0} not ({Format.Bytes(bytes)})" +
                             (groups > 0 ? $" · {groups:N0} sets of duplicates in the library" : "") +
                             (s.GetProperty("cancelled").GetBoolean() ? " · stopped early" : "");
            foreach (JsonElement f in s.GetProperty("found").EnumerateArray())
            {
                string path = f.GetProperty("path").GetString() ?? "";
                var where = f.GetProperty("where").EnumerateArray().Select(w => w.GetString() ?? "").ToList();
                _rows.Add(new CheckRowVm { Path = path, Title = "In the library: " + path, Detail = "= " + string.Join("  ·  ", where) });
            }
            foreach (JsonElement p in s.GetProperty("not_found").EnumerateArray())
            {
                string path = p.GetString() ?? "";
                _rows.Add(new CheckRowVm { Path = path, Title = "Not in the library: " + path, Detail = "" });
            }
            foreach (JsonElement g in s.GetProperty("duplicates").EnumerateArray())
            {
                var paths = g.GetProperty("paths").EnumerateArray().Select(x => x.GetString() ?? "").ToList();
                if (paths.Count == 0) continue;
                _rows.Add(new CheckRowVm
                {
                    Path = paths[0],
                    Title = $"Duplicate in the library: {paths.Count} identical files, {Format.Bytes(g.GetProperty("size").GetInt64())} each",
                    Detail = string.Join("  ·  ", paths),
                });
            }
        }
        try { _reportPath = await Task.Run(() => _api.ReportPath(job)); }
        catch (MediaViewerException) { _reportPath = ""; }
        UpdateButtons();
    }

    private void OpenSelected()
    {
        if (_list.SelectedItem is CheckRowVm r && r.Path.Length > 0) _chrome.Host.OpenInViewer(r.Path);
    }

    private void RevealSelected()
    {
        if (_list.SelectedItem is not CheckRowVm r || r.Path.Length == 0) return;
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{r.Path}\"") { UseShellExecute = false }); }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException) { }
    }
}
