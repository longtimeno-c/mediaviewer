// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;
using Windows.UI;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 9: the metadata pane (right) and the folder tree (left). Two islands that
/// float over the canvas like the gallery does; neither insets the swapchain, so
/// opening one never refits the photo or touches the present path (plan/12
/// 2026-09-24, the same call the macOS host made).
/// </summary>
/// <remarks>
/// Neither island reads a file. The pane shows three text tables native pushes in
/// from a record it already holds (<c>meta::summary_table</c> and friends) and
/// re-renders only when a new set arrives. The tree lists a folder's subfolders
/// on a worker task through the ABI's one directory read, never on the UI thread
/// (CLAUDE.md rule 1). Keys stay with the canvas: only the search box takes text.
/// </remarks>
public static partial class IslandHost
{
    internal const int PanelArgsSize = 24;
    internal const int MetaDataArgsSize = 40;
    internal const int MetaEditArgsSize = 24;

    private static DesktopWindowXamlSource? _metaPane;
    private static DesktopWindowXamlSource? _tree;
    private static bool _metaPaneVisible;
    private static bool _treeVisible;

    private const ChromeColour PanelBg = ChromeColour.PanelBg;

    // ---- geometry: native owns the maths, the island only moves --------------

    private static void MoveAt(DesktopWindowXamlSource? source, int x, int y, int width, int height)
    {
        if (source?.SiteBridge is null) return;
        source.SiteBridge.MoveAndResize(new RectInt32(
            Math.Max(x, 0), Math.Max(y, 0), Math.Max(width, 1), Math.Max(height, 1)));
    }

    public static int AttachPanels(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < FilmstripArgsSize) return unchecked((int)0x80070057);
            ChromeFilmstripArgs args = Marshal.PtrToStructure<ChromeFilmstripArgs>(arg);
            IntPtr parent = checked((IntPtr)args.ParentHwnd);
            if (parent == IntPtr.Zero) return unchecked((int)0x80070057);

            EnsureApp();
            _context = checked((IntPtr)args.Context);
            if (args.OnCommand != 0)
            {
                _onCommand = Marshal.GetDelegateForFunctionPointer<NativeCommand>(
                    checked((IntPtr)args.OnCommand));
            }

