// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Numerics;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;
using Windows.UI.Core;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// People (docs/design/17 PR 24), in Settings → Local search → People as on the
/// Mac (PeopleView.swift's PeopleGrid): circular covers cut from the cover
/// picture in memory; a click opens their photos in the gallery (Settings
/// steps aside); names edited in place; merging by dragging a person onto
/// another, Ctrl-clicking several then "Merge into…", or the context menu;
/// and a person's faces (the hover ⋯ or "Faces…") in <see cref="PersonSheet"/>.
/// A grouping that cannot be corrected is worse than none.
/// </summary>
/// <remarks>
/// The name field is a FakeInput (Shared\FakeInput.cs): a WinUI TextBox
/// fail-fasts in this island host. Reads run on workers; rule 6: names and
/// paths are shown, never logged. Keyboard: Tab reaches each cover (Enter or
/// Space shows their photos, the context-menu key or Shift+F10 its menu) and
/// each name (Enter edits it, Esc in the field cancels); Esc with people
/// selected clears the selection before Settings closes.
/// </remarks>
internal sealed class PeopleGrid
{
    private const double Cover = 84, CardWidth = 112, CardHeight = 140;
    private const string DragPrefix = "mediaviewer-person:";

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    // Slots, not the records: a person whose count or name changed is
    // redrawn in its card, never swapped under the repeater.
    private readonly ObservableCollection<Slot> _items = new();
    private readonly Dictionary<ulong, Card> _cards = new();
    private readonly HashSet<ulong> _selection = new();
    private readonly StackPanel _root;
    private readonly StackPanel _scopeRow;
    private readonly ToggleButton[] _scopeButtons = new ToggleButton[2];
    private readonly TextBlock _scopeFolder;
    private readonly TextBlock _empty;
    private readonly Grid _hintRow;
    private readonly MediaViewer.Shared.FlatBar _dedupeBar;
    private readonly Button _dedupe;
    private readonly Grid _mergeBar;
    private readonly TextBlock _mergeCount;
    private readonly Button _mergeSelected;
    private readonly ItemsRepeater _repeater;
    private readonly TextBlock _note;
    private MvAiScope _scope = MvAiScope.Tree;
    private bool _deduping;
    private ulong _opening;
    private int _noteGeneration;
    private int _loadGeneration;
    // people_json in flight; more AI_PEOPLE events while it runs fold into one
    // more read after it (a face landing posts one for every face).
    private bool _loading;
    private bool _loadAgain;
    // Read again the next time Settings shows it (nothing reads while hidden).
    private bool _stale = true;
    private PersonSheet? _sheet;

    private static readonly string[] ScopeNames = { "This folder", "+ Subfolders" };
    private static readonly string[] ScopeHelp =
    {
        "People with a face in the open folder only.",
        "People with a face in the open folder and the folders inside it.",
    };

    public FrameworkElement Root => _root;

    private sealed class Slot(PersonVm person)
    {
        public PersonVm P = person;
    }

    private IEnumerable<PersonVm> All => _items.Select(s => s.P);

