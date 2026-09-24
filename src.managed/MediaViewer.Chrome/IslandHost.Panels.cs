// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
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

    private static DesktopWindowXamlSource? _metaPane;
    private static DesktopWindowXamlSource? _tree;
    private static bool _metaPaneVisible;
    private static bool _treeVisible;

    private static readonly Color PanelBg = ColorHelper.FromArgb(255, 27, 29, 35);

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
            EnsureFocusHook();
            _metaPane = new DesktopWindowXamlSource();
            _metaPane.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            _tree = new DesktopWindowXamlSource();
            _tree.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // PR 11: the adjust pane, on the same right edge as the metadata pane.
            _adjustPane = new DesktopWindowXamlSource();
            _adjustPane.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // Parked below the client area with no content until first shown: a
            // default full-client island would flash over the canvas.
            MoveAt(_metaPane, 0, args.ClientHeight, 1, 1);
            MoveAt(_tree, 0, args.ClientHeight, 1, 1);
            MoveAt(_adjustPane, 0, args.ClientHeight, 1, 1);
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
            DropMetaUi();
            DropTreeUi();
            DropAdjustUi();
            DisposeSource(ref _metaPane);
            DisposeSource(ref _tree);
            DisposeSource(ref _adjustPane);
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
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ---- small builders -------------------------------------------------------

    private static TextBlock Text(string text, Color color, double size = UiFontSize,
                                  bool bold = false, int maxLines = 0)
    {
        var t = new TextBlock
        {
            Text = text,
            Foreground = Brush(color),
            FontFamily = UiFont,
            FontSize = size,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
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
        };
        b.Click += (_, _) => Send(command);
        return b;
    }

    private static Grid PanelShell(string title, int closeCommand, out Grid body)
    {
        var root = new Grid { Background = Brush(PanelBg), RequestedTheme = ElementTheme.Dark };
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
    private static bool _metaLoading;
    private static bool _metaIsClip;
    private static MetaTab _metaTab = MetaTab.Summary;
    private static string _metaQuery = "";

    private static Border? _metaContent;
    private static TextBlock? _metaLoadingText;
    private static StackPanel? _metaTabs;

    private static void DropMetaUi()
    {
        _metaContent = null;
        _metaLoadingText = null;
        _metaTabs = null;
    }

    private static UIElement BuildMetaPane()
    {
        Grid root = PanelShell("Metadata", Command.MetadataPane, out Grid body);
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var bar = new Grid { Padding = new Thickness(14, 0, 14, 8) };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _metaTabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        bar.Children.Add(_metaTabs);
        _metaLoadingText = Text("", Body);
        Grid.SetColumn(_metaLoadingText, 1);
        bar.Children.Add(_metaLoadingText);
        body.Children.Add(bar);

        _metaContent = new Border { BorderBrush = Brush(Hairline), BorderThickness = new Thickness(0, 1, 0, 0) };
        Grid.SetRow(_metaContent, 1);
        body.Children.Add(_metaContent);

        RenderMeta();
        return root;
    }

    private static void RenderMeta()
    {
        if (_metaContent is null || _metaTabs is null) return;
        // A still has no streams: fall back if the tab it was on went away.
        if (!_metaIsClip && _metaTab == MetaTab.Streams) _metaTab = MetaTab.Summary;
        if (_metaLoadingText is not null) _metaLoadingText.Text = _metaLoading ? "reading…" : "";

        _metaTabs.Children.Clear();
        AddMetaTab("Summary", MetaTab.Summary);
        AddMetaTab("All tags", MetaTab.Tags);
        if (_metaIsClip) AddMetaTab("Streams", MetaTab.Streams);

        _metaContent.Child = _metaTab switch
        {
            MetaTab.Tags => BuildTagTree(),
            MetaTab.Streams => BuildStreams(),
            _ => BuildSummary(),
        };
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
            Background = Brush(on ? Hairline : Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = new Thickness(10, 3, 10, 3),
            IsTabStop = false,
        };
        b.Click += (_, _) =>
        {
            _metaTab = tab;
            RenderMeta();
        };
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
        host.Children.Add(search);

        var list = new ListView
        {
            SelectionMode = ListViewSelectionMode.None,
            IsItemClickEnabled = false,
            Padding = new Thickness(6, 0, 6, 6),
        };
        Grid.SetRow(list, 1);
        host.Children.Add(list);

        void Fill()
        {
            list.Items.Clear();
            if (_metaTags.Count == 0)
            {
                list.Items.Add(Text(_metaLoading ? "Reading…" : "No metadata in this file", Body));
                return;
            }
            string needle = _metaQuery.Trim().ToLowerInvariant();
            string? group = null;
            int shown = 0;
            foreach (TagRow t in _metaTags)
            {
                if (needle.Length > 0 &&
                    !t.Label.Contains(needle, StringComparison.OrdinalIgnoreCase) &&
                    !t.Value.Contains(needle, StringComparison.OrdinalIgnoreCase) &&
                    !t.Raw.Contains(needle, StringComparison.OrdinalIgnoreCase)) continue;
                if (t.Group != group)
                {
                    group = t.Group;
                    var header = Text(group, Body, UiFontSize, bold: true);
                    header.Margin = new Thickness(4, 10, 0, 2);
                    list.Items.Add(header);
                }
                var cell = new StackPanel { Margin = new Thickness(4, 2, 4, 2) };
                cell.Children.Add(Text(t.Label, Title));
                cell.Children.Add(Text(t.Value.Length == 0 ? "—" : t.Value, Body, UiFontSize - 2, maxLines: 3));
                // The untranslated origin is always one hover away (plan/06).
                ToolTipService.SetToolTip(cell, t.Raw);
                list.Items.Add(cell);
                shown++;
            }
            if (shown == 0) list.Items.Add(Text("No tag matches", Body));
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

    private static Button FlatButton(UIElement content, Thickness padding)
    {
        return new Button
        {
            Content = content,
            Background = Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = padding,
            IsTabStop = false,  // keys stay with the canvas
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
            _treeUp.Child = up;
        }
        string label = Path.GetFileName(_treeRoot.TrimEnd('\\', '/'));
        // The open folder is the root, expanded, and shown as the current one.
        _treeList.Children.Add(BuildTreeNode(label.Length == 0 ? _treeRoot : label, _treeRoot, 0,
                                             current: true, expand: true));
    }

    // One folder: a row (expander glyph + name) and a children panel that is
    // filled on first expand. The listing is one directory read on a pool
    // thread; the rows are added back on the UI thread, so a slow disk never
    // freezes the chrome (CLAUDE.md rule 1).
    private static UIElement BuildTreeNode(string name, string path, int depth, bool current, bool expand)
    {
        var node = new StackPanel();
        var children = new StackPanel { Visibility = Visibility.Collapsed };
        bool loaded = false;
        bool open = false;

        var glyph = new TextBlock
        {
            Text = "▸", FontFamily = UiFont, FontSize = UiFontSize, Foreground = Brush(Body),
            Width = 16, TextAlignment = TextAlignment.Center,
        };
        Button toggle = FlatButton(glyph, new Thickness(0, 2, 0, 2));
        Button open_folder = FlatButton(
            Text(name, current ? Title : Body, UiFontSize, bold: current), new Thickness(2, 2, 8, 2));
        open_folder.Click += (_, _) => OpenTreePath(path);

        void Toggle()
        {
            open = !open;
            glyph.Text = open ? "▾" : "▸";
            children.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            if (!open || loaded) return;
            loaded = true;
            var queue = _dispatcher?.DispatcherQueue;
            _ = Task.Run(() =>
            {
                IReadOnlyList<(string Name, string Path)> subs = MediaViewerSession.ListSubdirectories(path);
                queue?.TryEnqueue(() =>
                {
                    if (_treeList is null) return;  // the pane was closed meanwhile
                    foreach ((string sub, string subPath) in subs)
                    {
                        children.Children.Add(BuildTreeNode(sub, subPath, depth + 1, false, false));
                    }
                    if (subs.Count == 0) glyph.Text = " ";  // a leaf: nothing to open
                });
            });
        }
        toggle.Click += (_, _) => Toggle();

        var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(depth * 14, 0, 0, 0) };
        row.Children.Add(toggle);
        row.Children.Add(open_folder);
        node.Children.Add(row);
        node.Children.Add(children);
        if (expand) Toggle();
        return node;
    }

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
            if (path == _treeRoot) return 0;
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
    public int Dpi;
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
