// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.System;
using Windows.UI.Core;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// A person's faces (the Mac's PersonSheet, PeopleView.swift): "Not this person"
/// (the hover ✕ or Delete), "Split into new person", "Refine faces", "Merge
/// into…" and "Show photos". A small owned window over Settings, as a sheet
/// is on the Mac; Done, Enter or Esc closes it.
/// </summary>
/// <remarks>Rule 6: file names are shown in tooltips, never logged.</remarks>
internal sealed class PersonSheet : Window
{
    private const double Face = 72, Cell = 84;

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly PeopleGrid _grid;
    private PersonVm _person;
    private readonly ObservableCollection<FaceVm> _faces = new();
    private readonly Dictionary<ulong, FaceCell> _cells = new();
    private readonly HashSet<ulong> _selection = new();
    private readonly TextBlock _title;
    private readonly Button _merge;
    private readonly Button _reject;
    private readonly Button _split;
    private readonly Button _refine;
    private readonly MediaViewer.Shared.FlatBar _refineBar;
    private readonly TextBlock _refineNote;
    private readonly MediaViewer.Shared.FlatBar _loadingBar;
    private readonly ContentControl _focusHost;
    private bool _loading = true;
    private bool _refining;
    private bool _closed;

    internal PersonSheet(AiChrome chrome, PeopleGrid grid, PersonVm person)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        _grid = grid;
        _person = person;
        Title = person.Label;

