// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// The playback transport: its own bottom island, centred, up only while a clip
/// is open.
/// </summary>
/// <remarks>
/// It used to be a StackPanel inside the command bar, which put the scrubber at
/// the top of the window and left it there — collapsed, but still holding a slot
/// — for every photo. As a strip it follows the filmstrip's contract: native
/// owns the geometry, the canvas rectangle shrinks by the strip height while it
/// is up, and so the bar can never cover the video (plan/16, "do not grow an
/// island over the canvas").
///
/// Show/hide is driven from here because this is where playback state is
/// already polled: <see cref="UpdateVideoControls"/> posts
/// <c>Command.VideoActive</c> on a change and native decides the layout.
/// </remarks>
public static partial class IslandHost
{
    private static DesktopWindowXamlSource? _transport;
    private static Slider? _seek;
    private static Button? _play;
    private static TextBlock? _videoTime;
    private static ComboBox? _audioTracks;
    private static DispatcherTimer? _videoTimer;
    private static SystemMediaTransportControls? _smtc;
    private static bool _updatingVideo, _draggingSeek;
    private static long _loopA;
    private static long _lastScrub;
    private static bool _videoActive;

    private const int TransportDip = 52;

    public static int AttachTransport(IntPtr arg, int sizeBytes)
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
            // The filmstrip normally attaches first and owns the drain and the
            // session. Borrow only if it did not.
            if (_folderSession is null && args.Session != 0)
            {
                _folderSession = MediaViewer.Interop.MediaViewerSession.Borrow(
                    checked((IntPtr)args.Session));
                StartDrain();
            }

            _transport?.Dispose();
            _transport = new DesktopWindowXamlSource();
            _transport.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
            // Parked, with no content: nothing is open, and a full-client
            // default island would flash over the canvas on startup.
            Move(_transport, 1, 1, args.ClientHeight);

