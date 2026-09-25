// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;
using Windows.Graphics;
using Windows.UI;

namespace MediaViewer.Chrome;

/// <summary>
/// hostfxr entry points. Each method is the default component signature
/// <c>(IntPtr arg, int sizeBytes) -> int</c> so native code does not have to
/// name a custom delegate type.
/// </summary>
/// <remarks>
/// Pixels never cross this line. The island is command-bar chrome; pan/zoom
/// stay on the native window procedure (plan/02, plan/14).
/// </remarks>
public static partial class IslandHost
{
    internal const int AttachArgsSize = 40;
    internal const int ResizeArgsSize = 16;
    internal const int FilmstripArgsSize = 48;
    internal const int ShowArgsSize = 16;
    internal const int FlagsArgsSize = 8;
    internal const int RateArgsSize = 8;

    internal static class Command
    {
        public const int Open = 1;
        public const int Fit = 2;
        public const int OneToOne = 3;
        public const int ZoomIn = 4;
        public const int ZoomOut = 5;
        public const int ZoomPreset = 6;
        public const int Overlay = 7;
        public const int SelectItem = 8;
        public const int Prev = 9;
        public const int Next = 10;
        public const int OpenFolder = 11;
        public const int ToggleGallery = 12;
        public const int CloseGallery = 13;
        public const int GalleryActivate = 14;
        public const int SetSettings = 15;
        public const int FolderReady = 16;
        public const int ToggleFilmstrip = 17;
        public const int VideoActive = 18;
        public const int SetRate = 19;
        public const int FocusChanged = 20;
        // Notifications added after the command table filled the low ids.
        public const int Popup = 1000;
        public const int Rebind = 1001;
        public const int ResetKeys = 1002;
        public const int UpdateRestart = 1003;  // PR 8: chrome, not a keyed command
        // PR 9. TreeOpen: native pulls the chosen folder with TakeTreePath.
        // SetSort: arg is the packed order (key in bits 0-2, descending in bit 3).
        public const int TreeOpen = 1004;
        public const int SetSort = 1005;
        // PR 10: the export dialog was confirmed; arg is the packed choice
        // (edit_session.h pack_export). Cancel sends nothing.
        public const int Export = 1006;
        // PR 26 folder tiles.
        public const int OpenSubfolder = 1007;
        public const int OpenCrumb = 1008;
        public const int GalleryColumns = 1009;
        public const int FolderUp = 115;  // mirrors command_id::folder_up
        // Milestone G: arg 1/0 = the Import add-on is loaded / absent (its
        // commands exist only while it is). OpenPath: native pulls the path
        // with TakeTreePath and opens it in the viewer.
        public const int AddonState = 1010;
        public const int OpenPath = 1011;
        // PR 12: the comment field was committed (native pulls the text with
        // TakeTreePath, as for OpenPath); Revert puts the file's fields back.
        public const int MetaComment = 1012;
        public const int MetaRevert = 1013;
        // PR 14: the clip tools flyout was confirmed; arg is the packed choice
        // (trim_state.h pack_clip_choice). PR 13: the drain saw a keyframe
        // index answer; arg is its request id (chrome_host.h).
        public const int ClipTool = 1014;
        public const int ClipIndex = 1015;
        // Command-table ids the island can post (commands.h).
        public const int Clipping = 46;
        public const int Fullscreen = 41;
        public const int Help = 75;
        public const int RevealInExplorer = 80;
        public const int OpenSettings = 84;
        // Keyed commands the View menu and the panes' close buttons also send
        // (mirrors command_id; chrome_host.h pins both with static_asserts).
        public const int FolderTree = 78;
        public const int MetadataPane = 92;
        // PR 11: the adjust pane's close button (the Shift+A command) and its
        // sliders, which carry their value (commands.h adjust_*).
        public const int AdjustPane = 118;
        public const int AdjustExposure = 119;
        public const int AdjustContrast = 120;
        public const int AdjustSaturation = 121;
        public const int AdjustTemperature = 122;
        public const int AdjustTint = 123;
        public const int AdjustReset = 124;
        // PR 12: the pane's stars post the rating keys' ids (commands.h
        // set_rating_0..5); 0 clears.
        public const int SetRating0 = 127;
        // PR 13 / 14 keyed commands the transport and the Jobs pane also send.
        public const int TrimMode = 134;
        public const int TrimKeyframe = 139;
        public const int TrimReencode = 140;
        public const int JobsPane = 143;
        public const int ClipToolsFlyout = 144;

        // Mirrors chrome_command_checksum() in chrome_host.h: same constants,
        // same order, same arithmetic. Probe hands it to native for the test.
        internal static int Checksum()
        {
            int[] ids =
            {
                Open, Fit, OneToOne, ZoomIn, ZoomOut, ZoomPreset, Overlay, SelectItem, Prev, Next,
                OpenFolder, ToggleGallery, CloseGallery, GalleryActivate, SetSettings, FolderReady,
                ToggleFilmstrip, VideoActive, SetRate, FocusChanged, Popup, Rebind, ResetKeys,
                UpdateRestart, TreeOpen, SetSort, Export, OpenSubfolder, OpenCrumb, GalleryColumns, AddonState, OpenPath,
                MetaComment, MetaRevert,
                ClipTool, ClipIndex,
            };
            unchecked
            {
                int h = 17;
                foreach (int id in ids) h = h * 31 + id;
                return h;
            }
        }
    }

    // Mirrors mv::shell::focus_kind (key_router.h).
    internal static class FocusKind
    {
        public const int CommandBar = 1;
        public const int Filmstrip = 2;
        public const int Gallery = 3;
        public const int Transport = 4;
        public const int Text = 5;
        // PR 9: the metadata pane or the folder tree holds focus (plan/16 "Pane").
        public const int Pane = 6;
    }

    // IslandWindow asks for the pane islands by these ids (past the focus kinds).
    internal const int PaneMetaIsland = 6;
    internal const int PaneTreeIsland = 7;

