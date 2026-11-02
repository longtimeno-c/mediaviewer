// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Text.Json;
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using Windows.UI.Core;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// "Unknown faces (N)" from the People grid (the Mac's UnknownFacesSheet):
/// faces clear enough to name that nobody is filed under, clearest first, 60
/// at a time ("Show more"), each with who it most resembles ("Anna?"). "Yes"
/// files each selected face under its suggestion, "Move to…" under anyone;
/// both pin them. Space shows the whole photo. Every pack call runs on a
/// worker (rule 1); rule 6: file names are shown, never logged.
/// </summary>
internal sealed class UnknownFacesSheet : Window
{
    private const int Page = 60;
    private const double TileWidth = 104;

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly PeopleGrid _grid;
    private readonly ObservableCollection<UnknownFace> _items = new();
    private readonly Dictionary<ulong, FaceTile> _tiles = new();
    private readonly HashSet<ulong> _selection = new();
    private readonly ItemsRepeater _repeater;
    private readonly TextBlock _count;
    private readonly TextBlock _empty;
    private readonly FlatBar _loadingBar;
    private readonly Button _more;
    private readonly Button _yes;
    private readonly PersonPicker _move;
    private readonly TextBlock _status;
    private readonly Button _undoButton;
    // The last Yes or Move: those faces, to be nobody's again (only the last action's).
    private List<ulong>? _undoFaces;
    private readonly ContentControl _focusHost;
    private readonly PhotoPreview _preview;
    private long _total;
    private int _cursor = -1;
    private int _generation;
    private bool _loading;
    private bool _closed;

    internal UnknownFacesSheet(AiChrome chrome, PeopleGrid grid)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        _grid = grid;
        Title = "Unknown faces";

