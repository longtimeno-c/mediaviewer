// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
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
    private static TextBlock? _videoElapsed;
    private static TextBlock? _videoDuration;
    private static Button? _muteButton;
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
    // Issue #362. The slider raises ValueChanged from its own pointer handler
    // before PointerPressed sets _draggingSeek, and the first sample is the
    // range minimum. Seeking that exactly presents the opening frame.
    // Half a second, player::scrub_origin_playhead_ns.
    private static bool _deferredSeekQueued, _seekPlayheadCaptured;
    private static long _deferredSeekNs = -1;
    private static long _seekPlayheadNs;
    private static int _originHoldEpoch;
    private static DispatcherQueueTimer? _originHoldTimer;
    private static bool _videoActive;
    private static bool _videoPlaying;
    private static bool _transportHeld;
    private static int _transportWidth;

    // TransportView.swift: a 44 pt strip, controls 10 apart, 14 in from each
    // side (kTransportDip / kTransportPadDip in chrome_host.h match). The
    // island is a rectangular window, so the Mac's radius-10 card is not.
    private const int TransportDip = 44;
    private const double TransportSpacing = 10;
    private const double TransportPad = 14;
    // The Mac's bar is 720 wide at most (main_mac.mm) and the scrubber takes
    // what the buttons leave. This is the row inside that, before native adds
    // the padding back; native narrows it to the window.
    private const double TransportRowDip = 720 - 2 * TransportPad;
    // The narrowest scrubber worth reporting a wider bar for (trim armed).
    private const double SeekMinWidth = 160;

    public static int AttachTransport(IntPtr arg, int sizeBytes)
    {
        try
        {
            int hr = BeginAttach(arg, sizeBytes, out ChromeFilmstripArgs args, out IntPtr parent);
            if (hr != 0) return hr;
            BorrowSessionIfNone(args);

            // Parked, with no content: nothing is open.
            Move(NewIsland(ref _transport, parent, takeFocus: true), 1, 1, args.ClientHeight);

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
                _videoElapsed = null;
                _videoDuration = null;
                _muteButton = null;
                _speed = null;
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
            _videoElapsed = null;
            _videoDuration = null;
            _muteButton = null;
            _speed = null;
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
            SetMuteIcon();
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
        // The Mac's strip (TransportView.swift): back 10 s, play / pause and
        // forward 10 s as icons, each with its key in the tooltip.
        _play = IconButton(Glyph.Pause, "Pause  Space", ToggleVideo);
        Button back = IconButton(Glyph.Rewind, "Back 10 seconds", () => SkipVideo(-10_000_000_000));
        Button forward = IconButton(Glyph.FastForward, "Forward 10 seconds", () => SkipVideo(10_000_000_000));
        // Flexes (issue #195): the scrubber takes whatever width the buttons
        // leave, as the Mac's does.
        _seek = new Slider
        {
            Minimum = 0,
            Maximum = 1,
            StepFrequency = 0.01,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
        };
        AutomationProperties_SetName(_seek, "Seek");
        _seek.AddHandler(UIElement.PointerPressedEvent,
                         new PointerEventHandler((_, _) =>
                         {
                             _draggingSeek = true;
                             SetTransportHeld(true);
                             CaptureSeekPlayhead();
                         }), true);
        _seek.AddHandler(UIElement.PointerReleasedEvent,
                         new PointerEventHandler((_, _) => { FinishSeek(); RestoreCanvasFocus(); }), true);
        _seek.PointerCaptureLost += (_, _) => FinishSeek();
        _seek.ValueChanged += (_, e) =>
        {
            if (_updatingVideo || _folderSession is null) return;
            CaptureSeekPlayhead();
            // A newer sample cancels an origin hold left by a click that
            // released on the slider's minimum (issue #362).
            CancelOriginHold();
            // Coalesce. A click's minimum and its real time both land before
            // the dispatcher runs, and PointerPressed has set _draggingSeek
            // by then. FinishSeek is the exact seek (docs/design/05).
            _deferredSeekNs = (long)(e.NewValue * 1e9);
            if (_deferredSeekQueued) return;
            _deferredSeekQueued = true;
            if (_dispatcher?.DispatcherQueue.TryEnqueue(ApplyDeferredTransportSeek) != true)
                ApplyDeferredTransportSeek();
        };
        // Elapsed left of the scrubber, duration right of it (TransportView.swift).
        _videoElapsed = ClockText(TextAlignment.Right);
        _videoDuration = ClockText(TextAlignment.Left);
        // Mute in the strip, as on the Mac. A request to native, which owns it
        // for Shift+M too and answers with ApplyAudio (issue #214).
        _muteButton = IconButton(Glyph.Volume, "Mute  Shift+M", () => Send(Command.VideoMuted, _audioMuted ? 0 : 1));
        SetMuteIcon();

        Button more = IconButton(Glyph.More, "More: frame step, volume, audio track, loop", () => { });
        var panel = new StackPanel { Spacing = Space(10), Padding = Pad(12), MinWidth = 260 };
        var steps = new StackPanel { Orientation = Orientation.Horizontal };
        steps.Children.Add(TextButton("Previous frame", () => _folderSession?.VideoStep(-1)));
        steps.Children.Add(TextButton("Next frame", () => _folderSession?.VideoStep(1)));
        ToolTipService.SetToolTip(steps.Children[0], ",");
        ToolTipService.SetToolTip(steps.Children[1], ".");
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

        // One row, the Mac's order: skip, play, skip, elapsed, scrubber,
        // duration, trim, speed, mute, More. Only the scrubber's column is
        // star-sized, so the strip's spare width all goes to it.
        var row = new Grid
        {
            ColumnSpacing = TransportSpacing,
            VerticalAlignment = VerticalAlignment.Center,
        };
        FrameworkElement[] cells =
        {
            back, _play, forward, _videoElapsed,
            // PR 13: trim's markers, keyframe grid and kept range over the
            // scrubber, and its label and save buttons after the clock
            // (IslandHost.Clip.cs).
            WrapSeekForTrim(_seek),
            _videoDuration, BuildTrimBar(), BuildSpeed(), _muteButton, more,
        };
        const int seekColumn = 4;
        for (int i = 0; i < cells.Length; ++i)
        {
            row.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = i == seekColumn ? new GridLength(1, GridUnitType.Star) : GridLength.Auto,
            });
            Grid.SetColumn(cells[i], i);
            row.Children.Add(cells[i]);
        }

        // Native sizes the bar from the width reported here and narrows it to
        // the window; the scrubber flexes inside whatever it gets.
        row.LayoutUpdated += (_, _) => ReportTransportWidth(row, seekColumn);

        // A floating bar over the video (issue #38): hairline all round, the
        // controls centred. The island window is rectangular, so no rounding.
        // The padding is inside the hairline, so it is one less than the Mac's 14.
        var ground = new Grid();
        ThemeField.Lay(ground);
        ground.Children.Add(row);
        var root = new Border
        {
            RequestedTheme = IslandTheme,
            Background = Brush(Canvas),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            Height = TransportDip,
            Padding = new Thickness(TransportPad - 1, 0, TransportPad - 1, 0),
            Child = ground,
        };
        // Issue #224: the island covers the canvas's WM_DROPFILES.
        WireFileDrop(root);
        return root;
    }

    private static TextBlock ClockText(TextAlignment align) => new()
    {
        FontFamily = UiFont,
        FontSize = TypeCaption,
        Foreground = Brush(Body),
        VerticalAlignment = VerticalAlignment.Center,
        TextAlignment = align,
        MinWidth = 40,
    };

    private static void SetMuteIcon()
    {
        if (_muteButton is null) return;
        SetIcon(_muteButton, _audioMuted ? Glyph.Mute : Glyph.Volume,
                _audioMuted ? "Unmute  Shift+M" : "Mute  Shift+M");
    }

    // The row's wanted width: its fixed controls at their natural size plus
    // the scrubber, and at least the Mac's 720 bar. Trim's label and buttons
    // can push past that; the bar then grows rather than clip them, and
    // native still bounds it by the window's sides.
    private static void ReportTransportWidth(Grid row, int seekColumn)
    {
        double width = 0;
        int shown = 0;
        foreach (UIElement child in row.Children)
        {
            if (child.Visibility != Visibility.Visible) continue;
            ++shown;
            if (child is FrameworkElement fe && Grid.GetColumn(fe) == seekColumn) continue;
            width += child.DesiredSize.Width;
        }
        if (shown > 1) width += row.ColumnSpacing * (shown - 1);
        int dip = (int)Math.Ceiling(Math.Max(TransportRowDip, width + SeekMinWidth));
        if (dip == _transportWidth) return;
        _transportWidth = dip;
        // Not from inside layout: native answers by moving this island.
        if (_dispatcher?.DispatcherQueue.TryEnqueue(() => Send(Command.TransportWidth, dip)) != true)
            Send(Command.TransportWidth, dip);
    }

    private static void CancelOriginHold()
    {
        ++_originHoldEpoch;
        _originHoldTimer?.Stop();
        _originHoldTimer = null;
    }

    private static void CaptureSeekPlayhead()
    {
        if (_seekPlayheadCaptured || _folderSession is null) return;
        _seekPlayheadNs = Math.Max(0, _folderSession.VideoPosition);
        _seekPlayheadCaptured = true;
    }

    // The latest slider sample. A drag scrubs by keyframe; anything else (a
    // keyboard change) seeks exactly. The minimum during a drag, while the
    // playhead was already past the start, is not a seek.
    private static void ApplyDeferredTransportSeek()
    {
        _deferredSeekQueued = false;
        if (_folderSession is null || _deferredSeekNs < 0) return;
        long ns = _deferredSeekNs;
        _deferredSeekNs = -1;
        if (_draggingSeek)
        {
            if (ns <= 0 && _seekPlayheadNs > 500_000_000) return;
            long now = Environment.TickCount64;
            if (now - _lastScrub < 75) return;
            _lastScrub = now;
            _folderSession.VideoSeek(ns, false);
            return;
        }
        _seekPlayheadCaptured = false;
        _folderSession.VideoSeek(ns, true);
    }

    private static void FinishSeek()
    {
        if (!_draggingSeek || _seek is null || _folderSession is null) return;
        long ns = (long)(_seek.Value * 1e9);
        // Clear the coalesced sample before dropping _draggingSeek, so an
        // apply queued from this gesture cannot seek the minimum exactly.
        _deferredSeekNs = -1;
        _draggingSeek = false;
        SetTransportHeld(false);
        CancelOriginHold();
        if (ns <= 0 && _seekPlayheadNs > 500_000_000)
        {
            // The real time may still be on its way. A sample cancels this.
            int epoch = _originHoldEpoch;
            var timer = _dispatcher?.DispatcherQueue.CreateTimer();
            if (timer is null)
            {
                _seekPlayheadCaptured = false;
                _folderSession.VideoSeek(0, true);
                return;
            }
            _originHoldTimer = timer;
            timer.Interval = TimeSpan.FromMilliseconds(80);
            timer.IsRepeating = false;
            timer.Tick += (_, _) =>
            {
                if (epoch != _originHoldEpoch || _folderSession is null || _seek is null) return;
                _originHoldTimer = null;
                _seekPlayheadCaptured = false;
                _folderSession.VideoSeek((long)(_seek.Value * 1e9), true);
            };
            timer.Start();
            return;
        }
        _seekPlayheadCaptured = false;
        _folderSession.VideoSeek(ns, true);
    }

    private static void SkipVideo(long deltaNs)
    {
        if (_folderSession is null) return;
        long duration = _folderSession.VideoInfo.DurationNs;
        long target = Math.Max(0, _folderSession.VideoPosition + deltaNs);
        // An unknown duration (0) must not clamp a forward skip back to the start.
        if (duration > 0) target = Math.Min(target, duration);
        _folderSession.VideoSeek(target, true);
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
        // Armed only while a clip is open (issue #216), so a still costs no
        // tick: the open and state completions start it (DrainFolder), and the
        // tick that finds no clip stops it.
        _videoTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        _videoTimer.Tick += (_, _) => UpdateVideoControls();
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
        ForgetMediaControls();
    }

    /// <summary>
    /// 150 ms playback poll, while a clip is open. It runs whether or not the
    /// strip is up, because it is also what tells native when to raise and
    /// lower it. Completions call it too, which is what starts it.
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
            _videoActive = video;
            _videoPlaying = playing;
            Send(Command.VideoActive, video ? (playing ? 2 : 1) : 0);
        }
        if (_videoTimer is not null && _videoTimer.IsEnabled != video)
        {
            if (video) _videoTimer.Start(); else _videoTimer.Stop();
        }
        if (_smtc is not null && _smtcEnabled != video)
        {
            _smtc.IsEnabled = video;
            _smtcEnabled = video;
        }
        if (!video)
        {
            ForgetMediaControls();
            return;
        }

        long position = Math.Max(0, _folderSession.VideoPosition);
        UpdateMatchDuration(Math.Max(0, info.DurationNs) / 1_000_000);
        if (_seek is not null && _play is not null && _videoElapsed is not null && _videoDuration is not null
            && _audioTracks is not null)
        {
            _updatingVideo = true;
            _seek.Maximum = Math.Max(.001, info.DurationNs / 1e9);
            if (!_draggingSeek) _seek.Value = Math.Clamp(position / 1e9, 0, _seek.Maximum);
            SetIcon(_play, state == 1 ? Glyph.Pause : Glyph.Play, state == 1 ? "Pause  Space" : "Play  Space");
            // m:ss, h:mm:ss from an hour: "mm\:ss" dropped the hours of a long clip.
            _videoElapsed.Text = Clock(position);
            _videoDuration.Text = Clock(info.DurationNs);
            if (_audioTracks.Items.Count != info.AudioTracks) {
                _audioTracks.Items.Clear();
                for (uint i = 0; i < info.AudioTracks; ++i) _audioTracks.Items.Add($"Track {i + 1}");
                _audioTracks.SelectedIndex = info.AudioTracks == 0 ? -1
                    : _audioTrack < info.AudioTracks ? _audioTrack : 0;
            }
            _updatingVideo = false;
        }

        UpdateMediaControls(state, Math.Max(0, info.DurationNs), position);
    }

    // What the media controls were last told (issue #216). Each setter is a
    // cross-process call, and the system runs the position on by itself while
    // it is told Playing, so they are pushed on a change, not per tick.
    private static bool? _smtcEnabled;
    private static uint _smtcState = uint.MaxValue;
    private static string? _smtcTitle;
    private static long _smtcDurationNs = -1;
    private static long _smtcPositionNs;
    private static long _smtcAtMs;
    private static double _smtcRate = 1;
    // Native's playback rate, from ApplyRate.
    private static double _videoRate = 1;

    // The next clip pushes everything again.
    private static void ForgetMediaControls()
    {
        _smtcState = uint.MaxValue;
        _smtcTitle = null;
        _smtcDurationNs = -1;
        if (_smtc is null) _smtcEnabled = null;
    }

    private static void UpdateMediaControls(uint state, long durationNs, long position)
    {
        if (_smtc is null) return;
        string title = _selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex].Name : "MediaViewer";
        if (title != _smtcTitle)
        {
            var updater = _smtc.DisplayUpdater; updater.Type = MediaPlaybackType.Video;
            updater.VideoProperties.Title = title;
            updater.Update();
            _smtcTitle = title;
        }

        long now = Environment.TickCount64;
        bool playing = state == 1;
        bool timeline = state != _smtcState || durationNs != _smtcDurationNs || _videoRate != _smtcRate;
        if (!timeline)
        {
            // A seek is the position leaving the line it was on: still while
            // paused, the rate times the wall clock while playing. Half a
            // second covers a tick's jitter and an audio-clock correction.
            double expected = _smtcPositionNs + (playing ? (now - _smtcAtMs) * 1e6 * _smtcRate : 0);
            timeline = Math.Abs(position - expected) > 500_000_000;
        }
        if (!timeline) return;

        if (state != _smtcState)
            _smtc.PlaybackStatus = playing ? MediaPlaybackStatus.Playing : MediaPlaybackStatus.Paused;
        if (_videoRate != _smtcRate) _smtc.PlaybackRate = _videoRate;
        _smtc.UpdateTimelineProperties(new SystemMediaTransportControlsTimelineProperties {
            StartTime = TimeSpan.Zero, MinSeekTime = TimeSpan.Zero,
            EndTime = TimeSpan.FromTicks(durationNs / 100),
            MaxSeekTime = TimeSpan.FromTicks(durationNs / 100),
            Position = TimeSpan.FromTicks(position / 100)
        });
        _smtcState = state;
        _smtcDurationNs = durationNs;
        _smtcRate = _videoRate;
        _smtcPositionNs = position;
        _smtcAtMs = now;
    }
}