    // Mirrors mv::shell::view_settings. The native side owns the file; the
    // menu is a view of it, pushed in by ApplySettings so a T keypress and the
    // checkmarks cannot drift apart.
    internal static class SettingFlag
    {
        public const int FilmstripForFolder = 1 << 0;
        public const int FilmstripForImage = 1 << 1;
        public const int Wrap = 1 << 2;
        public const int StickyZoom = 1 << 3;
        public const int BackgroundShift = 4;
        public const int BackgroundMask = 3 << 4;
        // [update] auto_check, not a view setting (update_guard.h kChromeFlagUpdateAutoCheck).
        public const int UpdateAutoCheck = 1 << 8;
        // [telemetry] enabled / asked (telemetry.h kChromeFlagTelemetry*).
        // Consent, default off. Asked records that the first-run screen has
        // been answered - either way. Asked is not consent (plan/13 Part 3).
        public const int Telemetry = 1 << 9;
        public const int TelemetryAsked = 1 << 10;
    }

    // Telemetry is absent from this initial word on purpose: until native
    // pushes the real settings in, the chrome assumes off (plan/13).
    // Packed sort order (key in bits 0-2: name, modified, size, type, date taken;
    // descending in bit 3). Native owns it; the menu and Settings are a view of it.
    private static int _sortPacked;

    private static int _settingFlags = SettingFlag.FilmstripForFolder | SettingFlag.Wrap | SettingFlag.UpdateAutoCheck;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void NativeCommand(IntPtr context, int command, float arg);

    private static DispatcherQueueController? _dispatcher;
    private static WindowsXamlManager? _xaml;
    private static DesktopWindowXamlSource? _source;
    private static NativeCommand? _onCommand;
    private static IntPtr _context;

    public static int Probe(IntPtr arg, int sizeBytes)
    {
        // With a 4-byte buffer, also report the command-id checksum so the
        // native test can prove both sides number the wire the same way.
        if (arg != IntPtr.Zero && sizeBytes >= 4) Marshal.WriteInt32(arg, Command.Checksum());
        if (Marshal.SizeOf<ChromeAttachArgs>() != AttachArgsSize) return -2;
        if (Marshal.SizeOf<ChromeResizeArgs>() != ResizeArgsSize) return -3;
        if (Marshal.SizeOf<ChromeFilmstripArgs>() != FilmstripArgsSize) return -4;
        if (Marshal.SizeOf<ChromeShowArgs>() != ShowArgsSize) return -5;
        if (Marshal.SizeOf<ChromeFlagsArgs>() != FlagsArgsSize) return -6;
        if (Marshal.SizeOf<ChromeRateArgs>() != RateArgsSize) return -7;
        if (Marshal.SizeOf<ChromePopupArgs>() != PopupArgsSize) return -8;
        if (Marshal.SizeOf<ChromeTableArgs>() != TableArgsSize) return -9;
        if (Marshal.SizeOf<ChromePanelArgs>() != PanelArgsSize) return -10;
        if (Marshal.SizeOf<ChromeMetaArgs>() != MetaDataArgsSize) return -11;
        if (Marshal.SizeOf<ChromeMetaEditArgs>() != MetaEditArgsSize) return -12;
        return AttachArgsSize;
    }

    /// <summary>
    /// The island's top-level bridge window. In: int32 island (FocusKind),
    /// int32 reserved. Out: int64 HWND at offset 8 (0 if not attached).
    /// Native caches these at attach and classifies GetFocus() with IsChild,
    /// so focus is never read from a notification that can go stale.
    /// </summary>
    public static int IslandWindow(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 16) return unchecked((int)0x80070057);
            int island = Marshal.ReadInt32(arg);
            DesktopWindowXamlSource? source = island switch
            {
                FocusKind.CommandBar => _source,
                FocusKind.Filmstrip => _filmstrip,
                FocusKind.Gallery => _gallery,
                FocusKind.Transport => _transport,
                PaneMetaIsland => _metaPane,
                PaneTreeIsland => _tree,
                _ => null,
            };
            long hwnd = source?.SiteBridge is null
                ? 0
                : (long)Win32Interop.GetWindowFromWindowId(source.SiteBridge.WindowId);
            Marshal.WriteInt64(arg, 8, hwnd);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int Attach(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < AttachArgsSize) return unchecked((int)0x80070057);

            ChromeAttachArgs args = Marshal.PtrToStructure<ChromeAttachArgs>(arg);
            IntPtr parent = checked((IntPtr)args.ParentHwnd);
            if (parent == IntPtr.Zero) return unchecked((int)0x80070057);

            // DirectWrite snapshots the system collection at XAML init.
            // Register Cozette first so a named FontFamily can resolve.
            RegisterUiFont();
            EnsureApp();

            _context = checked((IntPtr)args.Context);
            _onCommand = args.OnCommand == 0
                ? null
                : Marshal.GetDelegateForFunctionPointer<NativeCommand>(checked((IntPtr)args.OnCommand));

