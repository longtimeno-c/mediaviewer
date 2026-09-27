// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;

namespace MediaViewer.Chrome;

/// <summary>
/// The gallery search bar (plan/17 "Gallery search bar", plan/16 `/`): a slim
/// field pinned over the grid. Names mode, always there, filters the grid by
/// file and folder name as you type. With Local search loaded the same field
/// can search contents (the pack's text search, opened as a result list the
/// way the search panel's Ctrl+Enter does), and the pack vends the index
/// control for the folder on screen.
/// </summary>
/// <remarks>
/// The filter is a view over the gallery only: the folder model, the
/// filmstrip and next / previous in the viewer still walk the whole folder.
/// The grid binds <see cref="Items"/> itself until a filter is typed, then a
/// subset of the same view models, edited in place when a few change so the
/// tiles that stay keep their elements. Names are folded (case and
/// diacritics) once per listing on a worker, so a keystroke is one ordinal
/// substring pass. Everything AI is behind <see cref="IGallerySearchChrome"/>;
/// a pack without it leaves the bar at names only. No TextBox: FakeInput, as
/// in Settings (0xC000027B in these islands).
/// </remarks>
public static partial class IslandHost
{
    private enum GallerySearchMode { Names, Contents }

    private const string GallerySearchPrompt = "Search this folder";

    // Kept for the session, like the tile size.
    private static GallerySearchMode _gsMode = GallerySearchMode.Names;
    // The field's text: kept while the gallery closes and reopens on the same
    // folder; a new folder clears it.
    private static string _gsText = "";

    private static FakeInput? _gsField;
    private static Border? _gsPill;
    private static TextBlock? _gsCount;
    private static Button? _gsClear;
    private static FrameworkElement? _gsModes;
    private static Button? _gsNamesButton;
    private static Button? _gsContentsButton;
    private static Button? _gsBack;
    private static Border? _gsIndexSlot;
    private static StackPanel? _gsNote;
    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? _gsDebounce;

    // The names filter: on while names mode has text. _gsPos maps a folder
    // index to its position in _gsItems (-1: filtered out).
    private static bool _gsFiltering;
    private static ObservableCollection<FolderItemVm> _gsItems = new();
    private static ObservableCollection<FolderCardVm> _gsFolders = new();
    private static int[] _gsPos = Array.Empty<int>();
    private static bool _gsBound;   // the gallery's repeaters are realised
    private static bool _gsStale;   // the listing changed while the gallery was hidden

    // Folded names: generation of the listing, and of the folded copy.
    private static int _gsListing;
    private static int _gsFolded = -1;
    private static bool _gsFolding;
    private static Action? _gsAfterFold;

    // Contents: the latest query (older answers are dropped), and whether the
    // result list on screen is the bar's.
    private static int _gsQuery;
    private static bool _gsPendingList;
    private static bool _gsListFromBar;

    private static IGallerySearchChrome? GallerySearchChrome => AiSlot.Chrome as IGallerySearchChrome;

    private static IList<FolderItemVm> GalleryItems => _gsFiltering ? _gsItems : Items;
    private static IList<FolderCardVm> GalleryFolders => _gsFiltering ? _gsFolders : Folders;

    /// <summary>A folder index's position in the grid; -1 when the filter hides it.</summary>
    private static int GalleryPos(int index)
    {
        if (!_gsFiltering) return index;
        return (uint)index < (uint)_gsPos.Length ? _gsPos[index] : -1;
    }

    private static int GalleryFolderPos(int index)
    {
        if (!_gsFiltering) return index;
        for (int i = 0; i < _gsFolders.Count; ++i)
        {
            if (_gsFolders[i].Index == index) return i;
        }
        return -1;
    }

    // ---- native entry --------------------------------------------------------------