        if (AppWindow.Presenter is not OverlappedPresenter) AppWindow.SetPresenter(OverlappedPresenter.Create());
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.IsResizable = false;
            p.IsMaximizable = false;
            p.IsMinimizable = false;
        }
        AppWindow.IsShownInSwitchers = false;
        Native.Adopt(WinRT.Interop.WindowNative.GetWindowHandle(this), chrome.Host.MainWindow);
        Closed += (_, _) => _closed = true;

        var root = new Grid { Padding = new Thickness(20), RowSpacing = 10 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        TextBlock title = _look.Text("Unknown faces", 18, AddonColour.Title, wrap: false);
        title.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetHeadingLevel(title, AutomationHeadingLevel.Level1);
        head.Children.Add(title);
        _count = _look.Text("", 12, wrap: false);
        _count.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(_count);
        root.Children.Add(head);

        TextBlock hint = _look.Text(
            "Faces nobody is named for yet, clearest first. Ctrl-click selects several. Y: yes, M: move, Space: the whole photo.", 12);
        Grid.SetRow(hint, 1);
        root.Children.Add(hint);

        _loadingBar = new FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]) { IsIndeterminate = true };
        _loadingBar.Root.Width = 120;
        _loadingBar.Root.HorizontalAlignment = HorizontalAlignment.Center;
        _loadingBar.Root.Margin = new Thickness(0, 20, 0, 20);
        _empty = _look.Text("No unknown faces.", 12);
        _empty.Visibility = Visibility.Collapsed;
        _repeater = new ItemsRepeater
        {
            Layout = new UniformGridLayout
            {
                MinItemWidth = TileWidth,
                MinItemHeight = FaceTile.Cell + FaceTile.CaptionHeight,
                MinColumnSpacing = 10,
                MinRowSpacing = 10,
                ItemsStretch = UniformGridLayoutItemsStretch.Fill,
            },
            ItemTemplate = new TileFactory(o => MakeTile((UnknownFace)o), _tiles),
            ItemsSource = _items,
        };
        AutomationProperties.SetName(_repeater, "Unknown faces");
        _more = _look.Button(PeopleWords.ShowMore, LoadMore);
        _more.HorizontalAlignment = HorizontalAlignment.Left;
        _more.Visibility = Visibility.Collapsed;
        var column = new StackPanel { Padding = new Thickness(4), Spacing = 10 };
        column.Children.Add(_empty);
        column.Children.Add(_repeater);
        column.Children.Add(_loadingBar.Root);
        column.Children.Add(_more);
        _focusHost = new ContentControl
        {
            Content = new ScrollViewer
            {
                Content = column,
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            },
            IsTabStop = true,
            UseSystemFocusVisuals = false,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        AutomationProperties.SetName(_focusHost, "Unknown faces");
        Grid.SetRow(_focusHost, 2);
        root.Children.Add(_focusHost);

        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _yes = _look.Button(PeopleWords.Yes, Yes);
        ToolTipService.SetToolTip(_yes, "File each face under the person it looks like (Y)");
        AutomationProperties.SetName(_yes, "Yes, file each face under the person it looks like");
        actions.Children.Add(_yes);
        Button moveButton = _look.Button(PeopleWords.MoveTo, () => { });
        ToolTipService.SetToolTip(moveButton, "File these faces under someone (M)");
        AutomationProperties.SetName(moveButton, "Move to");
        _move = new PersonPicker(chrome, grid, moveButton, () => 0, MoveTo);
        actions.Children.Add(moveButton);
        Grid.SetRow(actions, 3);
        root.Children.Add(actions);

        var foot = new Grid { ColumnSpacing = 8 };
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _status = _look.Text("", 12, AddonColour.Title);
        _status.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetLiveSetting(_status, AutomationLiveSetting.Polite);
        foot.Children.Add(_status);
        _undoButton = _look.Button(PeopleWords.Undo, Undo);
        ToolTipService.SetToolTip(_undoButton, "Undo (Ctrl+Z)");
        AutomationProperties.SetName(_undoButton, "Undo");
        _undoButton.Visibility = Visibility.Collapsed;
        Grid.SetColumn(_undoButton, 1);
        foot.Children.Add(_undoButton);
        Button done = _look.Button("Done", Close, accent: true);
        Grid.SetColumn(done, 2);
        foot.Children.Add(done);
        Grid.SetRow(foot, 4);
        root.Children.Add(foot);

        _preview = new PhotoPreview(chrome);
        Grid.SetRowSpan(_preview.Root, 5);
        _preview.Root.Margin = new Thickness(-20);
        _preview.Hidden += () => _focusHost.Focus(FocusState.Programmatic);
        root.Children.Add(_preview.Root);

        root.KeyDown += OnKeyDown;
        _look.Dress(this, root);
        Content = root;
        UpdateButtons();
    }

    internal void Present()
    {
        AppWindow.MoveAndResize(Native.CentreOver(_chrome.Host.MainWindow, 720, 620));
        Activate();
        LoadMore();
    }

    // ---- faces (worker) -----------------------------------------------------------------

    /// <summary>The next 60 (the first 60 when the list is empty).</summary>
    private void LoadMore()
    {
        if (_loading || !_api.HasUnknownFacesJson) return;
        _loading = true;
        int g = _generation;
        uint offset = (uint)_items.Count;
        AiApi api = _api;
        UpdateButtons();
        _ = Task.Run(() =>
        {
            UnknownPage? page = null;
            try { page = PeopleWords.ParseUnknown(api.UnknownFacesJson(offset, Page)); }
            catch (Exception ex) when (ex is MediaViewerException or JsonException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                _loading = false;
                if (_closed || g != _generation) return;
                if (page is not null)
                {
                    _total = page.Total;
                    var have = _items.Select(f => f.Face).ToHashSet();
                    foreach (UnknownFace f in page.Faces)
                    {
                        if (have.Add(f.Face)) _items.Add(f);
                    }
                }
                UpdateButtons();
                if (FocusManager.GetFocusedElement(Content.XamlRoot) is not (ButtonBase or FakeInput)) _focusHost.Focus(FocusState.Programmatic);
            });
        });
    }

    private FaceTile MakeTile(UnknownFace f)
    {
        var data = new TileFace(f.Face, f.Path, f.PtsMs, f.Box, Caption: PeopleWords.SuggestCaption(f.Suggest, f.SuggestName));
        var tile = new FaceTile(_chrome, data, TileWidth, captioned: true, t => Tap(t.Id));
        tile.SetSelected(_selection.Contains(f.Face));
        var menu = new MenuFlyout();
        menu.Opening += (_, _) => FillMenu(menu, f);
        tile.Root.ContextFlyout = menu;
        return tile;
    }

    private void FillMenu(MenuFlyout menu, UnknownFace f)
    {
        menu.Items.Clear();
        if (!_selection.Contains(f.Face))
        {
            _selection.Clear();
            _selection.Add(f.Face);
            _cursor = _items.IndexOf(f);
            UpdateButtons();
        }
        var show = new MenuFlyoutItem { Text = PeopleWords.ShowPhoto };
        show.Click += (_, _) => Preview(f);
        menu.Items.Add(show);
        if (f.Suggest != 0 && _api.HasFacesAssign)
        {
            var yes = new MenuFlyoutItem { Text = f.SuggestName.Length > 0 ? $"Yes, this is {f.SuggestName}" : "Yes, same person" };
            yes.Click += (_, _) => Yes();
            menu.Items.Add(yes);
        }
        if (_api.HasFacesAssign)
        {
            var move = new MenuFlyoutItem { Text = PeopleWords.MoveTo };
            move.Click += (_, _) => _move.Show();
            menu.Items.Add(move);
        }
    }

    // ---- selection ------------------------------------------------------------------------

    private List<UnknownFace> Selected() => _items.Where(f => _selection.Contains(f.Face)).ToList();

    private void UpdateButtons()
    {
        int n = _selection.Count;
        bool assign = _api.HasFacesAssign;
        _yes.Visibility = assign ? Visibility.Visible : Visibility.Collapsed;
        _yes.IsEnabled = Selected().Any(f => f.Suggest != 0);
        _move.Button.Visibility = assign ? Visibility.Visible : Visibility.Collapsed;
        _move.Button.IsEnabled = n > 0;
        _count.Text = _total > 0 ? PeopleWords.Faces(_total) : "";
        _empty.Visibility = !_loading && _items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        _loadingBar.Root.Visibility = _loading ? Visibility.Visible : Visibility.Collapsed;
        _more.Visibility = !_loading && _items.Count < _total ? Visibility.Visible : Visibility.Collapsed;
        _more.Content = PeopleWords.ShowMoreLabel(_items.Count, _total);
        foreach ((ulong id, FaceTile tile) in _tiles) tile.SetSelected(_selection.Contains(id));
    }

    private static bool Down(VirtualKey key) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private void Tap(ulong face)
    {
        if (Down(VirtualKey.Control) || Down(VirtualKey.Shift))
        {
            if (!_selection.Add(face)) _selection.Remove(face);
        }
        else
        {
            _selection.Clear();
            _selection.Add(face);
        }
        _cursor = _items.ToList().FindIndex(f => f.Face == face);
        UpdateButtons();
        _focusHost.Focus(FocusState.Programmatic);
    }

    private void Step(int step, bool add)
    {
        if (_items.Count == 0) return;
        _cursor = PeopleWords.Step(_cursor, _items.Count, step);
        if (!add) _selection.Clear();
        _selection.Add(_items[_cursor].Face);
        UpdateButtons();
        if (_repeater.GetOrCreateElement(_cursor) is FrameworkElement e)
        {
            e.UpdateLayout();
            e.StartBringIntoView(new BringIntoViewOptions { VerticalAlignmentRatio = 0.5 });
        }
    }

    private UnknownFace? Focused()
    {
        if (_cursor >= 0 && _cursor < _items.Count && _selection.Contains(_items[_cursor].Face)) return _items[_cursor];
        return _items.FirstOrDefault(f => _selection.Contains(f.Face));
    }

    private void Preview(UnknownFace f) =>
        _preview.Show(f.Face, f.Box, f.Path, f.PtsMs, PeopleWords.SuggestCaption(f.Suggest, f.SuggestName));

    private void RemoveLocal(IEnumerable<ulong> ids)
    {
        var gone = ids.ToHashSet();
        foreach (UnknownFace f in _items.Where(f => gone.Contains(f.Face)).ToList()) _items.Remove(f);
        _selection.ExceptWith(gone);
        _total = Math.Max(0, _total - gone.Count);
        if (_cursor >= _items.Count) _cursor = _items.Count - 1;
        UpdateButtons();
    }

    // ---- naming (workers) -----------------------------------------------------------------

    /// <summary>"Yes": each selected face to the person it looks like, pinned.</summary>
    private void Yes()
    {
        if (!_api.HasFacesAssign) return;
        List<UnknownFace> chosen = Selected().Where(f => f.Suggest != 0).ToList();
        if (chosen.Count == 0) return;
        var groups = chosen.GroupBy(f => f.Suggest)
            .Select(g => (Person: g.Key, Name: g.First().SuggestName, Faces: g.Select(f => f.Face).ToList()))
            .ToList();
        Assign(groups);
    }

    /// <summary>"Move to…" picked someone: the selected faces are theirs, pinned.</summary>
    private void MoveTo(PickPerson to)
    {
        List<ulong> ids = Selected().Select(f => f.Face).ToList();
        if (ids.Count == 0 || !_api.HasFacesAssign) return;
        Assign(new List<(ulong, string, List<ulong>)> { (to.Id, to.Name, ids) });
    }

    private void Assign(List<(ulong Person, string Name, List<ulong> Faces)> groups)
    {
        RemoveLocal(groups.SelectMany(g => g.Faces));
        _status.Text = "";
        _undoFaces = null;
        _undoButton.Visibility = Visibility.Collapsed;
        ++_generation;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            var moved = new List<(string Name, int Count)>();
            var done = new List<ulong>();
            bool failed = false;
            foreach ((ulong person, string name, List<ulong> faces) in groups)
            {
                try
                {
                    api.FacesAssign(faces, person, pin: true);
                    moved.Add((name, faces.Count));
                    done.AddRange(faces);
                }
                catch (MediaViewerException) { failed = true; }
            }
            DispatcherQueue.TryEnqueue(() =>
            {
                _grid.Refresh();
                if (_closed) return;
                _status.Text = failed ? PeopleWords.CouldNotChange : PeopleWords.MovedEach(moved);
                _undoFaces = done.Count > 0 ? done : null;
                _undoButton.Visibility = _undoFaces is null ? Visibility.Collapsed : Visibility.Visible;
                // A short page after naming: fill it from the pack again.
                if (failed || (_items.Count < Page && _items.Count < _total)) Reload();
            });
        });
    }

    /// <summary>The Undo button and Ctrl+Z: the last Yes or Move put back, the faces nobody's again.</summary>
    private void Undo()
    {
        if (_undoFaces is not List<ulong> faces) return;
        _undoFaces = null;
        _undoButton.Visibility = Visibility.Collapsed;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            bool ok = true;
            try { api.FacesAssign(faces, 0, pin: false, undo: true); }
            catch (MediaViewerException) { ok = false; }
            DispatcherQueue.TryEnqueue(() =>
            {
                _grid.Refresh();
                if (_closed) return;
                _status.Text = ok ? "Undone" : "That can no longer be undone.";
                Reload();
            });
        });
    }

    private void Reload()
    {
        ++_generation;
        _loading = false;
        _items.Clear();
        _selection.Clear();
        _cursor = -1;
        LoadMore();
    }

    // ---- keys (the sheet's own; not command-table rows) --------------------------------

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        object? focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        if (_preview.IsOpen)
        {
            if (e.Key is VirtualKey.Escape or VirtualKey.Space or VirtualKey.Enter)
            {
                _preview.Hide();
                e.Handled = true;
            }
            return;
        }
        if (focused is FakeInput) return;
        bool ctrl = Down(VirtualKey.Control);
        int n = _selection.Count;
        switch (e.Key)
        {
            case VirtualKey.Y when !ctrl && n > 0:
                Yes();
                e.Handled = true;
                break;
            case VirtualKey.Z when ctrl && _undoFaces is not null:
                Undo();
                e.Handled = true;
                break;
            case VirtualKey.M when !ctrl && n > 0 && _api.HasFacesAssign:
                _move.Show();
                e.Handled = true;
                break;
            case VirtualKey.Left or VirtualKey.Right when focused is not ButtonBase:
                Step(e.Key == VirtualKey.Left ? -1 : 1, Down(VirtualKey.Shift));
                e.Handled = true;
                break;
            case VirtualKey.Space when focused is not ButtonBase && Focused() is UnknownFace face:
                Preview(face);
                e.Handled = true;
                break;
            case VirtualKey.Escape:
                Close();
                e.Handled = true;
                break;
            case VirtualKey.Enter when focused is not ButtonBase && !(ReferenceEquals(focused, _focusHost) && n > 0):
                Close();
                e.Handled = true;
                break;
        }
    }
}