            DisposeSource(ref _metaPane);
            DisposeSource(ref _tree);
            DisposeSource(ref _adjustPane);
            DisposeSource(ref _jobsPane);
            EnsureFocusHook();
            _metaPane = new DesktopWindowXamlSource();
            _metaPane.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            _metaPane.TakeFocusRequested += OnTakeFocusRequested;
            _tree = new DesktopWindowXamlSource();
            _tree.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            _tree.TakeFocusRequested += OnTakeFocusRequested;
            // PR 11: the adjust pane, on the same right edge as the metadata pane.
            _adjustPane = new DesktopWindowXamlSource();
            _adjustPane.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            _adjustPane.TakeFocusRequested += OnTakeFocusRequested;
            // PR 13 / 14: the Jobs pane, the same right edge again (one at a time).
            _jobsPane = new DesktopWindowXamlSource();
            _jobsPane.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            _jobsPane.TakeFocusRequested += OnTakeFocusRequested;
            // Parked below the client area with no content until first shown: a
            // default full-client island would flash over the canvas.
            MoveAt(_metaPane, 0, args.ClientHeight, 1, 1);
            MoveAt(_tree, 0, args.ClientHeight, 1, 1);
            MoveAt(_adjustPane, 0, args.ClientHeight, 1, 1);
            MoveAt(_jobsPane, 0, args.ClientHeight, 1, 1);
            _metaPaneVisible = false;
            _treeVisible = false;
            _adjustPaneVisible = false;
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("chrome AttachPanels: {0}", ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int DetachPanels(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            _metaPaneVisible = false;
            _treeVisible = false;
            _adjustPaneVisible = false;
            _jobsPaneVisible = false;
            _jobsTimer?.Stop();
            DropMetaUi();
            DropJobsUi();
            DropTreeUi();
            DropAdjustUi();
            DisposeSource(ref _metaPane);
            DisposeSource(ref _tree);
            DisposeSource(ref _adjustPane);
            DisposeSource(ref _jobsPane);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ShowMetaPane(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _metaPane, BuildMetaPane,
        onShown: () => _metaPaneVisible = true,
        onHidden: () =>
        {
            _metaPaneVisible = false;
            DropMetaUi();
        });

    public static int ShowFolderTree(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _tree, BuildTree,
        onShown: () => _treeVisible = true,
        onHidden: () =>
        {
            _treeVisible = false;
            DropTreeUi();
        });

    // Show is "move the bridge and build the tree", hide is "drop the tree and
    // park the bridge below the client area" — the gallery's rule, for the same
    // reason: a bridge left at 0,0 would eat the command bar's clicks.
    private static int ShowPanel(IntPtr arg, int sizeBytes, DesktopWindowXamlSource? source,
                                 Func<UIElement> build, Action onShown, Action onHidden)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < PanelArgsSize) return unchecked((int)0x80070057);
            if (source is null) return 1;
            ChromePanelArgs args = Marshal.PtrToStructure<ChromePanelArgs>(arg);
            if (args.Visible == 0)
            {
                onHidden();
                source.Content = null;
                MoveAt(source, args.X, args.Y, args.Width, args.Height);
                return 0;
            }
            MoveAt(source, args.X, args.Y, args.Width, args.Height);
            if (source.Content is null)
            {
                source.Content = build();
                onShown();
            }
            // `I` / Ctrl+Shift+E focus the pane (plan/16): the first focusable
            // element takes it, arrows walk, and Esc returns to the canvas.
            if (args.Focus != 0)
            {
                source.NavigateFocus(new XamlSourceFocusNavigationRequest(
                    XamlSourceFocusNavigationReason.First));
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ---- small builders -------------------------------------------------------

    private static TextBlock Text(string text, ChromeColour color, double size = UiFontSize,
                                  bool bold = false, int maxLines = 0)
    {
        var t = new TextBlock
        {
            Text = text,
            Foreground = Brush(color),
            FontFamily = UiFont,
            FontSize = size,
            TextWrapping = TextWrapping.Wrap,
        };
        if (bold) t.FontWeight = FontWeights.SemiBold;
        if (maxLines > 0) t.MaxLines = maxLines;
        return t;
    }

    private static Button CloseButton(int command)
    {
        var b = new Button
        {
            Content = new TextBlock { Text = "×", FontSize = 18, Foreground = Brush(Body) },
            Background = Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = new Thickness(8, 0, 8, 2),
            IsTabStop = false,
            AllowFocusOnInteraction = false,
        };
        b.Click += (_, _) => Send(command);
        return b;
    }

    private static Grid PanelShell(string title, int closeCommand, out Grid body)
    {
        var root = new Grid
        {
            Background = Brush(PanelBg),
            RequestedTheme = ElementTheme.Default,
            // No XY focus navigation: FocusManager.TryMoveFocus fail-fasts in these
            // islands (0xC000027B), so the arrows are handled explicitly per pane.
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var header = new Grid { Padding = new Thickness(14, 10, 6, 6) };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.Children.Add(Text(title, Title, UiFontSize, bold: true));
        Button close = CloseButton(closeCommand);
        Grid.SetColumn(close, 1);
        header.Children.Add(close);
        root.Children.Add(header);
        body = new Grid();
        Grid.SetRow(body, 1);
        root.Children.Add(body);
        return root;
    }

    // ==== metadata pane ========================================================

    private enum MetaTab { Summary, Tags, Streams }

    private sealed record TagRow(string Group, string Label, string Value, string Raw);

    private static List<(string Label, string Value)> _metaSummary = new();
    private static List<TagRow> _metaTags = new();
    private static List<string> _metaStreamLines = new();
    private static string _metaLast = "";
    private static FakeInput? _metaSearch;
    private static ScrollViewer? _metaScroll;
    private static bool _metaLoading;
    private static bool _metaIsClip;
    private static MetaTab _metaTab = MetaTab.Summary;
    private static string _metaQuery = "";

    private static Border? _metaContent;
    private static TextBlock? _metaLoadingText;
    private static StackPanel? _metaTabs;

    private static void DropMetaUi()
    {
        _metaLast = "";
        _metaContent = null;
        _metaLoadingText = null;
        _metaTabs = null;
        _metaStarsRow = null;
        _metaRejected = null;
        _metaCommentBox = null;
        _metaRevert = null;
        MetaStarButtons.Clear();
    }

    private static UIElement BuildMetaPane()
    {
        Grid root = PanelShell("Metadata", Command.MetadataPane, out Grid body);
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        // PR 12: rating, comment and Revert sit above the tabs, so Ctrl+I and the
        // stars are there whichever tab is open.
        body.Children.Add(BuildMetaEdit());

        var bar = new Grid { Padding = new Thickness(14, 0, 14, 8) };
        Grid.SetRow(bar, 1);
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _metaTabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        bar.Children.Add(_metaTabs);
        _metaLoadingText = Text("", Body);
        Grid.SetColumn(_metaLoadingText, 1);
        bar.Children.Add(_metaLoadingText);
        body.Children.Add(bar);

        _metaContent = new Border { BorderBrush = Brush(Hairline), BorderThickness = new Thickness(0, 1, 0, 0) };
        Grid.SetRow(_metaContent, 2);
        body.Children.Add(_metaContent);

        RenderMeta();
        return root;
    }

    private static readonly List<Button> MetaTabButtons = new();

    private static void RenderMeta(bool refocusTab = false)
    {
        if (_metaContent is null || _metaTabs is null) return;
        // The record arrives after the pane is shown and this rebuilds the tab bar:
        // if the keyboard was on a tab, it must still be on one afterwards, or the
        // arrows go nowhere until the next Tab.
        foreach (Button old in MetaTabButtons)
        {
            if (old.FocusState != FocusState.Unfocused) refocusTab = true;
        }
        // A still has no streams: fall back if the tab it was on went away.
        if (!_metaIsClip && _metaTab == MetaTab.Streams) _metaTab = MetaTab.Summary;
        if (_metaLoadingText is not null) _metaLoadingText.Text = _metaLoading ? "reading…" : "";

        _metaTabs.Children.Clear();
        MetaTabButtons.Clear();
        AddMetaTab("Summary", MetaTab.Summary);
        AddMetaTab("All tags", MetaTab.Tags);
        if (_metaIsClip) AddMetaTab("Streams", MetaTab.Streams);

        _metaContent.Child = _metaTab switch
        {
            MetaTab.Tags => BuildTagTree(),
            MetaTab.Streams => BuildStreams(),
            _ => BuildSummary(),
        };
        // Choosing a tab from the keyboard rebuilds the bar; keep the keyboard on it.
        if (refocusTab)
        {
            int at = _metaTab == MetaTab.Summary ? 0 : _metaTab == MetaTab.Tags ? 1 : 2;
            if (at < MetaTabButtons.Count) MetaTabButtons[at].Focus(FocusState.Keyboard);
        }
    }

    private static void AddMetaTab(string label, MetaTab tab)
    {
        bool on = _metaTab == tab;
        var b = new Button
        {
            Content = new TextBlock
            {
                Text = label, FontFamily = UiFont, FontSize = UiFontSize,
                Foreground = Brush(on ? Title : Body),
            },
            Background = on ? Brush(Hairline) : Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = new Thickness(10, 3, 10, 3),
            AllowFocusOnInteraction = false,
        };
        int index = MetaTabButtons.Count;
        b.Click += (_, _) =>
        {
            _metaTab = tab;
            RenderMeta(refocusTab: true);
        };
        // Left / Right walk the tabs, Down drops into the tab's content. Explicit,
        // because directional focus alone did not cross the bar reliably.
        b.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Right && index + 1 < MetaTabButtons.Count)
            {
                MetaTabButtons[index + 1].Focus(FocusState.Keyboard);
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Left && index > 0)
            {
                MetaTabButtons[index - 1].Focus(FocusState.Keyboard);
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Down && _metaTab == MetaTab.Tags)
            {
                _metaSearch?.Focus(FocusState.Keyboard);
                e.Handled = true;
            }
        };
        MetaTabButtons.Add(b);
        _metaTabs!.Children.Add(b);
    }

    // A field the file does not have shows as a dash; nothing here is an error.
    private static UIElement BuildSummary()
    {
        if (_metaSummary.Count == 0)
        {
            return new Border
            {
                Padding = new Thickness(14),
                Child = Text(_metaLoading ? "Reading…" : "Nothing selected", Body),
            };
        }
        var rows = new StackPanel { Spacing = 6, Padding = new Thickness(14, 10, 14, 14) };
        foreach ((string label, string value) in _metaSummary)
        {
            // PR 12: rating and comment have their own controls above the tabs.
            if (label is "Rating" or "Comment") continue;
            var row = new Grid();
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.Children.Add(Text(label, Body));
            TextBlock v = Text(value.Length == 0 ? "—" : value, value.Length == 0 ? Hairline : Title);
            Grid.SetColumn(v, 1);
            row.Children.Add(v);
            rows.Children.Add(row);
        }
        return new ScrollViewer { Content = rows, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    }

    // The full tree: every EXIF / IPTC / XMP / container tag, grouped, with a
    // search box over label, value and the untranslated key.
    private static UIElement BuildTagTree()
    {
        var host = new Grid();
        host.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        host.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        // Not a TextBox: that control fail-fasts in these islands (see FakeInput).
        var search = new FakeInput("Search tags and values") { Margin = new Thickness(10) };
        search.SetText(_metaQuery);
        _metaSearch = search;
        _metaScroll = null;
        host.Children.Add(search);

        // A plain scrolling stack, not a ListView: clearing and refilling a ListView of
        // elements from a key event fail-fasts in these islands. A tag list is at most
        // a few thousand rows, and the search narrows it, so the rows are capped.
        var list = new StackPanel { Padding = new Thickness(10, 0, 10, 10) };
        var scroll = new ScrollViewer
        {
            Content = list,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        Grid.SetRow(scroll, 1);
        host.Children.Add(scroll);
        _metaScroll = scroll;
        search.MoveDown += () => scroll.ChangeView(null, scroll.VerticalOffset + 90, null);
        search.PreviewKeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Up) return;
            if (scroll.VerticalOffset > 0) scroll.ChangeView(null, Math.Max(0, scroll.VerticalOffset - 90), null);
            else if (MetaTabButtons.Count > 1) MetaTabButtons[1].Focus(FocusState.Keyboard);
            e.Handled = true;
        };

        const int MaxRows = 400;
        void Fill()
        {
            list.Children.Clear();
            if (_metaTags.Count == 0)
            {
                list.Children.Add(Text(_metaLoading ? "Reading…" : "No metadata in this file", Body));
                return;
            }
            string needle = _metaQuery.Trim();
            string? group = null;
            int shown = 0;
            int matched = 0;
            foreach (TagRow t in _metaTags)
            {
                if (needle.Length > 0 &&
                    !t.Label.Contains(needle, StringComparison.OrdinalIgnoreCase) &&
                    !t.Value.Contains(needle, StringComparison.OrdinalIgnoreCase) &&
                    !t.Raw.Contains(needle, StringComparison.OrdinalIgnoreCase)) continue;
                matched++;
                if (shown >= MaxRows) continue;
                if (t.Group != group)
                {
                    group = t.Group;
                    var header = Text(group, Body, UiFontSize, bold: true);
                    header.Margin = new Thickness(0, 10, 0, 2);
                    list.Children.Add(header);
                }
                var cell = new StackPanel { Margin = new Thickness(0, 2, 0, 2) };
                cell.Children.Add(Text(t.Label, Title));
                cell.Children.Add(Text(t.Value.Length == 0 ? "—" : t.Value, Body, UiFontSize - 2, maxLines: 3));
                // The untranslated origin is always one hover away (plan/06).
                ToolTipService.SetToolTip(cell, t.Raw);
                list.Children.Add(cell);
                shown++;
            }
            if (matched == 0) list.Children.Add(Text("No tag matches", Body));
            else if (matched > shown)
            {
                list.Children.Add(Text($"{matched - shown} more: type to narrow the search", Body));
            }
        }

        search.Changed += () =>
        {
            _metaQuery = search.Text;
            Fill();
        };
        Fill();
        return host;
    }

    private static UIElement BuildStreams()
    {
        var col = new StackPanel { Spacing = 12, Padding = new Thickness(14, 10, 14, 14) };
        StackPanel? current = null;
        bool chapters = false;
        foreach (string line in _metaStreamLines)
        {
            string[] f = line.Split('\t');
            switch (f[0])
            {
                case "S" when f.Length >= 4:
                    current = new StackPanel { Spacing = 4 };
                    current.Children.Add(Text($"#{f[1]}  {Capitalise(f[2])}  —  {f[3]}", Title, UiFontSize, bold: true));
                    col.Children.Add(current);
                    break;
                case "F" when f.Length >= 3 && current is not null:
                {
                    var row = new Grid();
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(120) });
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    row.Children.Add(Text(f[1], Body));
                    TextBlock v = Text(f[2], Title);
                    Grid.SetColumn(v, 1);
                    row.Children.Add(v);
                    current.Children.Add(row);
                    break;
                }
                case "C" when f.Length >= 3:
                {
                    if (!chapters)
                    {
                        chapters = true;
                        col.Children.Add(Text("Chapters", Title, UiFontSize, bold: true));
                    }
                    long.TryParse(f[1], out long ms);
                    var s = TimeSpan.FromMilliseconds(ms);
                    string stamp = $"{(int)s.TotalHours}:{s.Minutes:00}:{s.Seconds:00}";
                    col.Children.Add(Text($"{stamp}   {(f[2].Length == 0 ? "Chapter" : f[2])}", Body));
                    break;
                }
            }
        }
        if (_metaStreamLines.Count == 0)
        {
            col.Children.Add(Text(_metaLoading ? "Reading…" : "No streams", Body));
        }
        return new ScrollViewer { Content = col, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    }