    /// <summary>
    /// Native: the gallery search bar. In: { int32 action; int32 arg }.
    /// Action 0 (`/`, gallery_search): focus the field and select its text.
    /// 1: Esc while a text control has the keyboard: the field clears, then
    /// hands the keyboard to the grid. 2: Left / Right (arg -1 / +1) while a
    /// filter is on: step within the matches. Returns 0 when the bar took it,
    /// 1 when native should do what it did before the bar.
    /// </summary>
    public static int GallerySearch(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            int action = Marshal.ReadInt32(arg);
            int value = Marshal.ReadInt32(arg, 4);
            bool took = action switch
            {
                0 => FocusGallerySearch(),
                1 => GallerySearchEscape(),
                2 => GallerySearchStep(value),
                _ => false,
            };
            return took ? 0 : 1;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static bool FocusGallerySearch()
    {
        if (!_galleryVisible || _gallery is null || _gsField is null) return false;
        // The island takes the Win32 focus first; the field is its first stop.
        _gallery.NavigateFocus(new XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason.First));
        _gsField.Focus(FocusState.Keyboard);
        _gsField.SelectAll();
        StartFold();
        return true;
    }

    private static bool GallerySearchEscape()
    {
        if (!_galleryVisible || _gsField is null || _gsField.FocusState == FocusState.Unfocused) return false;
        if (_gsField.Text.Length > 0)
        {
            _gsField.SetText("");
            OnGallerySearchText();
            return true;
        }
        return FocusGalleryGrid();
    }

    private static bool GallerySearchStep(int direction)
    {
        if (!_galleryVisible || !_gsFiltering || _gsItems.Count == 0 || _folderCursor >= 0) return false;
        int at = GalleryPos(_selectedIndex);
        int next = at < 0 ? 0 : Math.Clamp(at + Math.Sign(direction), 0, _gsItems.Count - 1);
        if (next != at) Send(Command.SelectItem, _gsItems[next].Index);
        GalleryScrollTo(_gsItems[next].Index);
        return true;
    }

    // Down / Return in the field, and the second Esc: the keyboard goes to the
    // grid, on the first tile in view (or the selection when it is in view).
    private static bool FocusGalleryGrid()
    {
        if (_galleryScroll is null) return false;
        IList<FolderItemVm> view = GalleryItems;
        if (view.Count > 0 && _folderCursor < 0)
        {
            int first = FirstVisibleTile();
            int at = GalleryPos(_selectedIndex);
            int columns = GalleryColumns;
            int rows = Math.Max(1, (int)(_galleryScroll.ViewportHeight / GalleryRowStride));
            bool inView = at >= first && at < first + rows * columns;
            if (!inView && first < view.Count) Send(Command.SelectItem, view[first].Index);
        }
        return _galleryScroll.Focus(FocusState.Keyboard);
    }

    // The grid's first position at or below the top of the viewport.
    private static int FirstVisibleTile()
    {
        if (_galleryScroll is null || _galleryRepeater is null || _galleryStack is null) return 0;
        try
        {
            double top = _galleryRepeater.TransformToVisual(_galleryStack).TransformPoint(default).Y;
            double y = _galleryScroll.VerticalOffset - top;
            if (y <= 0) return 0;
            int row = (int)Math.Ceiling(y / GalleryRowStride);
            return Math.Min(row * GalleryColumns, Math.Max(0, GalleryItems.Count - 1));
        }
        catch (Exception ex) when (ex is ArgumentException or COMException)
        {
            return 0;
        }
    }

    // ---- the bar -------------------------------------------------------------------

    private static FrameworkElement BuildGallerySearchBar()
    {
        _gsField = new FakeInput(GallerySearchPrompt, bare: true);
        _gsField.SetText(_gsText);
        AutomationProperties.SetName(_gsField, "Search in gallery");
        ToolTipService.SetToolTip(_gsField, "Search this folder by name (/)");
        _gsField.Changed += OnGallerySearchText;
        _gsField.Submitted += () => FocusGalleryGrid();
        _gsField.MoveDown += () => FocusGalleryGrid();
        _gsField.GotFocus += (_, _) =>
        {
            PaintGallerySearchPill();
            StartFold();
        };
        _gsField.LostFocus += (_, _) => PaintGallerySearchPill();

        _gsCount = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = UiFontSize - 3,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(6, 0, 4, 0),
        };
        _gsClear = EditButton("", () =>
        {
            _gsField?.SetText("");
            OnGallerySearchText();
        }, tip: "Clear the search");
        _gsClear.Content = new FontIcon { Glyph = "\uE711", FontSize = 11, Foreground = Brush(Body) };
        _gsClear.Padding = new Thickness(6, 4, 6, 4);
        AutomationProperties.SetName(_gsClear, "Clear the search");

