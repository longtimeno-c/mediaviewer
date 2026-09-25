// SPDX-License-Identifier: GPL-2.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;

namespace MediaViewer.Chrome;

/// <summary>
/// The gallery: a full-width thumbnail grid island below the command bar.
/// </summary>
/// <remarks>
/// Same contract as the filmstrip — chrome over the native swapchain, never a
/// XAML canvas (CLAUDE.md rule 2). It shares the filmstrip's
/// <see cref="Items"/> collection and its completion drain, so opening the
/// gallery costs a layout pass, not a second listing or a second thumb sweep.
///
/// Hiding is a move below the client area plus a collapsed root, driven by
/// native geometry (<c>chrome_show_args</c>). A bridge parked at 0,0 would sit
/// under the command bar and eat its clicks.
/// </remarks>
public static partial class IslandHost
{
    private static DesktopWindowXamlSource? _gallery;
    private static ItemsRepeater? _galleryRepeater;
    private static ItemsRepeater? _folderRepeater;
    private static ScrollViewer? _galleryScroll;
    private static FrameworkElement? _galleryRoot;
    private static TextBlock? _galleryCount;
    private static StackPanel? _galleryStack;
    private static FrameworkElement? _breadcrumbBar;
    private static Button? _upButton;
    private static Button? _rootButton;
    private static TextBlock? _pathCurrent;
    private static StackPanel? _crumbTrail;
    private static TextBlock? _photosHeader;
    private static TextBlock? _galleryEmpty;
    private static TextBlock? _folderFindLabel;
    private static ItemsRepeater? _folderChipRepeater;
    private static ScrollViewer? _folderStrip;
    private static FrameworkElement? _barPathRow;
    private static Button? _barUpButton;
    private static Button? _barRootButton;
    private static TextBlock? _barPathCurrent;
    private static StackPanel? _barCrumbTrail;
    private static bool _galleryVisible;
    private static readonly ObservableCollection<FolderCardVm> Folders = new();
    private static int _folderCursor = -1;
    private static bool _canGoUp;
    private static string? _folderQuery;
    private static readonly List<(string Name, string Path)> Crumbs = new();

    // Keep the chosen size when the gallery closes/reopens during this session.
    private static double GalleryTile = 152;
    // Mac: (width - 2*12 + 8) / (cell + 8). Same cell and gutter here so
    // Up/Down land in the same column on both hosts.
    private static double GalleryStride => GalleryTile + 8;
    private static double GalleryRowStride => GalleryTile + 28;
    private static int GalleryDecodeWidth => Math.Min(512, (int)Math.Ceiling(GalleryTile * 2));

