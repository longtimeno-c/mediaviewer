// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace MediaViewer.Chrome;

/// <summary>
/// Issue #429, the page sidebar (docs/design/04 "Page sidebar"): a strip of page
/// thumbnails down the left of a PDF, DOCX or PPTX with the filmstrip hidden, the
/// Mac's PageSidebarView. Native decides when it shows (main.cpp
/// <c>page_sidebar_wanted</c>), reserves its width on the canvas and pushes the
/// page count and the page on screen (<c>SetPageSidebar</c>); this island asks
/// the core for the thumbnails of the pages it shows and sends a click back as
/// <see cref="Command.SelectPage"/>.
/// </summary>
/// <remarks>
/// A panel island like the folder tree (IslandHost.Panels.cs): created parked in
/// AttachPanels, placed by <c>ShowPageSidebar</c>. Thumbnails render on the core's
/// background workers from one open of the file and arrive as
/// <see cref="MvCompletionKind.PageThumbReady"/> in the folder drain; nothing here
/// reads a file but the JPEG the core names, which BitmapImage decodes itself.
/// </remarks>
public static partial class IslandHost
{
    internal const int PageSidebarArgsSize = 12;
    // One row: the 136 DIP thumbnail box, its number and the gap. Fixed, so
    // the rows on screen follow from the scroll offset with no layout pass.
    private const double PageRowDip = 176;
    private const int PageThumbDecodeWidth = 208;  // a 104 DIP page at 2x

    private static DesktopWindowXamlSource? _pageSidebar;
    private static ItemsRepeater? _pageRepeater;
    private static ScrollViewer? _pageScroll;
    private static readonly ObservableCollection<PageVm> Pages = new();
    private static int _pageCurrent = -1;
    private static int _pageStop = -1;  // native's name for the document listed
    // The core's name for the document the thumbnails belong to
    // (mv_folder_page_thumbs); a completion with another one is stale.
    private static uint _pageSerial;

    public static int ShowPageSidebar(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _pageSidebar, BuildPageSidebar,
        onShown: () => RequestPageThumbs(),
        onHidden: DropPageSidebarUi);

    /// <summary>The page count (0: nothing to show) and the page on screen.</summary>
    public static int SetPageSidebar(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < PageSidebarArgsSize) return unchecked((int)0x80070057);
            ChromePageSidebarArgs args = Marshal.PtrToStructure<ChromePageSidebarArgs>(arg);
            int count = Math.Max(0, args.Count);
            if (count != Pages.Count || args.Stop != _pageStop)
            {
                _pageStop = args.Stop;
                // Rebuilt rather than trimmed: another document's pages share
                // nothing with these.
                Pages.Clear();
                for (int i = 0; i < count; ++i) Pages.Add(new PageVm { Page = i });
                _pageCurrent = -1;
            }
            SetPageCurrent(args.Current);
            RequestPageThumbs();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static void SetPageCurrent(int page)
    {
        if (page == _pageCurrent) return;
        if (_pageCurrent >= 0 && _pageCurrent < Pages.Count) Pages[_pageCurrent].Current = false;
        _pageCurrent = page;
        if (page < 0 || page >= Pages.Count) return;
        Pages[page].Current = true;
        // The page on screen stays in view as the document scrolls; one that
        // is already visible does not move the strip under the pointer.
        if (_pageScroll is null) return;
        double top = page * PageRowDip;
        double offset = _pageScroll.VerticalOffset;
        if (top >= offset && top + PageRowDip <= offset + _pageScroll.ViewportHeight) return;
        double to = top - (_pageScroll.ViewportHeight - PageRowDip) * 0.5;
        _pageScroll.ChangeView(null, Math.Max(0, to), null, disableAnimation: true);
    }