    public PeopleGrid(AiChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        _root = new StackPanel { Spacing = 10, Padding = new Thickness(12) };

        // "People in · This folder | + Subfolders · Photos": the grid follows the
        // folder the viewer has open (docs/design/17 "People in the open folder").
        // Absent when no folder is open: everyone shows then.
        _scopeRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        TextBlock scopeLabel = _look.Text("People in", 12, wrap: false);
        scopeLabel.VerticalAlignment = VerticalAlignment.Center;
        _scopeRow.Children.Add(scopeLabel);
        var segments = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        for (int i = 0; i < 2; ++i)
        {
            var scope = (MvAiScope)i;
            var b = new ToggleButton
            {
                Content = ScopeNames[i],
                FontFamily = _look.Font,
                FontSize = 12,
                Padding = new Thickness(12, 3, 12, 4),
                CornerRadius = i == 0 ? new CornerRadius(6, 0, 0, 6) : new CornerRadius(0, 6, 6, 0),
            };
            AutomationProperties.SetName(b, "People in " + ScopeNames[i]);
            ToolTipService.SetToolTip(b, ScopeHelp[i]);
            b.Click += (_, _) => SetScope(scope);
            _scopeButtons[i] = b;
            segments.Children.Add(b);
        }
        _scopeRow.Children.Add(segments);
        _scopeFolder = _look.Text("", 12, AddonColour.Title, wrap: false);
        _scopeFolder.VerticalAlignment = VerticalAlignment.Center;
        _scopeFolder.TextTrimming = TextTrimming.CharacterEllipsis;
        _scopeFolder.MaxWidth = 280;
        _scopeRow.Children.Add(_scopeFolder);
        _root.Children.Add(_scopeRow);

        _empty = _look.Text("", 12);
        _empty.Visibility = Visibility.Collapsed;
        _root.Children.Add(_empty);

        // The hint, and "Merge duplicates" with its activity while it runs.
        _hintRow = new Grid { ColumnSpacing = 10 };
        _hintRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _hintRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _hintRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock hint = _look.Text(
            "Click a person to see their photos. The same person twice? Drag one onto the other, or Ctrl-click several and merge them.", 12);
        hint.VerticalAlignment = VerticalAlignment.Center;
        _hintRow.Children.Add(hint);
        // FlatBar, not a WinUI ProgressRing: that fail-fasts in this island host.
        _dedupeBar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]) { IsIndeterminate = true };
        _dedupeBar.Root.Width = 48;
        _dedupeBar.Root.VerticalAlignment = VerticalAlignment.Center;
        _dedupeBar.Root.Visibility = Visibility.Collapsed;
        Grid.SetColumn(_dedupeBar.Root, 1);
        _hintRow.Children.Add(_dedupeBar.Root);
        _dedupe = _look.Button("Merge duplicates", MergeDuplicates);
        _dedupe.VerticalAlignment = VerticalAlignment.Center;
        ToolTipService.SetToolTip(_dedupe,
            "Re-check every face and merge people who are the same person. Two people you named differently are never merged; undo with Split.");
        AutomationProperties.SetName(_dedupe, "Merge duplicate people");
        Grid.SetColumn(_dedupe, 2);
        _hintRow.Children.Add(_dedupe);
        _root.Children.Add(_hintRow);

        // "3 people selected · Merge into… · Cancel": the target keeps its name.
        _mergeBar = new Grid
        {
            ColumnSpacing = 10,
            Padding = new Thickness(10),
            CornerRadius = new CornerRadius(6),
            Background = _look.Tint(AddonColour.Accent, 20),
            Visibility = Visibility.Collapsed,
        };
        _mergeBar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _mergeBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _mergeBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _mergeCount = _look.Text("", 12, AddonColour.Title, wrap: false);
        _mergeCount.VerticalAlignment = VerticalAlignment.Center;
        _mergeBar.Children.Add(_mergeCount);
        // A Button with a Flyout: DropDownButton fail-fasts in this host.
        _mergeSelected = _look.Button("Merge into…  ▾", () => { });
        _mergeSelected.Flyout = new MenuFlyout();
        ((MenuFlyout)_mergeSelected.Flyout).Opening += (_, _) => FillMergeSelected();
        Grid.SetColumn(_mergeSelected, 1);
        _mergeBar.Children.Add(_mergeSelected);
        Button cancel = _look.Button("Cancel", ClearSelection);
        Grid.SetColumn(cancel, 2);
        _mergeBar.Children.Add(cancel);
        _root.Children.Add(_mergeBar);

        // Adaptive columns, as the Mac's LazyVGrid(minimum 104, maximum 132).
        // Virtualised against Settings' own scroller.
        _repeater = new ItemsRepeater
        {
            Layout = new UniformGridLayout
            {
                MinItemWidth = CardWidth,
                MinItemHeight = CardHeight,
                MinColumnSpacing = 14,
                MinRowSpacing = 16,
                ItemsStretch = UniformGridLayoutItemsStretch.Fill,
                ItemsJustification = UniformGridLayoutItemsJustification.Start,
            },
            ItemTemplate = new CardFactory(this),
            ItemsSource = _items,
        };
        _root.Children.Add(_repeater);

        _note = _look.Text("", 12, AddonColour.Title);
        _note.Visibility = Visibility.Collapsed;
        _root.Children.Add(_note);

        _root.KeyDown += (_, e) =>
        {
            // Esc clears a selection before Settings closes (the Mac's onExitCommand).
            if (e.Key != VirtualKey.Escape || _selection.Count == 0) return;
            ClearSelection();
            e.Handled = true;
        };
        // Read when Settings shows the grid again (re-parented, or un-collapsed):
        // nothing reads while it is hidden.
        _root.Loaded += (_, _) =>
        {
            if (_stale) Refresh();
        };
        _root.EffectiveViewportChanged += (_, _) =>
        {
            if (_stale && Shown()) Refresh();
        };
        UpdateScope();
        UpdateChrome();
    }

    // ---- the open folder --------------------------------------------------------------

    /// <summary>The viewer opened another folder: the grid follows it.</summary>
    internal void OnFolderChanged()
    {
        // A folder is open: its people, never everyone.
        if (!string.IsNullOrEmpty(_chrome.Folder) && _scope == MvAiScope.All) _scope = MvAiScope.Tree;
        UpdateScope();
        Refresh();
    }

    private void SetScope(MvAiScope scope)
    {
        if (scope != _scope)
        {
            _scope = scope;
            Refresh();
        }
        UpdateScope();
    }

    /// <summary>The pack's scope for the grid: none when no folder is open.</summary>
    private string? ScopeDir => string.IsNullOrEmpty(_chrome.Folder) || _scope == MvAiScope.All ? null : _chrome.Folder;

    private MvAiScope ScopeValue => ScopeDir is null ? MvAiScope.All : _scope;

    private string FolderName
    {
        get
        {
            string dir = _chrome.Folder ?? "";
            string leaf = System.IO.Path.GetFileName(dir.TrimEnd('\\', '/'));
            return leaf.Length == 0 ? dir : leaf;
        }
    }

    private void UpdateScope()
    {
        string? folder = _chrome.Folder;
        bool open = !string.IsNullOrEmpty(folder);
        _scopeRow.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
        for (int i = 0; i < 2; ++i) _scopeButtons[i].IsChecked = (int)_scope == i;
        _scopeFolder.Text = open ? FolderName : "";
        ToolTipService.SetToolTip(_scopeFolder, folder);
    }

    // ---- loading (worker) --------------------------------------------------------------

    /// <summary>AI_PEOPLE or the people count moved: read again if Settings shows the grid.</summary>
    internal void Refresh()
    {
        if (!Shown())
        {
            _stale = true;
            return;
        }
        _stale = false;
        if (_loading)
        {
            _loadAgain = true;
            return;
        }
        _loading = true;
        int generation = ++_loadGeneration;
        AiApi api = _api;
        string? scopeDir = ScopeDir;
        MvAiScope scope = ScopeValue;
        _ = Task.Run(() =>
        {
            var people = new List<PersonVm>();
            try
            {
                using JsonDocument doc = JsonDocument.Parse(api.PeopleJson(scopeDir, scope));
                foreach (JsonElement p in doc.RootElement.EnumerateArray())
                {
                    people.Add(new PersonVm(
                        p.GetProperty("id").GetUInt64(),
                        p.TryGetProperty("name", out JsonElement n) ? n.GetString() ?? "" : "",
                        p.TryGetProperty("faces", out JsonElement f) ? f.GetInt64() : 0,
                        p.TryGetProperty("cover_face", out JsonElement c) ? c.GetUInt64() : 0,
                        Box(p, "cover_box")));
                }
            }
            // FormatException too: a read that throws must still clear _loading,
            // or the grid would stop following the index.
            catch (Exception ex) when (ex is MediaViewerException or JsonException or InvalidOperationException
                                           or FormatException) { }
            _root.DispatcherQueue.TryEnqueue(() =>
            {
                if (generation == _loadGeneration) Apply(people);
                if (!_loadAgain)
                {
                    _loading = false;
                    return;
                }
                // While faces stream in: at most two reads a second.
                _loadAgain = false;
                _ = Task.Delay(500).ContinueWith(_ => _root.DispatcherQueue.TryEnqueue(() =>
                {
                    _loading = false;
                    Refresh();
                }), TaskScheduler.Default);
            });
        });
    }

    // Collapsed anywhere above (Settings hidden, People off): nothing to read for.
    private bool Shown()
    {
        if (_root.XamlRoot is null) return false;
        for (DependencyObject? d = _root; d is not null; d = VisualTreeHelper.GetParent(d))
        {
            if (d is UIElement { Visibility: Visibility.Collapsed }) return false;
        }
        return true;
    }

    internal static double[] Box(JsonElement e, string key)
    {
        if (!e.TryGetProperty(key, out JsonElement b) || b.ValueKind != JsonValueKind.Array || b.GetArrayLength() != 4)
        {
            return new[] { 0.0, 0.0, 1.0, 1.0 };
        }
        return b.EnumerateArray().Select(v => v.GetDouble()).ToArray();
    }

    /// <summary>
    /// The new list, changed in place where it can be: a rebuilt grid loses
    /// its scroll and crops every time a face lands.
    /// </summary>
    private void Apply(List<PersonVm> people)
    {
        if (All.Select(p => p.Id).SequenceEqual(people.Select(p => p.Id)))
        {
            for (int i = 0; i < people.Count; ++i)
            {
                if (!SameValues(_items[i].P, people[i])) Set(_items[i], people[i]);
            }
        }
        else
        {
            _items.Clear();
            foreach (PersonVm p in people) _items.Add(new Slot(p));
        }
        // A merged or regrouped person leaves the selection with the grid.
        _selection.IntersectWith(All.Select(p => p.Id));
        if (_opening != 0 && All.All(p => p.Id != _opening)) _opening = 0;
        UpdateChrome();
        _sheet?.PeopleChanged();
    }

    private void Set(Slot slot, PersonVm person)
    {
        slot.P = person;
        if (_cards.TryGetValue(person.Id, out Card? card)) card.Bind(person);
    }

    private static bool SameValues(PersonVm a, PersonVm b) =>
        a.Id == b.Id && a.Name == b.Name && a.Faces == b.Faces && a.CoverFace == b.CoverFace && a.Box.SequenceEqual(b.Box);

    private void UpdateChrome()
    {
        bool any = _items.Count > 0;
        if (!any && _loadGeneration > 0)
        {
            _empty.Text = ScopeDir is null
                ? "No people yet. Faces are grouped as your folders are indexed."
                : _scope == MvAiScope.Folder
                    ? $"Nobody in {FolderName} yet. Faces are grouped as the folder is indexed; everyone found shows when no folder is open."
                    : $"Nobody in {FolderName} or its subfolders yet. Faces are grouped as the folder is indexed; everyone found shows when no folder is open.";
            _empty.Visibility = Visibility.Visible;
        }
        else
        {
            _empty.Visibility = Visibility.Collapsed;
        }
        int n = _selection.Count;
        _mergeBar.Visibility = any && n >= 2 ? Visibility.Visible : Visibility.Collapsed;
        _mergeCount.Text = $"{n} people selected";
        _hintRow.Visibility = any && n < 2 ? Visibility.Visible : Visibility.Collapsed;
        _dedupe.IsEnabled = !_deduping;
        _dedupeBar.Root.Visibility = _deduping ? Visibility.Visible : Visibility.Collapsed;
        _repeater.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
        foreach (Card card in _cards.Values) card.Update();
    }

    // ---- actions ------------------------------------------------------------------------

    private static bool Modified() =>
        InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control).HasFlag(CoreVirtualKeyStates.Down) ||
        InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(CoreVirtualKeyStates.Down);

    private void Tap(PersonVm person)
    {
        if (Modified())
        {
            if (!_selection.Add(person.Id)) _selection.Remove(person.Id);
        }
        else if (_selection.Count > 0)
        {
            _selection.Clear();
        }
        else
        {
            // A click shows who it is: their photos, in the gallery.
            ShowPhotos(person);
            return;
        }
        UpdateChrome();
    }

    private void ClearSelection()
    {
        _selection.Clear();
        UpdateChrome();
    }

    /// <summary>
    /// A person's photos, in the gallery (Settings closes as it opens). The
    /// search takes a moment: the card says so, and says if nothing came back.
    /// </summary>
    internal void ShowPhotos(PersonVm person)
    {
        if (_opening != 0) return;
        string name = person.Name.Length > 0 ? person.Name : "this person";
        // The grid's own scope: the list is the photos the card counted.
        ulong search;
        try { search = _api.SearchPerson(person.Id, ScopeDir, ScopeValue); }
        catch (MediaViewerException)
        {
            Note($"Photos of {name} could not be opened. Try again.");
            return;
        }
        _opening = person.Id;
        UpdateChrome();
        _chrome.OpenSearchAsList(search, person.Name.Length > 0 ? $"Photos of {person.Name}" : "Photos of this person", outcome =>
        {
            _opening = 0;
            UpdateChrome();
            if (outcome == AiChrome.PersonOpen.Nothing) Note($"No photos of {name} are indexed yet.");
            else if (outcome == AiChrome.PersonOpen.Failed) Note($"Photos of {name} could not be opened. Try again.");
        });
    }

    /// <summary>A person's faces, to correct (the Mac's sheet).</summary>
    internal void OpenFaces(PersonVm person)
    {
        _sheet?.Close();
        _sheet = new PersonSheet(_chrome, this, person);
        _sheet.Closed += (s, _) =>
        {
            if (ReferenceEquals(s, _sheet)) _sheet = null;
        };
        _sheet.Present();
    }

    /// <summary>Settings built a newer panel, or the pack is going: the sheet goes too.</summary>
    internal void Detach()
    {
        _sheet?.Close();
        _sheet = null;
    }

    internal IReadOnlyList<PersonVm> People => All.ToList();

    internal PersonVm? Find(ulong id) => _items.FirstOrDefault(s => s.P.Id == id)?.P;

    internal void Rename(ulong id, string name)
    {
        try { _api.PersonRename(id, name); }
        catch (MediaViewerException) { return; }
        Slot? slot = _items.FirstOrDefault(s => s.P.Id == id);
        if (slot is not null) Set(slot, slot.P with { Name = name });
        _sheet?.PeopleChanged();
    }

    /// <summary>
    /// Several people are one: their faces move to <paramref name="into"/>,
    /// which keeps its name (or takes the first name among them, as faces_db
    /// does). The grid changes now; person_merge can wait for the indexer, so
    /// the rows are rewritten on a worker and AI_PEOPLE brings the truth.
    /// </summary>
    internal void Merge(ulong into, IEnumerable<ulong> ids)
    {
        List<ulong> from = ids.Where(id => id != into).Distinct().ToList();
        Slot? at = _items.FirstOrDefault(s => s.P.Id == into);
        if (from.Count == 0 || at is null) return;
        List<Slot> moved = _items.Where(s => from.Contains(s.P.Id)).ToList();
        if (moved.Count == 0) return;
        PersonVm target = at.P;
        string name = target.Name.Length > 0 ? target.Name : moved.FirstOrDefault(s => s.P.Name.Length > 0)?.P.Name ?? "";
        Set(at, target with { Name = name, Faces = target.Faces + moved.Sum(s => s.P.Faces) });
        foreach (Slot s in moved) _items.Remove(s);
        _selection.Clear();
        string who = name.Length > 0 ? name : "one person";
        Note(moved.Count == 1 ? $"Merged into {who}." : $"Merged {moved.Count + 1} people into {who}.");
        UpdateChrome();
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            bool failed = false;
            foreach (ulong f in from)
            {
                try { api.PersonMerge(into, f); }
                catch (MediaViewerException) { failed = true; }
            }
            _root.DispatcherQueue.TryEnqueue(() =>
            {
                if (failed) Note("Some faces could not be merged. Try again.");
                Refresh();
            });
        });
    }

    /// <summary>"Merge duplicates" (docs/design/17): the whole library, on request.</summary>
    private void MergeDuplicates()
    {
        if (_deduping) return;
        _deduping = true;
        UpdateChrome();
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            (uint Merged, uint Moved)? result = null;
            try { result = api.PeopleDedupe(); }
            catch (MediaViewerException) { }
            _root.DispatcherQueue.TryEnqueue(() =>
            {
                _deduping = false;
                UpdateChrome();
                Note(result switch
                {
                    null => "Couldn't check for duplicates. Try again.",
                    (0, 0) => "No duplicates found, and every face matches.",
                    var (m, f) => (m == 1 ? "1 person" : $"{m} people") + " merged, " + (f == 1 ? "1 face" : $"{f} faces") + " moved.",
                });
                Refresh();
            });
        });
    }

    private void FillMergeSelected()
    {
        var menu = (MenuFlyout)_mergeSelected.Flyout;
        menu.Items.Clear();
        List<PersonVm> chosen = All.Where(p => _selection.Contains(p.Id)).ToList();
        foreach (PersonVm target in chosen)
        {
            var item = new MenuFlyoutItem { Text = target.MenuName };
            item.Click += (_, _) => Merge(target.Id, chosen.Select(p => p.Id));
            menu.Items.Add(item);
        }
    }

    /// <summary>A line under the grid that clears itself after a few seconds.</summary>
    internal void Note(string text)
    {
        int g = ++_noteGeneration;
        _note.Text = text;
        _note.Visibility = Visibility.Visible;
        _ = Task.Delay(5000).ContinueWith(_ => _root.DispatcherQueue.TryEnqueue(() =>
        {
            if (g == _noteGeneration) _note.Visibility = Visibility.Collapsed;
        }), TaskScheduler.Default);
    }

    // ---- a card ------------------------------------------------------------------------

    /// <summary>
    /// One person: the cover (click, hover ⋯, drag out, drop onto, selection
    /// badge, the opening veil), the name (a label until clicked, then a
    /// field), and "N photos". Its state is redrawn in place, never rebuilt,
    /// so a count moving while faces stream in does not re-crop the cover.
    /// </summary>
    private sealed class Card
    {
        private readonly PeopleGrid _grid;
        private readonly Look _look;
        public readonly Grid Root;
        private readonly ContentControl _coverHost;
        private readonly Grid _cover;
        private readonly Ellipse _ring;
        private readonly Grid _veil;
        private readonly Button _more;
        private readonly Grid _check;
        private readonly ContentControl _nameHost;
        private readonly TextBlock _name;
        private readonly TextBlock _count;
        private MediaViewer.Shared.FakeInput? _editor;
        private FrameworkElement? _crop;
        private ulong _cropFace;
        private bool _hover;
        private bool _dropTarget;
        public PersonVm Person { get; private set; }

        public Card(PeopleGrid grid, PersonVm person)
        {
            _grid = grid;
            _look = grid._look;
            Person = person;
            Root = new Grid { Width = CardWidth, Height = CardHeight };
            var column = new StackPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Center };
            Root.Children.Add(column);

            // A transparent background: the whole square hit-tests (hover, drop).
            _cover = new Grid { Width = Cover, Height = Cover, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            _ring = new Ellipse { Width = Cover, Height = Cover, IsHitTestVisible = false };
            _cover.Children.Add(_ring);
            // Their photos are being opened: a veil and an activity bar (no ProgressRing here).
            _veil = new Grid { Visibility = Visibility.Collapsed, IsHitTestVisible = false };
            _veil.Children.Add(new Ellipse { Width = Cover, Height = Cover, Fill = new SolidColorBrush(Windows.UI.Color.FromArgb(90, 0, 0, 0)) });
            var bar = new MediaViewer.Shared.FlatBar(
                new SolidColorBrush(Windows.UI.Color.FromArgb(70, 255, 255, 255)),
                new SolidColorBrush(Microsoft.UI.Colors.White)) { IsIndeterminate = true };
            bar.Root.Width = 36;
            bar.Root.HorizontalAlignment = HorizontalAlignment.Center;
            bar.Root.VerticalAlignment = VerticalAlignment.Center;
            _veil.Children.Add(bar.Root);
            _cover.Children.Add(_veil);
            // Correcting a person's faces: here on hover, and in the context menu.
            _more = Badge("⋯", _look[AddonColour.Title], _look.Tint(AddonColour.Canvas, 230));
            _more.HorizontalAlignment = HorizontalAlignment.Right;
            _more.VerticalAlignment = VerticalAlignment.Top;
            _more.Visibility = Visibility.Collapsed;
            _more.Click += (_, _) => _grid.OpenFaces(Person);
            ToolTipService.SetToolTip(_more, "Faces… — rename, merge, or remove faces that are someone else");
            AutomationProperties.SetName(_more, "Faces");
            _cover.Children.Add(_more);
            _check = new Grid
            {
                Width = 22,
                Height = 22,
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Bottom,
                Visibility = Visibility.Collapsed,
                IsHitTestVisible = false,
            };
            _check.Children.Add(new Ellipse { Fill = _look[AddonColour.Accent], Stroke = new SolidColorBrush(Microsoft.UI.Colors.White), StrokeThickness = 1.5 });
            TextBlock tick = _look.Text("✓", 12, AddonColour.Title, wrap: false);
            tick.Foreground = new SolidColorBrush(Microsoft.UI.Colors.White);
            tick.HorizontalAlignment = HorizontalAlignment.Center;
            tick.VerticalAlignment = VerticalAlignment.Center;
            _check.Children.Add(tick);
            _cover.Children.Add(_check);

            _coverHost = new ContentControl
            {
                Content = _cover,
                IsTabStop = true,
                UseSystemFocusVisuals = true,
                HorizontalAlignment = HorizontalAlignment.Center,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                VerticalContentAlignment = VerticalAlignment.Stretch,
                AllowDrop = true,
                CanDrag = true,
                CornerRadius = new CornerRadius(Cover / 2),
            };
            ToolTipService.SetToolTip(_coverHost, "Click to see this person's photos. Drag onto another person to merge them.");
            AutomationProperties.SetHelpText(_coverHost, "Shows their photos in the gallery");
            _coverHost.PointerEntered += (_, _) => SetHover(true);
            _coverHost.PointerExited += (_, _) => SetHover(false);
            _coverHost.PointerCanceled += (_, _) => SetHover(false);
            _coverHost.Tapped += (_, e) =>
            {
                if (IsInside(e.OriginalSource as DependencyObject, _more)) return;
                _grid.Tap(Person);
                e.Handled = true;
            };
            _coverHost.KeyDown += (_, e) =>
            {
                if (e.Key is not (VirtualKey.Enter or VirtualKey.Space)) return;
                _grid.Tap(Person);
                e.Handled = true;
            };
            _coverHost.DragStarting += (_, e) =>
            {
                // A selected person carries the selection with it.
                IEnumerable<ulong> ids = _grid._selection.Contains(Person.Id) ? _grid._selection : new[] { Person.Id };
                e.Data.SetText(DragPrefix + string.Join(",", ids));
                e.Data.RequestedOperation = DataPackageOperation.Move;
            };
            _coverHost.DragOver += (_, e) =>
            {
                if (!e.DataView.Contains(StandardDataFormats.Text)) return;
                e.AcceptedOperation = DataPackageOperation.Move;
                e.DragUIOverride.Caption = $"Merge into {Person.MenuName}";
                e.DragUIOverride.IsGlyphVisible = false;
                SetDropTarget(true);
            };
            _coverHost.DragLeave += (_, _) => SetDropTarget(false);
            _coverHost.Drop += async (_, e) =>
            {
                SetDropTarget(false);
                if (!e.DataView.Contains(StandardDataFormats.Text)) return;
                DragOperationDeferral deferral = e.GetDeferral();
                try
                {
                    string text = await e.DataView.GetTextAsync();
                    if (!text.StartsWith(DragPrefix, StringComparison.Ordinal)) return;
                    var ids = text[DragPrefix.Length..].Split(',')
                        .Select(t => ulong.TryParse(t, out ulong v) ? v : 0)
                        .Where(v => v != 0 && v != Person.Id)
                        .ToList();
                    if (ids.Count > 0) _grid.Merge(Person.Id, ids);
                }
                catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { }
                finally
                {
                    deferral.Complete();
                }
            };
            var menu = new MenuFlyout();
            menu.Opening += (_, _) => FillMenu(menu);
            _coverHost.ContextFlyout = menu;
            column.Children.Add(_coverHost);

            _name = _look.Text("", 12, AddonColour.Title, wrap: false);
            _name.TextTrimming = TextTrimming.CharacterEllipsis;
            _name.TextAlignment = TextAlignment.Center;
            _name.HorizontalAlignment = HorizontalAlignment.Stretch;
            _nameHost = new ContentControl
            {
                Content = _name,
                IsTabStop = true,
                UseSystemFocusVisuals = true,
                Width = CardWidth,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
            };
            _nameHost.Tapped += (_, e) =>
            {
                if (_editor is not null) return;
                BeginEditing();
                e.Handled = true;
            };
            _nameHost.KeyDown += (_, e) =>
            {
                if (_editor is not null || e.Key != VirtualKey.Enter) return;
                BeginEditing();
                e.Handled = true;
            };
            column.Children.Add(_nameHost);

            _count = _look.Text("", 11, AddonColour.Body, wrap: false);
            _count.HorizontalAlignment = HorizontalAlignment.Center;
            column.Children.Add(_count);
            Update();
        }

        private Button Badge(string glyph, Brush ink, Brush fill)
        {
            var b = new Button
            {
                Content = glyph,
                Width = 24,
                Height = 24,
                Padding = new Thickness(0, -3, 0, 0),
                FontSize = 14,
                CornerRadius = new CornerRadius(12),
                BorderThickness = new Thickness(0),
                Background = fill,
                Foreground = ink,
                HorizontalContentAlignment = HorizontalAlignment.Center,
                VerticalContentAlignment = VerticalAlignment.Center,
            };
            return b;
        }

        private static bool IsInside(DependencyObject? d, DependencyObject ancestor)
        {
            for (; d is not null; d = VisualTreeHelper.GetParent(d))
            {
                if (ReferenceEquals(d, ancestor)) return true;
            }
            return false;
        }

        public void Bind(PersonVm person)
        {
            Person = person;
            Update();
        }

        public void Update()
        {
            PersonVm p = Person;
            bool selected = _grid._selection.Contains(p.Id);
            bool opening = _grid._opening == p.Id;
            if (_crop is null || _cropFace != p.CoverFace)
            {
                if (_crop is not null) _cover.Children.Remove(_crop);
                _crop = _grid._chrome.Crops.Crop(p.CoverFace, p.Box, Cover, FaceCrops.Monogram(p.Name), _grid._root.DispatcherQueue);
                _cropFace = p.CoverFace;
                _cover.Children.Insert(0, _crop);
            }
            bool lit = selected || _hover || _dropTarget;
            _ring.StrokeThickness = lit ? 2.5 : 1;
            _ring.Stroke = selected || _dropTarget ? _look[AddonColour.Accent]
                : _hover ? _look.LiveTint(AddonColour.Accent, 180)
                : _look[AddonColour.Hairline];
            _veil.Visibility = opening ? Visibility.Visible : Visibility.Collapsed;
            _more.Visibility = _hover && !selected ? Visibility.Visible : Visibility.Collapsed;
            _check.Visibility = selected ? Visibility.Visible : Visibility.Collapsed;
            float scale = (_hover || _dropTarget) && _look.Motion ? 1.06f : 1f;
            if (_coverHost.Scale.X != scale)
            {
                _coverHost.CenterPoint = new Vector3((float)(Cover / 2), (float)(Cover / 2), 0);
                _coverHost.ScaleTransition ??= new Vector3Transition { Duration = TimeSpan.FromMilliseconds(150) };
                _coverHost.Scale = new Vector3(scale, scale, 1);
            }
            AutomationProperties.SetName(_coverHost, p.Name.Length > 0 ? p.Name : "Unnamed person");
            AutomationProperties.SetItemStatus(_coverHost, selected ? "Selected" : "");
            if (_editor is null)
            {
                _name.Text = p.Name.Length > 0 ? p.Name : "Add a name";
                _name.Foreground = p.Name.Length > 0 ? _look[AddonColour.Title] : _look[AddonColour.Body];
                ToolTipService.SetToolTip(_nameHost, p.Name.Length > 0 ? "Click to rename" : "Click to name this person");
                AutomationProperties.SetName(_nameHost, p.Name.Length > 0 ? $"Name, {p.Name}" : "Add a name");
            }
            _count.Text = p.Photos;
        }

        private void SetHover(bool on)
        {
            if (_hover == on) return;
            _hover = on;
            Update();
        }

        private void SetDropTarget(bool on)
        {
            if (_dropTarget == on) return;
            _dropTarget = on;
            Update();
        }

        private void FillMenu(MenuFlyout menu)
        {
            menu.Items.Clear();
            var photos = new MenuFlyoutItem { Text = "Show photos" };
            photos.Click += (_, _) => _grid.ShowPhotos(Person);
            menu.Items.Add(photos);
            var faces = new MenuFlyoutItem { Text = "Faces…" };
            faces.Click += (_, _) => _grid.OpenFaces(Person);
            menu.Items.Add(faces);
            var merge = new MenuFlyoutSubItem { Text = "Merge into…" };
            foreach (PersonVm other in _grid.All.Where(o => o.Id != Person.Id))
            {
                var item = new MenuFlyoutItem { Text = other.MenuName };
                ulong into = other.Id, from = Person.Id;
                item.Click += (_, _) => _grid.Merge(into, new[] { from });
                merge.Items.Add(item);
            }
            merge.IsEnabled = merge.Items.Count > 0;
            menu.Items.Add(merge);
        }

        // The name: a label until clicked (or Enter on it), then a field that
        // saves on Enter or when focus leaves; Esc puts the label back.
        private void BeginEditing()
        {
            var editor = new MediaViewer.Shared.FakeInput(_look.Input(12), "Add a name", CardWidth);
            AutomationProperties.SetName(editor, "Name");
            editor.SetText(Person.Name);
            editor.Submitted += () => Commit(save: true, refocus: true);
            editor.LostFocus += (_, _) => Commit(save: true, refocus: false);
            editor.KeyDown += (_, e) =>
            {
                if (e.Key != VirtualKey.Escape) return;
                e.Handled = true;  // Esc ends the edit, not Settings
                Commit(save: false, refocus: true);
            };
            _editor = editor;
            _nameHost.Content = editor;
            _nameHost.IsTabStop = false;
            editor.Loaded += (_, _) =>
            {
                editor.Focus(FocusState.Keyboard);
                editor.SelectAll();
            };
        }

        private void Commit(bool save, bool refocus)
        {
            if (_editor is not MediaViewer.Shared.FakeInput editor) return;
            _editor = null;
            string name = editor.Text.Trim();
            _nameHost.Content = _name;
            _nameHost.IsTabStop = true;
            if (save && name != Person.Name) _grid.Rename(Person.Id, name);
            Update();
            if (refocus) _nameHost.Focus(FocusState.Keyboard);
        }
    }

    private sealed class CardFactory(PeopleGrid grid) : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            PersonVm person = ((Slot)args.Data).P;
            var card = new Card(grid, person);
            grid._cards[person.Id] = card;
            return card.Root;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            // Not pooled: a card is cheap, and its crop belongs to its person.
            foreach ((ulong id, Card card) in grid._cards)
            {
                if (!ReferenceEquals(card.Root, args.Element)) continue;
                grid._cards.Remove(id);
                break;
            }
        }
    }
}