        _gsNamesButton = EditButton("Names", () => SetGallerySearchMode(GallerySearchMode.Names),
                                    tip: "Match file and folder names");
        _gsContentsButton = EditButton("Contents", () => SetGallerySearchMode(GallerySearchMode.Contents),
                                       tip: "Search what is in the photos and videos (Local search)");
        var modes = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, Margin = new Thickness(4, 0, 0, 0) };
        modes.Children.Add(_gsNamesButton);
        modes.Children.Add(_gsContentsButton);
        _gsModes = modes;

        var inner = new Grid { ColumnSpacing = 2 };
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var glyph = new FontIcon
        {
            Glyph = "\uE721",
            FontSize = 13,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(2, 0, 2, 0),
        };
        inner.Children.Add(glyph);
        Grid.SetColumn(_gsField, 1);
        inner.Children.Add(_gsField);
        Grid.SetColumn(_gsCount, 2);
        inner.Children.Add(_gsCount);
        Grid.SetColumn(_gsClear, 3);
        inner.Children.Add(_gsClear);
        Grid.SetColumn(modes, 4);
        inner.Children.Add(modes);
        _gsPill = new Border
        {
            Child = inner,
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(6),
            Padding = new Thickness(8, 1, 3, 1),
            MinHeight = 32,
            MaxWidth = 640,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };

        _gsBack = EditButton("Back to folder", () => Send(Command.FolderUp), tip: "Back to the folder (Esc)");
        _gsBack.Margin = new Thickness(8, 0, 0, 0);
        _gsIndexSlot = new Border { Margin = new Thickness(8, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };

        var bar = new Grid { Padding = new Thickness(12, 8, 12, 2) };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.Children.Add(_gsPill);
        Grid.SetColumn(_gsBack, 1);
        bar.Children.Add(_gsBack);
        Grid.SetColumn(_gsIndexSlot, 2);
        bar.Children.Add(_gsIndexSlot);

        FillGalleryIndexSlot();
        PaintGallerySearchPill();
        UpdateGallerySearchBar();
        return bar;
    }

    // The empty and "not indexed" states, in the grid's place.
    private static StackPanel BuildGallerySearchNote()
    {
        _gsNote = new StackPanel
        {
            Spacing = 10,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 60, 0, 0),
            MaxWidth = 520,
            Visibility = Visibility.Collapsed,
        };
        return _gsNote;
    }

    private static void PaintGallerySearchPill()
    {
        if (_gsPill is null || _gsField is null) return;
        bool focused = _gsField.FocusState != FocusState.Unfocused;
        // Same thickness either way: the grid under it must not move a pixel.
        _gsPill.BorderBrush = Brush(focused ? Title : Hairline);
    }

    private static void GallerySearchHidden()
    {
        _gsDebounce?.Stop();
        _gsBound = false;
        if (_gsIndexSlot is not null) _gsIndexSlot.Child = null;
        if (GallerySearchChrome is IGallerySearchChrome pack)
        {
            try { pack.SetGalleryFolder(null); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        }
        _gsField = null;
        _gsPill = null;
        _gsCount = null;
        _gsClear = null;
        _gsModes = null;
        _gsNamesButton = null;
        _gsContentsButton = null;
        _gsBack = null;
        _gsIndexSlot = null;
        _gsNote = null;
    }

    // The pack's control for the folder on screen, or nothing without it.
    private static void FillGalleryIndexSlot()
    {
        if (_gsIndexSlot is null) return;
        _gsIndexSlot.Child = null;
        if (GallerySearchChrome is IGallerySearchChrome pack)
        {
            try
            {
                pack.SetGalleryFolder(_openedFolder.Length > 0 ? _openedFolder : null);
                _gsIndexSlot.Child = pack.BuildGalleryIndexControl() as UIElement;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                _gsIndexSlot.Child = null;
            }
        }
        _gsIndexSlot.Visibility = _gsIndexSlot.Child is null ? Visibility.Collapsed : Visibility.Visible;
    }

    // Toggle, count, clear, back: what the state says, nothing rebuilt.
    private static void UpdateGallerySearchBar()
    {
        bool pack = GallerySearchChrome is not null;
        if (_gsModes is not null) _gsModes.Visibility = pack ? Visibility.Visible : Visibility.Collapsed;
        if (_gsNamesButton is not null) SetSelected(_gsNamesButton, _gsMode == GallerySearchMode.Names);
        if (_gsContentsButton is not null) SetSelected(_gsContentsButton, _gsMode == GallerySearchMode.Contents);
        if (_gsClear is not null) _gsClear.Visibility = _gsText.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (_gsBack is not null) _gsBack.Visibility = _listOpen ? Visibility.Visible : Visibility.Collapsed;
        if (_gsCount is not null)
        {
            int total = Items.Count + Folders.Count;
            _gsCount.Text = _gsFiltering ? $"{_gsItems.Count + _gsFolders.Count:N0} of {total:N0}" : "";
            _gsCount.Visibility = _gsCount.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private static void ShowGallerySearchNote(string? title, bool offerContents = false, bool offerIndex = false,
                                              bool offerBrowse = false, bool offerRetry = false)
    {
        if (_gsNote is null) return;
        _gsNote.Children.Clear();
        if (title is null)
        {
            _gsNote.Visibility = Visibility.Collapsed;
            return;
        }
        var line = new TextBlock
        {
            Text = title,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Body),
            TextWrapping = TextWrapping.Wrap,
            TextAlignment = TextAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        _gsNote.Children.Add(line);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        if (offerContents && GallerySearchChrome is not null)
        {
            buttons.Children.Add(SettingsButton("Search contents instead",
                () => SetGallerySearchMode(GallerySearchMode.Contents)));
        }
        if (offerIndex && GallerySearchChrome is IGallerySearchChrome pack && _openedFolder.Length > 0)
        {
            string folder = _openedFolder;
            void Index(bool recursive)
            {
                try { pack.IndexGalleryFolder(folder, recursive); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
                ShowGallerySearchNote("Indexing this folder in the background: keep browsing while it works, " +
                                      "its progress is in the command bar. Results appear as the index grows.",
                                      offerBrowse: true, offerRetry: true);
            }
            buttons.Children.Add(SettingsButton("Index this folder", () => Index(false)));
            buttons.Children.Add(SettingsButton("Index this folder and subfolders", () => Index(true)));
        }
        if (offerRetry)
        {
            buttons.Children.Add(SettingsButton("Search again", RunContentsQuery));
        }
        if (offerBrowse)
        {
            // Never a dead end: the files are one click away, indexing or not.
            buttons.Children.Add(SettingsButton("Show all files", () =>
            {
                _gsField?.SetText("");
                OnGallerySearchText();
            }));
        }
        if (buttons.Children.Count > 0) _gsNote.Children.Add(buttons);
        _gsNote.Visibility = Visibility.Visible;
    }

    // ---- typing ------------------------------------------------------------------------

    private static void OnGallerySearchText()
    {
        if (_gsField is null) return;
        _gsText = _gsField.Text;
        UpdateGallerySearchBar();
        _gsDebounce ??= CreateGallerySearchDebounce();
        _gsDebounce?.Stop();
        if (_gsText.Trim().Length == 0)
        {
            // Clearing is immediate: the whole folder, or back from the bar's list.
            ++_gsQuery;
            ShowGallerySearchNote(null);
            if (_gsMode == GallerySearchMode.Contents && _gsListFromBar && _listOpen) Send(Command.FolderUp);
            ApplyNamesFilter();
            return;
        }
        if (_gsDebounce is null)
        {
            RunGallerySearch();
            return;
        }
        // ~70 ms for names (a pass over folded strings), ~300 ms for contents (a search).
        _gsDebounce.Interval = TimeSpan.FromMilliseconds(_gsMode == GallerySearchMode.Names ? 70 : 300);
        _gsDebounce.Start();
    }

    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? CreateGallerySearchDebounce()
    {
        Microsoft.UI.Dispatching.DispatcherQueueTimer? timer = _dispatcher?.DispatcherQueue.CreateTimer();
        if (timer is null) return null;
        timer.IsRepeating = false;
        timer.Tick += (_, _) =>
        {
            try { RunGallerySearch(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        };
        return timer;
    }

    private static void RunGallerySearch()
    {
        if (_gsMode == GallerySearchMode.Names) ApplyNamesFilter();
        else RunContentsQuery();
    }

    private static void SetGallerySearchMode(GallerySearchMode mode)
    {
        if (mode == GallerySearchMode.Contents && GallerySearchChrome is null) mode = GallerySearchMode.Names;
        _gsDebounce?.Stop();
        ++_gsQuery;
        ShowGallerySearchNote(null);
        bool changed = mode != _gsMode;
        _gsMode = mode;
        if (mode == GallerySearchMode.Contents)
        {
            EndNamesFilter();
            if (_gsText.Trim().Length > 0) RunContentsQuery();
        }
        else if (changed)
        {
            // Back to the folder the bar's list came from; its listing re-filters.
            if (_gsListFromBar && _listOpen) Send(Command.FolderUp);
            ApplyNamesFilter();
        }
        UpdateGallerySearchBar();
    }

    // ---- names --------------------------------------------------------------------------

    /// <summary>Case- and diacritic-insensitive key for a name or a query.</summary>
    internal static string FoldName(string s)
    {
        bool ascii = true;
        foreach (char c in s)
        {
            if (c > 0x7F) { ascii = false; break; }
        }
        if (ascii) return s.ToLowerInvariant();
        try
        {
            string d = s.Normalize(NormalizationForm.FormD);
            var b = new StringBuilder(d.Length);
            foreach (char c in d)
            {
                if (CharUnicodeInfo.GetUnicodeCategory(c) != UnicodeCategory.NonSpacingMark) b.Append(c);
            }
            return b.ToString().Normalize(NormalizationForm.FormC).ToLowerInvariant();
        }
        catch (ArgumentException)
        {
            // An unpaired surrogate: still case-insensitive.
            return s.ToLowerInvariant();
        }
    }

    // Folding a 10,000-item listing is a few ms: a worker's, not a frame's.
    // Started when the field gets the keyboard, so it is done before the
    // first debounce fires.
    private static void StartFold() => FoldThen(null);

    private static void FoldThen(Action? then)
    {
        if (_gsFolded == _gsListing)
        {
            then?.Invoke();
            return;
        }
        if (then is not null) _gsAfterFold = then;
        if (_gsFolding) return;
        _gsFolding = true;
        int listing = _gsListing;
        FolderItemVm[] vms = Items.ToArray();
        _ = Task.Run(() =>
        {
            var folded = new string[vms.Length];
            for (int i = 0; i < vms.Length; ++i) folded[i] = FoldName(vms[i].Name);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _gsFolding = false;
                if (listing == _gsListing)
                {
                    for (int i = 0; i < vms.Length; ++i) vms[i].Folded = folded[i];
                    _gsFolded = listing;
                }
                Action? next = _gsAfterFold;
                _gsAfterFold = null;
                // A newer listing: fold it too (the next call starts that).
                if (next is not null) FoldThen(next);
                else if (listing != _gsListing && _gsText.Length > 0) StartFold();
            });
        });
    }

    private static void ApplyNamesFilter()
    {
        _gsStale = false;
        string q = _gsMode == GallerySearchMode.Names ? FoldName(_gsText.Trim()) : "";
        if (q.Length == 0)
        {
            EndNamesFilter();
            ShowGallerySearchNote(null);
            UpdateGallerySearchBar();
            return;
        }
        if (_gsFolded != _gsListing)
        {
            FoldThen(ApplyNamesFilter);
            return;
        }

        // One ordinal pass over the folded names; no thumbnail is touched.
        int n = Items.Count;
        var items = new List<FolderItemVm>();
        int[] pos = new int[n];
        for (int i = 0; i < n; ++i)
        {
            FolderItemVm vm = Items[i];
            string key = vm.Folded ??= FoldName(vm.Name);
            if (key.Contains(q, StringComparison.Ordinal))
            {
                pos[i] = items.Count;
                items.Add(vm);
            }
            else
            {
                pos[i] = -1;
            }
        }
        var folders = new List<FolderCardVm>();
        foreach (FolderCardVm card in Folders)
        {
            if (card.FoldedName.Contains(q, StringComparison.Ordinal)) folders.Add(card);
        }

        bool first = !_gsFiltering;
        _gsFiltering = true;
        _gsPos = pos;
        if (first || !SyncView(_gsItems, items, v => v.Index)) _gsItems = new ObservableCollection<FolderItemVm>(items);
        if (first || !SyncView(_gsFolders, folders, c => c.Index)) _gsFolders = new ObservableCollection<FolderCardVm>(folders);
        BindGallerySources();
        UpdateGallerySections();
        UpdateGallerySearchBar();

        if (items.Count == 0 && folders.Count == 0)
        {
            string shown = _gsText.Trim();
            ShowGallerySearchNote($"No files named “{shown}” in this folder.", offerContents: true);
            return;
        }
        ShowGallerySearchNote(null);
        // The selection stays on a match: a hidden one hands over to the first.
        if (_folderCursor < 0 && items.Count > 0 && GalleryPos(_selectedIndex) < 0)
        {
            Send(Command.SelectItem, items[0].Index);
        }
        else if (_folderCursor < 0)
        {
            GalleryScrollTo(_selectedIndex);
        }
    }

    private static void EndNamesFilter()
    {
        if (!_gsFiltering) return;
        _gsFiltering = false;
        _gsPos = Array.Empty<int>();
        BindGallerySources();
        _gsItems = new ObservableCollection<FolderItemVm>();
        _gsFolders = new ObservableCollection<FolderCardVm>();
        UpdateGallerySections();
        if (_folderCursor < 0) GalleryScrollTo(_selectedIndex);
    }

    /// <summary>
    /// Edits <paramref name="view"/> into <paramref name="target"/> in place
    /// (both in folder order), so the tiles that stay keep their elements.
    /// False, untouched, when more than a screenful would change: a fresh
    /// collection is cheaper for the repeater than hundreds of events.
    /// </summary>
    private static bool SyncView<T>(ObservableCollection<T> view, List<T> target, Func<T, int> order) where T : class
    {
        const int Budget = 64;
        int edits = 0;
        for (int i = 0, j = 0; i < view.Count || j < target.Count;)
        {
            if (i < view.Count && j < target.Count && ReferenceEquals(view[i], target[j])) { ++i; ++j; continue; }
            if (++edits > Budget) return false;
            if (j >= target.Count || (i < view.Count && order(view[i]) <= order(target[j]))) ++i;
            else ++j;
        }
        for (int i = 0, j = 0; i < view.Count || j < target.Count;)
        {
            if (i < view.Count && j < target.Count && ReferenceEquals(view[i], target[j])) { ++i; ++j; continue; }
            if (j >= target.Count || (i < view.Count && order(view[i]) <= order(target[j])))
            {
                view.RemoveAt(i);
            }
            else
            {
                view.Insert(i, target[j]);
                ++i;
                ++j;
            }
        }
        return true;
    }

    // The repeaters follow the view; before RealiseGallery they bind nothing
    // (ItemsSource on a repeater outside a XamlRoot is the set_ItemsSource AV).
    private static void BindGallerySources()
    {
        if (!_galleryVisible || !_gsBound) return;
        BindSource(_galleryRepeater, GalleryItems);
        BindSource(_folderRepeater, GalleryFolders);
        BindSource(_folderChipRepeater, GalleryFolders);
    }

    private static void BindSource(ItemsRepeater? repeater, object source)
    {
        if (repeater is null) return;
        try
        {
            if (!ReferenceEquals(repeater.ItemsSource, source)) repeater.ItemsSource = source;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    /// <summary>ReloadItems (IslandHost.Filmstrip.cs): a listing landed.</summary>
    private static void OnGalleryListing(bool list, bool newFolder)
    {
        ++_gsListing;
        if (list)
        {
            _gsListFromBar = _gsPendingList;
            _gsPendingList = false;
        }
        else
        {
            _gsListFromBar = false;
        }
        if (newFolder)
        {
            // A different folder starts with an empty field.
            _gsText = "";
            _gsField?.SetText("");
            ++_gsQuery;
            _gsDebounce?.Stop();
            ShowGallerySearchNote(null);
            // The index control follows the folder on screen, only while it is.
            if (_galleryVisible && GallerySearchChrome is IGallerySearchChrome pack)
            {
                try { pack.SetGalleryFolder(_openedFolder.Length > 0 ? _openedFolder : null); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
        }
        if (_gsFiltering || (_gsMode == GallerySearchMode.Names && _gsText.Trim().Length > 0))
        {
            if (_galleryVisible) ApplyNamesFilter();
            else _gsStale = true;
        }
        UpdateGallerySearchBar();
    }

    /// <summary>RealiseGallery: the repeaters are live; a filter kept while hidden
    /// catches up with the listing and shows its note again.</summary>
    private static void OnGalleryRealised()
    {
        _gsBound = true;
        if (_gsStale || _gsFiltering) ApplyNamesFilter();
        BindGallerySources();
        UpdateGallerySearchBar();
    }

    // ---- contents -----------------------------------------------------------------------

    private static void RunContentsQuery()
    {
        string text = _gsText.Trim();
        if (text.Length == 0) return;
        if (GallerySearchChrome is not IGallerySearchChrome pack)
        {
            SetGallerySearchMode(GallerySearchMode.Names);
            return;
        }
        if (_openedFolder.Length == 0)
        {
            ShowGallerySearchNote("Open a folder to search its contents.");
            return;
        }
        int query = ++_gsQuery;
        if (_gsCount is not null)
        {
            _gsCount.Text = "Searching…";
            _gsCount.Visibility = Visibility.Visible;
        }
        try
        {
            pack.GalleryQuery(text, _openedFolder, outcome =>
            {
                try { OnContentsOutcome(query, text, outcome); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            });
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            OnContentsOutcome(query, text, GallerySearchOutcome.Failed);
        }
    }

    private static void OnContentsOutcome(int query, string text, GallerySearchOutcome outcome)
    {
        if (query != _gsQuery || _gsMode != GallerySearchMode.Contents) return;
        UpdateGallerySearchBar();  // drops "Searching…"
        switch (outcome)
        {
            case GallerySearchOutcome.Opened:
                // The list lands through the drain (ReloadItems); it is the bar's.
                _gsPendingList = true;
                ShowGallerySearchNote(null);
                break;
            case GallerySearchOutcome.NothingFound:
                // The pack's own rule: nothing it would show. Back to the folder
                // rather than leave an older query's results under this one.
                if (_gsListFromBar && _listOpen) Send(Command.FolderUp);
                ShowGallerySearchNote($"Nothing in this folder matches “{text}”.", offerBrowse: true);
                break;
            case GallerySearchOutcome.NotIndexed:
                ShowGallerySearchNote("This folder is not indexed yet. Indexing runs in the background: " +
                                      "you can keep browsing while it works.", offerIndex: true);
                break;
            default:
                ShowGallerySearchNote("Search is not available right now. Local search may still be getting ready.");
                break;
        }
    }

    // ---- the pack comes and goes (IslandHost.LocalSearch.cs) ----------------------------

    private static void OnGallerySearchPackChanged()
    {
        if (GallerySearchChrome is null && _gsMode == GallerySearchMode.Contents)
        {
            _gsMode = GallerySearchMode.Names;
            ++_gsQuery;
            ShowGallerySearchNote(null);
            if (_galleryVisible) ApplyNamesFilter();
        }
        if (_galleryVisible) FillGalleryIndexSlot();
        UpdateGallerySearchBar();
    }
}
