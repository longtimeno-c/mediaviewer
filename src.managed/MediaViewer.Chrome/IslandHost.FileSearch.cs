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
/// File search (docs/design/16 "File search", 2026-09-28): a find-by-name field over
/// the gallery, part of the base app. No add-on and no index: it filters the
/// folder already listed by file and folder name as you type. The path bar's
/// search icon and Ctrl+F open it whenever Local search is not installed (or
/// failed to load); with Local search they open its panel.
/// </summary>
/// <remarks>
/// It is the 2026-09-27 gallery search bar's Names mode, shown only while
/// open: the field appears over the grid when asked for and goes when Esc
/// empties it, the gallery closes, or another folder opens. The filter is a
/// view over the gallery only: the folder model, the filmstrip and next /
/// previous in the viewer still walk the whole folder. The grid binds
/// <see cref="Items"/> itself until a filter is typed, then a subset of the
/// same view models, edited in place when a few change so the tiles that stay
/// keep their elements. Names are folded (case and diacritics) once per
/// listing on a worker, so a keystroke is one ordinal substring pass. No
/// TextBox: FakeInput, as in Settings (0xC000027B in these islands).
/// </remarks>
public static partial class IslandHost
{
    private const string FileSearchPrompt = "Find files by name in this folder";

    // OpenFileSearch's answers, and the native entry's (chrome_host.h
    // gallery_search_action::open).
    private const int FileSearchTook = 0;
    private const int FileSearchDeclined = 1;
    private const int FileSearchNeedsGallery = 2;

    // The field is on screen over the grid.
    private static bool _gsOpen;
    // Asked for while the gallery was hidden: it opens when the grid is shown.
    private static bool _gsOpenPending;
    private static string _gsText = "";

    private static FrameworkElement? _gsBar;
    private static FakeInput? _gsField;
    private static Border? _gsPill;
    private static TextBlock? _gsCount;
    private static StackPanel? _gsNote;
    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? _gsDebounce;

    // The filter: on while the field has text. _gsPos maps a folder index to
    // its position in _gsItems (-1: filtered out).
    private static bool _gsFiltering;
    private static ObservableCollection<FolderItemVm> _gsItems = new();
    private static ObservableCollection<FolderCardVm> _gsFolders = new();
    private static int[] _gsPos = Array.Empty<int>();
    private static bool _gsBound;   // the gallery's repeaters are realised

    // Folded names: generation of the listing, and of the folded copy.
    private static int _gsListing;
    private static int _gsFolded = -1;
    private static bool _gsFolding;
    private static Action? _gsAfterFold;

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
    /// Native: file search. In: { int32 action; int32 arg }. Action 0 (Ctrl+F
    /// while the Local search command is not there): see <see cref="OpenFileSearch"/>;
    /// 2 back asks native to show the gallery, and the field opens with it.
    /// 1: Esc while a text control has the keyboard: the field clears, then
    /// closes. 2: Left / Right (arg -1 / +1) while a filter is on: step within
    /// the matches. Returns 0 when it was taken, 1 when native should do what
    /// it did without file search.
    /// </summary>
    public static int GallerySearch(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            int action = Marshal.ReadInt32(arg);
            int value = Marshal.ReadInt32(arg, 4);
            return action switch
            {
                0 => OpenFileSearch(),
                1 => FileSearchEscape() ? FileSearchTook : FileSearchDeclined,
                2 => FileSearchStep(value) ? FileSearchTook : FileSearchDeclined,
                _ => FileSearchDeclined,
            };
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>
    /// Ctrl+F or the path bar's icon. Local search loaded: its panel. Starting
    /// at launch: its panel once it attaches (IslandHost.LocalSearch.cs).
    /// Otherwise file search: the field opens and takes the keyboard, or, with
    /// the gallery hidden, <see cref="FileSearchNeedsGallery"/> and it opens
    /// once the grid is shown. Nothing listed: declined.
    /// </summary>
    private static int OpenFileSearch()
    {
        if (SearchChrome is ISearchChrome search)
        {
            search.RunCommand(SearchCommand.Open);
            return FileSearchTook;
        }
        if (AiStarting())
        {
            _searchOpenPending = true;
            return FileSearchTook;
        }
        if (Items.Count == 0 && Folders.Count == 0) return FileSearchDeclined;
        if (!_galleryVisible || _gsField is null)
        {
            _gsOpenPending = true;
            return FileSearchNeedsGallery;
        }
        ShowFileSearch();
        return FileSearchTook;
    }

    private static void ShowFileSearch()
    {
        if (_gallery is null || _gsField is null || _gsBar is null) return;
        _gsOpenPending = false;
        _gsOpen = true;
        _gsBar.Visibility = Visibility.Visible;
        // The island takes the Win32 focus first; the field is its first stop.
        _gallery.NavigateFocus(new XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason.First));
        _gsField.Focus(FocusState.Keyboard);
        _gsField.SelectAll();
        StartFold();
    }