    private static string Capitalise(string s) => s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];

    /// <summary>
    /// Native pushes the record's three tables (and whether a read is still in
    /// flight) whenever the record changes. In: ChromeMetaArgs.
    /// </summary>
    public static int SetMetaData(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < MetaDataArgsSize) return unchecked((int)0x80070057);
            ChromeMetaArgs a = Marshal.PtrToStructure<ChromeMetaArgs>(arg);
            static string Utf8(ulong ptr, int len) =>
                ptr == 0 || len <= 0 ? "" : Marshal.PtrToStringUTF8(checked((IntPtr)ptr), len) ?? "";
            string summary = Utf8(a.Summary, a.SummaryLen);
            string props = Utf8(a.Properties, a.PropertiesLen);
            string streams = Utf8(a.Streams, a.StreamsLen);

            // Native re-pushes on every layout; identical data must not rebuild the
            // pane, which would drop the keyboard focus sitting on one of its rows.
            string fingerprint = string.Concat(a.Loading.ToString(), "\u0001", summary, "\u0001", props, "\u0001", streams);
            if (fingerprint == _metaLast) return 0;
            _metaLast = fingerprint;

            _metaSummary = summary.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                .Select(l => l.Split('\t'))
                .Select(f => (f[0], f.Length > 1 ? f[1] : ""))
                .ToList();
            _metaTags = props.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                .Select(l => l.Split('\t'))
                .Where(f => f.Length >= 5)
                .Select(f => new TagRow(f[1], f[2], f[3], f[4]))
                .ToList();
            _metaStreamLines = streams.Split('\n', StringSplitOptions.RemoveEmptyEntries).ToList();
            _metaIsClip = _metaStreamLines.Count > 0;
            _metaLoading = a.Loading != 0;
            if (_metaPaneVisible) RenderMeta();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ==== PR 12: rating, comment, revert =======================================
    //
    // Native owns the truth and pushes it (SetMetaEdit) whenever it may have
    // moved; a change still in its write queue already counts, so a star clicked
    // or a key pressed shows at once. The stars post the rating keys' command
    // ids, the comment is parked for native to pull (TakeTreePath) and Revert
    // is a notification. Nothing here reads or writes a file.

    private static int _metaRating;
    private static string _metaComment = "";
    private static bool _metaCanEdit;
    private static bool _metaCanRevert;

    private static StackPanel? _metaStarsRow;
    private static TextBlock? _metaRejected;
    private static FakeInput? _metaCommentBox;
    private static Button? _metaRevert;
    private static readonly List<Button> MetaStarButtons = new();

    private const ChromeColour StarOn = ChromeColour.StarOn;

    private static UIElement BuildMetaEdit()
    {
        var col = new StackPanel { Spacing = 8, Padding = new Thickness(14, 0, 14, 10) };

        var rating = new Grid();
        rating.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
        rating.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        TextBlock ratingLabel = Text("Rating", Body);
        ratingLabel.VerticalAlignment = VerticalAlignment.Center;
        rating.Children.Add(ratingLabel);
        _metaStarsRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        MetaStarButtons.Clear();
        for (int n = 1; n <= 5; n++)
        {
            int stars = n;
            var b = new Button
            {
                Content = new TextBlock { FontSize = 18 },
                Background = Brush(Colors.Transparent),
                BorderThickness = new Thickness(0),
                Padding = new Thickness(4, 0, 4, 2),
                AllowFocusOnInteraction = false,
            };
            // The star that is already the rating clears it (the Mac pane's rule).
            b.Click += (_, _) => Send(Command.SetRating0 + (_metaRating == stars ? 0 : stars));
            // Left / Right walk the stars, Down drops to the comment. Explicit:
            // XY focus navigation fail-fasts in these islands.
            int index = n - 1;
            b.KeyDown += (_, e) =>
            {
                if (e.Key == Windows.System.VirtualKey.Right && index + 1 < MetaStarButtons.Count)
                {
                    MetaStarButtons[index + 1].Focus(FocusState.Keyboard);
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Left && index > 0)
                {
                    MetaStarButtons[index - 1].Focus(FocusState.Keyboard);
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Down)
                {
                    _metaCommentBox?.Focus(FocusState.Keyboard);
                    e.Handled = true;
                }
            };
            ToolTipService.SetToolTip(b, n == 1 ? "1 star" : $"{n} stars");
            MetaStarButtons.Add(b);
            _metaStarsRow.Children.Add(b);
        }
        Grid.SetColumn(_metaStarsRow, 1);
        rating.Children.Add(_metaStarsRow);
        _metaRejected = Text("Rejected", Body);
        _metaRejected.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(_metaRejected, 1);
        rating.Children.Add(_metaRejected);
        col.Children.Add(rating);

        var comment = new Grid();
        comment.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
        comment.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        TextBlock commentLabel = Text("Comment", Body);
        commentLabel.VerticalAlignment = VerticalAlignment.Center;
        comment.Children.Add(commentLabel);
        // Not a TextBox: that control fail-fasts in these islands (see FakeInput).
        // Return saves and gives the keyboard back to the canvas; Esc is native's
        // (it pushes DropDraft first, so leaving the field then saves nothing).
        var box = new FakeInput("Add a comment");
        box.SetText(_metaComment);
        box.Submitted += () => CommitMetaComment(box);
        box.LostFocus += (_, _) =>
        {
            try
            {
                if (box.Text != _metaComment) CommitMetaComment(box);
            }
            catch (Exception ex)
            {
                // Never let an exception out of a XAML event: that is a fail-fast.
                System.Diagnostics.Debug.WriteLine(ex);
            }
        };
        box.MoveDown += () => _metaRevert?.Focus(FocusState.Keyboard);
        box.PreviewKeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Up || MetaStarButtons.Count == 0) return;
            MetaStarButtons[Math.Clamp(_metaRating, 1, MetaStarButtons.Count) - 1].Focus(FocusState.Keyboard);
            e.Handled = true;
        };
        _metaCommentBox = box;
        Grid.SetColumn(box, 1);
        comment.Children.Add(box);
        col.Children.Add(comment);

        _metaRevert = new Button
        {
            Content = Text("Revert metadata", Body),
            HorizontalAlignment = HorizontalAlignment.Right,
            AllowFocusOnInteraction = false,
        };
        ToolTipService.SetToolTip(_metaRevert,
            "Put the rating, comment and orientation back to how this file was before this session's first change");
        _metaRevert.Click += (_, _) => Send(Command.MetaRevert);
        _metaRevert.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Up)
            {
                _metaCommentBox?.Focus(FocusState.Keyboard);
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Down && MetaTabButtons.Count > 0)
            {
                MetaTabButtons[0].Focus(FocusState.Keyboard);
                e.Handled = true;
            }
        };
        col.Children.Add(_metaRevert);

        RenderMetaEdit();
        return col;
    }

    private static void CommitMetaComment(FakeInput box)
    {
        _treePending = box.Text;
        Send(Command.MetaComment);
    }

    // In place, never a rebuild: the keyboard may be on a star or in the field.
    private static void RenderMetaEdit()
    {
        if (_metaStarsRow is null || _metaRejected is null) return;
        bool rejected = _metaRating < 0;
        _metaStarsRow.Visibility = rejected ? Visibility.Collapsed : Visibility.Visible;
        _metaRejected.Visibility = rejected ? Visibility.Visible : Visibility.Collapsed;
        for (int i = 0; i < MetaStarButtons.Count; i++)
        {
            bool on = i < _metaRating;
            if (MetaStarButtons[i].Content is TextBlock t)
            {
                t.Text = on ? "★" : "☆";
                t.Foreground = Brush(on ? StarOn : Body);
            }
            MetaStarButtons[i].IsEnabled = _metaCanEdit;
        }
        if (_metaCommentBox is not null) _metaCommentBox.IsEnabled = _metaCanEdit;
        if (_metaRevert is not null) _metaRevert.IsEnabled = _metaCanRevert;
    }

    /// <summary>
    /// Native pushes the rating, the comment and what may be done with them
    /// whenever any of them may have moved (every pane push). In: ChromeMetaEditArgs.
    /// </summary>
    public static int SetMetaEdit(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < MetaEditArgsSize) return unchecked((int)0x80070057);
            ChromeMetaEditArgs a = Marshal.PtrToStructure<ChromeMetaEditArgs>(arg);
            string comment = a.Comment == 0 || a.CommentLen <= 0
                ? ""
                : Marshal.PtrToStringUTF8(checked((IntPtr)a.Comment), a.CommentLen) ?? "";
            _metaRating = a.Rating;
            _metaComment = comment;
            _metaCanEdit = (a.Flags & MetaEditFlags.CanEdit) != 0;
            _metaCanRevert = (a.Flags & MetaEditFlags.CanRevert) != 0;
            if (!_metaPaneVisible) return 0;
            RenderMetaEdit();
            if (_metaCommentBox is FakeInput box)
            {
                // Follow the file (another item, a write landed) unless the user is
                // typing; Esc drops what was typed.
                bool typing = box.FocusState != FocusState.Unfocused;
                if (!typing || (a.Flags & MetaEditFlags.DropDraft) != 0) box.SetText(comment);
                if ((a.Flags & MetaEditFlags.Focus) != 0 && _metaCanEdit)
                {
                    // The island's window takes the keyboard, then the field does.
                    _metaPane?.NavigateFocus(new XamlSourceFocusNavigationRequest(
                        XamlSourceFocusNavigationReason.First));
                    box.Focus(FocusState.Keyboard);
                }
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // Mirrors kMetaEdit* in chrome_host.h.
    private static class MetaEditFlags
    {
        public const int CanEdit = 1;
        public const int CanRevert = 2;
        public const int Focus = 4;
        public const int DropDraft = 8;
    }

    // ==== folder tree ==========================================================
    //
    // Plain StackPanels and Buttons, not a TreeView: XAML controls that pull in
    // text services or their own scrolling machinery fail-fast (0xC000027B) in
    // these islands, TreeView did on first show, and the row count here is a
    // folder's subfolders, so virtualising buys nothing.

    private static string _treeRoot = "";
    private static string _treePending = "";
    private static StackPanel? _treeList;
    private static Border? _treeUp;

    private static void DropTreeUi()
    {
        _treeList = null;
        _treeUp = null;
        _treeRootNode = null;
    }

    private static UIElement BuildTree()
    {
        Grid root = PanelShell("Folders", Command.FolderTree, out Grid body);
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        _treeUp = new Border { Padding = new Thickness(14, 2, 14, 8) };
        body.Children.Add(_treeUp);
        _treeList = new StackPanel { Padding = new Thickness(8, 0, 8, 12) };
        var scroll = new ScrollViewer
        {
            Content = _treeList,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        Grid.SetRow(scroll, 1);
        body.Children.Add(scroll);
        RebuildTreeRoot();
        return root;
    }

    // Keyboard-focusable (arrows walk the rows), but a mouse click never moves
    // keyboard focus into the pane: the mouse user keeps the arrow keys on the canvas.
    // The visible folder rows, top to bottom: the "Up to" button, then each open
    // node's row and its expanded children. Arrows walk this list explicitly.
    private static List<Button> TreeButtons()
    {
        var all = new List<Button>();
        if (_treeUp?.Child is Button up) all.Add(up);
        if (_treeList is not null)
        {
            foreach (UIElement node in _treeList.Children) CollectTreeButtons(node, all);
        }
        return all;
    }

    private static void CollectTreeButtons(UIElement node, List<Button> into)
    {
        if (node is not StackPanel sp || sp.Children.Count < 2) return;
        if (sp.Children[0] is StackPanel row)
        {
            foreach (UIElement c in row.Children)
            {
                if (c is Button b && b.Tag is string tag && tag == "row") into.Add(b);
            }
        }
        if (sp.Children[1] is StackPanel kids && kids.Visibility == Visibility.Visible)
        {
            foreach (UIElement k in kids.Children) CollectTreeButtons(k, into);
        }
    }

    private static void TreeArrow(Button from, KeyRoutedEventArgs e)
    {
        int step = e.Key == Windows.System.VirtualKey.Down ? 1
                 : e.Key == Windows.System.VirtualKey.Up ? -1 : 0;
        if (step == 0) return;
        List<Button> all = TreeButtons();
        int at = all.IndexOf(from);
        if (at >= 0 && at + step >= 0 && at + step < all.Count)
        {
            all[at + step].Focus(FocusState.Keyboard);
            all[at + step].StartBringIntoView();
        }
        e.Handled = true;
    }

    private static Button FlatButton(UIElement content, Thickness padding, bool tabStop = true)
    {
        return new Button
        {
            Content = content,
            Background = Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = padding,
            IsTabStop = tabStop,
            AllowFocusOnInteraction = false,
            HorizontalContentAlignment = HorizontalAlignment.Left,
        };
    }

    private static void RebuildTreeRoot()
    {
        if (_treeList is null || _treeUp is null) return;
        _treeList.Children.Clear();
        _treeUp.Child = null;
        if (_treeRoot.Length == 0)
        {
            _treeUp.Child = Text("No folder open", Body);
            return;
        }
        string? parent = Path.GetDirectoryName(_treeRoot.TrimEnd('\\', '/'));
        if (!string.IsNullOrEmpty(parent))
        {
            string name = Path.GetFileName(parent.TrimEnd('\\', '/'));
            string target = parent;
            Button up = FlatButton(
                Text("↑  Up to " + (name.Length == 0 ? parent : name), Body), new Thickness(0, 2, 8, 2));
            up.Click += (_, _) => OpenTreePath(target);
            up.KeyDown += (_, e) => TreeArrow(up, e);
            _treeUp.Child = up;
        }
        string label = Path.GetFileName(_treeRoot.TrimEnd('\\', '/'));
        // The open folder is the root, expanded, and shown as the current one.
        _treeRootNode = new TreeNodeUi(label.Length == 0 ? _treeRoot : label, _treeRoot, 0, current: true);
        _treeList.Children.Add(_treeRootNode.Node);
        _treeRootNode.SetOpen(true);
    }

    // One folder: a row (expander glyph + name) and a children panel that is
    // filled on first expand. The listing is one directory read on a pool thread;
    // the rows are added back on the UI thread, so a slow disk never freezes the
    // chrome (CLAUDE.md rule 1). Refresh() re-lists an already-listed folder and
    // diffs it in place, so the tree follows the watcher without collapsing what
    // the user opened or dropping the keyboard focus sitting on a row.
    private sealed class TreeNodeUi
    {
        public readonly string Path;
        public readonly int Depth;
        public readonly StackPanel Node = new();
        public readonly StackPanel Children = new() { Visibility = Visibility.Collapsed };
        private readonly Dictionary<string, TreeNodeUi> _kids = new(StringComparer.OrdinalIgnoreCase);
        private readonly TextBlock _glyph;
        private bool _open;
        private bool _loaded;

        public TreeNodeUi(string name, string path, int depth, bool current)
        {
            Path = path;
            Depth = depth;
            _glyph = new TextBlock
            {
                Text = "▸", FontFamily = UiFont, FontSize = UiFontSize, Foreground = Brush(Body),
                Width = 16, TextAlignment = TextAlignment.Center,
            };
            Button toggle = FlatButton(_glyph, new Thickness(0, 2, 0, 2), tabStop: false);
            Button open_folder = FlatButton(
                Text(name, current ? Title : Body, UiFontSize, bold: current), new Thickness(2, 2, 8, 2));
            open_folder.Click += (_, _) => OpenTreePath(Path);
            toggle.Click += (_, _) => SetOpen(!_open);
            // Right opens a folder's children, Left closes them; Enter (the button's
            // own click) opens the folder itself; Up / Down walk the visible rows.
            open_folder.Tag = "row";
            open_folder.KeyDown += (_, e) =>
            {
                if (e.Key == Windows.System.VirtualKey.Right && !_open) { SetOpen(true); e.Handled = true; }
                else if (e.Key == Windows.System.VirtualKey.Left && _open) { SetOpen(false); e.Handled = true; }
                else TreeArrow(open_folder, e);
            };
            var row = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Margin = new Thickness(depth * 14, 0, 0, 0),
            };
            row.Children.Add(toggle);
            row.Children.Add(open_folder);
            Node.Children.Add(row);
            Node.Children.Add(Children);
        }

        public void SetOpen(bool want)
        {
            if (want == _open) return;
            _open = want;
            _glyph.Text = _open ? "▾" : "▸";
            Children.Visibility = _open ? Visibility.Visible : Visibility.Collapsed;
            if (_open && !_loaded)
            {
                _loaded = true;
                List();
            }
        }

        // Re-list a folder that has been listed before (the watcher saw a change).
        public void Refresh()
        {
            if (_loaded) List();
        }

        private void List()
        {
            string dir = Path;
            var queue = _dispatcher?.DispatcherQueue;
            _ = Task.Run(() =>
            {
                IReadOnlyList<(string Name, string Path)> subs = MediaViewerSession.ListSubdirectories(dir);
                queue?.TryEnqueue(() => Apply(subs));
            });
        }

        private void Apply(IReadOnlyList<(string Name, string Path)> subs)
        {
            if (_treeList is null) return;  // the pane was closed meanwhile
            var order = subs.Select(x => x.Path).ToList();
            var keep = new HashSet<string>(order, StringComparer.OrdinalIgnoreCase);
            // Only what changed is touched: a row that stays keeps its place in the
            // visual tree, and with it the keyboard focus and its expanded children.
            foreach (string gone in _kids.Keys.Where(k => !keep.Contains(k)).ToList())
            {
                Children.Children.Remove(_kids[gone].Node);
                _kids.Remove(gone);
            }
            for (int i = 0; i < subs.Count; i++)
            {
                if (_kids.ContainsKey(subs[i].Path)) continue;
                var kid = new TreeNodeUi(subs[i].Name, subs[i].Path, Depth + 1, false);
                _kids[subs[i].Path] = kid;
                Children.Children.Insert(Math.Min(i, Children.Children.Count), kid.Node);
            }
            _glyph.Text = subs.Count == 0 ? " " : (_open ? "▾" : "▸");
        }
    }

    private static TreeNodeUi? _treeRootNode;

    // The watcher saw the open folder change (a subfolder came or went): re-list
    // the root and diff. Deeper folders are not watched, so they refresh when opened.
    private static void RefreshTreeRoot() => _treeRootNode?.Refresh();

    // Choosing a folder opens it exactly as Open Folder does. Native asks for the
    // path with TakeTreePath: the command callback carries only a float.
    private static void OpenTreePath(string path)
    {
        _treePending = path;
        Send(Command.TreeOpen);
    }

    /// <summary>Native pulls the folder the user chose. In/out: ChromeTableArgs (buffer, capacity).
    /// Returns the byte count written, or a negative HRESULT.</summary>
    public static int TakeTreePath(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < TableArgsSize) return unchecked((int)0x80070057);
            ChromeTableArgs a = Marshal.PtrToStructure<ChromeTableArgs>(arg);
            byte[] bytes = System.Text.Encoding.UTF8.GetBytes(_treePending);
            _treePending = "";
            if (a.Utf8 == 0 || a.Length <= bytes.Length) return -1;  // room for the NUL
            IntPtr buf = checked((IntPtr)a.Utf8);
            Marshal.Copy(bytes, 0, buf, bytes.Length);
            Marshal.WriteByte(buf, bytes.Length, 0);
            return bytes.Length;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>Native tells the tree which folder is open. In: ChromeTableArgs (UTF-8 path).</summary>
    public static int SetTreeRoot(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < TableArgsSize) return unchecked((int)0x80070057);
            ChromeTableArgs a = Marshal.PtrToStructure<ChromeTableArgs>(arg);
            string path = a.Utf8 == 0 || a.Length <= 0
                ? ""
                : Marshal.PtrToStringUTF8(checked((IntPtr)a.Utf8), a.Length) ?? "";
            if (path == _treeRoot)
            {
                // Same folder pushed again: native saw the listing change, so a
                // subfolder may have come or gone (io::directory_watcher).
                if (_treeVisible) RefreshTreeRoot();
                return 0;
            }
            _treeRoot = path;
            if (_treeVisible) RebuildTreeRoot();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromePanelArgs
{
    public int Visible;
    public int X;
    public int Y;
    public int Width;
    public int Height;
    public int Focus;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeMetaArgs
{
    public ulong Summary;
    public ulong Properties;
    public ulong Streams;
    public int SummaryLen;
    public int PropertiesLen;
    public int StreamsLen;
    public int Loading;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeMetaEditArgs
{
    public ulong Comment;
    public int CommentLen;
    public int Rating;
    public int Flags;
    public int Reserved;
}
