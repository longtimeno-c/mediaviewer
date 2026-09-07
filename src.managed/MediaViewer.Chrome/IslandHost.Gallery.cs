// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
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
    private static ScrollViewer? _galleryScroll;
    private static FrameworkElement? _galleryRoot;
    private static TextBlock? _galleryCount;
    private static bool _galleryVisible;

    private const double GalleryTile = 152;
    private const double GalleryStride = 176;  // tile + padding + spacing

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

            _gallery?.Dispose();
            _gallery = new DesktopWindowXamlSource();
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
            _galleryRoot = null;
            _galleryRepeater = null;
            _galleryScroll = null;
            _galleryCount = null;
        });

    public static int ShowFilmstrip(IntPtr arg, int sizeBytes) => ShowIsland(
        arg, sizeBytes, _filmstrip, BuildFilmstrip,
        onShown: () => _dispatcher?.DispatcherQueue.TryEnqueue(RealiseFilmstrip),
        onHidden: () =>
        {
            _filmstripRoot = null;
            _repeater = null;
            _filmstripScroll = null;
        });

    public static int DetachGallery(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            if (_gallery is not null)
            {
                _gallery.Content = null;
                _gallery.Dispose();
                _gallery = null;
            }
            _galleryRepeater = null;
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
                source.Content = null;
                onHidden();
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

    // ItemsRepeater realises from the viewport change raised during a layout
    // pass, and the pass a fresh island content triggers does not always carry
    // one. Force it, and if nothing realised, reset the source, which always
    // does. One arrange of a screenful of tiles, only when a strip appears.
    private static void Realise(ItemsRepeater? repeater, FrameworkElement? root)
    {
        root?.UpdateLayout();
        if (repeater is null || Items.Count == 0) return;
        if (repeater.TryGetElement(0) is null)
        {
            repeater.ItemsSource = null;
            repeater.ItemsSource = Items;
            root?.UpdateLayout();
        }
    }

    private static void RealiseFilmstrip()
    {
        Realise(_repeater, _filmstripRoot);
        ScrollTo(_selectedIndex);
    }

    private static void RealiseGallery()
    {
        Realise(_galleryRepeater, _galleryRoot);
        GalleryScrollTo(_selectedIndex);
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
        int columns = Math.Max(1, (int)(_galleryScroll.ViewportWidth / GalleryStride));
        double y = index / columns * GalleryStride - _galleryScroll.ViewportHeight * 0.5;
        _galleryScroll.ChangeView(null, Math.Max(0, y), null, disableAnimation: true);
    }

    private static UIElement BuildGallery()
    {
        _galleryRepeater = new ItemsRepeater
        {
            ItemsSource = Items,
            Layout = new UniformGridLayout
            {
                MinItemWidth = GalleryTile + 16,
                MinItemHeight = GalleryTile + 40,
                MinColumnSpacing = 8,
                MinRowSpacing = 8,
                ItemsStretch = UniformGridLayoutItemsStretch.None,
            },
            ItemTemplate = new GalleryFactory(),
            Margin = new Thickness(12, 8, 12, 12),
        };
        _galleryScroll = new ScrollViewer
        {
            Content = _galleryRepeater,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollMode = ScrollMode.Disabled,
            VerticalScrollMode = ScrollMode.Enabled,
        };
        _galleryScroll.KeyDown += (_, e) =>
        {
            switch (e.Key)
            {
                case Windows.System.VirtualKey.Escape:
                    Send(Command.CloseGallery);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Enter:
                    if (_selectedIndex >= 0) Send(Command.GalleryActivate, _selectedIndex);
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

        _galleryCount = new TextBlock
        {
            Text = "",
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var header = new Grid { Height = 34, Padding = new Thickness(16, 0, 8, 0) };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.Children.Add(_galleryCount);
        Button close = TextButton("Close  Esc", () => Send(Command.CloseGallery));
        Grid.SetColumn(close, 1);
        header.Children.Add(close);

        var root = new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(header, 0);
        root.Children.Add(header);
        Grid.SetRow(_galleryScroll, 1);
        root.Children.Add(_galleryScroll);
        _galleryRoot = root;
        return root;
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
                Stretch = Stretch.UniformToFill,
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
            var col = new StackPanel { Spacing = 2 };
            col.Children.Add(image);
            col.Children.Add(name);
            var border = new Border
            {
                Width = GalleryTile + 12,
                Padding = new Thickness(6),
                CornerRadius = new CornerRadius(4),
                BorderThickness = new Thickness(vm.Selected ? 2 : 0),
                BorderBrush = Brush(Title),
                Child = col,
            };

            void SetThumb()
            {
                if (string.IsNullOrEmpty(vm.ThumbPath)) { image.Source = null; return; }
                try
                {
                    // The cache is JPEG-512; decoding it at 512 for a 152 DIP
                    // tile is 8x the pixels for nothing on a 2000-file dump.
                    image.Source = new BitmapImage(new Uri(vm.ThumbPath)) { DecodePixelWidth = 320 };
                }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
            SetThumb();

            void OnChanged(object? _, System.ComponentModel.PropertyChangedEventArgs e)
            {
                if (e.PropertyName is nameof(FolderItemVm.ThumbPath) or null) SetThumb();
                if (e.PropertyName is nameof(FolderItemVm.Selected) or null)
                    border.BorderThickness = new Thickness(vm.Selected ? 2 : 0);
            }
            vm.PropertyChanged += OnChanged;
            // Recycling without this leaks a handler per realisation, and every
            // thumb completion then walks a growing list of dead elements.
            border.Tag = (Action)(() => vm.PropertyChanged -= OnChanged);

            border.PointerPressed += (_, _) => Send(Command.GalleryActivate, vm.Index);
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
    public int Reserved;
}
