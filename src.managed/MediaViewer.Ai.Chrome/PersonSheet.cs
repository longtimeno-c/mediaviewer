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
/// A person's faces (the Mac's PersonSheet, PeopleView.swift): the faces worth
/// a look first ("Check these", person_review_json), then the rest; "Yes, this
/// is …" (pin), "Not …" (the hover ✕, Delete or N), "Move to…" (M), "Unpin",
/// "Split into new person", "Refine faces", "Merge into…", "Show photos", and
/// "Might also be …" at the bottom. Space shows the whole photo with the face
/// outlined; Ctrl+Z undoes the last reject, move or merge. A small owned window
/// over Settings, as a sheet is on the Mac; Done or Esc closes it, and Enter
/// when no face is focused.
/// </summary>
/// <remarks>
/// Every pack call runs on a worker (rule 1); the entries added for correcting
/// people are each used only when the pack has them (AiApi.Has*), else the
/// sheet does what it did before. Rule 6: file names are shown in tooltips and
/// read by Narrator, never logged.
/// </remarks>
internal sealed class PersonSheet : Window
{
    private const double CheckWidth = 104, SuggestWidth = 104;

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly PeopleGrid _grid;
    private PersonVm _person;
    // Everything person_review_json said, in its order (doubtful first).
    private List<ReviewFace> _all = new();
    private readonly ObservableCollection<ReviewFace> _checkItems = new();
    private readonly ObservableCollection<ReviewFace> _otherItems = new();
    private readonly Dictionary<ulong, FaceTile> _tiles = new();
    private readonly HashSet<ulong> _selection = new();
    private int _cursor = -1;
    private bool _clearest;
    private readonly TextBlock _title;
    private readonly Button _merge;
    private readonly ToggleButton[] _sortButtons = new ToggleButton[2];
    private readonly FrameworkElement _sortTrack;
    private readonly TextBlock _checkHeader;
    private readonly ItemsRepeater _checkRepeater;
    private readonly TextBlock _otherHeader;
    private readonly ItemsRepeater _otherRepeater;
    private readonly StackPanel _suggest;
    private readonly Button _yes;
    private readonly Button _reject;
    private readonly PersonPicker _move;
    private readonly Button _unpin;
    private readonly Button _split;
    private readonly Button _refine;
    private readonly FlatBar _refineBar;
    private readonly TextBlock _refineNote;
    private readonly FlatBar _loadingBar;
    private readonly ContentControl _focusHost;
    private readonly Grid _statusRow;
    private readonly TextBlock _status;
    private readonly Button _undoButton;
    private readonly PhotoPreview _preview;
    // The last reject, move or merge, put back on a worker; only the last one.
    private Action<AiApi>? _undo;
    private int _action;
    private int _loadGeneration;
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
        Native.Adopt(WinRT.Interop.WindowNative.GetWindowHandle(this), chrome.Host.MainWindow);
        Closed += (_, _) => _closed = true;