    // The rows on screen and two either side. The core renders only what is not
    // cached or already in flight, and a later request replaces the window, so a
    // fling renders where it lands rather than everything it passed.
    private static void RequestPageThumbs()
    {
        if (_folderSession is null || Pages.Count < 2) return;
        double offset = _pageScroll?.VerticalOffset ?? 0;
        double viewport = _pageScroll?.ViewportHeight is > 0 and double v ? v : 1080;
        int first = Math.Max(0, (int)(offset / PageRowDip) - 2);
        int last = Math.Min(Pages.Count - 1, (int)((offset + viewport) / PageRowDip) + 2);
        if (first > last) return;
        uint serial;
        try { serial = _folderSession.FolderPageThumbs((uint)first, (uint)(last - first + 1)); }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return;
        }
        if (serial == 0 || serial == _pageSerial) return;
        // Another document: the pictures on the rows were the last one's.
        _pageSerial = serial;
        foreach (PageVm vm in Pages) vm.ThumbPath = "";
    }

    // From the folder drain (IslandHost.Filmstrip.cs).
    private static void OnPageThumbReady(uint serial, int page)
    {
        if (_folderSession is null || serial != _pageSerial || page < 0 || page >= Pages.Count) return;
        Pages[page].ThumbPath = _folderSession.FolderPageThumbPath((uint)page);
    }

    private static UIElement BuildPageSidebar()
    {
        _pageRepeater = new ItemsRepeater
        {
            Layout = new StackLayout { Orientation = Orientation.Vertical, Spacing = 0 },
            ItemTemplate = new PageSidebarFactory(),
        };
        _pageScroll = new ScrollViewer
        {
            Content = _pageRepeater,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollMode = ScrollMode.Disabled,
            VerticalScrollMode = ScrollMode.Enabled,
            // Clicks turn the page; the keys stay with the canvas.
            IsTabStop = false,
            AllowFocusOnInteraction = false,
            Padding = new Thickness(0, 10, 0, 10),
        };
        AutomationProperties_SetName(_pageScroll, "Pages");
        _pageScroll.ViewChanged += (_, _) => RequestPageThumbs();
        var root = new Grid
        {
            RequestedTheme = IslandTheme,
            Background = Brush(ChromeColour.PanelBg),
            BorderBrush = Brush(ChromeColour.Hairline),
            BorderThickness = new Thickness(0, 0, 1, 0),
            Children = { _pageScroll },
        };
        // Bound once parented, as the filmstrip's repeater is (BuildFilmstrip).
        root.Loaded += (_, _) =>
        {
            if (_pageRepeater is not null && _pageRepeater.ItemsSource is null) _pageRepeater.ItemsSource = Pages;
            int current = _pageCurrent;
            _pageCurrent = -1;
            SetPageCurrent(current);
            RequestPageThumbs();
        };
        ThemeField.Lay(root);
        return root;
    }

    private static void DropPageSidebarUi()
    {
        if (_pageRepeater is not null)
        {
            try { _pageRepeater.ItemsSource = null; }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        }
        _pageRepeater = null;
        _pageScroll = null;
    }

    private sealed class PageSidebarFactory : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var vm = (PageVm)args.Data;
            var image = new Image { Width = 104, Height = 136, Stretch = Stretch.Uniform };
            var paper = new Border
            {
                Width = 112,
                Height = 144,
                Padding = new Thickness(4),
                CornerRadius = Corner(RadiusButton),
                BorderThickness = new Thickness(2),
                Child = image,
            };
            var number = new TextBlock
            {
                Text = (vm.Page + 1).ToString(System.Globalization.CultureInfo.CurrentCulture),
                FontFamily = UiFont,
                FontSize = 11,
                HorizontalAlignment = HorizontalAlignment.Center,
            };
            var row = new StackPanel
            {
                Height = PageRowDip,
                Spacing = 4,
                HorizontalAlignment = HorizontalAlignment.Center,
                Children = { paper, number },
            };
            AutomationProperties_SetName(row, $"Page {vm.Page + 1}");

            void SetThumb()
            {
                if (string.IsNullOrEmpty(vm.ThumbPath))
                {
                    image.Source = null;
                    return;
                }
                try
                {
                    image.Source = new BitmapImage(new Uri(vm.ThumbPath)) { DecodePixelWidth = PageThumbDecodeWidth };
                }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
            void SetCurrent()
            {
                paper.BorderBrush = vm.Current ? Brush(ChromeColour.Accent) : Brush(Microsoft.UI.Colors.Transparent);
                number.Foreground = Brush(vm.Current ? Title : Body);
            }
            SetThumb();
            SetCurrent();

            void OnChanged(object? _, PropertyChangedEventArgs e)
            {
                if (e.PropertyName is nameof(PageVm.ThumbPath) or null) SetThumb();
                if (e.PropertyName is nameof(PageVm.Current) or null) SetCurrent();
            }
            vm.PropertyChanged += OnChanged;
            row.Tag = (Action)(() => vm.PropertyChanged -= OnChanged);
            row.Tapped += (_, _) => Send(Command.SelectPage, vm.Page);
            return row;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            if (args.Element is StackPanel { Tag: Action unsubscribe } row)
            {
                unsubscribe();
                row.Tag = null;
            }
        }
    }
}

internal sealed class PageVm : INotifyPropertyChanged
{
    private string _thumbPath = "";
    private bool _current;
    public int Page { get; init; }
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
    public bool Current
    {
        get => _current;
        set
        {
            if (_current == value) return;
            _current = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Current)));
        }
    }
    public event PropertyChangedEventHandler? PropertyChanged;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromePageSidebarArgs
{
    public int Count;
    public int Current;
    public int Stop;
}