        if (AppWindow.Presenter is not OverlappedPresenter) AppWindow.SetPresenter(OverlappedPresenter.Create());
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            // A sheet, not a document window: no resize, no min / max.
            p.IsResizable = false;
            p.IsMaximizable = false;
            p.IsMinimizable = false;
        }
        AppWindow.IsShownInSwitchers = false;
        try { SystemBackdrop = new MicaBackdrop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        Native.Adopt(WinRT.Interop.WindowNative.GetWindowHandle(this), chrome.Host.MainWindow);
        Closed += (_, _) => _closed = true;

        var root = new Grid { Padding = new Thickness(20), RowSpacing = 12 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        // Name · Show photos · Merge into…
        var head = new Grid { ColumnSpacing = 8 };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _title = _look.Text(person.Label, 18, AddonColour.Title, wrap: false);
        _title.TextTrimming = TextTrimming.CharacterEllipsis;
        _title.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(_title);
        Button photos = _look.Button("Show photos", () =>
        {
            PersonVm who = _person;
            Close();
            _grid.ShowPhotos(who);
        });
        Grid.SetColumn(photos, 1);
        head.Children.Add(photos);
        // A Button with a Flyout: DropDownButton fail-fasts in this host.
        _merge = _look.Button("Merge into…  ▾", () => { });
        _merge.Flyout = new MenuFlyout();
        ((MenuFlyout)_merge.Flyout).Opening += (_, _) => FillMerge();
        Grid.SetColumn(_merge, 2);
        head.Children.Add(_merge);
        root.Children.Add(head);

        TextBlock hint = _look.Text("Select faces that are someone else. Ctrl-click selects several; Delete removes them from this person.", 12);
        Grid.SetRow(hint, 1);
        root.Children.Add(hint);

        var repeater = new ItemsRepeater
        {
            Layout = new UniformGridLayout
            {
                MinItemWidth = Cell,
                MinItemHeight = Cell,
                MinColumnSpacing = 10,
                MinRowSpacing = 10,
                ItemsStretch = UniformGridLayoutItemsStretch.Fill,
            },
            ItemTemplate = new CellFactory(this),
            ItemsSource = _faces,
        };
        _loadingBar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]) { IsIndeterminate = true };
        _loadingBar.Root.Width = 120;
        _loadingBar.Root.HorizontalAlignment = HorizontalAlignment.Center;
        _loadingBar.Root.Margin = new Thickness(0, 40, 0, 40);
        var faceColumn = new StackPanel { Padding = new Thickness(4) };
        faceColumn.Children.Add(_loadingBar.Root);
        faceColumn.Children.Add(repeater);
        // The faces take keyboard focus for Delete; the scroller carries it.
        _focusHost = new ContentControl
        {
            Content = new ScrollViewer
            {
                Content = faceColumn,
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            },
            IsTabStop = true,
            UseSystemFocusVisuals = false,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        AutomationProperties.SetName(_focusHost, "Faces");
        Grid.SetRow(_focusHost, 2);
        root.Children.Add(_focusHost);

        // Not this person · Split into new person · Refine faces · … · Done
        var foot = new Grid { ColumnSpacing = 8 };
        for (int i = 0; i < 5; ++i)
        {
            foot.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = i == 4 ? GridLength.Auto : i == 3 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto,
            });
        }
        _reject = _look.Button("Not this person", () => Reject(_selection.ToList()));
        ToolTipService.SetToolTip(_reject, "The face leaves this person and never rejoins them (Delete)");
        foot.Children.Add(_reject);
        _split = _look.Button("Split into new person", Split);
        Grid.SetColumn(_split, 1);
        foot.Children.Add(_split);
        _refine = _look.Button("Refine faces", Refine);
        ToolTipService.SetToolTip(_refine, "Check every face against this person and move out the ones that don't match");
        Grid.SetColumn(_refine, 2);
        foot.Children.Add(_refine);
        var refineState = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        // FlatBar, not a ProgressRing: that fail-fasts in this island host.
        _refineBar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]) { IsIndeterminate = true };
        _refineBar.Root.Width = 40;
        _refineBar.Root.VerticalAlignment = VerticalAlignment.Center;
        _refineBar.Root.Visibility = Visibility.Collapsed;
        refineState.Children.Add(_refineBar.Root);
        _refineNote = _look.Text("", 12, wrap: false);
        _refineNote.VerticalAlignment = VerticalAlignment.Center;
        refineState.Children.Add(_refineNote);
        Grid.SetColumn(refineState, 3);
        foot.Children.Add(refineState);
        Button done = _look.Button("Done", Close, accent: true);
        Grid.SetColumn(done, 4);
        foot.Children.Add(done);
        Grid.SetRow(foot, 3);
        root.Children.Add(foot);

        root.KeyDown += OnKeyDown;
        Content = root;
        UpdateButtons();
    }

    internal void Present()
    {
        AppWindow.MoveAndResize(Native.CentreOver(_chrome.Host.MainWindow, 620, 520));
        Activate();
        Load();
    }

    /// <summary>The grid read the people again: the title and the merge menu follow.</summary>
    internal void PeopleChanged()
    {
        if (_closed) return;
        PersonVm? now = _grid.Find(_person.Id);
        if (now is null)
        {
            // Merged away (or gone): nothing left to correct here.
            Close();
            return;
        }
        bool more = now.Faces != _person.Faces;
        _person = now;
        _title.Text = now.Label;
        Title = now.Label;
        UpdateButtons();
        if (more && !_refining) Load();
    }

    // ---- faces (worker) -----------------------------------------------------------------

    private void Load()
    {
        ulong id = _person.Id;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            var faces = new List<FaceVm>();
            try
            {
                using JsonDocument doc = JsonDocument.Parse(api.PersonFacesJson(id));
                foreach (JsonElement f in doc.RootElement.EnumerateArray())
                {
                    faces.Add(new FaceVm(
                        f.GetProperty("face").GetUInt64(),
                        f.TryGetProperty("path", out JsonElement path) ? path.GetString() ?? "" : "",
                        f.TryGetProperty("pts_ms", out JsonElement pts) ? pts.GetInt64() : -1,
                        PeopleGrid.Box(f, "box")));
                }
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException or InvalidOperationException
                                           or FormatException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_closed || _person.Id != id) return;
                _loading = false;
                if (!_faces.Select(f => f.Id).SequenceEqual(faces.Select(f => f.Id)))
                {
                    _faces.Clear();
                    foreach (FaceVm f in faces) _faces.Add(f);
                }
                _selection.IntersectWith(_faces.Select(f => f.Id));
                UpdateButtons();
                _focusHost.Focus(FocusState.Programmatic);
            });
        });
    }

    // ---- corrections --------------------------------------------------------------------

    private void UpdateButtons()
    {
        int n = _selection.Count;
        _reject.IsEnabled = n > 0;
        _split.IsEnabled = n > 0;
        _refine.IsEnabled = !_refining && !_loading;
        _merge.IsEnabled = _grid.People.Count > 1;
        _loadingBar.Root.Visibility = _loading ? Visibility.Visible : Visibility.Collapsed;
        _refineBar.Root.Visibility = _refining ? Visibility.Visible : Visibility.Collapsed;
        _refineNote.Visibility = _refining ? Visibility.Collapsed : Visibility.Visible;
        foreach ((ulong id, FaceCell cell) in _cells) cell.SetSelected(_selection.Contains(id));
    }

    private static bool Modified() =>
        InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control).HasFlag(CoreVirtualKeyStates.Down) ||
        InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(CoreVirtualKeyStates.Down);

    private void Tap(FaceVm face)
    {
        if (Modified())
        {
            if (!_selection.Add(face.Id)) _selection.Remove(face.Id);
        }
        else
        {
            _selection.Clear();
            _selection.Add(face.Id);
        }
        UpdateButtons();
        _focusHost.Focus(FocusState.Programmatic);
    }

    /// <summary>"Not this person" for each face, then the grid reads again.</summary>
    private void Reject(List<ulong> ids)
    {
        if (ids.Count == 0) return;
        foreach (FaceVm vm in _faces.Where(f => ids.Contains(f.Id)).ToList()) _faces.Remove(vm);
        _selection.ExceptWith(ids);
        UpdateButtons();
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            foreach (ulong f in ids)
            {
                try { api.FaceReject(f); }
                catch (MediaViewerException) { }
            }
            DispatcherQueue.TryEnqueue(_grid.Refresh);
        });
    }

    private void Split()
    {
        List<ulong> ids = _selection.ToList();
        if (ids.Count == 0) return;
        foreach (FaceVm vm in _faces.Where(f => ids.Contains(f.Id)).ToList()) _faces.Remove(vm);
        _selection.Clear();
        UpdateButtons();
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            try { api.FaceSplit(ids); }
            catch (MediaViewerException) { }
            DispatcherQueue.TryEnqueue(_grid.Refresh);
        });
    }

    private void Refine()
    {
        if (_refining) return;
        _refining = true;
        _refineNote.Text = "";
        _selection.Clear();
        UpdateButtons();
        ulong id = _person.Id;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            uint? removed = null;
            try { removed = api.PersonRefine(id); }
            catch (MediaViewerException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                _refining = false;
                _refineNote.Text = removed switch
                {
                    null => "Couldn't refine",
                    0 => "All faces match",
                    1 => "1 face moved out",
                    _ => $"{removed} faces moved out",
                };
                UpdateButtons();
                if (_closed) return;
                Load();
                _grid.Refresh();
            });
        });
    }

    private void FillMerge()
    {
        var menu = (MenuFlyout)_merge.Flyout;
        menu.Items.Clear();
        foreach (PersonVm other in _grid.People.Where(o => o.Id != _person.Id))
        {
            var item = new MenuFlyoutItem { Text = other.MenuName };
            ulong into = other.Id, from = _person.Id;
            item.Click += (_, _) =>
            {
                _grid.Merge(into, new[] { from });
                Close();
            };
            menu.Items.Add(item);
        }
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        switch (e.Key)
        {
            case VirtualKey.Delete or VirtualKey.Back when _selection.Count > 0:
                Reject(_selection.ToList());
                e.Handled = true;
                break;
            case VirtualKey.Escape:
                Close();
                e.Handled = true;
                break;
            // Done is the default button, as on the Mac (a focused button keeps its own Enter).
            case VirtualKey.Enter when FocusManager.GetFocusedElement(Content.XamlRoot) is not Button:
                Close();
                e.Handled = true;
                break;
        }
    }

    // ---- a face ---------------------------------------------------------------------------

    /// <summary>A face, its selection ring and the hover ✕ ("Not this person").</summary>
    private sealed class FaceCell
    {
        public readonly Grid Root;
        private readonly Ellipse _ring;
        private readonly Button _reject;
        private readonly Look _look;
        private bool _selected;

        public FaceCell(PersonSheet sheet, FaceVm face)
        {
            _look = sheet._look;
            Root = new Grid { Width = Cell, Height = Cell, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            Root.Children.Add(sheet._chrome.Crops.Crop(face.Id, face.Box, Face, "", sheet.DispatcherQueue));
            _ring = new Ellipse
            {
                Width = Face + 4,
                Height = Face + 4,
                StrokeThickness = 2.5,
                Stroke = _look[AddonColour.Accent],
                Visibility = Visibility.Collapsed,
                IsHitTestVisible = false,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            Root.Children.Add(_ring);
            _reject = new Button
            {
                Content = "✕",
                Width = 22,
                Height = 22,
                Padding = new Thickness(0),
                FontSize = 11,
                CornerRadius = new CornerRadius(11),
                BorderThickness = new Thickness(0),
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(166, 0, 0, 0)),
                Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(0, 2, 4, 0),
                Visibility = Visibility.Collapsed,
            };
            ToolTipService.SetToolTip(_reject, "Not this person");
            AutomationProperties.SetName(_reject, "Not this person");
            _reject.Click += (_, _) => sheet.Reject(new List<ulong> { face.Id });
            Root.Children.Add(_reject);
            string file = System.IO.Path.GetFileName(face.Path);
            ToolTipService.SetToolTip(Root, face.PtsMs >= 0 ? $"{file} at {Look.Moment(face.PtsMs)}" : file);
            AutomationProperties.SetName(Root, file);
            Root.PointerEntered += (_, _) => SetHover(true);
            Root.PointerExited += (_, _) => SetHover(false);
            Root.Tapped += (_, e) =>
            {
                if (e.OriginalSource is DependencyObject d && IsInside(d, _reject)) return;
                sheet.Tap(face);
                e.Handled = true;
            };
        }

        private static bool IsInside(DependencyObject? d, DependencyObject ancestor)
        {
            for (; d is not null; d = VisualTreeHelper.GetParent(d))
            {
                if (ReferenceEquals(d, ancestor)) return true;
            }
            return false;
        }

        private void SetHover(bool on)
        {
            _reject.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        }

        public void SetSelected(bool on)
        {
            if (_selected == on) return;
            _selected = on;
            _ring.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            AutomationProperties.SetItemStatus(Root, on ? "Selected" : "");
        }
    }

    private sealed class CellFactory(PersonSheet sheet) : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var face = (FaceVm)args.Data;
            var cell = new FaceCell(sheet, face);
            cell.SetSelected(sheet._selection.Contains(face.Id));
            sheet._cells[face.Id] = cell;
            return cell.Root;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            foreach ((ulong id, FaceCell cell) in sheet._cells)
            {
                if (!ReferenceEquals(cell.Root, args.Element)) continue;
                sheet._cells.Remove(id);
                break;
            }
        }
    }
}