    public static int ScaleGallery(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            if (!_galleryVisible || _galleryRepeater?.Layout is not UniformGridLayout layout) return 1;
            ChromeGalleryNavigationArgs args = Marshal.PtrToStructure<ChromeGalleryNavigationArgs>(arg);
            double next = Math.Clamp(GalleryTile + Math.Sign(args.Direction) * 24, 80, 344);
            if (next == GalleryTile) return 0;
            GalleryTile = next;
            layout.MinItemWidth = GalleryTile;
            layout.MinItemHeight = GalleryTile + 24;
            if (Folders.Count > 0 && Items.Count == 0 &&
                _folderRepeater?.Layout is UniformGridLayout folderLayout)
            {
                folderLayout.MinItemWidth = GalleryTile;
                folderLayout.MinItemHeight = GalleryTile + 24;
                ResizeRealisedTiles(_folderRepeater);
            }
            ResizeRealisedTiles(_galleryRepeater);
            _galleryRoot?.UpdateLayout();
            ReportGalleryColumns();
            if (_folderCursor >= 0) FolderScrollTo(_folderCursor);
            else GalleryScrollTo(args.Index);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static int GalleryColumns => Math.Max(1,
        (int)((Math.Max(0, (_galleryScroll?.ViewportWidth ?? _galleryRepeater?.ActualWidth ?? 0) - 24) + 8)
              / GalleryStride));

    private static void ReportGalleryColumns()
    {
        if (!_galleryVisible) return;
        Send(Command.GalleryColumns, GalleryColumns);
    }

    private static void ResizeRealisedTiles(ItemsRepeater? repeater)
    {
        if (repeater is null) return;
        int children = VisualTreeHelper.GetChildrenCount(repeater);
        for (int i = 0; i < children; ++i)
        {
            if (VisualTreeHelper.GetChild(repeater, i) is not Border border ||
                border.Child is not StackPanel col) continue;
            border.Width = GalleryTile;
            FrameworkElement? box = col.Children.Count > 0 ? col.Children[0] as FrameworkElement : null;
            if (box is not null) box.Width = box.Height = GalleryTile;
            if (box is Image image && image.Source is BitmapImage bitmap)
                bitmap.DecodePixelWidth = GalleryDecodeWidth;
            if (col.Children.Count > 1 && col.Children[1] is TextBlock name) name.MaxWidth = GalleryTile;
            if (col.Children.Count > 1 && col.Children[1] is StackPanel caption)
                caption.Width = GalleryTile;
        }
    }

    public static int ApplyBrowse(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 32) return unchecked((int)0x80070057);
            ChromeBrowseArgs args = Marshal.PtrToStructure<ChromeBrowseArgs>(arg);
            _folderCursor = args.FolderCursor;
            _canGoUp = args.CanGoUp != 0;
            _folderQuery = args.QueryBytes < 0
                ? null
                : (args.QueryBytes == 0 || args.QueryUtf8 == 0
                    ? ""
                    : Marshal.PtrToStringUTF8((IntPtr)args.QueryUtf8, args.QueryBytes) ?? "");
            string previous = Crumbs.Count > 0 ? Crumbs[^1].Path : "";
            if (args.CrumbsUtf8 != 0 && args.CrumbsBytes >= 0)
            {
                Crumbs.Clear();
                if (args.CrumbsBytes > 0)
                {
                    string blob = Marshal.PtrToStringUTF8((IntPtr)args.CrumbsUtf8, args.CrumbsBytes) ?? "";
                    foreach (string line in blob.Split('\n', StringSplitOptions.RemoveEmptyEntries))
                    {
                        int tab = line.IndexOf('\t');
                        if (tab < 0) Crumbs.Add((line, line));
                        else Crumbs.Add((line[..tab], line[(tab + 1)..]));
                    }
                }
            }
            string current = Crumbs.Count > 0 ? Crumbs[^1].Path : "";
            // Mac clears tiles the moment you leave a folder, before the new
            // listing lands. The ABI has already dropped the old subdirs.
            if (!string.Equals(previous, current, StringComparison.OrdinalIgnoreCase))
            {
                ReloadFolders();
            }
            RebuildPathBars();
            ApplyFolderCursor();
            UpdateGallerySections();
            if (_folderCursor >= 0) FolderScrollTo(_folderCursor);
            else GalleryScrollTo(_selectedIndex);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int NavigateGallery(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            if (!_galleryVisible || Items.Count == 0) return 1;
            ChromeGalleryNavigationArgs args = Marshal.PtrToStructure<ChromeGalleryNavigationArgs>(arg);
            if (args.Index < 0 || args.Index >= Items.Count) return 1;
            int columns = GalleryColumns;
            // Stay in the current row at the top/bottom; clamp into a short last row.
            if ((args.Direction < 0 && args.Index < columns) ||
                (args.Direction > 0 && args.Index / columns == (Items.Count - 1) / columns)) return 0;
            int next = Math.Clamp(args.Index + Math.Sign(args.Direction) * columns, 0, Items.Count - 1);
            Send(Command.SelectItem, next);
            GalleryScrollTo(next);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int AttachGallery(IntPtr arg, int sizeBytes)
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

            // The filmstrip normally attaches first and owns the drain. Borrow
            // only if it did not, so a --no-filmstrip layout still lists.
            if (_folderSession is null && args.Session != 0)
            {
                _folderSession = MediaViewerSession.Borrow(checked((IntPtr)args.Session));
                StartDrain();
            }

            DisposeSource(ref _gallery);
            _gallery = new DesktopWindowXamlSource();
            EnsureFocusHook();
            _gallery.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // Park it before content exists: a default full-client island would
            // flash over the canvas for a frame on startup.
            // No content until the first show: the island exists so native can
            // size it, and building the grid for a folder nobody has opened yet
            // would be a listing's worth of work for a hidden view.
            Move(_gallery, 1, 1, args.ClientHeight);
            _galleryVisible = false;
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("chrome AttachGallery: {0}", ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ResizeGallery(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < ResizeArgsSize) return unchecked((int)0x80070057);
            ChromeResizeArgs args = Marshal.PtrToStructure<ChromeResizeArgs>(arg);
            Move(_gallery, args.Width, args.Height, args.Y);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The gallery is rebuilt on every show rather than collapsed and revealed.
    // An ItemsRepeater whose viewport has been squeezed to nothing drops its
    // realised elements and does not bring them back when the viewport grows
    // again — the second G showed a correct header over an empty grid. Building
    // the tree fresh is the path that is known to realise, and it costs one
    // ScrollViewer plus one screenful of tiles, not a listing or a thumb sweep.
    public static int ShowGallery(IntPtr arg, int sizeBytes) => ShowIsland(
        arg, sizeBytes, _gallery, BuildGallery,
        onShown: () =>
        {
            _galleryVisible = true;
            UpdateGalleryCount();
            _dispatcher?.DispatcherQueue.TryEnqueue(RealiseGallery);
        },
        onHidden: () =>
        {
            _galleryVisible = false;
            ReleaseRepeater(ref _galleryRepeater);
            ReleaseRepeater(ref _folderRepeater);
            ReleaseRepeater(ref _folderChipRepeater);
            _galleryRoot = null;
            _galleryScroll = null;
            _galleryCount = null;
            _galleryStack = null;
            _breadcrumbBar = null;
            _upButton = null;
            _crumbTrail = null;
            _photosHeader = null;
            _galleryEmpty = null;
            _folderFindLabel = null;
            _folderStrip = null;
        });

    public static int ShowFilmstrip(IntPtr arg, int sizeBytes) => ShowIsland(
        arg, sizeBytes, _filmstrip, BuildFilmstrip,
        onShown: () => _dispatcher?.DispatcherQueue.TryEnqueue(RealiseFilmstrip),
        onHidden: () =>
        {
            ReleaseRepeater(ref _repeater);
            _filmstripRoot = null;
            _filmstripScroll = null;
        });

    public static int DetachGallery(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            UnhookFocus();
            ReleaseRepeater(ref _galleryRepeater);
            ReleaseRepeater(ref _folderRepeater);
            ReleaseRepeater(ref _folderChipRepeater);
            DisposeSource(ref _gallery);
            _galleryScroll = null;
            _galleryRoot = null;
            _galleryCount = null;
            _galleryVisible = false;
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // Show is "move the bridge and build the tree", hide is "drop the tree and
    // park the bridge below the client area". Collapsing the root instead left
    // the ItemsRepeater with a squeezed viewport, and a repeater that has
    // dropped its realised elements does not reliably bring them back when the
    // viewport grows again — that is the strip that came back empty, and the
    // gallery that opened onto a correct header over nothing.
    private static int ShowIsland(IntPtr arg, int sizeBytes, DesktopWindowXamlSource? source,
                                  Func<UIElement> build, Action onShown, Action onHidden)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < ShowArgsSize) return unchecked((int)0x80070057);
            if (source is null) return 1;
            ChromeShowArgs args = Marshal.PtrToStructure<ChromeShowArgs>(arg);
            if (args.Visible == 0)
            {
                onHidden();
                source.Content = null;
                Move(source, args.Width, args.Height, args.Y);
                return 0;
            }
            Move(source, args.Width, args.Height, args.Y);
            source.Content = build();
            onShown();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // Bind after the repeater is in a XamlRoot. BuildFilmstrip used to assign
    // ItemsSource in the constructor; that AV'd on every folder open once the
    // listing was populated. A fresh island content pass also does not always
    // realise tiles — force a layout, then bind, then layout again.
    private static void Realise(ItemsRepeater? repeater, FrameworkElement? root)
    {
        if (repeater is null) return;
        root?.UpdateLayout();
        try
        {
            if (!ReferenceEquals(repeater.ItemsSource, Items))
                repeater.ItemsSource = Items;
            else if (Items.Count > 0 && repeater.TryGetElement(0) is null)
            {
                repeater.ItemsSource = null;
                repeater.ItemsSource = Items;
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
        root?.UpdateLayout();
    }

    private static void RealiseFilmstrip()
    {
        Realise(_repeater, _filmstripRoot);
        ScrollTo(_selectedIndex);
    }

    private static void RealiseGallery()
    {
        RealiseSource(_folderRepeater, Folders, _galleryRoot);
        RealiseSource(_folderChipRepeater, Folders, _galleryRoot);
        Realise(_galleryRepeater, _galleryRoot);
        ReportGalleryColumns();
        UpdateGallerySections();
        RebuildPathBars();
        ApplyFolderCursor();
        if (_folderCursor >= 0) FolderScrollTo(_folderCursor);
        else GalleryScrollTo(_selectedIndex);
    }

    private static void RealiseSource(ItemsRepeater? repeater, object source, FrameworkElement? root)
    {
        if (repeater is null) return;
        root?.UpdateLayout();
        try
        {
            if (!ReferenceEquals(repeater.ItemsSource, source))
                repeater.ItemsSource = source;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
        root?.UpdateLayout();
    }

    private static void UpdateGalleryCount()
    {
        if (_galleryCount is null) return;
        _galleryCount.Text = Items.Count == 1 ? "1 item" : $"{Items.Count} items";
    }

    private static void GalleryScrollTo(int index)
    {
        if (!_galleryVisible || index < 0 || index >= Items.Count) return;
        UIElement? el = _galleryRepeater?.TryGetElement(index);
        if (el is not null)
        {
            el.StartBringIntoView(new BringIntoViewOptions
            {
                AnimationDesired = false,
                VerticalAlignmentRatio = 0.5,
            });
            return;
        }
        if (_galleryScroll is null || _galleryScroll.ViewportWidth <= 0) return;
        // Not realised yet: estimate from the uniform grid stride. Off by at
        // most one row, and the next realised BringIntoView corrects it.
        int columns = GalleryColumns;
        double y = index / columns * GalleryRowStride - _galleryScroll.ViewportHeight * 0.5;
        _galleryScroll.ChangeView(null, Math.Max(0, y), null, disableAnimation: true);
    }

    private static UniformGridLayout GalleryGridLayout() => new()
    {
        MinItemWidth = GalleryTile,
        MinItemHeight = GalleryTile + 24,
        MinColumnSpacing = 8,
        MinRowSpacing = 8,
        ItemsStretch = UniformGridLayoutItemsStretch.None,
    };

    private static UIElement BuildGallery()
    {
        ReleaseRepeater(ref _galleryRepeater);
        ReleaseRepeater(ref _folderRepeater);
        ReleaseRepeater(ref _folderChipRepeater);
        _folderRepeater = new ItemsRepeater
        {
            Layout = GalleryGridLayout(),
            ItemTemplate = new FolderTileFactory(),
        };
        _folderChipRepeater = new ItemsRepeater
        {
            Layout = new StackLayout { Orientation = Orientation.Horizontal, Spacing = 8 },
            ItemTemplate = new FolderChipFactory(),
        };
        _galleryRepeater = new ItemsRepeater
        {
            Layout = GalleryGridLayout(),
            ItemTemplate = new GalleryFactory(),
        };

        _folderFindLabel = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = 13,
            FontWeight = Microsoft.UI.Text.FontWeights.Medium,
            Foreground = Brush(Title),
            Margin = new Thickness(2, 8, 2, 4),
            Visibility = Visibility.Collapsed,
        };
        _folderStrip = new ScrollViewer
        {
            Content = _folderChipRepeater,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden,
            VerticalScrollBarVisibility = ScrollBarVisibility.Disabled,
            HorizontalScrollMode = ScrollMode.Enabled,
            VerticalScrollMode = ScrollMode.Disabled,
            Height = 80,
            Visibility = Visibility.Collapsed,
        };

        _photosHeader = SectionLabel("");
        _galleryEmpty = new TextBlock
        {
            Text = "No supported photos or videos in this folder",
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Body),
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 60, 0, 0),
            Visibility = Visibility.Collapsed,
        };

        _galleryStack = new StackPanel { Padding = new Thickness(12, 8, 12, 12) };
        _galleryStack.Children.Add(_folderFindLabel);
        _galleryStack.Children.Add(_folderStrip);
        _galleryStack.Children.Add(_folderRepeater);
        _galleryStack.Children.Add(_photosHeader);
        _galleryStack.Children.Add(_galleryRepeater);
        _galleryStack.Children.Add(_galleryEmpty);

        _galleryScroll = new ScrollViewer
        {
            Content = _galleryStack,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollMode = ScrollMode.Disabled,
            VerticalScrollMode = ScrollMode.Enabled,
        };
        _galleryScroll.CharacterReceived += OnTypeahead;
        _galleryScroll.SizeChanged += (_, _) => ReportGalleryColumns();
        _galleryScroll.KeyDown += (_, e) =>
        {
            switch (e.Key)
            {
                case Windows.System.VirtualKey.Escape:
                    Send(Command.CloseGallery);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Left:
                    Send(Command.Prev);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Right:
                    Send(Command.Next);
                    e.Handled = true;
                    break;
            }
        };

        _breadcrumbBar = BuildBreadcrumb();
        var root = new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(_breadcrumbBar, 0);
        root.Children.Add(_breadcrumbBar);
        WireFileDrop(root);
        Grid.SetRow(_galleryScroll, 1);
        root.Children.Add(_galleryScroll);
        _galleryRoot = root;
        UpdateGallerySections();
        RebuildPathBars();
        return root;
    }

    private static TextBlock SectionLabel(string text) => new()
    {
        Text = text,
        FontFamily = UiFont,
        FontSize = 12,
        FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        Foreground = Brush(Body),
        Margin = new Thickness(2, 6, 2, 4),
        Visibility = Visibility.Collapsed,
    };

    private static FrameworkElement BuildBreadcrumb() => BuildPathRow(36,
        out _upButton, out _rootButton, out _crumbTrail, out _pathCurrent);

    private static FrameworkElement BuildBarPathRow()
    {
        _barPathRow = BuildPathRow(27,
            out _barUpButton, out _barRootButton, out _barCrumbTrail, out _barPathCurrent);
        return _barPathRow;
    }

    private static FrameworkElement BuildPathRow(double height, out Button up, out Button root,
        out StackPanel trail, out TextBlock current)
    {
        up = TextButton("↑ Up", () => Send(Command.FolderUp));
        root = TextButton("Root", () => Send(Command.OpenCrumb, 0));
        foreach (Button button in new[] { up, root })
        {
            button.Padding = new Thickness(8, 2, 8, 2);
            button.MinHeight = 24;
            button.VerticalAlignment = VerticalAlignment.Center;
        }
        AutomationProperties.SetName(up, "Up one folder");
        AutomationProperties.SetName(root, "Return to browsing root");
        ToolTipService.SetToolTip(up, "Open the enclosing folder (Ctrl+Up)");
        trail = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        var scroller = PathScroller(trail);
        current = new TextBlock
        {
            FontFamily = UiFont, FontSize = 13,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = Brush(Title), VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 240, TextTrimming = TextTrimming.CharacterEllipsis,
            Margin = new Thickness(8, 0, 0, 0),
        };
        var row = new Grid { Height = height, Padding = new Thickness(8, 0, 12, 0), Background = Brush(Canvas) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(up);
        Grid.SetColumn(root, 1);
        row.Children.Add(root);
        Grid.SetColumn(scroller, 2);
        row.Children.Add(scroller);
        Grid.SetColumn(current, 3);
        row.Children.Add(current);
        return row;
    }

    private static ScrollViewer PathScroller(StackPanel trail) => new()
    {
        Content = trail,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden,
        VerticalScrollBarVisibility = ScrollBarVisibility.Disabled,
        HorizontalScrollMode = ScrollMode.Enabled,
        VerticalScrollMode = ScrollMode.Disabled,
    };

    private static void RebuildPathBars()
    {
        FillPathTrail(_crumbTrail, _upButton, _rootButton, _pathCurrent, _breadcrumbBar);
        FillPathTrail(_barCrumbTrail, _barUpButton, _barRootButton, _barPathCurrent, _barPathRow);
    }

    private static void FillPathTrail(StackPanel? trail, Button? up, Button? root, TextBlock? current, FrameworkElement? row)
    {
        if (trail is null || up is null || root is null || current is null || row is null) return;
        up.IsEnabled = _canGoUp;
        up.Opacity = _canGoUp ? 1 : 0.4;
        row.Visibility = Crumbs.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        root.IsEnabled = Crumbs.Count > 1;
        root.Opacity = root.IsEnabled ? 1 : 0.4;
        ToolTipService.SetToolTip(root, Crumbs.Count > 0 ? "Return to browsing root: " + Crumbs[0].Path : "No folder open");
        current.Text = Crumbs.Count > 0 ? (Crumbs.Count > 1 ? "›  " : "") + Crumbs[^1].Name : "";
        ToolTipService.SetToolTip(current, Crumbs.Count > 0 ? Crumbs[^1].Path : "");
        trail.Children.Clear();
        var shown = DisplayCrumbs();
        for (int i = 0; i < shown.Count - 1; ++i)
        {
            if (i > 0)
            {
                trail.Children.Add(new FontIcon
                {
                    Glyph = "\uE76C",
                    FontSize = 8,
                    Foreground = Brush(Body),
                    Margin = new Thickness(2, 0, 2, 0),
                    VerticalAlignment = VerticalAlignment.Center,
                });
            }
            var piece = shown[i];
            if (piece.Ellipsis)
            {
                var menu = new MenuFlyout { MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle() };
                for (int hidden = 1; hidden < Crumbs.Count - 2; ++hidden)
                {
                    int index = hidden;
                    var item = new MenuFlyoutItem { Text = Crumbs[index].Name };
                    ToolTipService.SetToolTip(item, Crumbs[index].Path);
                    item.Click += (_, _) => Send(Command.OpenCrumb, index);
                    menu.Items.Add(item);
                }
                Button more = TextButton("…", () => { });
                more.Flyout = menu;
                more.Padding = new Thickness(4, 2, 4, 2);
                more.MinHeight = 24;
                AutomationProperties.SetName(more, "Hidden parent folders");
                ToolTipService.SetToolTip(more, "Open a parent folder");
                trail.Children.Add(more);
            }
            else
            {
                int index = piece.Index;
                Button crumb = TextButton(piece.Name, () => Send(Command.OpenCrumb, index));
                crumb.Padding = new Thickness(4, 2, 4, 2);
                crumb.MinHeight = 24;
                crumb.MaxWidth = 160;
                if (crumb.Content is TextBlock label) label.TextTrimming = TextTrimming.CharacterEllipsis;
                ToolTipService.SetToolTip(crumb, Crumbs[index].Path);
                trail.Children.Add(crumb);
            }
        }
    }

    private readonly record struct PathPiece(int Index, string Name, bool Ellipsis);

    private static List<PathPiece> DisplayCrumbs()
    {
        if (Crumbs.Count <= 4)
        {
            var all = new List<PathPiece>(Crumbs.Count);
            for (int i = 0; i < Crumbs.Count; ++i) all.Add(new PathPiece(i, Crumbs[i].Name, false));
            return all;
        }
        int n = Crumbs.Count;
        return
        [
            new PathPiece(0, Crumbs[0].Name, false),
            new PathPiece(-1, "…", true),
            new PathPiece(n - 2, Crumbs[n - 2].Name, false),
            new PathPiece(n - 1, Crumbs[n - 1].Name, false),
        ];
    }

    private static void UpdateGallerySections()
    {
        bool folders = Folders.Count > 0;
        bool photos = Items.Count > 0;
        bool mixed = folders && photos;
        bool foldersOnly = folders && !photos;
        if (_folderFindLabel is not null)
        {
            _folderFindLabel.Text = _folderQuery is null ? ""
                : (_folderQuery.Length == 0 ? "Find folder" : $"Find folder: {_folderQuery}");
            _folderFindLabel.Visibility = folders && _folderQuery is not null
                ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_folderStrip is not null)
            _folderStrip.Visibility = mixed ? Visibility.Visible : Visibility.Collapsed;
        if (_folderRepeater is not null)
            _folderRepeater.Visibility = foldersOnly ? Visibility.Visible : Visibility.Collapsed;
        if (_photosHeader is not null)
        {
            _photosHeader.Text = $"Photos and videos  {Items.Count}";
            _photosHeader.Visibility = mixed ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_galleryRepeater is not null)
            _galleryRepeater.Visibility = photos ? Visibility.Visible : Visibility.Collapsed;
        if (_galleryEmpty is not null)
            _galleryEmpty.Visibility = !folders && !photos ? Visibility.Visible : Visibility.Collapsed;
        if (_galleryCount is not null)
            _galleryCount.Text = Items.Count == 1 ? "1 item" : $"{Items.Count} items";
    }

    private static void ApplyFolderCursor()
    {
        for (int i = 0; i < Folders.Count; ++i) Folders[i].Cursor = i == _folderCursor;
        for (int i = 0; i < Items.Count; ++i)
            Items[i].GalleryCurrent = Items[i].Selected && _folderCursor < 0;
    }

    private static void FolderScrollTo(int index)
    {
        if (!_galleryVisible || index < 0 || index >= Folders.Count) return;
        UIElement? el = Items.Count > 0
            ? _folderChipRepeater?.TryGetElement(index)
            : _folderRepeater?.TryGetElement(index);
        if (el is not null)
        {
            el.StartBringIntoView(new BringIntoViewOptions
            {
                AnimationDesired = false,
                VerticalAlignmentRatio = 0.5,
                HorizontalAlignmentRatio = 0.5,
            });
        }
    }

    private static void ReloadFolders()
    {
        if (_folderSession is null) return;
        var keep = new Dictionary<string, FolderCardVm>(StringComparer.OrdinalIgnoreCase);
        foreach (FolderCardVm old in Folders) keep[old.Path] = old;
        Folders.Clear();
        uint count = _folderSession.FolderSubfolderCount;
        for (uint i = 0; i < count; ++i)
        {
            string path = _folderSession.FolderSubfolderPath(i);
            string name = _folderSession.FolderSubfolderName(i);
            if (keep.TryGetValue(path, out FolderCardVm? existing))
            {
                existing.Index = (int)i;
                existing.Name = name;
                existing.Cursor = existing.Index == _folderCursor;
                Folders.Add(existing);
                continue;
            }
            MvFolderSummary summary = _folderSession.FolderSummaryAt(i);
            Folders.Add(ApplySummary(new FolderCardVm
            {
                Index = (int)i,
                Path = path,
                Name = name,
                Cursor = (int)i == _folderCursor,
            }, summary, i));
        }
        UpdateGallerySections();
    }

    private static void ApplyFolderSummary(int index)
    {
        if (_folderSession is null || index < 0 || index >= Folders.Count) return;
        string path = _folderSession.FolderSubfolderPath((uint)index);
        FolderCardVm? card = Folders.FirstOrDefault(c => c.Path == path) ?? Folders[index];
        ApplySummary(card, _folderSession.FolderSummaryAt((uint)card.Index), (uint)card.Index);
        if (!card.Loaded) card.Requested = false;
    }

    private static FolderCardVm ApplySummary(FolderCardVm card, MvFolderSummary summary, uint index)
    {
        card.MediaCount = (int)summary.MediaCount;
        card.SubfolderCount = (int)summary.SubdirCount;
        card.PhotosInside = (summary.Flags & MvFolderSummary.FlagPhotosInside) != 0;
        card.SearchStopped = (summary.Flags & MvFolderSummary.FlagSearchIncomplete) != 0;
        card.Loaded = (summary.Flags & MvFolderSummary.FlagLoaded) != 0;
        card.CoverPath = (summary.Flags & MvFolderSummary.FlagHasCover) != 0
            ? _folderSession!.FolderSummaryCoverThumb(index) : "";
        return card;
    }

    private sealed class GalleryFactory : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var vm = (FolderItemVm)args.Data;
            var image = new Image
            {
                Width = GalleryTile,
                Height = GalleryTile,
                Stretch = Stretch.Uniform,
            };
            var name = new TextBlock
            {
                Text = vm.Name,
                FontFamily = UiFont,
                FontSize = 12,
                Foreground = Brush(Body),
                TextTrimming = TextTrimming.CharacterEllipsis,
                MaxWidth = GalleryTile,
                Margin = new Thickness(0, 2, 0, 0),
            };
            var col = new StackPanel { Spacing = 4 };
            var clipped = new Border
            {
                CornerRadius = new CornerRadius(6),
                Child = WithBadge(image, vm.Badge),
                Width = GalleryTile,
                Height = GalleryTile,
            };
            col.Children.Add(clipped);
            col.Children.Add(name);
            var border = new Border
            {
                Width = GalleryTile,
                Padding = new Thickness(0),
                CornerRadius = new CornerRadius(6),
                BorderThickness = new Thickness(vm.Selected && _folderCursor < 0 ? 2 : 0),
                BorderBrush = Brush(Title),
                Child = col,
            };

            void SetThumb()
            {
                if (string.IsNullOrEmpty(vm.ThumbPath)) { image.Source = null; return; }
                try
                {
                    // Decode for the current tile size, bounded by the JPEG-512 cache.
                    image.Source = new BitmapImage(new Uri(vm.ThumbPath)) { DecodePixelWidth = GalleryDecodeWidth };
                }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
            SetThumb();

            void OnChanged(object? _, System.ComponentModel.PropertyChangedEventArgs e)
            {
                if (e.PropertyName is nameof(FolderItemVm.ThumbPath) or null) SetThumb();
                if (e.PropertyName is nameof(FolderItemVm.Selected) or nameof(FolderItemVm.GalleryCurrent) or null)
                    border.BorderThickness = new Thickness(vm.Selected && _folderCursor < 0 ? 2 : 0);
            }
            vm.PropertyChanged += OnChanged;
            // Recycling without this leaks a handler per realisation, and every
            // thumb completion then walks a growing list of dead elements.
            border.Tag = (Action)(() => vm.PropertyChanged -= OnChanged);

            border.Tapped += (_, _) => Send(Command.GalleryActivate, vm.Index);
            WireFileDrag(border, vm);
            return border;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            if (args.Element is Border { Tag: Action unsubscribe })
            {
                unsubscribe();
                ((Border)args.Element).Tag = null;
            }
        }
    }

    private sealed class FolderTileFactory : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var vm = (FolderCardVm)args.Data;
            var cover = new Image
            {
                Width = GalleryTile,
                Height = GalleryTile,
                Stretch = Stretch.UniformToFill,
            };
            var placeholder = new Grid
            {
                Width = GalleryTile,
                Height = GalleryTile,
                Background = new SolidColorBrush(ColorHelper.FromArgb(255, 48, 50, 58)),
            };
            placeholder.Children.Add(new FontIcon
            {
                Glyph = "\uE8B7",
                FontSize = GalleryTile * 0.32,
                Foreground = Brush(Body),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            });
            var box = new Grid { Width = GalleryTile, Height = GalleryTile };
            box.Children.Add(placeholder);
            box.Children.Add(cover);

            var count = new Border
            {
                Background = new SolidColorBrush(ColorHelper.FromArgb(0xC0, 0, 0, 0)),
                CornerRadius = new CornerRadius(10),
                Padding = new Thickness(6, 2, 6, 2),
                Margin = new Thickness(6),
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Bottom,
                Child = new TextBlock
                {
                    FontFamily = UiFont,
                    FontSize = 11,
                    Foreground = new SolidColorBrush(Colors.White),
                },
            };
            var overlay = new Grid { Width = GalleryTile, Height = GalleryTile };
            overlay.Children.Add(box);
            overlay.Children.Add(count);

            var caption = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Width = GalleryTile };
            caption.Children.Add(new FontIcon { Glyph = "\uE8B7", FontSize = 12, Foreground = Brush(Body) });
            caption.Children.Add(new TextBlock
            {
                Text = vm.Name,
                FontFamily = UiFont,
                FontSize = 12,
                Foreground = Brush(Body),
                TextTrimming = TextTrimming.CharacterEllipsis,
                MaxWidth = GalleryTile - 20,
            });

            var col = new StackPanel { Spacing = 4 };
            var clipped = new Border
            {
                CornerRadius = new CornerRadius(6),
                Child = overlay,
                Width = GalleryTile,
                Height = GalleryTile,
            };
            col.Children.Add(clipped);
            col.Children.Add(caption);
            var border = new Border
            {
                Width = GalleryTile,
                Padding = new Thickness(0),
                CornerRadius = new CornerRadius(6),
                BorderThickness = new Thickness(vm.Cursor ? 2 : 0),
                BorderBrush = Brush(Title),
                Child = col,
            };

            void SetCover()
            {
                if (string.IsNullOrEmpty(vm.CoverPath))
                {
                    cover.Source = null;
                    cover.Visibility = Visibility.Collapsed;
                    placeholder.Visibility = Visibility.Visible;
                    return;
                }
                try
                {
                    cover.Source = new BitmapImage(new Uri(vm.CoverPath)) { DecodePixelWidth = GalleryDecodeWidth };
                    cover.Visibility = Visibility.Visible;
                    placeholder.Visibility = Visibility.Collapsed;
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine(ex);
                    cover.Visibility = Visibility.Collapsed;
                    placeholder.Visibility = Visibility.Visible;
                }
            }
            void SetCount()
            {
                count.Visibility = vm.Loaded ? Visibility.Visible : Visibility.Collapsed;
                if (count.Child is TextBlock label) label.Text = vm.Status;
            }
            SetCover();
            SetCount();

            void OnChanged(object? _, System.ComponentModel.PropertyChangedEventArgs e)
            {
                if (e.PropertyName is nameof(FolderCardVm.CoverPath) or null) SetCover();
                if (e.PropertyName is nameof(FolderCardVm.Loaded) or nameof(FolderCardVm.MediaCount)
                    or nameof(FolderCardVm.SubfolderCount) or nameof(FolderCardVm.PhotosInside)
                    or nameof(FolderCardVm.SearchStopped) or nameof(FolderCardVm.Status) or null)
                    SetCount();
                if (e.PropertyName is nameof(FolderCardVm.Cursor) or null)
                    border.BorderThickness = new Thickness(vm.Cursor ? 2 : 0);
            }
            vm.PropertyChanged += OnChanged;
            border.Tag = (Action)(() => vm.PropertyChanged -= OnChanged);
            border.Tapped += (_, _) => Send(Command.OpenSubfolder, vm.Index);
            if (!vm.Requested)
            {
                vm.Requested = true;
                try { _folderSession?.FolderRequestSummary((uint)vm.Index); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); vm.Requested = false; }
            }
            return border;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            if (args.Element is Border { Tag: Action unsubscribe })
            {
                unsubscribe();
                ((Border)args.Element).Tag = null;
            }
        }
    }

    private sealed class FolderChipFactory : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var vm = (FolderCardVm)args.Data;
            var cover = new Image { Width = 36, Height = 36, Stretch = Stretch.UniformToFill };
            var placeholder = new FontIcon
            {
                Glyph = "\uE8B7",
                FontSize = 16,
                Foreground = Brush(Body),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            var box = new Grid
            {
                Width = 36,
                Height = 36,
                Background = new SolidColorBrush(ColorHelper.FromArgb(255, 48, 50, 58)),
            };
            box.Children.Add(placeholder);
            box.Children.Add(cover);
            var clipped = new Border { CornerRadius = new CornerRadius(4), Child = box, Width = 36, Height = 36 };

            var name = new TextBlock
            {
                FontFamily = UiFont,
                FontSize = 12,
                Foreground = Brush(Title),
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
            var status = new TextBlock
            {
                FontFamily = UiFont,
                FontSize = 11,
                Foreground = Brush(Body),
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
            var text = new StackPanel { Spacing = 1, Width = 120 };
            text.Children.Add(name);
            text.Children.Add(status);
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            row.Children.Add(clipped);
            row.Children.Add(text);
            var border = new Border
            {
                Padding = new Thickness(4),
                CornerRadius = new CornerRadius(6),
                BorderThickness = new Thickness(vm.Cursor ? 2 : 0),
                BorderBrush = Brush(Title),
                Background = new SolidColorBrush(ColorHelper.FromArgb(vm.Cursor ? (byte)0x2E : (byte)0x0A, 255, 255, 255)),
                Child = row,
            };

            void SetCover()
            {
                if (string.IsNullOrEmpty(vm.CoverPath))
                {
                    cover.Source = null;
                    cover.Visibility = Visibility.Collapsed;
                    placeholder.Visibility = Visibility.Visible;
                    return;
                }
                try
                {
                    cover.Source = new BitmapImage(new Uri(vm.CoverPath)) { DecodePixelWidth = 72 };
                    cover.Visibility = Visibility.Visible;
                    placeholder.Visibility = Visibility.Collapsed;
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine(ex);
                    cover.Visibility = Visibility.Collapsed;
                    placeholder.Visibility = Visibility.Visible;
                }
            }
            void SetText()
            {
                name.Text = vm.Name;
                status.Text = vm.Status;
                status.Visibility = vm.Loaded ? Visibility.Visible : Visibility.Collapsed;
            }
            void SetCursor()
            {
                border.BorderThickness = new Thickness(vm.Cursor ? 2 : 0);
                border.Background = new SolidColorBrush(
                    ColorHelper.FromArgb(vm.Cursor ? (byte)0x2E : (byte)0x0A, 255, 255, 255));
            }
            SetCover();
            SetText();
            SetCursor();

            void OnChanged(object? _, PropertyChangedEventArgs e)
            {
                if (e.PropertyName is nameof(FolderCardVm.CoverPath) or null) SetCover();
                if (e.PropertyName is nameof(FolderCardVm.Name) or nameof(FolderCardVm.Loaded)
                    or nameof(FolderCardVm.MediaCount) or nameof(FolderCardVm.SubfolderCount)
                    or nameof(FolderCardVm.PhotosInside) or nameof(FolderCardVm.SearchStopped)
                    or nameof(FolderCardVm.Status) or null)
                    SetText();
                if (e.PropertyName is nameof(FolderCardVm.Cursor) or null) SetCursor();
            }
            vm.PropertyChanged += OnChanged;
            border.Tag = (Action)(() => vm.PropertyChanged -= OnChanged);
            border.Tapped += (_, _) => Send(Command.OpenSubfolder, vm.Index);
            if (!vm.Requested)
            {
                vm.Requested = true;
                try { _folderSession?.FolderRequestSummary((uint)vm.Index); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); vm.Requested = false; }
            }
            return border;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            if (args.Element is Border { Tag: Action unsubscribe })
            {
                unsubscribe();
                ((Border)args.Element).Tag = null;
            }
        }
    }
}

