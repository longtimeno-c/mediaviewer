// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
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
/// The playback transport: its own island, a centred bar floating over the
/// bottom of the video, up only while a clip is open.
/// </summary>
/// <remarks>
/// It used to be a StackPanel inside the command bar, which put the scrubber at
/// the top of the window and left it there — collapsed, but still holding a slot
/// — for every photo. Then it was a full-width strip the canvas shrank for.
/// Issue #38 (docs/design/12 2026-09-26) made it float like the Mac's and leave after
/// an idle interval while the clip plays: native owns the geometry and the idle
/// rule (shell/transport_autohide.h), and parks the island — content kept —
/// when it hides, so the canvas never refits.
///
/// Show/hide is driven from here because this is where playback state is
/// already polled: <see cref="UpdateVideoControls"/> posts
/// <c>Command.VideoActive</c> (0 none, 1 paused, 2 playing) on a change, and
/// <c>Command.TransportHold</c> while a scrub or the More flyout holds it up.
/// </remarks>
public static partial class IslandHost
{
    private static DesktopWindowXamlSource? _transport;
    private static Slider? _seek;
    private static Button? _play;
    private static TextBlock? _videoTime;
    private static ComboBox? _audioTracks;
    private static Slider? _volume;
    private static ToggleSwitch? _mute;
    private static bool _updatingAudio;
    // Native's last ApplyAudio. The strip is rebuilt every time it is shown
    // (ShowTransport), so a new slider, switch and track box start from these,
    // not from 100 % / unmuted / track 1.
    private static float _audioVolume = 1f;
    private static bool _audioMuted;
    private static int _audioTrack;
    private static DispatcherTimer? _videoTimer;
    private static SystemMediaTransportControls? _smtc;
    private static bool _updatingVideo, _draggingSeek;
    private static long _lastScrub;
    private static bool _videoActive;
    private static bool _videoPlaying;
    private static bool _transportHeld;
    private static int _transportWidth;

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

            DisposeSource(ref _transport);
            _transport = new DesktopWindowXamlSource();
            EnsureFocusHook();
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
            if (arg == IntPtr.Zero || sizeBytes < PanelArgsSize) return unchecked((int)0x80070057);
            ChromePanelArgs args = Marshal.PtrToStructure<ChromePanelArgs>(arg);
            // Also issue #38's park and unpark: a move, never a rebuild.
            MoveAt(_transport, args.X, args.Y, args.Width, args.Height);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ShowIsland's contract (build on show, drop and park on hide), with the
    // bar's x as well: it is centred, not full width.
    public static int ShowTransport(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < PanelArgsSize) return unchecked((int)0x80070057);
            if (_transport is null) return 1;
            ChromePanelArgs args = Marshal.PtrToStructure<ChromePanelArgs>(arg);
            if (args.Visible == 0)
            {
                _seek = null;
                _play = null;
                _videoTime = null;
                _audioTracks = null;
                _volume = null;
                _mute = null;
                DropTrimUi();
                SetTransportHeld(false);
                _transport.Content = null;
                MoveAt(_transport, args.X, args.Y, args.Width, args.Height);
                return 0;
            }
            MoveAt(_transport, args.X, args.Y, args.Width, args.Height);
            _transport.Content = BuildTransport();
            UpdateVideoControls();
            RenderTrim();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // Tell native once per change; it keeps the bar up while this is set.
    private static void SetTransportHeld(bool held)
    {
        if (held == _transportHeld) return;
        _transportHeld = held;
        Send(Command.TransportHold, held ? 1 : 0);
    }