        var root = new Grid { Padding = new Thickness(20), RowSpacing = 10 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                      // name
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                      // hint · sort
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) }); // faces
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                      // actions
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                      // status · Undo
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                      // Refine · Done

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
        AutomationProperties.SetName(_merge, "Merge into");
        Grid.SetColumn(_merge, 2);
        head.Children.Add(_merge);
        root.Children.Add(head);

        // The hint, and "Doubtful first | Clearest".
        var hintRow = new Grid { ColumnSpacing = 10 };
        hintRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        hintRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock hint = _look.Text(
            "Select faces that are someone else. Ctrl-click selects several. Y: yes, N: not this person, M: move, Space: the whole photo.", 12);
        hint.VerticalAlignment = VerticalAlignment.Center;
        hintRow.Children.Add(hint);
        bool hc = Look.HighContrast();
        var segments = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        string[] sortNames = { PeopleWords.SortDoubtful, PeopleWords.SortClearest };
        for (int i = 0; i < 2; ++i)
        {
            bool clearest = i == 1;
            ToggleButton b = _look.Segment(sortNames[i], () => SetSort(clearest), hc, fontSize: 12);
            AutomationProperties.SetName(b, "Sort faces, " + sortNames[i]);
            _sortButtons[i] = b;
            segments.Children.Add(b);
        }
        Border track = _look.Track(segments, hc);
        track.VerticalAlignment = VerticalAlignment.Center;
        _sortTrack = track;
        Grid.SetColumn(track, 1);
        hintRow.Children.Add(track);
        Grid.SetRow(hintRow, 1);
        root.Children.Add(hintRow);

        // "Check these (k)", the rest, then "Might also be …".
        _loadingBar = new FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]) { IsIndeterminate = true };
        _loadingBar.Root.Width = 120;
        _loadingBar.Root.HorizontalAlignment = HorizontalAlignment.Center;
        _loadingBar.Root.Margin = new Thickness(0, 40, 0, 40);
        _checkHeader = _look.Text("", 13, AddonColour.Title, wrap: false);
        AutomationProperties.SetHeadingLevel(_checkHeader, AutomationHeadingLevel.Level2);
        _checkRepeater = Repeater(CheckWidth, FaceTile.Cell + FaceTile.CaptionHeight, _checkItems, captioned: true);
        AutomationProperties.SetName(_checkRepeater, "Faces to check");
        _otherHeader = _look.Text(PeopleWords.OtherFaces, 13, AddonColour.Title, wrap: false);
        _otherHeader.Margin = new Thickness(0, 8, 0, 0);
        AutomationProperties.SetHeadingLevel(_otherHeader, AutomationHeadingLevel.Level2);
        _otherRepeater = Repeater(FaceTile.Cell, FaceTile.Cell, _otherItems, captioned: false);
        AutomationProperties.SetName(_otherRepeater, "Faces");
        _suggest = new StackPanel { Spacing = 8, Margin = new Thickness(0, 14, 0, 0), Visibility = Visibility.Collapsed };
        var faceColumn = new StackPanel { Padding = new Thickness(4), Spacing = 6 };
        faceColumn.Children.Add(_loadingBar.Root);
        faceColumn.Children.Add(_checkHeader);
        faceColumn.Children.Add(_checkRepeater);
        faceColumn.Children.Add(_otherHeader);
        faceColumn.Children.Add(_otherRepeater);
        faceColumn.Children.Add(_suggest);
        // The faces take keyboard focus (arrows, Delete, Y / N / M, Space); the scroller carries it.
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

        // Yes, this is … · Not … · Move to… · Unpin · Split into new person
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _yes = _look.Button("", Yes);
        ToolTipService.SetToolTip(_yes, "Keep these faces with this person for good (Y)");
        actions.Children.Add(_yes);
        _reject = _look.Button("", () => Reject(_selection.ToList()));
        ToolTipService.SetToolTip(_reject, "The face leaves this person and never rejoins them (N or Delete)");
        actions.Children.Add(_reject);
        Button moveButton = _look.Button(PeopleWords.MoveTo, () => { });
        ToolTipService.SetToolTip(moveButton, "Move these faces to someone else (M)");
        AutomationProperties.SetName(moveButton, "Move to");
        _move = new PersonPicker(chrome, grid, moveButton, () => _person.Id, MoveTo);
        actions.Children.Add(moveButton);
        _unpin = _look.Button(PeopleWords.Unpin, Unpin);
        ToolTipService.SetToolTip(_unpin, "Let these faces be checked again with the others");
        actions.Children.Add(_unpin);
        _split = _look.Button("Split into new person", Split);
        actions.Children.Add(_split);
        Grid.SetRow(actions, 3);
        root.Children.Add(actions);

        // Where the last change went, and its Undo.
        _statusRow = new Grid { ColumnSpacing = 10, Visibility = Visibility.Collapsed };
        _statusRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _statusRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _status = _look.Text("", 12, AddonColour.Title);
        _status.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetLiveSetting(_status, AutomationLiveSetting.Polite);
        _statusRow.Children.Add(_status);
        _undoButton = _look.Button(PeopleWords.Undo, Undo);
        ToolTipService.SetToolTip(_undoButton, "Undo (Ctrl+Z)");
        AutomationProperties.SetName(_undoButton, "Undo");
        Grid.SetColumn(_undoButton, 1);
        _statusRow.Children.Add(_undoButton);
        Grid.SetRow(_statusRow, 4);
        root.Children.Add(_statusRow);

        // Refine faces · … · Done
        var foot = new Grid { ColumnSpacing = 8 };
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _refine = _look.Button("Refine faces", Refine);
        ToolTipService.SetToolTip(_refine, "Check every face against this person and move out the ones that don't match");
        foot.Children.Add(_refine);
        var refineState = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        // FlatBar, not a ProgressRing: that fail-fasts in this island host.
        _refineBar = new FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]) { IsIndeterminate = true };
        _refineBar.Root.Width = 40;
        _refineBar.Root.VerticalAlignment = VerticalAlignment.Center;
        _refineBar.Root.Visibility = Visibility.Collapsed;
        refineState.Children.Add(_refineBar.Root);
        _refineNote = _look.Text("", 12, wrap: false);
        _refineNote.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetLiveSetting(_refineNote, AutomationLiveSetting.Polite);
        refineState.Children.Add(_refineNote);
        Grid.SetColumn(refineState, 1);
        foot.Children.Add(refineState);
        Button done = _look.Button("Done", Close, accent: true);
        Grid.SetColumn(done, 2);
        foot.Children.Add(done);
        Grid.SetRow(foot, 5);
        root.Children.Add(foot);

        // "Show photo": over the whole sheet.
        _preview = new PhotoPreview(chrome);
        Grid.SetRowSpan(_preview.Root, 6);
        _preview.Root.Margin = new Thickness(-20);
        _preview.Hidden += () => _focusHost.Focus(FocusState.Programmatic);
        root.Children.Add(_preview.Root);

        root.KeyDown += OnKeyDown;
        _look.Dress(this, root);
        Content = root;
        UpdateSort();
        UpdateGroups();
        UpdateButtons();
    }

    private ItemsRepeater Repeater(double width, double height, ObservableCollection<ReviewFace> items, bool captioned) => new()
    {
        Layout = new UniformGridLayout
        {
            MinItemWidth = width,
            MinItemHeight = height,
            MinColumnSpacing = 10,
            MinRowSpacing = 10,
            ItemsStretch = UniformGridLayoutItemsStretch.Fill,
        },
        ItemTemplate = new TileFactory(o => MakeTile((ReviewFace)o, width, captioned), _tiles),
        ItemsSource = items,
    };

    internal void Present()
    {
        AppWindow.MoveAndResize(Native.CentreOver(_chrome.Host.MainWindow, 720, 620));
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

    private string Name => _person.Name;

    private void Load()
    {
        ulong id = _person.Id;
        int g = ++_loadGeneration;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            List<ReviewFace>? faces = null;
            try
            {
                // person_faces_json from an older pack: the same rows without the review fields.
                faces = PeopleWords.ParseReview(api.HasPersonReviewJson ? api.PersonReviewJson(id) : api.PersonFacesJson(id));
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_closed || _person.Id != id || g != _loadGeneration) return;
                _loading = false;
                if (faces is not null) _all = faces;
                _selection.IntersectWith(_all.Select(f => f.Face));
                UpdateGroups();
                UpdateButtons();
                if (FocusManager.GetFocusedElement(Content.XamlRoot) is not (ButtonBase or FakeInput)) _focusHost.Focus(FocusState.Programmatic);
                LoadSuggestions();
            });
        });
    }

    /// <summary>"Might also be …", after the faces: people and unknown faces that resemble this one.</summary>
    private void LoadSuggestions()
    {
        if (!_api.HasPersonSuggestJson)
        {
            _suggest.Visibility = Visibility.Collapsed;
            return;
        }
        ulong id = _person.Id;
        int g = _loadGeneration;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            Suggestions s = Suggestions.None;
            try { s = PeopleWords.ParseSuggest(api.PersonSuggestJson(id, 12)); }
            catch (Exception ex) when (ex is MediaViewerException or JsonException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_closed || _person.Id != id || g != _loadGeneration) return;
                ShowSuggestions(s);
            });
        });
    }

    private void ShowSuggestions(Suggestions s)
    {
        _suggest.Children.Clear();
        if (s.IsEmpty)
        {
            _suggest.Visibility = Visibility.Collapsed;
            return;
        }
        TextBlock header = _look.Text(PeopleWords.MightAlsoBe(Name), 13, AddonColour.Title, wrap: false);
        AutomationProperties.SetHeadingLevel(header, AutomationHeadingLevel.Level2);
        _suggest.Children.Add(header);
        var tiles = new List<UIElement>();
        foreach (SuggestedPerson sp in s.People)
        {
            SuggestedPerson who = sp;
            var face = new TileFace(sp.CoverFace, sp.CoverPath, sp.CoverMs, sp.CoverBox, Caption: "Unnamed · " + PeopleWords.Faces(sp.Faces));
            var tile = new FaceTile(_chrome, face, SuggestWidth, captioned: true, t => Preview(t.Data),
                                    action: (PeopleWords.Merge, $"Merge this person into {(_person.Label)}", () => MergeSuggested(who)));
            tiles.Add(tile.Root);
        }
        foreach (SuggestedFace sf in s.Faces)
        {
            SuggestedFace one = sf;
            var face = new TileFace(sf.Face, sf.Path, sf.PtsMs, sf.Box);
            var tile = new FaceTile(_chrome, face, SuggestWidth, captioned: true, t => Preview(t.Data),
                                    action: (PeopleWords.Yes, PeopleWords.YesLabel(Name), () => AddSuggested(one)));
            tiles.Add(tile.Root);
        }
        var strip = new ItemsRepeater
        {
            Layout = new UniformGridLayout
            {
                MinItemWidth = SuggestWidth,
                MinItemHeight = FaceTile.Cell + FaceTile.CaptionHeight + FaceTile.ActionHeight,
                MinColumnSpacing = 10,
                MinRowSpacing = 10,
            },
            ItemsSource = tiles,
        };
        AutomationProperties.SetName(strip, PeopleWords.MightAlsoBe(Name));
        _suggest.Children.Add(strip);
        _suggest.Visibility = Visibility.Visible;
    }

    // ---- the two groups -------------------------------------------------------------------

    private void SetSort(bool clearest)
    {
        if (clearest != _clearest)
        {
            _clearest = clearest;
            UpdateGroups();
        }
        UpdateSort();
    }

    private void UpdateSort()
    {
        for (int i = 0; i < 2; ++i) _sortButtons[i].IsChecked = (i == 1) == _clearest;
    }

    private static bool SameRows(IReadOnlyList<ReviewFace> a, IReadOnlyList<ReviewFace> b) =>
        a.Count == b.Count && a.Zip(b).All(p => p.First.Face == p.Second.Face && p.First.Pinned == p.Second.Pinned &&
                                                  p.First.RivalName == p.Second.RivalName && p.First.DoubtfulPin == p.Second.DoubtfulPin);

    private static void Replace(ObservableCollection<ReviewFace> items, List<ReviewFace> want)
    {
        if (SameRows(items, want)) return;
        items.Clear();
        foreach (ReviewFace f in want) items.Add(f);
    }

    /// <summary>"Check these" and the rest, in the chosen order (changed only when they differ).</summary>
    private void UpdateGroups()
    {
        (List<ReviewFace> check, List<ReviewFace> others) = PeopleWords.Group(_all, _clearest);
        Replace(_checkItems, check);
        Replace(_otherItems, others);
        bool any = check.Count > 0;
        _checkHeader.Text = PeopleWords.CheckHeader(check.Count);
        _checkHeader.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
        _checkRepeater.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
        _otherHeader.Visibility = any && others.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        // Nothing to sort without the review fields (an older pack) or with one face.
        _sortTrack.Visibility = _api.HasPersonReviewJson && _all.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
        if (_cursor >= Order.Count) _cursor = Order.Count - 1;
    }

    /// <summary>The faces as shown, top to bottom: the arrows' order.</summary>
    private List<ReviewFace> Order => _checkItems.Concat(_otherItems).ToList();

    private FaceTile MakeTile(ReviewFace f, double width, bool captioned)
    {
        var data = new TileFace(f.Face, f.Path, f.PtsMs, f.Box, f.Pinned, captioned ? PeopleWords.CheckCaption(f) : "");
        var tile = new FaceTile(_chrome, data, width, captioned, t => Tap(t.Id), reject: () => Reject(new List<ulong> { f.Face }));
        tile.SetSelected(_selection.Contains(f.Face));
        var menu = new MenuFlyout();
        menu.Opening += (_, _) => FillFaceMenu(menu, f);
        tile.Root.ContextFlyout = menu;
        return tile;
    }

    private void FillFaceMenu(MenuFlyout menu, ReviewFace f)
    {
        menu.Items.Clear();
        // The menu acts on this face, with the selection when the face is in it.
        if (!_selection.Contains(f.Face))
        {
            _selection.Clear();
            _selection.Add(f.Face);
            _cursor = Order.FindIndex(o => o.Face == f.Face);
            UpdateButtons();
        }
        menu.Items.Add(Item(PeopleWords.ShowPhoto, () => Preview(f)));
        if (_api.HasFacesPin) menu.Items.Add(Item(PeopleWords.YesLabel(Name), Yes));
        menu.Items.Add(Item(PeopleWords.NotLabel(Name), () => Reject(_selection.ToList())));
        if (_api.HasFacesAssign) menu.Items.Add(Item(PeopleWords.MoveTo, _move.Show));
        if (_api.HasFacesPin && AllPinned()) menu.Items.Add(Item(PeopleWords.Unpin, Unpin));
    }

    private static MenuFlyoutItem Item(string text, Action click)
    {
        var item = new MenuFlyoutItem { Text = text };
        item.Click += (_, _) => click();
        return item;
    }

    // ---- selection ------------------------------------------------------------------------

    private bool IsPinned(ulong face) => _all.Any(f => f.Face == face && f.Pinned);

    private bool AllPinned() => _selection.Count > 0 && _selection.All(IsPinned);

    private void UpdateButtons()
    {
        int n = _selection.Count;
        _yes.Content = PeopleWords.YesLabel(Name);
        AutomationProperties.SetName(_yes, PeopleWords.YesLabel(Name));
        _yes.Visibility = _api.HasFacesPin ? Visibility.Visible : Visibility.Collapsed;
        _yes.IsEnabled = n > 0 && !AllPinned();
        _reject.Content = PeopleWords.NotLabel(Name);
        AutomationProperties.SetName(_reject, PeopleWords.NotLabel(Name));
        _reject.IsEnabled = n > 0;
        _move.Button.Visibility = _api.HasFacesAssign ? Visibility.Visible : Visibility.Collapsed;
        _move.Button.IsEnabled = n > 0;
        _unpin.Visibility = _api.HasFacesPin && AllPinned() ? Visibility.Visible : Visibility.Collapsed;
        _split.IsEnabled = n > 0;
        _refine.IsEnabled = !_refining && !_loading;
        _merge.IsEnabled = _grid.People.Count > 1;
        _loadingBar.Root.Visibility = _loading ? Visibility.Visible : Visibility.Collapsed;
        _refineBar.Root.Visibility = _refining ? Visibility.Visible : Visibility.Collapsed;
        _refineNote.Visibility = _refining ? Visibility.Collapsed : Visibility.Visible;
        foreach ((ulong id, FaceTile tile) in _tiles) tile.SetSelected(_selection.Contains(id));
    }

    private static bool Down(VirtualKey key) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private static bool Modified() => Down(VirtualKey.Control) || Down(VirtualKey.Shift);

    private void Tap(ulong face)
    {
        if (Modified())
        {
            if (!_selection.Add(face)) _selection.Remove(face);
        }
        else
        {
            _selection.Clear();
            _selection.Add(face);
        }
        _cursor = Order.FindIndex(o => o.Face == face);
        UpdateButtons();
        _focusHost.Focus(FocusState.Programmatic);
    }

    /// <summary>← → move the selection, Shift adds to it (the Mac's sheet, issue #192).</summary>
    private void Step(int step, bool add)
    {
        List<ReviewFace> order = Order;
        if (order.Count == 0) return;
        _cursor = PeopleWords.Step(_cursor, order.Count, step);
        ulong id = order[_cursor].Face;
        if (!add) _selection.Clear();
        _selection.Add(id);
        UpdateButtons();
        Reveal(_cursor);
    }

    private void Reveal(int index)
    {
        bool inCheck = index < _checkItems.Count;
        ItemsRepeater repeater = inCheck ? _checkRepeater : _otherRepeater;
        int at = inCheck ? index : index - _checkItems.Count;
        if (repeater.GetOrCreateElement(at) is FrameworkElement e)
        {
            e.UpdateLayout();
            e.StartBringIntoView(new BringIntoViewOptions { VerticalAlignmentRatio = 0.5 });
        }
    }

    private ReviewFace? Focused()
    {
        List<ReviewFace> order = Order;
        if (_cursor >= 0 && _cursor < order.Count && _selection.Contains(order[_cursor].Face)) return order[_cursor];
        return order.FirstOrDefault(f => _selection.Contains(f.Face));
    }

    private void Preview(ReviewFace f) =>
        _preview.Show(f.Face, f.Box, f.Path, f.PtsMs, f.Check ? PeopleWords.CheckCaption(f) : "");

    private void Preview(TileFace f) => _preview.Show(f.Id, f.Box, f.Path, f.PtsMs, f.Caption);

    /// <summary>Faces leave the sheet now; the pack's answer and AI_PEOPLE bring the truth.</summary>
    private void RemoveLocal(IReadOnlyCollection<ulong> ids)
    {
        var gone = ids.ToHashSet();
        _all = _all.Where(f => !gone.Contains(f.Face)).ToList();
        _selection.ExceptWith(gone);
        UpdateGroups();
        UpdateButtons();
    }

    // ---- corrections (workers) -------------------------------------------------------------

    /// <summary>Where the last change went, and its Undo when it has one (only the last action's).</summary>
    private void SetStatus(string text, Action<AiApi>? undo)
    {
        _undo = undo;
        _status.Text = text;
        _statusRow.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        _undoButton.Visibility = undo is null ? Visibility.Collapsed : Visibility.Visible;
    }

    /// <summary>Faces put back in <paramref name="person"/> with the pin each had before: two calls at most.</summary>
    private static void PutBack(AiApi api, IReadOnlyList<ulong> faces, ulong person, IReadOnlySet<ulong> pinnedBefore)
    {
        (List<ulong> pinned, List<ulong> unpinned) = PeopleWords.ByPin(faces, pinnedBefore);
        if (pinned.Count > 0) api.FacesAssign(pinned, person, pin: true, undo: true);
        if (unpinned.Count > 0) api.FacesAssign(unpinned, person, pin: false, undo: true);
    }

    /// <summary>Runs <paramref name="work"/> on a worker, then <paramref name="done"/> here (unless closed).</summary>
    // A later action supersedes an earlier one's answer: only the last action's
    // line and Undo show, so a slow write cannot put back an older Undo.
    private void Run<T>(Func<AiApi, T> work, Action<T> done)
    {
        AiApi api = _api;
        int action = ++_action;
        _ = Task.Run(() =>
        {
            T result = work(api);
            DispatcherQueue.TryEnqueue(() =>
            {
                if (!_closed)
                {
                    // Superseded: its line is gone, but the faces shown follow the pack.
                    if (action == _action) done(result);
                    else Load();
                }
                _grid.Refresh();
            });
        });
    }

    /// <summary>"Not …": each face leaves this person for good; the line says where they went.</summary>
    private void Reject(List<ulong> ids)
    {
        if (ids.Count == 0) return;
        var pinnedBefore = ids.Where(IsPinned).ToHashSet();
        ulong person = _person.Id;
        RemoveLocal(ids);
        SetStatus("", null);
        Run(api =>
        {
            if (api.HasFacesRejectJson)
            {
                try
                {
                    string json = api.FacesRejectJson(ids);
                    string line;
                    try { line = PeopleWords.RejectSummary(PeopleWords.ParseReject(json), ids.Count); }
                    catch (JsonException) { line = PeopleWords.Removed(ids.Count); }
                    return (Line: line, Undoable: api.HasFacesAssign, Failed: false);
                }
                catch (MediaViewerException) { return (Line: PeopleWords.CouldNotChange, Undoable: false, Failed: true); }
            }
            // An older pack: one at a time, nothing to say where they went.
            bool failed = false;
            foreach (ulong f in ids)
            {
                try { api.FaceReject(f); }
                catch (MediaViewerException) { failed = true; }
            }
            return (Line: failed ? PeopleWords.CouldNotChange : PeopleWords.Removed(ids.Count), Undoable: false, Failed: failed);
        }, r =>
        {
            SetStatus(r.Line, r.Undoable ? api => PutBack(api, ids, person, pinnedBefore) : null);
            if (r.Failed) Load();
        });
    }

    /// <summary>"Move to…" picked someone: the faces are theirs, pinned.</summary>
    private void MoveTo(PickPerson to)
    {
        List<ulong> ids = _selection.ToList();
        if (ids.Count == 0 || !_api.HasFacesAssign) return;
        var pinnedBefore = ids.Where(IsPinned).ToHashSet();
        ulong person = _person.Id;
        RemoveLocal(ids);
        SetStatus("", null);
        Run(api =>
        {
            try { api.FacesAssign(ids, to.Id, pin: true); return true; }
            catch (MediaViewerException) { return false; }
        }, ok =>
        {
            if (ok) SetStatus(PeopleWords.Moved(ids.Count, to.Name), api => PutBack(api, ids, person, pinnedBefore));
            else
            {
                SetStatus(PeopleWords.CouldNotChange, null);
                Load();
            }
        });
    }

    /// <summary>"Yes, this is …": the faces become the user's here (pinned).</summary>
    private void Yes() => Pin(true);

    private void Unpin() => Pin(false);

    private void Pin(bool pinned)
    {
        List<ulong> ids = _selection.ToList();
        if (ids.Count == 0 || !_api.HasFacesPin) return;
        var chosen = ids.ToHashSet();
        // Shown now; the pack's own answer (and "Check these") comes with the read after.
        _all = _all.Select(f => chosen.Contains(f.Face) ? f with { Pinned = pinned, DoubtfulPin = false, Check = !pinned && f.Check } : f).ToList();
        UpdateGroups();
        UpdateButtons();
        SetStatus("", null);
        Run(api =>
        {
            try { api.FacesPin(ids, pinned); return true; }
            catch (MediaViewerException) { return false; }
        }, ok =>
        {
            if (!ok) SetStatus(PeopleWords.CouldNotChange, null);
            Load();
        });
    }

    private void Split()
    {
        List<ulong> ids = _selection.ToList();
        if (ids.Count == 0) return;
        RemoveLocal(ids);
        SetStatus("", null);
        Run(api =>
        {
            try { api.FaceSplit(ids); }
            catch (MediaViewerException) { }
            return 0;
        }, _ => { });
    }

    /// <summary>"Might also be": a suggested person is this one.</summary>
    private void MergeSuggested(SuggestedPerson sp)
    {
        ulong into = _person.Id, coverInto = _person.CoverFace;
        SetStatus("", null);
        Run(api =>
        {
            try
            {
                if (api.HasPersonMergeCovers) return (Ok: true, Token: api.PersonMergeCovers(into, sp.Id, coverInto, sp.CoverFace));
                api.PersonMerge(into, sp.Id);
                return (Ok: true, Token: 0UL);
            }
            catch (MediaViewerException) { return (Ok: false, Token: 0UL); }
        }, r =>
        {
            if (!r.Ok) SetStatus(PeopleWords.CouldNotChange, null);
            else SetStatus(PeopleWords.Merged, r.Token != 0 && _api.HasPersonMergeUndo ? api => api.PersonMergeUndo(r.Token) : null);
            Load();
        });
    }

    /// <summary>"Might also be": an unknown face is this person.</summary>
    private void AddSuggested(SuggestedFace sf)
    {
        if (!_api.HasFacesAssign) return;
        ulong person = _person.Id;
        string name = Name;
        SetStatus("", null);
        Run(api =>
        {
            try { api.FacesAssign(new[] { sf.Face }, person, pin: true); return true; }
            catch (MediaViewerException) { return false; }
        }, ok =>
        {
            SetStatus(ok ? PeopleWords.Added(1, name) : PeopleWords.CouldNotChange, null);
            Load();
        });
    }

    /// <summary>The Undo button and Ctrl+Z: the last reject, move or merge, put back.</summary>
    private void Undo()
    {
        if (_undo is not Action<AiApi> work) return;
        _undo = null;
        _undoButton.Visibility = Visibility.Collapsed;
        Run(api =>
        {
            try { work(api); return true; }
            catch (MediaViewerException) { return false; }
        }, ok =>
        {
            SetStatus(ok ? PeopleWords.Undone : PeopleWords.CouldNotChange, null);
            Load();
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

    // ---- keys (the sheet's own; not command-table rows) --------------------------------

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        object? focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        bool typing = focused is FakeInput;
        if (_preview.IsOpen)
        {
            if (e.Key is VirtualKey.Escape or VirtualKey.Space or VirtualKey.Enter)
            {
                _preview.Hide();
                e.Handled = true;
            }
            return;
        }
        if (typing) return;
        bool ctrl = Down(VirtualKey.Control);
        int n = _selection.Count;
        switch (e.Key)
        {
            case VirtualKey.Z when ctrl:
                Undo();
                e.Handled = true;
                break;
            case VirtualKey.Delete or VirtualKey.Back when n > 0:
            case VirtualKey.N when !ctrl && n > 0:
                Reject(_selection.ToList());
                e.Handled = true;
                break;
            case VirtualKey.Y when !ctrl && n > 0 && _api.HasFacesPin:
                Yes();
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
            case VirtualKey.Space when focused is not ButtonBase && Focused() is ReviewFace face:
                Preview(face);
                e.Handled = true;
                break;
            case VirtualKey.Escape:
                Close();
                e.Handled = true;
                break;
            // Done is the default button, as on the Mac (a focused button keeps its own
            // Enter), but never while a face is focused: Enter there is not "close".
            case VirtualKey.Enter when focused is not ButtonBase && !(ReferenceEquals(focused, _focusHost) && n > 0):
                Close();
                e.Handled = true;
                break;
        }
    }
}
