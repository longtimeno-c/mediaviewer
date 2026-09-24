// SPDX-License-Identifier: GPL-2.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    private static DesktopWindowXamlSource? _filmstrip;
    private static MediaViewerSession? _folderSession;
    private static RegisteredWaitHandle? _completionWait;
    private static readonly ObservableCollection<FolderItemVm> Items = new();
    private static ItemsRepeater? _repeater;
    private static ScrollViewer? _filmstripScroll;
    private static FrameworkElement? _filmstripRoot;
    private static int _selectedIndex = -1;
    private const int FilmstripDip = 112;
    private const double ItemStride = 102; // 96 width + 6 spacing

    public static int AttachFilmstrip(IntPtr arg, int sizeBytes)
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

            if (args.Session != 0)
            {
                StopVideoControls();
                _folderSession?.Dispose();
                _folderSession = MediaViewerSession.Borrow(checked((IntPtr)args.Session));
                StartDrain();
                StartVideoControls(parent);
            }

            DisposeSource(ref _filmstrip);
            _filmstrip = new DesktopWindowXamlSource();
            EnsureFocusHook();
            _filmstrip.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // Park with no content, same as the gallery. Building the repeater
            // here and then immediately hiding it left an ItemsRepeater bound
            // to `Items`; the first folder-open show bound a second one and
            // AccessViolation'd in set_ItemsSource.
            Move(_filmstrip, 1, 1, args.ClientHeight);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("chrome AttachFilmstrip: {0}", ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ResizeFilmstrip(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < ResizeArgsSize) return unchecked((int)0x80070057);
            ChromeResizeArgs args = Marshal.PtrToStructure<ChromeResizeArgs>(arg);
            Move(_filmstrip, args.Width, args.Height, args.Y);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int DetachFilmstrip(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            UnhookFocus();
            _completionWait?.Unregister(null);
            _completionWait = null;
            StopVideoControls();
            _folderSession?.Dispose();
            _folderSession = null;
            UnbindSharedItems();
            DisposeSource(ref _filmstrip);
            _filmstripRoot = null;
            Items.Clear();
            _selectedIndex = -1;
            SetBusy(false);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static void StartDrain()
    {
        _completionWait?.Unregister(null);
        if (_folderSession is null) return;
        StartAddons();  // Milestone G: needs the session (IslandHost.Addons.cs)
        _completionWait = ThreadPool.RegisterWaitForSingleObject(
            _folderSession.CompletionSignal,
            (_, _) =>
            {
                _dispatcher?.DispatcherQueue.TryEnqueue(DrainFolder);
            },
            null,
            -1,
            false);
    }

    private static void DrainFolder()
    {
        if (_folderSession is null) return;

        // Coalesce. Holding an arrow key produces a selection change per key
        // repeat; walking every view model and forcing a layout pass for each
        // one turns the UI thread into the bottleneck the decode pool no longer
        // is. Only the last selection in a batch is worth applying.
        int select = -1;
        bool? busy = null;
        bool video = false;
        foreach (var c in _folderSession.Drain())
        {
            if (AddonCompletion.TryFrom(c, out AddonCompletion addon))
            {
                OnAddonCompletion(addon);  // Milestone G (IslandHost.Addons.cs)
            }
            else if (c.Kind is MvCompletionKind.FolderReady or MvCompletionKind.FolderChanged)
            {
                ReloadItems();
                select = -1;
                busy = null;
            }
            else if (c.Kind == MvCompletionKind.ThumbReady && c.Status == MvStatus.Ok)
            {
                int index = (int)c.Payload;
                UpdateThumb(index);
            }
            else if (c.Kind == MvCompletionKind.FolderSelected && c.Status == MvStatus.Ok)
            {
                select = (int)c.Payload;
                busy = true;
            }
            else if (c.Kind is MvCompletionKind.ImageOpened or MvCompletionKind.VideoOpened)
            {
                // Published (or failed) for the current selection — either way
                // there is nothing left to wait for.
                busy = false;
                video = true;
            }
            else if (c.Kind is MvCompletionKind.VideoState or MvCompletionKind.VideoEnded)
            {
                // The core changed state on its own (end of clip, device loss)
                // or acknowledged a change we asked for. Either way the Play /
                // Pause label and the transport strip react now instead of on
                // the next 150 ms tick. The timer stays: it is the position
                // pump for the scrubber and the clock, and no completion can
                // replace something that moves continuously.
                video = true;
            }
        }
        if (select >= 0) SetSelected(select);
        if (busy is bool want) SetBusy(want);
        if (video) UpdateVideoControls();
    }

    private static void ReloadItems()
    {
        if (_folderSession is null) return;
        Items.Clear();
        _selectedIndex = -1;
        uint count = _folderSession.FolderCount;
        for (uint i = 0; i < count; ++i)
        {
            MvFolderItem rec = _folderSession.FolderItemAt(i);
            Items.Add(new FolderItemVm
            {
                Index = (int)i,
                Name = _folderSession.FolderItemName(i),
                Path = _folderSession.FolderItemPath(i),
                PairPath = rec.PairKind == MvPairKind.None ? "" : _folderSession.FolderItemPairPath(i),
                Badge = BadgeFor(rec),
                ThumbPath = _folderSession.FolderItemThumbPath(i),
                Selected = (rec.Flags & MvFolderItem.FlagSelected) != 0,
            });
        }
        int selected = -1;
        for (int i = 0; i < Items.Count; ++i)
        {
            if (Items[i].Selected) { selected = i; break; }
        }
        _selectedIndex = selected;
        if (selected >= 0) ScrollTo(selected);
        UpdateGalleryCount();
        // Native does not drain completions while an island is attached, so
        // this is how it learns a listing landed. Do not Send synchronously:
        // FolderReady -> apply_view_state -> ShowFilmstrip -> BuildFilmstrip
        // re-entered from DrainFolder and AccessViolation'd in set_ItemsSource.
        int listed = Items.Count;
        if (_dispatcher is not null)
            _dispatcher.DispatcherQueue.TryEnqueue(() => Send(Command.FolderReady, listed));
        else
            Send(Command.FolderReady, listed);
    }

    // PR 7 (plan/04): "LIVE" on a Live Photo stop, "RAW" on a RAW+JPEG stop and
    // on a RAW with no JPEG beside it. Decided at scan time; nothing is read.
    private static string BadgeFor(MvFolderItem rec) => rec.PairKind switch
    {
        MvPairKind.LivePhoto => "LIVE",
        MvPairKind.RawJpeg => "RAW",
        _ => (rec.Flags & MvFolderItem.FlagPrimaryRaw) != 0 ? "RAW" : "",
    };

    // The thumbnail with its badge laid over the top-left corner. Not hit-test
    // visible and not focusable, so taps, drags and keyboard focus are the
    // tile's exactly as before.
    private static UIElement WithBadge(Image image, string badge)
    {
        if (string.IsNullOrEmpty(badge)) return image;
        var grid = new Grid();
        grid.Children.Add(image);
        grid.Children.Add(new Border
        {
            Background = new SolidColorBrush(ColorHelper.FromArgb(0xC0, 0, 0, 0)),
            CornerRadius = new CornerRadius(2),
            Padding = new Thickness(3, 0, 3, 1),
            Margin = new Thickness(3),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            IsHitTestVisible = false,
            Child = new TextBlock
            {
                Text = badge,
                FontFamily = UiFont,
                FontSize = 10,
                Foreground = new SolidColorBrush(Colors.White),
            },
        });
        return grid;
    }

    private static void SetSelected(int index)
    {
        // Touch two items, not two thousand: the old loop was O(folder) per key
        // repeat and every write raised PropertyChanged.
        if (_selectedIndex >= 0 && _selectedIndex < Items.Count) Items[_selectedIndex].Selected = false;
        if (index >= 0 && index < Items.Count) Items[index].Selected = true;
        _selectedIndex = index;
        ScrollTo(index);
        GalleryScrollTo(index);
    }

    private static void ScrollTo(int index)
    {
        if (index < 0 || index >= Items.Count) return;
        // No UpdateLayout() here. A synchronous layout pass of the strip on
        // every arrow-key repeat is the UI thread doing the pool's old job.
        UIElement? el = _repeater?.TryGetElement(index);
        if (el is not null)
        {
            el.StartBringIntoView(new BringIntoViewOptions
            {
                AnimationDesired = false,
                HorizontalAlignmentRatio = 0.5,
            });
            return;
        }
        if (_filmstripScroll is null) return;
        double x = index * ItemStride - _filmstripScroll.ViewportWidth * 0.5 + 48;
        _filmstripScroll.ChangeView(Math.Max(0, x), null, null, disableAnimation: true);
    }

    private static void UpdateThumb(int index)
    {
        if (_folderSession is null || index < 0 || index >= Items.Count) return;
        Items[index].ThumbPath = _folderSession.FolderItemThumbPath((uint)index);
    }

    private static UIElement BuildFilmstrip()
    {
        ReleaseRepeater(ref _repeater);
        // Bind Items after the tree is parented (RealiseFilmstrip). Setting
        // ItemsSource here, before the repeater has a XamlRoot, is a native
        // AV in IItemsRepeaterMethods.set_ItemsSource on a populated listing.
        _repeater = new ItemsRepeater
        {
            Layout = new StackLayout { Orientation = Orientation.Horizontal, Spacing = 6 },
            ItemTemplate = new FilmstripFactory(),
        };
        _filmstripScroll = new ScrollViewer
        {
            Content = _repeater,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Disabled,
            HorizontalScrollMode = ScrollMode.Enabled,
            VerticalScrollMode = ScrollMode.Disabled,
        };
        var scroll = _filmstripScroll;
        scroll.CharacterReceived += OnTypeahead;  // plan/16 typeahead
        scroll.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Left) { Send(Command.Prev); e.Handled = true; }
            if (e.Key == Windows.System.VirtualKey.Right) { Send(Command.Next); e.Handled = true; }
        };
        var root = new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
            Height = FilmstripDip,
            Children = { scroll },
        };
        _filmstripRoot = root;
        WireFileDrop(root);
        return root;
    }

    private sealed class FilmstripFactory : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var vm = (FolderItemVm)args.Data;
            var image = new Image
            {
                Width = 80,
                Height = 80,
                Stretch = Stretch.UniformToFill,
            };
            if (!string.IsNullOrEmpty(vm.ThumbPath))
            {
                try { image.Source = new BitmapImage(new Uri(vm.ThumbPath)); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
            var name = new TextBlock
            {
                Text = vm.Name,
                FontFamily = UiFont,
                FontSize = 12,
                Foreground = Brush(Body),
                TextTrimming = TextTrimming.CharacterEllipsis,
                MaxWidth = 88,
            };
            var col = new StackPanel { Spacing = 4 };
            col.Children.Add(WithBadge(image, vm.Badge));
            col.Children.Add(name);
            var border = new Border
            {
                Width = 96,
                Padding = new Thickness(4),
                BorderThickness = new Thickness(vm.Selected ? 2 : 0),
                BorderBrush = Brush(Title),
                Child = col,
            };
            vm.PropertyChanged += (_, e) =>
            {
                if (e.PropertyName is nameof(FolderItemVm.ThumbPath) or null)
                {
                    if (string.IsNullOrEmpty(vm.ThumbPath)) return;
                    try { image.Source = new BitmapImage(new Uri(vm.ThumbPath)); }
                    catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
                }
                if (e.PropertyName is nameof(FolderItemVm.Selected) or null)
                    border.BorderThickness = new Thickness(vm.Selected ? 2 : 0);
            };
            border.Tapped += (_, _) => Send(Command.SelectItem, vm.Index);
            WireFileDrag(border, vm);
            return border;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            _ = args;
        }
    }
}

internal sealed class FolderItemVm : INotifyPropertyChanged
{
    private string _thumbPath = "";
    private bool _selected;
    public int Index { get; set; }
    public string Name { get; set; } = "";
    public string Path { get; set; } = "";
    // PR 7: the other half of a paired stop (RAW or Live Photo MOV), else "".
    public string PairPath { get; set; } = "";
    // PR 7: "RAW", "LIVE" or "".
    public string Badge { get; set; } = "";
    public bool Selected
    {
        get => _selected;
        set
        {
            if (_selected == value) return;
            _selected = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Selected)));
        }
    }
    public string ThumbPath
    {
        get => _thumbPath;
        set
        {
            if (_thumbPath == value) return;
            _thumbPath = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ThumbPath)));
        }
    }
    public event PropertyChangedEventHandler? PropertyChanged;
}