    // Esc on an empty field, the close button, another folder: the field goes
    // and the grid shows the whole folder again.
    private static void CloseFileSearch()
    {
        _gsOpenPending = false;
        if (!_gsOpen) return;
        _gsOpen = false;
        _gsDebounce?.Stop();
        _gsText = "";
        _gsField?.SetText("");
        if (_gsBar is not null) _gsBar.Visibility = Visibility.Collapsed;
        ShowFileSearchNote(null);
        ApplyNamesFilter();
    }

    private static bool FileSearchEscape()
    {
        if (!_galleryVisible || _gsField is null || _gsField.FocusState == FocusState.Unfocused) return false;
        if (_gsField.Text.Length > 0)
        {
            _gsField.SetText("");
            OnFileSearchText();
            return true;
        }
        CloseFileSearch();
        return FocusGalleryGrid();
    }

    private static bool FileSearchStep(int direction)
    {
        if (!_galleryVisible || !_gsFiltering || _gsItems.Count == 0 || _folderCursor >= 0) return false;
        int at = GalleryPos(_selectedIndex);
        int next = at < 0 ? 0 : Math.Clamp(at + Math.Sign(direction), 0, _gsItems.Count - 1);
        if (next != at) Send(Command.SelectItem, _gsItems[next].Index);
        GalleryScrollTo(_gsItems[next].Index);
        return true;
    }

    // Down / Return in the field, and the closing Esc: the keyboard goes to
    // the grid, on the first tile in view (or the selection when it is in view).
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

    // ---- the field -----------------------------------------------------------------

    private static FrameworkElement BuildGallerySearchBar()
    {
        _gsField = new FakeInput(FileSearchPrompt, bare: true);
        _gsField.SetText(_gsText);
        AutomationProperties.SetName(_gsField, "Find files by name");
        _gsField.Changed += OnFileSearchText;
        _gsField.Submitted += () => FocusGalleryGrid();
        _gsField.MoveDown += () => FocusGalleryGrid();
        _gsField.GotFocus += (_, _) =>
        {
            PaintFileSearchPill();
            StartFold();
        };
        _gsField.LostFocus += (_, _) => PaintFileSearchPill();

        _gsCount = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = UiFontSize - 3,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(6, 0, 4, 0),
        };
        Button close = EditButton("", () =>
        {
            CloseFileSearch();
            FocusGalleryGrid();
        }, tip: "Close file search (Esc)");
        close.Content = new FontIcon { Glyph = "", FontSize = 11, Foreground = Brush(Body) };
        close.Padding = new Thickness(6, 4, 6, 4);
        AutomationProperties.SetName(close, "Close file search");

        var inner = new Grid { ColumnSpacing = 2 };
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        inner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var glyph = new FontIcon
        {
            Glyph = "",
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
        Grid.SetColumn(close, 3);
        inner.Children.Add(close);
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

        var bar = new Grid
        {
            Padding = new Thickness(12, 8, 12, 2),
            // Built with every gallery; shown only while file search is open.
            Visibility = _gsOpen ? Visibility.Visible : Visibility.Collapsed,
        };
        bar.Children.Add(_gsPill);
        _gsBar = bar;

        PaintFileSearchPill();
        UpdateFileSearchCount();
        return bar;
    }