            StartVideoControls(parent);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("chrome AttachTransport: {0}", ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ResizeTransport(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < ResizeArgsSize) return unchecked((int)0x80070057);
            ChromeResizeArgs args = Marshal.PtrToStructure<ChromeResizeArgs>(arg);
            Move(_transport, args.Width, args.Height, args.Y);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ShowTransport(IntPtr arg, int sizeBytes) => ShowIsland(
        arg, sizeBytes, _transport, BuildTransport,
        onShown: () => UpdateVideoControls(),
        onHidden: () =>
        {
            _seek = null;
            _play = null;
            _videoTime = null;
            _audioTracks = null;
        });

    public static int DetachTransport(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            StopVideoControls();
            if (_transport is not null)
            {
                _transport.Content = null;
                _transport.Dispose();
                _transport = null;
            }
            _seek = null;
            _play = null;
            _videoTime = null;
            _audioTracks = null;
            _videoActive = false;
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static UIElement BuildTransport()
    {
        _play = TextButton("Pause", ToggleVideo);
        _seek = new Slider
        {
            Minimum = 0,
            Maximum = 1,
            Width = 320,
            StepFrequency = 0.01,
            VerticalAlignment = VerticalAlignment.Center,
        };
        _seek.AddHandler(UIElement.PointerPressedEvent,
                         new PointerEventHandler((_, _) => { _draggingSeek = true; }), true);
        _seek.AddHandler(UIElement.PointerReleasedEvent,
                         new PointerEventHandler((_, _) => FinishSeek()), true);
        _seek.PointerCaptureLost += (_, _) => FinishSeek();
        _seek.ValueChanged += (_, e) =>
        {
            if (_updatingVideo || _folderSession is null) return;
            long now = Environment.TickCount64;
            // A drag is a scrub: nearest keyframe, throttled. The release is the
            // exact seek (plan/05's two modes).
            if (_draggingSeek && now - _lastScrub < 75) return;
            _lastScrub = now;
            _folderSession.VideoSeek((long)(e.NewValue * 1e9), !_draggingSeek);
        };
        _videoTime = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = 12,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            MinWidth = 92,
        };

        var more = TextButton("More", () => { });
        var panel = new StackPanel { Spacing = 10, Padding = new Thickness(12), MinWidth = 260 };
        var steps = new StackPanel { Orientation = Orientation.Horizontal };
        steps.Children.Add(TextButton("Previous frame", () => _folderSession?.VideoStep(-1)));
        steps.Children.Add(TextButton("Next frame", () => _folderSession?.VideoStep(1)));
        panel.Children.Add(steps);
        var volume = new Slider { Header = "Volume", Minimum = 0, Maximum = 1, Value = 1, StepFrequency = .01 };
        volume.ValueChanged += (_, e) => _folderSession?.VideoVolume((float)e.NewValue);
        panel.Children.Add(volume);
        var mute = new ToggleSwitch { Header = "Mute" };
        mute.Toggled += (_, _) => _folderSession?.VideoMuted(mute.IsOn);
        panel.Children.Add(mute);
        _audioTracks = new ComboBox { Header = "Audio track" };
        _audioTracks.SelectionChanged += (_, _) =>
        {
            if (!_updatingVideo && _audioTracks.SelectedIndex >= 0)
                _folderSession?.VideoTrack((uint)_audioTracks.SelectedIndex);
        };
        panel.Children.Add(_audioTracks);
        var loop = new StackPanel { Orientation = Orientation.Horizontal };
        loop.Children.Add(TextButton("Set A", () => _loopA = _folderSession?.VideoPosition ?? 0));
        loop.Children.Add(TextButton("Set B", () => _folderSession?.VideoLoop(_loopA, _folderSession.VideoPosition)));
        loop.Children.Add(TextButton("Clear loop", () => _folderSession?.VideoLoop(0, -1)));
        panel.Children.Add(loop);
        var flyout = new Flyout
        {
            Content = panel,
            ShouldConstrainToRootBounds = false,
            Placement = FlyoutPlacementMode.Top,
            FlyoutPresenterStyle = FlyoutPresenterStyle(),
        };
        FlyoutBase.SetAttachedFlyout(more, flyout);
        more.Click += (_, _) => FlyoutBase.ShowAttachedFlyout(more);

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            // Bottom middle. The strip is full width so the hairline reads as a
            // strip; the controls inside it are centred.
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        row.Children.Add(_play);
        row.Children.Add(_seek);
        row.Children.Add(_videoTime);
        row.Children.Add(more);

        var root = new Grid
        {
            RequestedTheme = ElementTheme.Dark,
            Background = Brush(Canvas),
            Height = TransportDip,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1) });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var rule = new Border { Background = Brush(Hairline) };
        Grid.SetRow(rule, 0);
        root.Children.Add(rule);
        Grid.SetRow(row, 1);
        root.Children.Add(row);
        return root;
    }

    private static void FinishSeek()
    {
        if (!_draggingSeek || _seek is null) return;
        _draggingSeek = false;
        _folderSession?.VideoSeek((long)(_seek.Value * 1e9), true);
    }

    private static void ToggleVideo()
    {
        if (_folderSession is null) return;
        if (_folderSession.VideoState == 1) _folderSession.VideoPause(); else _folderSession.VideoPlay();
    }

    private static void StartVideoControls(IntPtr parent)
    {
        StopVideoControls();
        try
        {
            // Desktop HWND binding, per Microsoft's WinRT COM interop contract.
            _smtc = SystemMediaTransportControlsInterop.GetForWindow(parent);
            _smtc.IsPlayEnabled = true; _smtc.IsPauseEnabled = true;
            _smtc.IsNextEnabled = true; _smtc.IsPreviousEnabled = true;
            _smtc.ButtonPressed += OnMediaButton;
            _smtc.PlaybackPositionChangeRequested += OnMediaSeek;
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        _videoTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        _videoTimer.Tick += (_, _) => UpdateVideoControls();
        _videoTimer.Start();
    }

    private static void OnMediaButton(SystemMediaTransportControls sender, SystemMediaTransportControlsButtonPressedEventArgs args)
    {
        _dispatcher?.DispatcherQueue.TryEnqueue(() => {
            if (args.Button == SystemMediaTransportControlsButton.Play) _folderSession?.VideoPlay();
            else if (args.Button == SystemMediaTransportControlsButton.Pause) _folderSession?.VideoPause();
            else if (args.Button == SystemMediaTransportControlsButton.Next) Send(Command.Next);
            else if (args.Button == SystemMediaTransportControlsButton.Previous) Send(Command.Prev);
        });
    }

    private static void OnMediaSeek(SystemMediaTransportControls sender, PlaybackPositionChangeRequestedEventArgs args) =>
        _dispatcher?.DispatcherQueue.TryEnqueue(() => _folderSession?.VideoSeek(args.RequestedPlaybackPosition.Ticks * 100, true));

    private static void StopVideoControls()
    {
        _videoTimer?.Stop(); _videoTimer = null;
        if (_smtc is not null) {
            _smtc.ButtonPressed -= OnMediaButton;
            _smtc.PlaybackPositionChangeRequested -= OnMediaSeek;
            _smtc.IsEnabled = false; _smtc = null;
        }
    }

    /// <summary>
    /// 150 ms playback poll. It runs whether or not the strip is up, because it
    /// is also what tells native when to raise and lower it.
    /// </summary>
    private static void UpdateVideoControls()
    {
        if (_folderSession is null) return;
        var info = _folderSession.VideoInfo;
        uint state = _folderSession.VideoState;
        bool video = info.Width > 0 && state != 0;

        // Auto show/hide. Native owns the geometry (and the canvas rectangle it
        // steals from), so this reports the fact and does not move a window.
        if (video != _videoActive)
        {
            _videoActive = video;
            Send(Command.VideoActive, video ? 1 : 0);
            SetSpeedVisible(video);
        }
        if (_smtc is not null) _smtc.IsEnabled = video;
        if (!video) return;

        long position = Math.Max(0, _folderSession.VideoPosition);
        if (_seek is not null && _play is not null && _videoTime is not null && _audioTracks is not null)
        {
            _updatingVideo = true;
            _seek.Maximum = Math.Max(.001, info.DurationNs / 1e9);
            if (!_draggingSeek) _seek.Value = Math.Clamp(position / 1e9, 0, _seek.Maximum);
            if (_play.Content is TextBlock label) label.Text = state == 1 ? "Pause" : "Play";
            _videoTime.Text = $"{TimeSpan.FromTicks(position / 100):mm\\:ss} / {TimeSpan.FromTicks(Math.Max(0, info.DurationNs) / 100):mm\\:ss}";
            if (_audioTracks.Items.Count != info.AudioTracks) {
                _audioTracks.Items.Clear();
                for (uint i = 0; i < info.AudioTracks; ++i) _audioTracks.Items.Add($"Track {i + 1}");
                _audioTracks.SelectedIndex = info.AudioTracks > 0 ? 0 : -1;
            }
            _updatingVideo = false;
        }

        if (_smtc is not null) {
            _smtc.PlaybackStatus = state == 1 ? MediaPlaybackStatus.Playing : MediaPlaybackStatus.Paused;
            var updater = _smtc.DisplayUpdater; updater.Type = MediaPlaybackType.Video;
            updater.VideoProperties.Title = _selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex].Name : "MediaViewer";
            updater.Update();
            _smtc.UpdateTimelineProperties(new SystemMediaTransportControlsTimelineProperties {
                StartTime = TimeSpan.Zero, MinSeekTime = TimeSpan.Zero,
                EndTime = TimeSpan.FromTicks(Math.Max(0, info.DurationNs) / 100),
                MaxSeekTime = TimeSpan.FromTicks(Math.Max(0, info.DurationNs) / 100),
                Position = TimeSpan.FromTicks(position / 100)
            });
        }
    }
}
