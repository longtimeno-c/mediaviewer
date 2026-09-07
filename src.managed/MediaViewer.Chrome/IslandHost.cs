// SPDX-License-Identifier: GPL-2.0-or-later
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
    }

    // Mirrors mv::shell::view_settings. The native side owns the file; the
    // menu is a view of it, pushed in by ApplySettings so a T keypress and the
    // checkmarks cannot drift apart.
    internal static class SettingFlag
    {
        public const int FilmstripForFolder = 1 << 0;
        public const int FilmstripForImage = 1 << 1;
    }

    private static int _settingFlags = SettingFlag.FilmstripForFolder;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void NativeCommand(IntPtr context, int command, float arg);

    private static DispatcherQueueController? _dispatcher;
    private static WindowsXamlManager? _xaml;
    private static DesktopWindowXamlSource? _source;
    private static NativeCommand? _onCommand;
    private static IntPtr _context;

    public static int Probe(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        if (Marshal.SizeOf<ChromeAttachArgs>() != AttachArgsSize) return -2;
        if (Marshal.SizeOf<ChromeResizeArgs>() != ResizeArgsSize) return -3;
        if (Marshal.SizeOf<ChromeFilmstripArgs>() != FilmstripArgsSize) return -4;
        if (Marshal.SizeOf<ChromeShowArgs>() != ShowArgsSize) return -5;
        if (Marshal.SizeOf<ChromeFlagsArgs>() != FlagsArgsSize) return -6;
        if (Marshal.SizeOf<ChromeRateArgs>() != RateArgsSize) return -7;
        return AttachArgsSize;
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

            _source?.Dispose();
            _source = new DesktopWindowXamlSource();
            _source.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // Constrain the island to the bar before assigning content so a
            // full-client default size never covers the canvas.
            Move(_source, args.ClientWidth, args.ClientHeight, 0);
            _source.TakeFocusRequested += OnTakeFocusRequested;
            _source.Content = BuildChrome();
            Move(_source, args.ClientWidth, args.ClientHeight, 0);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            Console.Error.WriteLine("chrome Attach: {0}", ex);
            return unchecked((int)0x80004005);
        }
    }

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
            if (_source is not null)
            {
                _source.TakeFocusRequested -= OnTakeFocusRequested;
                _source.Content = null;
                _source.Dispose();
                _source = null;
            }
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
            RefreshSettingsMenu();
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
    /// router (plan/16), so A/D and this menu cannot drift apart.
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
        ToolTipService.SetToolTip(_speed, "Playback speed — tap A / D to step, hold to skim");
        return _speed;
    }

    private static void EnsureApp()
    {
        if (_dispatcher is not null) return;
        _dispatcher = DispatcherQueueController.CreateOnCurrentThread();
        // Island init path. Application after this throws; styles are set on
        // the bar itself (custom templates, not generic.xaml).
        _xaml = WindowsXamlManager.InitializeForCurrentThread();
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
        viewFlyout.Items.Add(Sep());
        viewFlyout.Items.Add(Item("Gallery", "G", () => Send(Command.ToggleGallery)));
        viewFlyout.Items.Add(Item("Filmstrip", "T", () => Send(Command.ToggleFilmstrip)));
        viewFlyout.Items.Add(Sep());
        viewFlyout.Items.Add(Item("Frame-time overlay", "F", () => Send(Command.Overlay)));

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
        aboutFlyout.Content = new TextBlock
        {
            Text = "MediaViewer — GPL-2.0-or-later\n\nG opens the gallery, T shows or hides the filmstrip, F toggles the frame-time overlay. On a clip: space plays/pauses, A and D skim, J and L jump 10 s. Wheel zooms toward the cursor; drag pans.",
            Margin = new Thickness(12, 10, 12, 10),
            MaxWidth = 400,
            TextWrapping = TextWrapping.Wrap,
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
        };

        var openFlyout = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false,
            MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle(),
        };
        // Reads as "Open media" / "Open folder". The picker takes clips as well
        // as photos, and calling it "Image" was the last place the UI still
        // claimed this was a photo-only viewer.
        openFlyout.Items.Add(Item("Media…", "Ctrl+O", () => Send(Command.Open)));
        openFlyout.Items.Add(Item("Folder…", null, () => Send(Command.OpenFolder)));

        Button? openBtn = null;
        openBtn = TextButton("Open", () =>
        {
            if (openBtn is not null) FlyoutBase.ShowAttachedFlyout(openBtn);
        });
        FlyoutBase.SetAttachedFlyout(openBtn, openFlyout);
        Button? viewBtn = null;
        viewBtn = TextButton("View", () =>
        {
            if (viewBtn is not null) FlyoutBase.ShowAttachedFlyout(viewBtn);
        });
        FlyoutBase.SetAttachedFlyout(viewBtn, viewFlyout);
        Button? settingsBtn = null;
        settingsBtn = TextButton("Settings", () =>
        {
            if (settingsBtn is not null) FlyoutBase.ShowAttachedFlyout(settingsBtn);
        });
        FlyoutBase.SetAttachedFlyout(settingsBtn, _settingsFlyout);
        Button? aboutBtn = null;
        aboutBtn = TextButton("About", () =>
        {
            if (aboutBtn is not null) FlyoutBase.ShowAttachedFlyout(aboutBtn);
        });
        FlyoutBase.SetAttachedFlyout(aboutBtn, aboutFlyout);

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

        var speed = BuildSpeed();
        speed.HorizontalAlignment = HorizontalAlignment.Right;

        // Menus left, speed far right. A StackPanel cannot pin one child to the
        // right edge, so the bar row is a two-column Grid.
        var bar = new Grid { VerticalAlignment = VerticalAlignment.Stretch };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(row, 0);
        bar.Children.Add(row);
        Grid.SetColumn(speed, 1);
        bar.Children.Add(speed);

        var root = new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
            Height = 48,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1) });
        Grid.SetRow(bar, 0);
        root.Children.Add(bar);
        Grid busy = BuildBusyBar();
        Grid.SetRow(busy, 0);
        root.Children.Add(busy);
        var rule = new Border { Background = Brush(Hairline) };
        Grid.SetRow(rule, 1);
        root.Children.Add(rule);
        return root;
    }

    private const uint GaRoot = 2;

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr hWnd);
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