    // The "nothing matches" line, in the grid's place.
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

    private static void PaintFileSearchPill()
    {
        if (_gsPill is null || _gsField is null) return;
        bool focused = _gsField.FocusState != FocusState.Unfocused;
        // Same thickness either way: the grid under it must not move a pixel.
        _gsPill.BorderBrush = Brush(focused ? Title : Hairline);
    }

    // The gallery closed (its tree is rebuilt on every show): so does file
    // search, filter and all.
    private static void GallerySearchHidden()
    {
        _gsDebounce?.Stop();
        _gsBound = false;
        _gsOpen = false;
        _gsText = "";
        _gsFiltering = false;
        _gsPos = Array.Empty<int>();
        _gsItems = new ObservableCollection<FolderItemVm>();
        _gsFolders = new ObservableCollection<FolderCardVm>();
        _gsBar = null;
        _gsField = null;
        _gsPill = null;
        _gsCount = null;
        _gsNote = null;
    }

    private static void UpdateFileSearchCount()
    {
        if (_gsCount is null) return;
        int total = Items.Count + Folders.Count;
        _gsCount.Text = _gsFiltering ? $"{_gsItems.Count + _gsFolders.Count:N0} of {total:N0}" : "";
        _gsCount.Visibility = _gsCount.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static void ShowFileSearchNote(string? title)
    {
        if (_gsNote is null) return;
        _gsNote.Children.Clear();
        if (title is null)
        {
            _gsNote.Visibility = Visibility.Collapsed;
            return;
        }
        _gsNote.Children.Add(new TextBlock
        {
            Text = title,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Body),
            TextWrapping = TextWrapping.Wrap,
            TextAlignment = TextAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Center,
        });
        _gsNote.Visibility = Visibility.Visible;
    }

    // ---- typing ------------------------------------------------------------------------

    private static void OnFileSearchText()
    {
        if (_gsField is null) return;
        _gsText = _gsField.Text;
        _gsDebounce ??= CreateFileSearchDebounce();
        _gsDebounce?.Stop();
        // Clearing is immediate: the whole folder again.
        if (_gsText.Trim().Length == 0 || _gsDebounce is null)
        {
            ApplyNamesFilter();
            return;
        }
        // ~70 ms: a pass over folded strings.
        _gsDebounce.Interval = TimeSpan.FromMilliseconds(70);
        _gsDebounce.Start();
    }

    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? CreateFileSearchDebounce()
    {
        Microsoft.UI.Dispatching.DispatcherQueueTimer? timer = _dispatcher?.DispatcherQueue.CreateTimer();
        if (timer is null) return null;
        timer.IsRepeating = false;
        timer.Tick += (_, _) =>
        {
            try { ApplyNamesFilter(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        };
        return timer;
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
    // Started when the field opens or gets the keyboard, so it is done before
    // the first debounce fires.
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
                else if (listing != _gsListing && _gsOpen) StartFold();
            });
        });
    }

    private static void ApplyNamesFilter()
    {
        string q = _gsOpen ? FoldName(_gsText.Trim()) : "";
        if (q.Length == 0)
        {
            EndNamesFilter();
            ShowFileSearchNote(null);
            UpdateFileSearchCount();
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
        UpdateFileSearchCount();

        if (items.Count == 0 && folders.Count == 0)
        {
            ShowFileSearchNote($"No files named “{_gsText.Trim()}” in this folder.");
            return;
        }
        ShowFileSearchNote(null);
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
        _ = list;
        ++_gsListing;
        // Another folder closes file search; the same one re-filters.
        if (newFolder) CloseFileSearch();
        else if (_gsFiltering && _galleryVisible) ApplyNamesFilter();
        UpdateFileSearchCount();
    }

    /// <summary>RealiseGallery: the repeaters are live; file search asked for
    /// while the gallery was hidden opens now.</summary>
    private static void OnGalleryRealised()
    {
        _gsBound = true;
        BindGallerySources();
        if (_gsOpenPending) ShowFileSearch();
    }
}
