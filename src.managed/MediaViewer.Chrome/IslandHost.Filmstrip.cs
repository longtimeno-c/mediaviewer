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
                _folderSession?.Dispose();
                _folderSession = MediaViewerSession.Borrow(checked((IntPtr)args.Session));
                StartDrain();
            }

            _filmstrip?.Dispose();
            _filmstrip = new DesktopWindowXamlSource();
            _filmstrip.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            int strip = Math.Max((int)(FilmstripDip * (args.Dpi <= 0 ? 96 : args.Dpi) / 96.0), 1);
            int y = Math.Max(args.ClientHeight - strip, 0);
            Move(_filmstrip, args.ClientWidth, strip, y);
            _filmstrip.Content = BuildFilmstrip();
            Move(_filmstrip, args.ClientWidth, strip, y);
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
            _completionWait?.Unregister(null);
            _completionWait = null;
            _folderSession?.Dispose();
            _folderSession = null;
            if (_filmstrip is not null)
            {
                _filmstrip.Content = null;
                _filmstrip.Dispose();
                _filmstrip = null;
            }
            Items.Clear();
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
        foreach (var c in _folderSession.Drain())
        {
            if (c.Kind is MvCompletionKind.FolderReady or MvCompletionKind.FolderChanged)
            {
                ReloadItems();
            }
            else if (c.Kind == MvCompletionKind.ThumbReady && c.Status == MvStatus.Ok)
            {
                int index = (int)c.Payload;
                UpdateThumb(index);
            }
            else if (c.Kind == MvCompletionKind.FolderSelected && c.Status == MvStatus.Ok)
            {
                SetSelected((int)c.Payload);
            }
        }
    }

    private static void ReloadItems()
    {
        if (_folderSession is null) return;
        Items.Clear();
        uint count = _folderSession.FolderCount;
        for (uint i = 0; i < count; ++i)
        {
            MvFolderItem rec = _folderSession.FolderItemAt(i);
            Items.Add(new FolderItemVm
            {
                Index = (int)i,
                Name = _folderSession.FolderItemName(i),
                ThumbPath = _folderSession.FolderItemThumbPath(i),
                Selected = (rec.Flags & 1) != 0,
            });
        }
        int selected = -1;
        for (int i = 0; i < Items.Count; ++i)
        {
            if (Items[i].Selected) { selected = i; break; }
        }
        if (selected >= 0) ScrollTo(selected);
    }

    private static void SetSelected(int index)
    {
        for (int i = 0; i < Items.Count; ++i)
            Items[i].Selected = i == index;
        ScrollTo(index);
    }

    private static void ScrollTo(int index)
    {
        if (index < 0 || index >= Items.Count) return;
        _repeater?.UpdateLayout();
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
        _repeater = new ItemsRepeater
        {
            ItemsSource = Items,
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
        scroll.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Left) { Send(Command.Prev); e.Handled = true; }
            if (e.Key == Windows.System.VirtualKey.Right) { Send(Command.Next); e.Handled = true; }
        };
        return new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
            Height = FilmstripDip,
            Children = { scroll },
        };
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
            col.Children.Add(image);
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
            border.PointerPressed += (_, _) => Send(Command.SelectItem, vm.Index);
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