internal sealed class FolderCardVm : INotifyPropertyChanged
{
    private string _coverPath = "";
    private bool _cursor;
    private bool _loaded;
    private int _media;
    private int _subs;
    private bool _photosInside;
    private bool _searchStopped;
    public int Index { get; set; }
    public string Path { get; set; } = "";
    public string Name { get; set; } = "";
    public bool Requested { get; set; }
    public int MediaCount
    {
        get => _media;
        set { if (_media == value) return; _media = value; Changed(nameof(MediaCount)); }
    }
    public int SubfolderCount
    {
        get => _subs;
        set { if (_subs == value) return; _subs = value; Changed(nameof(SubfolderCount)); }
    }
    public bool PhotosInside
    {
        get => _photosInside;
        set { if (_photosInside == value) return; _photosInside = value; Changed(nameof(PhotosInside)); }
    }
    public bool SearchStopped
    {
        get => _searchStopped;
        set { if (_searchStopped == value) return; _searchStopped = value; Changed(nameof(SearchStopped)); }
    }
    public bool Loaded
    {
        get => _loaded;
        set { if (_loaded == value) return; _loaded = value; Changed(nameof(Loaded)); }
    }
    public bool Cursor
    {
        get => _cursor;
        set { if (_cursor == value) return; _cursor = value; Changed(nameof(Cursor)); }
    }
    public string CoverPath
    {
        get => _coverPath;
        set { if (_coverPath == value) return; _coverPath = value; Changed(nameof(CoverPath)); }
    }
    public string Status
    {
        get
        {
            if (!_loaded) return "";
            if (_searchStopped) return "Search stopped";
            if (_media > 0)
            {
                string items = _media == 1 ? "1 item" : $"{_media} items";
                if (_subs == 0) return items;
                string folders = _subs == 1 ? "1 folder" : $"{_subs} folders";
                return $"{items}, {folders}";
            }
            if (_photosInside) return "Photos inside";
            if (_subs > 0) return "Folders only";
            return "Empty";
        }
    }
    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed(string name) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeBrowseArgs
{
    public int FolderCursor;
    public int CanGoUp;
    public int CrumbsBytes;
    public int QueryBytes;
    public ulong CrumbsUtf8;
    public ulong QueryUtf8;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeShowArgs
{
    public int Visible;
    public int Width;
    public int Height;
    public int Y;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeFlagsArgs
{
    public int Flags;
    public int Sort;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeGalleryNavigationArgs
{
    public int Direction;
    public int Index;
}