            if (_source is not null) _source.TakeFocusRequested -= OnTakeFocusRequested;
            DisposeSource(ref _source);
            _source = new DesktopWindowXamlSource();
            _source.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // Constrain the island to the bar before assigning content so a
            // full-client default size never covers the canvas.
            Move(_source, args.ClientWidth, args.ClientHeight, 0);
            _source.TakeFocusRequested += OnTakeFocusRequested;
            _source.Content = BuildChrome();
            Move(_source, args.ClientWidth, args.ClientHeight, 0);
            EnsureFocusHook();
            StartUpdater();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            Console.Error.WriteLine("chrome Attach: {0}", ex);
            return unchecked((int)0x80004005);
        }
    }

    private static bool _focusHooked;

    // Set before any island is torn down. FocusManager.GotFocus is static and
    // fires while a DesktopWindowXamlSource disposes; an exception escaping a
    // XAML event there is a fail-fast in CoreUIComponents (0xC0000602), seen
    // on PR 6a soak exits. Native calls BeginDetach first; every Detach* also
    // unhooks in case it is called alone.
    private static bool _detaching;

    public static int BeginDetach(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        _detaching = true;  // only a whole-chrome teardown sets this
        UnhookFocus();
        return 0;
    }

    // Every Attach* calls this. A single island can be re-attached on its own
    // (chrome_host re-attach, a DPI or adapter move, 6f's folder tree toggling),
    // and its Detach* unhooks; without re-hooking here focus tracking would
    // silently stop for the rest of the session.
    private static void EnsureFocusHook()
    {
        _detaching = false;
        if (_focusHooked) return;
        Microsoft.UI.Xaml.Input.FocusManager.GotFocus += OnXamlGotFocus;
        _focusHooked = true;
    }

    // Unhooks without claiming the whole chrome is going away.
    private static void UnhookFocus()
    {
        if (!_focusHooked) return;
        try
        {
            Microsoft.UI.Xaml.Input.FocusManager.GotFocus -= OnXamlGotFocus;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
        _focusHooked = false;
    }

    // Null the static field before Dispose, so anything Dispose raises cannot
    // reach a half-disposed source through it. Content = null first: Dispose
    // of a live tree without it AVs in IDisposableMethods.Dispose (soak exit
    // after a folder open). A bare test HWND AVs on that assignment instead;
    // that path is not the lab.
    private static void DisposeSource(ref DesktopWindowXamlSource? source)
    {
        DesktopWindowXamlSource? old = source;
        source = null;
        if (old is null) return;
        try { old.Content = null; }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        old.Dispose();
    }

    // Filmstrip and gallery share `Items`. Two ItemsRepeaters bound to that
    // collection at once is a native AV in set_ItemsSource (folder-open crash:
    // attach built a strip, park dropped Content without unbinding, show built
    // a second repeater on the same ObservableCollection).
    private static void ReleaseRepeater(ref ItemsRepeater? repeater)
    {
        if (repeater is null) return;
        try { repeater.ItemsSource = null; }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        repeater = null;
    }

    private static void UnbindSharedItems()
    {
        ReleaseRepeater(ref _repeater);
        ReleaseRepeater(ref _galleryRepeater);
        ReleaseRepeater(ref _folderRepeater);
    }

    /// <summary>
    /// Tells the native key router which island holds focus, and whether it is
    /// a text control (plan/16: then every key but Esc belongs to the island).
    /// Native checks GetFocus() itself for the canvas, so a stale island value
    /// after focus returns to the swapchain is harmless.
    /// </summary>
    private static void OnXamlGotFocus(object? sender,
                                       Microsoft.UI.Xaml.Input.FocusManagerGotFocusEventArgs e)
    {
        _ = sender;
        if (_detaching || _onCommand is null) return;
        try
        {
            int kind = FocusKind.CommandBar;
            if (_popupTakesText ||
                e.NewFocusedElement is TextBox or PasswordBox or RichEditBox or AutoSuggestBox or FakeInput)
            {
                kind = FocusKind.Text;
            }
            else if (e.NewFocusedElement is UIElement element && element.XamlRoot is XamlRoot root)
            {
                if (OwnsRoot(_filmstrip, root)) kind = FocusKind.Filmstrip;
                else if (OwnsRoot(_gallery, root)) kind = FocusKind.Gallery;
                else if (OwnsRoot(_transport, root)) kind = FocusKind.Transport;
                else if (OwnsRoot(_metaPane, root) || OwnsRoot(_tree, root) || OwnsRoot(_adjustPane, root))
                {
                    // PR 11: the adjust pane too — its sliders own the arrows.
                    kind = FocusKind.Pane;
                }
            }
            Send(Command.FocusChanged, kind);
        }
        catch (Exception ex)
        {
            // Never let an exception out of a XAML event: that is a fail-fast.
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static bool OwnsRoot(DesktopWindowXamlSource? source, XamlRoot root) =>
        source?.Content is UIElement content && content.XamlRoot == root;

    public static int Resize(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < ResizeArgsSize) return unchecked((int)0x80070057);
            ChromeResizeArgs args = Marshal.PtrToStructure<ChromeResizeArgs>(arg);
            Move(_source, args.Width, args.Height, args.Y);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int NavigateFocus(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (_source is null) return 1;
            int reverse = 0;
            if (arg != IntPtr.Zero && sizeBytes >= 4) reverse = Marshal.ReadInt32(arg);
            var reason = reverse != 0
                ? XamlSourceFocusNavigationReason.Last
                : XamlSourceFocusNavigationReason.First;
            _source.NavigateFocus(new XamlSourceFocusNavigationRequest(reason));
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int Detach(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            UnhookFocus();
            if (_source is not null) _source.TakeFocusRequested -= OnTakeFocusRequested;
            DisposeSource(ref _source);
            _onCommand = null;
            _context = IntPtr.Zero;
            UnregisterUiFont();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ApplySettings(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < FlagsArgsSize) return unchecked((int)0x80070057);
            ChromeFlagsArgs args = Marshal.PtrToStructure<ChromeFlagsArgs>(arg);
            _settingFlags = args.Flags;
            _sortPacked = args.Sort;
            RefreshSettingsMenu();
            RefreshSettingsScreen();
            // PR 8: the telemetry first-run screen, once, when native reports
            // the choice has never been made (IslandHost.Telemetry.cs). It
            // rides this push rather than the bar build so a fresh profile
            // sees it on the first launch, not the second.
            if (_telemetryAnchor is not null) MaybeShowConsent(_telemetryAnchor);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>
    /// Native pushing the current playback rate in. The dropdown is a view of
    /// the rate, never a second place it is decided — the keyboard is the one
    /// router (plan/16), so Q/E and this menu cannot drift apart.
    /// </summary>
    public static int ApplyRate(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < RateArgsSize) return unchecked((int)0x80070057);
            ChromeRateArgs args = Marshal.PtrToStructure<ChromeRateArgs>(arg);
            SetSpeedSelection(args.Rate);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The ladder the keyboard steps through, mirrored from main.cpp's
    // kRateLadder. Every value is exact in float, so no epsilon is needed.
    private static readonly double[] SpeedLadder = { 0.25, 0.5, 1, 1.5, 2, 4 };
    private static ComboBox? _speed;
    private static bool _updatingSpeed;

    private static string SpeedLabel(double rate) =>
        rate == Math.Floor(rate) ? $"{rate:0}x" : $"{rate:0.##}x";

    private static void SetSpeedSelection(double rate)
    {
        if (_speed is null) return;
        int index = 0;
        double best = double.MaxValue;
        for (int i = 0; i < SpeedLadder.Length; ++i)
        {
            double delta = Math.Abs(SpeedLadder[i] - rate);
            if (delta < best) { best = delta; index = i; }
        }
        _updatingSpeed = true;
        _speed.SelectedIndex = index;
        _updatingSpeed = false;
    }

    // The dropdown is dead weight with no clip open, and a speed control on a
    // photo is a lie about what the key does.
    private static void SetSpeedVisible(bool visible)
    {
        if (_speed is not null) _speed.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
    }

    private static ComboBox BuildSpeed()
    {
        _speed = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            MinWidth = 88,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 8, 0),
            Visibility = Visibility.Collapsed,
        };
        foreach (double rate in SpeedLadder) _speed.Items.Add(SpeedLabel(rate));
        _speed.SelectedIndex = 2;  // 1x
        _speed.SelectionChanged += (_, _) =>
        {
            if (_updatingSpeed || _speed.SelectedIndex < 0) return;
            Send(Command.SetRate, (float)SpeedLadder[_speed.SelectedIndex]);
        };
        ToolTipService.SetToolTip(_speed, "Playback speed. Q / E skip ±2 s (hold to skim)");
        _speed.DropDownClosed += (_, _) => RestoreCanvasFocus();
        return _speed;
    }

    private static void EnsureApp()
    {
        if (_dispatcher is not null) return;
        CrashCapture.Install();  // plan/13: managed exceptions -> local report
        _dispatcher = DispatcherQueueController.CreateOnCurrentThread();
        // Island init path. Application after this throws; styles are set on
        // the bar itself (custom templates, not generic.xaml).
        _xaml = WindowsXamlManager.InitializeForCurrentThread();
    }

    /// <summary>
    /// Process-exit only: the host calls this from WM_CLOSE after every island
    /// has detached. Not part of Detach, because re-creating XAML on a thread
    /// whose dispatcher queue has shut down is not something to rely on, and
    /// the tests (and a later island toggle) attach again in one process.
    /// </summary>
    public static int ShutdownForExit(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            // 1: an island was never detached, so the runtime was left up.
            return ShutdownXaml() ? 0 : 1;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The XAML runtime for this thread goes last, after every island source
    // is disposed: WindowsXamlManager first, then the dispatcher queue. Left to
    // process exit it was torn down under live sources.
    private static bool ShutdownXaml()
    {
        if (_source is not null || _filmstrip is not null || _gallery is not null ||
            _transport is not null)
        {
            return false;
        }
        _busyDelay?.Stop();
        _busyDelay = null;
        _xaml?.Dispose();
        _xaml = null;
        _dispatcher?.ShutdownQueue();
        _dispatcher = null;
        return true;
    }

    private static void Send(int command, float arg = 0)
    {
        _onCommand?.Invoke(_context, command, arg);
    }

    private static void Move(DesktopWindowXamlSource? source, int width, int height, int y)
    {
        if (source?.SiteBridge is null) return;
        int w = Math.Max(width, 1);
        int h = Math.Max(height, 1);
        int top = Math.Max(y, 0);
        source.SiteBridge.MoveAndResize(new RectInt32(0, top, w, h));
    }

    private static void OnTakeFocusRequested(DesktopWindowXamlSource sender,
                                             DesktopWindowXamlSourceTakeFocusRequestedEventArgs args)
    {
        XamlSourceFocusNavigationReason reason = args.Request.Reason;
        if (reason is XamlSourceFocusNavigationReason.First or XamlSourceFocusNavigationReason.Last)
        {
            if (_settingsVisible && sender == _source)
            {
                // Tab stays in Settings instead of stranding focus on the canvas.
                sender.NavigateFocus(new XamlSourceFocusNavigationRequest(reason));
                return;
            }
            IntPtr hwnd = Win32Interop.GetWindowFromWindowId(sender.SiteBridge.WindowId);
            IntPtr root = GetAncestor(hwnd, GaRoot);
            if (root != IntPtr.Zero) SetFocus(root);
        }
    }

    // Same palette as the empty canvas (present_lab.cpp). The swapchain clear
    // is linear 0.016/0.018/0.024; encoded to sRGB that is about 33,35,42.
    // Type colours match the ImGui welcome: title / body / mute.
    private static readonly Color Canvas = ColorHelper.FromArgb(255, 33, 35, 42);
    private static readonly Color Title = ColorHelper.FromArgb(255, 220, 222, 228);
    private static readonly Color Body = ColorHelper.FromArgb(255, 150, 154, 164);
    private static readonly Color Hairline = ColorHelper.FromArgb(255, 58, 60, 68);

    private static SolidColorBrush Brush(Color c) => new(c);

    // CozetteVector — bitmap terminal face (Proggy/Dina lineage), same file
    // as the empty canvas. 16 DIP is readable on the 48 DIP bar. Created
    // after WindowsXamlManager; a static FontFamily ctor runs too early.
    private const string UiFontFile = "CozetteVector.ttf";
    private const string UiFontFamilyName = "CozetteVector";
    private const double UiFontSize = 16;
    private static FontFamily? _uiFont;
    private static FontFamily UiFont => _uiFont ??= LoadUiFont();
    private static string? _gdiFontPath;

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    private static extern int AddFontResourceExW(string name, uint fl, IntPtr pdv);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RemoveFontResourceExW(string name, uint fl, IntPtr pdv);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetModuleFileNameW(IntPtr hModule, [Out] char[] lpFilename, uint nSize);

    private static string? FindUiFontPath()
    {
        char[] buf = new char[32768];
        uint n = GetModuleFileNameW(IntPtr.Zero, buf, (uint)buf.Length);
        if (n > 0 && n < buf.Length)
        {
            string? dir = Path.GetDirectoryName(new string(buf, 0, (int)n));
            if (dir is not null)
            {
                string nextToExe = Path.Combine(dir, UiFontFile);
                if (File.Exists(nextToExe)) return nextToExe;
            }
        }
        string nextToChrome = Path.Combine(AppContext.BaseDirectory, UiFontFile);
        return File.Exists(nextToChrome) ? nextToChrome : null;
    }

    private static void RegisterUiFont()
    {
        if (_gdiFontPath is not null) return;
        string? path = FindUiFontPath();
        if (path is null) return;
        // fl=0 so DirectWrite can see it. FR_PRIVATE is GDI-only and WinUI
        // silently falls back to Segoe. Session-wide until UnregisterUiFont.
        if (AddFontResourceExW(path, 0, IntPtr.Zero) != 0)
            _gdiFontPath = path;
    }

    private static void UnregisterUiFont()
    {
        if (_gdiFontPath is null) return;
        RemoveFontResourceExW(_gdiFontPath, 0, IntPtr.Zero);
        _gdiFontPath = null;
        _uiFont = null;
    }

    private static FontFamily LoadUiFont()
    {
        if (FindUiFontPath() is not null)
        {
            try
            {
                // Unpackaged islands resolve ms-appx to the native exe directory
                // (microsoft-ui-xaml#10054). file:// throws and kills the island.
                return new FontFamily($"ms-appx:///{UiFontFile}#{UiFontFamilyName}");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
            }
            try
            {
                return new FontFamily(UiFontFamilyName);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
            }
        }
        return new FontFamily("Consolas");
    }

    private static ControlTemplate? _flatButtonTemplate;

    private static ControlTemplate? FlatButtonTemplate()
    {
        if (_flatButtonTemplate is not null) return _flatButtonTemplate;
        const string xaml =
            """
            <ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                             TargetType="Button">
              <Border x:Name="Root" Background="Transparent" Padding="14,7" CornerRadius="4">
                <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"
                                  Content="{TemplateBinding Content}"
                                  ContentTemplate="{TemplateBinding ContentTemplate}"
                                  FontFamily="{TemplateBinding FontFamily}"
                                  FontSize="{TemplateBinding FontSize}"
                                  FontWeight="{TemplateBinding FontWeight}"
                                  Foreground="{TemplateBinding Foreground}"/>
                <VisualStateManager.VisualStateGroups>
                  <VisualStateGroup x:Name="CommonStates">
                    <VisualState x:Name="Normal"/>
                    <VisualState x:Name="PointerOver">
                      <VisualState.Setters>
                        <Setter Target="Root.Background" Value="#10FFFFFF"/>
                      </VisualState.Setters>
                    </VisualState>
                    <VisualState x:Name="Pressed">
                      <VisualState.Setters>
                        <Setter Target="Root.Background" Value="#1CFFFFFF"/>
                      </VisualState.Setters>
                    </VisualState>
                    <VisualState x:Name="Disabled"/>
                  </VisualStateGroup>
                </VisualStateManager.VisualStateGroups>
              </Border>
            </ControlTemplate>
            """;
        try
        {
            _flatButtonTemplate = (ControlTemplate)XamlReader.Load(xaml);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            _flatButtonTemplate = null;
        }
        return _flatButtonTemplate;
    }

    private static Button TextButton(string label, Action click)
    {
        var button = new Button
        {
            Content = new TextBlock
            {
                Text = label,
                FontFamily = UiFont,
                FontSize = UiFontSize,
                FontWeight = Microsoft.UI.Text.FontWeights.Normal,
                Foreground = Brush(Title),
            },
            Foreground = Brush(Title),
            Background = new SolidColorBrush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = new Thickness(14, 7, 14, 7),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            FontWeight = Microsoft.UI.Text.FontWeights.Normal,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            // Clicking Open / Play / More must not park keyboard focus on the
            // island, or A/D/Q/E wait for the window to be deactivated.
            AllowFocusOnInteraction = false,
        };
        ControlTemplate? template = FlatButtonTemplate();
        if (template is not null) button.Template = template;
        button.Click += (_, _) => click();
        return button;
    }

    private static Style FlyoutPresenterStyle()
    {
        var style = new Style(typeof(FlyoutPresenter));
        style.Setters.Add(new Setter(FlyoutPresenter.BackgroundProperty, Brush(Canvas)));
        style.Setters.Add(new Setter(FlyoutPresenter.BorderBrushProperty, Brush(Hairline)));
        style.Setters.Add(new Setter(FlyoutPresenter.BorderThicknessProperty, new Thickness(1)));
        style.Setters.Add(new Setter(FlyoutPresenter.CornerRadiusProperty, new CornerRadius(6)));
        style.Setters.Add(new Setter(FlyoutPresenter.PaddingProperty, new Thickness(4)));
        style.Setters.Add(new Setter(FlyoutPresenter.FontFamilyProperty, UiFont));
        style.Setters.Add(new Setter(FlyoutPresenter.FontSizeProperty, UiFontSize));
        style.Setters.Add(new Setter(Control.AllowFocusOnInteractionProperty, true));
        return style;
    }

    private static Style MenuFlyoutPresenterStyle()
    {
        var style = new Style(typeof(MenuFlyoutPresenter));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.BackgroundProperty, Brush(Canvas)));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.BorderBrushProperty, Brush(Hairline)));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.BorderThicknessProperty, new Thickness(1)));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.CornerRadiusProperty, new CornerRadius(6)));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.PaddingProperty, new Thickness(4)));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.FontFamilyProperty, UiFont));
        style.Setters.Add(new Setter(MenuFlyoutPresenter.FontSizeProperty, UiFontSize));
        return style;
    }

    private static ControlTemplate? _flatMenuItemTemplate;

    private static ControlTemplate? FlatMenuItemTemplate()
    {
        if (_flatMenuItemTemplate is not null) return _flatMenuItemTemplate;
        const string xaml =
            """
            <ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                             TargetType="MenuFlyoutItem">
              <Border x:Name="Root" Background="Transparent" Padding="12,8" CornerRadius="4">
                <Grid>
                  <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                  </Grid.ColumnDefinitions>
                  <TextBlock Text="{TemplateBinding Text}"
                             FontFamily="{TemplateBinding FontFamily}"
                             FontSize="{TemplateBinding FontSize}"
                             Foreground="{TemplateBinding Foreground}"
                             VerticalAlignment="Center"/>
                  <TextBlock Grid.Column="1"
                             Text="{TemplateBinding KeyboardAcceleratorTextOverride}"
                             FontFamily="{TemplateBinding FontFamily}"
                             FontSize="{TemplateBinding FontSize}"
                             Foreground="{TemplateBinding Foreground}"
                             Margin="28,0,4,0"
                             Opacity="0.5"
                             VerticalAlignment="Center"/>
                </Grid>
                <VisualStateManager.VisualStateGroups>
                  <VisualStateGroup x:Name="CommonStates">
                    <VisualState x:Name="Normal"/>
                    <VisualState x:Name="PointerOver">
                      <VisualState.Setters>
                        <Setter Target="Root.Background" Value="#10FFFFFF"/>
                      </VisualState.Setters>
                    </VisualState>
                    <VisualState x:Name="Pressed">
                      <VisualState.Setters>
                        <Setter Target="Root.Background" Value="#1CFFFFFF"/>
                      </VisualState.Setters>
                    </VisualState>
                    <VisualState x:Name="Disabled"/>
                  </VisualStateGroup>
                </VisualStateManager.VisualStateGroups>
              </Border>
            </ControlTemplate>
            """;
        try
        {
            _flatMenuItemTemplate = (ControlTemplate)XamlReader.Load(xaml);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            _flatMenuItemTemplate = null;
        }
        return _flatMenuItemTemplate;
    }

    private static MenuFlyoutItem Item(string text, string? shortcut, Action action)
    {
        var item = new MenuFlyoutItem
        {
            Text = text,
            Foreground = Brush(Title),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Background = new SolidColorBrush(Colors.Transparent),
            Padding = new Thickness(12, 8, 12, 8),
        };
        ControlTemplate? template = FlatMenuItemTemplate();
        if (template is not null) item.Template = template;
        if (!string.IsNullOrEmpty(shortcut))
        {
            item.KeyboardAcceleratorTextOverride = shortcut;
        }
        // Same as TextButton: a focused menu item parks keys on the island
        // until the window is deactivated and reactivated.
        item.AllowFocusOnInteraction = false;
        item.Click += (_, _) => action();
        return item;
    }

    private static MenuFlyoutSeparator Sep() =>
        new() { Foreground = Brush(Hairline), Background = Brush(Hairline) };

    // Loading indicator. A decode that misses the viewer cache is not
    // instantaneous on a camera dump, and a viewer that shows the previous
    // photo with no sign that a new one is coming reads as a hang. This is the
    // chrome's job: the canvas is native and ImGui is the F3 instrument, never
    // shipped UI (D1).
    private static Grid? _busyTrack;
    private static Border? _busyBar;
    private static TranslateTransform? _busySlide;
    private static Storyboard? _busyAnim;
    private static DispatcherQueueTimer? _busyDelay;
    private static bool _busyVisible;

    private const double BusyBarWidth = 140;

    private static Grid BuildBusyBar()
    {
        _busySlide = new TranslateTransform { X = -BusyBarWidth };
        _busyBar = new Border
        {
            Background = Brush(Title),
            Width = BusyBarWidth,
            HorizontalAlignment = HorizontalAlignment.Left,
            RenderTransform = _busySlide,
        };
        _busyTrack = new Grid
        {
            Height = 2,
            VerticalAlignment = VerticalAlignment.Bottom,
            Visibility = Visibility.Collapsed,
            Children = { _busyBar },
        };
        _busyTrack.SizeChanged += (_, e) =>
        {
            // The track has to clip by hand: a Grid does not, and the sweep runs
            // off both ends by a full bar width.
            _busyTrack.Clip = new RectangleGeometry
            {
                Rect = new Rect(0, 0, e.NewSize.Width, e.NewSize.Height),
            };
            RestartBusyAnimation(e.NewSize.Width);
        };
        return _busyTrack;
    }

    private static void RestartBusyAnimation(double trackWidth)
    {
        _busyAnim?.Stop();
        _busyAnim = null;
        if (_busySlide is null || trackWidth <= 0 || !_busyVisible) return;

        var slide = new DoubleAnimation
        {
            From = -BusyBarWidth,
            To = trackWidth,
            Duration = new Duration(TimeSpan.FromMilliseconds(1100)),
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut },
        };
        Storyboard.SetTarget(slide, _busySlide);
        Storyboard.SetTargetProperty(slide, "X");
        _busyAnim = new Storyboard();
        _busyAnim.Children.Add(slide);
        _busyAnim.Begin();
    }

    /// <summary>
    /// Shows or hides the load indicator. Showing is delayed: a hit in the
    /// viewer cache publishes in the same drain as the selection change, and a
    /// bar that flashes on every arrow key is worse than no bar at all.
    /// </summary>
    private static void SetBusy(bool busy)
    {
        _busyDelay?.Stop();
        if (!busy)
        {
            ApplyBusy(false);
            return;
        }
        if (_busyVisible || _dispatcher is null) return;
        _busyDelay ??= _dispatcher.DispatcherQueue.CreateTimer();
        _busyDelay.Interval = TimeSpan.FromMilliseconds(150);
        _busyDelay.IsRepeating = false;
        _busyDelay.Tick -= OnBusyDelay;
        _busyDelay.Tick += OnBusyDelay;
        _busyDelay.Start();
    }

    private static void OnBusyDelay(DispatcherQueueTimer sender, object args) => ApplyBusy(true);

    private static void ApplyBusy(bool visible)
    {
        if (_busyVisible == visible) return;
        _busyVisible = visible;
        if (_busyTrack is null) return;
        _busyTrack.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        if (visible) RestartBusyAnimation(_busyTrack.ActualWidth);
        else
        {
            _busyAnim?.Stop();
            _busyAnim = null;
        }
    }

    private static MenuFlyout? _settingsFlyout;

    private static bool HasFlag(int flag) => (_settingFlags & flag) != 0;

    // Native owns the file. Send the whole word, let it persist, and wait for
    // ApplySettings to come back before the menu changes — one direction, so
    // a failed write cannot leave a lying checkmark on screen.
    private static void SetFlag(int flag, bool on)
    {
        int next = on ? _settingFlags | flag : _settingFlags & ~flag;
        Send(Command.SetSettings, next);
    }

    private static void RefreshOpenMenu(MenuFlyout flyout)
    {
        flyout.Items.Clear();
        flyout.Items.Add(Item("Media…", "Ctrl+O", () => { Send(Command.Open); RestoreCanvasFocus(); }));
        flyout.Items.Add(Item("Folder…", "Ctrl+Shift+O", () => { Send(Command.OpenFolder); RestoreCanvasFocus(); }));
        flyout.Items.Add(Sep());
        string? name = _selectedIndex >= 0 && _selectedIndex < Items.Count
            ? Items[_selectedIndex].Name
            : null;
        if (string.IsNullOrEmpty(name))
        {
            MenuFlyoutItem empty = Item("Open: (nothing open)", "Ctrl+E", () => { });
            empty.IsEnabled = false;
            flyout.Items.Add(empty);
        }
        else
        {
            MenuFlyoutItem reveal = Item("Open: " + name, "Ctrl+E", () =>
            {
                Send(Command.RevealInExplorer);
                RestoreCanvasFocus();
            });
            string path = Items[_selectedIndex].Path;
            if (!string.IsNullOrEmpty(path)) ToolTipService.SetToolTip(reveal, path);
            flyout.Items.Add(reveal);
        }
    }

    private static void RefreshSettingsMenu()
    {
        if (_settingsFlyout is null) return;
        _settingsFlyout.Items.Clear();
        _settingsFlyout.Items.Add(
            Item("Filmstrip when opening a folder",
                 HasFlag(SettingFlag.FilmstripForFolder) ? "on" : "off",
                 () => SetFlag(SettingFlag.FilmstripForFolder,
                               !HasFlag(SettingFlag.FilmstripForFolder))));
        _settingsFlyout.Items.Add(
            Item("Filmstrip when opening an image",
                 HasFlag(SettingFlag.FilmstripForImage) ? "on" : "off",
                 () => SetFlag(SettingFlag.FilmstripForImage,
                               !HasFlag(SettingFlag.FilmstripForImage))));
        _settingsFlyout.Items.Add(
            Item("Wrap at the end of the folder",
                 HasFlag(SettingFlag.Wrap) ? "on" : "off",
                 () => SetFlag(SettingFlag.Wrap, !HasFlag(SettingFlag.Wrap))));
    }

    private static readonly string[] SortNames =
        { "Name", "Date modified", "Size", "Type", "Date taken (EXIF)" };

    // View > Sort by. Rebuilt each time the flyout opens so the marker is the
    // order native actually applied, never a guess.
    private static MenuFlyoutSubItem SortSubMenu(MenuFlyout owner)
    {
        var sub = new MenuFlyoutSubItem { Text = "Sort by" };
        void Fill()
        {
            sub.Items.Clear();
            for (int k = 0; k < SortNames.Length; k++)
            {
                int key = k;
                sub.Items.Add(Item(SortNames[k], (_sortPacked & 7) == k ? "●" : null,
                    () => Send(Command.SetSort, (_sortPacked & 8) | key)));
            }
            sub.Items.Add(Sep());
            sub.Items.Add(Item("Descending", (_sortPacked & 8) != 0 ? "on" : "off",
                () => Send(Command.SetSort, _sortPacked ^ 8)));
        }
        owner.Opening += (_, _) => Fill();
        Fill();
        return sub;
    }

    private static UIElement BuildChrome()
    {
        var viewFlyout = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false,
            MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle(),
        };
        viewFlyout.Items.Add(Item("Zoom in", "+", () => Send(Command.ZoomIn)));
        viewFlyout.Items.Add(Item("Zoom out", "-", () => Send(Command.ZoomOut)));
        viewFlyout.Items.Add(Sep());
        viewFlyout.Items.Add(Item("Fit to window", "0", () => Send(Command.Fit)));
        viewFlyout.Items.Add(Item("50 %", null, () => Send(Command.ZoomPreset, 0.5f)));
        viewFlyout.Items.Add(Item("100 %", "1", () => Send(Command.OneToOne)));
        viewFlyout.Items.Add(Item("200 %", null, () => Send(Command.ZoomPreset, 2.0f)));
        viewFlyout.Items.Add(Item("400 %", null, () => Send(Command.ZoomPreset, 4.0f)));
        viewFlyout.Items.Add(Sep());
        viewFlyout.Items.Add(Item("Gallery", "G", () => Send(Command.ToggleGallery)));
        viewFlyout.Items.Add(Item("Full screen", "F11", () => Send(Command.Fullscreen)));
        viewFlyout.Items.Add(Item("Filmstrip", "T", () => Send(Command.ToggleFilmstrip)));
        viewFlyout.Items.Add(Item("Metadata pane", "I", () => Send(Command.MetadataPane)));
        viewFlyout.Items.Add(Item("Folder tree", "Ctrl+Shift+E", () => Send(Command.FolderTree)));
        viewFlyout.Items.Add(SortSubMenu(viewFlyout));
        viewFlyout.Items.Add(Sep());
        viewFlyout.Items.Add(Item("Clipping warnings", "C", () => Send(Command.Clipping)));
        viewFlyout.Items.Add(Item("Frame-time overlay", "F3", () => Send(Command.Overlay)));
        viewFlyout.Items.Add(Item("Keyboard shortcuts", "?", () => Send(Command.Help)));

        _settingsFlyout = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false,
            MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle(),
        };
        // Rebuild on open as well as on ApplySettings, so the on/off column is
        // right even if a keyboard toggle raced the last push.
        _settingsFlyout.Opening += (_, _) => RefreshSettingsMenu();
        RefreshSettingsMenu();

        var aboutFlyout = new Flyout
        {
            ShouldConstrainToRootBounds = false,
            FlyoutPresenterStyle = FlyoutPresenterStyle(),
        };
        aboutFlyout.Content = BuildAboutContent();  // IslandHost.About.cs (PR 8)

        var openFlyout = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false,
            MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle(),
        };
        // Reads as "Open media" / "Open folder". The picker takes clips as well
        // as photos, and calling it "Image" was the last place the UI still
        // claimed this was a photo-only viewer.
        openFlyout.Opening += (_, _) => RefreshOpenMenu(openFlyout);
        RefreshOpenMenu(openFlyout);

        Button? openBtn = null;
        openBtn = TextButton("Open", () =>
        {
            if (openBtn is not null) FlyoutBase.ShowAttachedFlyout(openBtn);
        });
        AttachBarFlyout(openBtn, openFlyout);
        Button? viewBtn = null;
        viewBtn = TextButton("View", () =>
        {
            if (viewBtn is not null) FlyoutBase.ShowAttachedFlyout(viewBtn);
        });
        AttachBarFlyout(viewBtn, viewFlyout);
        Button? settingsBtn = null;
        settingsBtn = TextButton("Settings", () => Send(Command.OpenSettings));
        Button? aboutBtn = null;
        aboutBtn = TextButton("About", () =>
        {
            if (aboutBtn is not null) FlyoutBase.ShowAttachedFlyout(aboutBtn);
        });
        AttachBarFlyout(aboutBtn, aboutFlyout);
        // The first-run telemetry screen hangs off About, which is where the
        // privacy note and the licence already live.
        _telemetryAnchor = aboutBtn;

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 2,
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
            Padding = new Thickness(8, 0, 8, 0),
        };
        row.Children.Add(openBtn);
        row.Children.Add(viewBtn);
        row.Children.Add(settingsBtn);
        row.Children.Add(aboutBtn);
        row.Children.Add(BuildUpdateButton());
        row.Children.Add(BuildImportHint());  // Milestone G (IslandHost.Addons.cs)

        var speed = BuildSpeed();
        speed.HorizontalAlignment = HorizontalAlignment.Right;
        Button helpBtn = TextButton("?", () => Send(Command.Help));
        ToolTipService.SetToolTip(helpBtn, "Keyboard shortcuts  ?");

        // Menus left, speed then `?` on the far right.
        var bar = new Grid { VerticalAlignment = VerticalAlignment.Stretch };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(row, 0);
        bar.Children.Add(row);
        Grid.SetColumn(speed, 1);
        bar.Children.Add(speed);
        Grid.SetColumn(helpBtn, 2);
        bar.Children.Add(helpBtn);

        var root = new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(48) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1) });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(bar, 0);
        root.Children.Add(bar);
        FrameworkElement path = BuildBarPathRow();
        Grid.SetRow(path, 1);
        root.Children.Add(path);
        Grid busy = BuildBusyBar();
        Grid.SetRow(busy, 0);
        root.Children.Add(busy);
        _chromeRoot = root;
        WireFileDrop(root);
        // Settings is built on first open. Building it into the 48 DIP bar at
        // attach left the command row blank (star-row height 0).
        var rule = new Border { Background = Brush(Hairline) };
        Grid.SetRow(rule, 2);
        root.Children.Add(rule);
        root.PreviewKeyDown += OnSettingsKeyDown;
        root.PreviewKeyUp += OnSettingsKeyUp;
        root.KeyDown += OnSettingsNavigationKeyDown;
        return root;
    }

    private const uint GaRoot = 2;

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    // The native canvas HWND. Clicking chrome, closing a flyout, or finishing
    // a scrub must put keys back here so the router sees them.
    private static void AttachBarFlyout(FrameworkElement target, FlyoutBase flyout)
    {
        FlyoutBase.SetAttachedFlyout(target, flyout);
        flyout.Closed += (_, _) => RestoreCanvasFocus();
    }

    private static void RestoreCanvasFocus()
    {
        if (_settingsVisible)
        {
            FocusSettings();
            return;
        }
        DesktopWindowXamlSource? source = _source ?? _filmstrip ?? _transport ?? _gallery;
        if (source?.SiteBridge is null) return;
        IntPtr hwnd = Win32Interop.GetWindowFromWindowId(source.SiteBridge.WindowId);
        IntPtr root = GetAncestor(hwnd, GaRoot);
        if (root == IntPtr.Zero) return;
        // Do not yank focus from Explorer after Ctrl+E / Open: filename.
        IntPtr fg = GetForegroundWindow();
        if (fg != IntPtr.Zero && fg != root && GetAncestor(fg, GaRoot) != root) return;
        SetFocus(root);
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeAttachArgs
{
    public ulong ParentHwnd;
    public ulong Context;
    public ulong OnCommand;
    public int ClientWidth;
    public int ClientHeight;
    public int Dpi;
    public int Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeResizeArgs
{
    public int Width;
    public int Height;
    public int Dpi;
    public int Y;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeRateArgs
{
    public float Rate;
    public int Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeFilmstripArgs
{
    public ulong ParentHwnd;
    public ulong Context;
    public ulong OnCommand;
    public ulong Session;
    public int ClientWidth;
    public int ClientHeight;
    public int Dpi;
    public int Reserved;
}