    public static int DetachTransport(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            UnhookFocus();
            StopVideoControls();
            DisposeSource(ref _transport);
            _seek = null;
            _play = null;
            _videoTime = null;
            _audioTracks = null;
            _volume = null;
            _mute = null;
            DropTrimUi();
            _videoActive = false;
            _transportWidth = 0;
            _matchMs = Array.Empty<long>();
            _videoPlaying = false;
            _transportHeld = false;
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // Mirrors mv::shell::chrome_loop_action (chrome_host.h).
    private static class LoopActions
    {
        public const int SetA = 0;
        public const int SetB = 1;
        public const int Clear = 2;
    }

    // ApplyAudio's half: native's volume, mute and track, without echoing them
    // back as commands.
    private static void SetAudioSelection(float volume, bool muted, int track)
    {
        _audioVolume = Math.Clamp(volume, 0f, 1f);
        _audioMuted = muted;
        _audioTrack = Math.Max(0, track);
        _updatingAudio = true;
        try
        {
            if (_volume is not null) _volume.Value = _audioVolume;
            if (_mute is not null) _mute.IsOn = muted;
            if (_audioTracks is not null && track >= 0 && track < _audioTracks.Items.Count)
                _audioTracks.SelectedIndex = track;
        }
        finally
        {
            _updatingAudio = false;
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
                         new PointerEventHandler((_, _) => { _draggingSeek = true; SetTransportHeld(true); }), true);
        _seek.AddHandler(UIElement.PointerReleasedEvent,
                         new PointerEventHandler((_, _) => { FinishSeek(); RestoreCanvasFocus(); }), true);
        _seek.PointerCaptureLost += (_, _) => FinishSeek();
        _seek.ValueChanged += (_, e) =>
        {
            if (_updatingVideo || _folderSession is null) return;
            long now = Environment.TickCount64;
            // A drag is a scrub: nearest keyframe, throttled. The release is the
            // exact seek (docs/design/05's two modes).
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
        // Issue #214: volume, mute, the track and the loop go through native,
        // which owns them for the keys too (Up / Down, Shift+M, trim's P) and
        // pushes the result back (ApplyAudio). Nothing here calls the session.
        _volume = new Slider { Header = "Volume", Minimum = 0, Maximum = 1, Value = _audioVolume, StepFrequency = .01 };
        _volume.ValueChanged += (_, e) =>
        {
            if (!_updatingAudio) Send(Command.VideoVolume, (float)e.NewValue);
        };
        panel.Children.Add(_volume);
        _mute = new ToggleSwitch { Header = "Mute", IsOn = _audioMuted };
        _mute.Toggled += (_, _) =>
        {
            if (!_updatingAudio) Send(Command.VideoMuted, _mute.IsOn ? 1 : 0);
        };
        panel.Children.Add(_mute);
        _audioTracks = new ComboBox { Header = "Audio track" };
        _audioTracks.SelectionChanged += (_, _) =>
        {
            if (!_updatingVideo && !_updatingAudio && _audioTracks.SelectedIndex >= 0)
                Send(Command.VideoTrack, _audioTracks.SelectedIndex);
        };
        panel.Children.Add(_audioTracks);
        var loop = new StackPanel { Orientation = Orientation.Horizontal };
        loop.Children.Add(TextButton("Set A", () => Send(Command.VideoLoop, LoopActions.SetA)));
        loop.Children.Add(TextButton("Set B", () => Send(Command.VideoLoop, LoopActions.SetB)));
        loop.Children.Add(TextButton("Clear loop", () => Send(Command.VideoLoop, LoopActions.Clear)));
        panel.Children.Add(loop);
        var flyout = new Flyout
        {
            Content = panel,
            ShouldConstrainToRootBounds = false,
            Placement = FlyoutPlacementMode.Top,
            FlyoutPresenterStyle = FlyoutPresenterStyle(),
        };
        // The flyout is its own popup: the pointer over it is not over the bar,
        // so it holds the bar up explicitly (issue #38).
        flyout.Opened += (_, _) => SetTransportHeld(true);
        flyout.Closed += (_, _) => SetTransportHeld(false);
        FlyoutBase.SetAttachedFlyout(more, flyout);
        more.Click += (_, _) => FlyoutBase.ShowAttachedFlyout(more);

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            // Centred. Native sizes the bar to this row's width, so the
            // hairline hugs the controls rather than boxing empty space.
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        row.Children.Add(_play);
        // PR 13: trim's markers, keyframe grid and kept range over the scrubber,
        // and its label and save buttons after the clock (IslandHost.Clip.cs).
        row.Children.Add(WrapSeekForTrim(_seek));
        row.Children.Add(_videoTime);
        row.Children.Add(BuildTrimBar());
        row.Children.Add(more);

        // The bar is as wide as its controls. The row may be clipped while the
        // island is narrower than it (trim just armed), so the natural width is
        // summed from the children, which a horizontal StackPanel measures
        // unconstrained.
        row.LayoutUpdated += (_, _) => ReportTransportWidth(row);

        // A floating bar over the video (issue #38): hairline all round, the
        // controls centred. The island window is rectangular, so no rounding.
        var root = new Border
        {
            RequestedTheme = IslandTheme,
            Background = Brush(Canvas),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            Height = TransportDip,
            Child = row,
        };
        return root;
    }

    private static void ReportTransportWidth(StackPanel row)
    {
        double width = 0;
        int shown = 0;
        foreach (UIElement child in row.Children)
        {
            if (child.Visibility != Visibility.Visible) continue;
            width += child.DesiredSize.Width;
            ++shown;
        }
        if (shown > 1) width += row.Spacing * (shown - 1);
        int dip = (int)Math.Ceiling(width);
        if (dip <= 0 || dip == _transportWidth) return;
        _transportWidth = dip;
        // Not from inside layout: native answers by moving this island.
        if (_dispatcher?.DispatcherQueue.TryEnqueue(() => Send(Command.TransportWidth, dip)) != true)
            Send(Command.TransportWidth, dip);
    }

    private static void FinishSeek()
    {
        if (!_draggingSeek || _seek is null) return;
        _draggingSeek = false;
        SetTransportHeld(false);
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
        // The helper catches: native refusing a play / seek (MediaViewerException)
        // must not escape a dispatcher callback.
        DispatcherQueueControllerTryEnqueue(() => {
            if (args.Button == SystemMediaTransportControlsButton.Play) _folderSession?.VideoPlay();
            else if (args.Button == SystemMediaTransportControlsButton.Pause) _folderSession?.VideoPause();
            else if (args.Button == SystemMediaTransportControlsButton.Next) Send(Command.Next);
            else if (args.Button == SystemMediaTransportControlsButton.Previous) Send(Command.Prev);
        });
    }

    private static void OnMediaSeek(SystemMediaTransportControls sender, PlaybackPositionChangeRequestedEventArgs args) =>
        DispatcherQueueControllerTryEnqueue(() => _folderSession?.VideoSeek(args.RequestedPlaybackPosition.Ticks * 100, true));

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
        bool playing = video && state == 1;

        // Auto show/hide. Native owns the geometry and the idle rule, so this
        // reports the facts and does not move a window. Playing vs paused rides
        // on the same notification: pause and the end bring the bar back.
        if (video != _videoActive || playing != _videoPlaying)
        {
            if (video != _videoActive) SetSpeedVisible(video);
            _videoActive = video;
            _videoPlaying = playing;
            Send(Command.VideoActive, video ? (playing ? 2 : 1) : 0);
        }
        if (_smtc is not null) _smtc.IsEnabled = video;
        if (!video) return;

        long position = Math.Max(0, _folderSession.VideoPosition);
        UpdateMatchDuration(Math.Max(0, info.DurationNs) / 1_000_000);
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
                _audioTracks.SelectedIndex = info.AudioTracks == 0 ? -1
                    : _audioTrack < info.AudioTracks ? _audioTrack : 0;
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
