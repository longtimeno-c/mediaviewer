// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using XamlCanvas = Microsoft.UI.Xaml.Controls.Canvas;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 30 (plan/21, issue #40; owner 2026-09-26): the Video Editor window's
/// timeline — the WinUI twin of VideoEditorView.swift. The preview above it is
/// the viewer's own swapchain, moved into the window by native (one canvas,
/// one present path, rule 2); this island sits under it: transport, the cut
/// tools, a thumbnail track and a waveform over the edited program, a playhead
/// you can drag, and Export. A second island on the viewer's window says where
/// the picture went.
/// </summary>
/// <remarks>
/// The cut list is native's (shell/video_timeline.h, shared with the Mac).
/// Native pushes what to show (SetVideoEditorView, SetVideoEditorStrip) and
/// every control sends a command back; nothing here reads the clip. The keys
/// are native's too (main.cpp routes every key aimed at the editor window), so
/// the tooltips name them and the buttons never take focus on a click.
/// </remarks>
public static partial class IslandHost
{
    // chrome_host.h chrome_editor_*_args.
    internal const int EditorAttachArgsSize = 16;
    internal const int EditorLayoutArgsSize = 40;
    internal const int EditorViewArgsSize = 88;
    internal const int EditorStripArgsSize = 24;
    internal const int EditorThumbSize = 24;

    // Mirrors chrome_editor_action (chrome_host.h).
    private static class EditorActions
    {
        public const int Split = 1;
        public const int Delete = 2;
        public const int SetIn = 3;
        public const int SetOut = 4;
        public const int Undo = 5;
        public const int Redo = 6;
        public const int TogglePlay = 7;
        public const int StepBack = 8;
        public const int StepForward = 9;
        public const int Export = 10;
        public const int ExportExact = 11;
        public const int Close = 12;
        public const int Show = 13;
    }

    private static DesktopWindowXamlSource? _editorTimeline;
    private static DesktopWindowXamlSource? _editorAway;
    private static bool _editorAwayVisible;

    // Last view native pushed.
    private static bool _editorReady;
    private static long _editorLengthNs;
    private static long _editorPlayheadNs;
    private static long _editorSourceNs;
    private static bool _editorPlaying;
    private static int _editorSelected = -1;
    private static bool _editorCanUndo;
    private static bool _editorCanRedo;
    private static bool _editorEdited;
    private static ulong _editorGeneration = ulong.MaxValue;
    private static string _editorName = "";
    private static (long In, long Out)[] _editorPieces = Array.Empty<(long, long)>();
    private static readonly List<(long ShownNs, WriteableBitmap Image, int Width, int Height)> EditorThumbs = new();
    private static float[] _editorPeaks = Array.Empty<float>();
    // While the playhead is dragged it shows the pointer, not the player.
    private static long? _editorScrubNs;

    private static Grid? _editorRoot;
    private static Canvas? _editorTrack;
    private static Canvas? _editorPlayheadMark;
    private static TextBlock? _editorTimecode;
    private static TextBlock? _editorStatus;
    private static Button? _editorPlay;
    private static Button? _editorDelete;
    private static Button? _editorUndo;
    private static Button? _editorRedo;
    private static Button? _editorExport;
    private static Button? _editorExportExact;
    private static TextBlock? _editorAwayName;

    private const double RulerH = 18, VideoH = 76, AudioH = 52, TrackGap = 6;

    /// <summary>
    /// The window opened. In: chrome_editor_attach_args (the editor's HWND, the
    /// viewer's HWND). The timeline is built parked; LayoutVideoEditor places it.
    /// </summary>
    public static int AttachVideoEditor(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < EditorAttachArgsSize) return unchecked((int)0x80070057);
            IntPtr editor = checked((IntPtr)Marshal.ReadInt64(arg, 0));
            IntPtr viewer = checked((IntPtr)Marshal.ReadInt64(arg, 8));
            if (editor == IntPtr.Zero) return unchecked((int)0x80070057);
            EnsureApp();
            DetachEditorSources();
            _editorThemeWindow = editor;
            ApplyTitleBarTheme(editor);
            ResetEditorView();
            EnsureFocusHook();
            _editorTimeline = new DesktopWindowXamlSource();
            _editorTimeline.Initialize(Win32Interop.GetWindowIdFromWindow(editor));
            _editorTimeline.TakeFocusRequested += OnTakeFocusRequested;
            MoveAt(_editorTimeline, 0, 0, 1, 1);
            _editorTimeline.Content = BuildEditorTimeline();
            if (viewer != IntPtr.Zero)
            {
                _editorAway = new DesktopWindowXamlSource();
                _editorAway.Initialize(Win32Interop.GetWindowIdFromWindow(viewer));
                _editorAway.TakeFocusRequested += OnTakeFocusRequested;
                MoveAt(_editorAway, 0, 0, 1, 1);
            }
            _editorAwayVisible = false;
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("chrome AttachVideoEditor: {0}", ex);
            DetachEditorSources();
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>In: chrome_editor_layout_args (client pixels; native owns the maths).</summary>
    public static int LayoutVideoEditor(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < EditorLayoutArgsSize) return unchecked((int)0x80070057);
            int x = Marshal.ReadInt32(arg, 0), y = Marshal.ReadInt32(arg, 4);
            int w = Marshal.ReadInt32(arg, 8), h = Marshal.ReadInt32(arg, 12);
            bool away = Marshal.ReadInt32(arg, 16) != 0;
            int ax = Marshal.ReadInt32(arg, 20), ay = Marshal.ReadInt32(arg, 24);
            int aw = Marshal.ReadInt32(arg, 28), ah = Marshal.ReadInt32(arg, 32);
            bool focus = Marshal.ReadInt32(arg, 36) != 0;
            MoveAt(_editorTimeline, x, y, w, h);
            if (_editorAway is not null)
            {
                MoveAt(_editorAway, ax, ay, aw, ah);
                if (away && !_editorAwayVisible)
                {
                    _editorAway.Content = BuildEditorAway();
                    _editorAwayVisible = true;
                }
                else if (!away && _editorAwayVisible)
                {
                    _editorAway.Content = null;
                    _editorAwayName = null;
                    _editorAwayVisible = false;
                }
            }
            if (focus)
            {
                _editorTimeline?.NavigateFocus(new XamlSourceFocusNavigationRequest(
                    XamlSourceFocusNavigationReason.First));
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>
    /// In: chrome_editor_view_args. Pushed on every edit (a new generation) and
    /// by the playback tick (the playhead only). Pointers are valid for the call.
    /// </summary>
    public static int SetVideoEditorView(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < EditorViewArgsSize) return unchecked((int)0x80070057);
            _editorPlayheadNs = Marshal.ReadInt64(arg, 16);
            _editorPlaying = Marshal.ReadInt32(arg, 32) != 0;
            ulong generation = unchecked((ulong)Marshal.ReadInt64(arg, 56));
            if (generation != _editorGeneration)
            {
                _editorGeneration = generation;
                _editorReady = Marshal.ReadInt32(arg, 4) != 0;
                _editorLengthNs = Marshal.ReadInt64(arg, 8);
                _editorSourceNs = Marshal.ReadInt64(arg, 24);
                int count = Math.Max(0, Marshal.ReadInt32(arg, 36));
                _editorSelected = Marshal.ReadInt32(arg, 40);
                _editorCanUndo = Marshal.ReadInt32(arg, 44) != 0;
                _editorCanRedo = Marshal.ReadInt32(arg, 48) != 0;
                _editorEdited = Marshal.ReadInt32(arg, 52) != 0;
                long pieces = Marshal.ReadInt64(arg, 64);
                var list = new (long, long)[pieces == 0 ? 0 : count];
                for (int i = 0; i < list.Length; i++)
                {
                    IntPtr p = checked((IntPtr)pieces);
                    list[i] = (Marshal.ReadInt64(p, 16 * i), Marshal.ReadInt64(p, 16 * i + 8));
                }
                _editorPieces = list;
                long name = Marshal.ReadInt64(arg, 72);
                int nameLen = Marshal.ReadInt32(arg, 80);
                _editorName = name == 0 || nameLen <= 0
                    ? ""
                    : Marshal.PtrToStringUTF8(checked((IntPtr)name), nameLen) ?? "";
                RenderEditor(full: true);
            }
            else
            {
                RenderEditor(full: false);
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>
    /// In: chrome_editor_strip_args — the thumbnails (RGBA8, sRGB) and the audio
    /// envelope, once per clip. Copied here; native frees them after the call.
    /// </summary>
    public static int SetVideoEditorStrip(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < EditorStripArgsSize) return unchecked((int)0x80070057);
            long thumbs = Marshal.ReadInt64(arg, 0);
            int thumbCount = Math.Max(0, Marshal.ReadInt32(arg, 8));
            int peakCount = Math.Max(0, Marshal.ReadInt32(arg, 12));
            long peaks = Marshal.ReadInt64(arg, 16);
            EditorThumbs.Clear();
            for (int i = 0; thumbs != 0 && i < thumbCount; i++)
            {
                IntPtr t = checked((IntPtr)(thumbs + (long)EditorThumbSize * i));
                long shown = Marshal.ReadInt64(t, 0);
                int w = Marshal.ReadInt32(t, 8), h = Marshal.ReadInt32(t, 12);
                long rgba = Marshal.ReadInt64(t, 16);
                if (w <= 0 || h <= 0 || rgba == 0) continue;
                int bytes = checked(w * h * 4);
                byte[] px = new byte[bytes];
                Marshal.Copy(checked((IntPtr)rgba), px, 0, bytes);
                // RGBA -> BGRA (opaque, so premultiplied is the same bytes).
                for (int o = 0; o < bytes; o += 4) (px[o], px[o + 2]) = (px[o + 2], px[o]);
                var bitmap = new WriteableBitmap(w, h);
                px.CopyTo(0, bitmap.PixelBuffer, 0, bytes);
                bitmap.Invalidate();
                EditorThumbs.Add((shown, bitmap, w, h));
            }
            var env = new float[peaks == 0 ? 0 : peakCount];
            if (env.Length > 0) Marshal.Copy(checked((IntPtr)peaks), env, 0, env.Length);
            _editorPeaks = env;
            RenderEditor(full: true);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>The window closed: both islands go.</summary>
    public static int DetachVideoEditor(IntPtr arg, int sizeBytes)
    {
        _ = arg;
        _ = sizeBytes;
        try
        {
            DetachEditorSources();
            ResetEditorView();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static void DetachEditorSources()
    {
        _editorThemeWindow = IntPtr.Zero;
        _editorRoot = null;
        _editorTrack = null;
        _editorPlayheadMark = null;
        _editorTimecode = null;
        _editorStatus = null;
        _editorPlay = _editorDelete = _editorUndo = _editorRedo = _editorExport = _editorExportExact = null;
        _editorAwayName = null;
        _editorAwayVisible = false;
        if (_editorTimeline is not null) _editorTimeline.TakeFocusRequested -= OnTakeFocusRequested;
        if (_editorAway is not null) _editorAway.TakeFocusRequested -= OnTakeFocusRequested;
        DisposeSource(ref _editorTimeline);
        DisposeSource(ref _editorAway);
    }

    private static void ResetEditorView()
    {
        _editorReady = false;
        _editorLengthNs = _editorPlayheadNs = _editorSourceNs = 0;
        _editorPlaying = false;
        _editorSelected = -1;
        _editorCanUndo = _editorCanRedo = _editorEdited = false;
        _editorGeneration = ulong.MaxValue;
        _editorName = "";
        _editorPieces = Array.Empty<(long, long)>();
        EditorThumbs.Clear();
        _editorPeaks = Array.Empty<float>();
        _editorScrubNs = null;
    }

    private static void SendEditor(int action) => Send(Command.EditorAction, action);

    private static string EditorTimecode(long ns)
    {
        long cs = Math.Max(0, ns) / 10_000_000;
        long s = cs / 100;
        return s >= 3600
            ? $"{s / 3600}:{s / 60 % 60:00}:{s % 60:00}.{cs % 100:00}"
            : $"{s / 60}:{s % 60:00}.{cs % 100:00}";
    }

    private static long EditorShownPlayhead => _editorScrubNs ?? _editorPlayheadNs;

    // ---- the timeline island -------------------------------------------------------

    private static UIElement BuildEditorTimeline()
    {
        var root = new Grid { Background = Brush(Canvas), RequestedTheme = ElementTheme.Default };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // rule
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // toolbar
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // rule
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // key hints
        root.Children.Add(new Border { Height = 1, Background = Brush(Hairline) });

        var bar = new Grid { Padding = new Thickness(10, 6, 10, 6) };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _editorPlay = EditButton("Play", () => SendEditor(EditorActions.TogglePlay), tip: "Play / pause  Space");
        _editorTimecode = Text("", Title, UiFontSize - 2);
        _editorTimecode.VerticalAlignment = VerticalAlignment.Center;
        _editorTimecode.Margin = new Thickness(10, 0, 10, 0);
        _editorDelete = EditButton("Delete", () => SendEditor(EditorActions.Delete),
                                   tip: "Delete the selected piece  Delete");
        _editorUndo = EditButton("Undo", () => SendEditor(EditorActions.Undo), tip: "Undo  Ctrl+Z");
        _editorRedo = EditButton("Redo", () => SendEditor(EditorActions.Redo), tip: "Redo  Ctrl+Shift+Z");
        var left = Row(
            EditButton("< Frame", () => SendEditor(EditorActions.StepBack), tip: "Previous frame  Left"),
            _editorPlay,
            EditButton("Frame >", () => SendEditor(EditorActions.StepForward), tip: "Next frame  Right"),
            _editorTimecode,
            EditorDivider(),
            EditButton("Split", () => SendEditor(EditorActions.Split), tip: "Split at the playhead  Ctrl+B"),
            _editorDelete,
            EditButton("Set in", () => SendEditor(EditorActions.SetIn), tip: "Cut everything before the playhead  I"),
            EditButton("Set out", () => SendEditor(EditorActions.SetOut), tip: "Cut everything after the playhead  O"),
            EditorDivider(),
            _editorUndo,
            _editorRedo);
        bar.Children.Add(left);
        _editorExport = EditButton("Export", () => SendEditor(EditorActions.Export),
            tip: "Write the edit as a new file, cut on keyframes: instant, no quality loss  Ctrl+E");
        _editorExportExact = EditButton("Export exact", () => SendEditor(EditorActions.ExportExact),
            tip: "Frame-accurate: re-encoded on the hardware encoder, slower  Ctrl+Shift+E");
        var right = Row(_editorExport, _editorExportExact, EditorDivider(),
                        EditButton("Done", () => SendEditor(EditorActions.Close), tip: "Close the editor  Esc or Ctrl+W"));
        Grid.SetColumn(right, 2);
        bar.Children.Add(right);
        Grid.SetRow(bar, 1);
        root.Children.Add(bar);

        var rule = new Border { Height = 1, Background = Brush(Hairline) };
        Grid.SetRow(rule, 2);
        root.Children.Add(rule);

        var area = new Grid { Padding = new Thickness(12, 10, 12, 10) };
        _editorStatus = Text("Reading the clip…", Body, UiFontSize - 2);
        _editorStatus.HorizontalAlignment = HorizontalAlignment.Center;
        _editorStatus.VerticalAlignment = VerticalAlignment.Center;
        area.Children.Add(_editorStatus);
        _editorTrack = new Canvas
        {
            Height = RulerH + TrackGap + VideoH + TrackGap + AudioH + 4,
            VerticalAlignment = VerticalAlignment.Top,
            Background = Brush(Colors.Transparent),  // hit-testable everywhere
        };
        AutomationProperties_SetName(_editorTrack, "Timeline");
        _editorTrack.SizeChanged += (_, _) => Guard(() => RenderEditor(full: true));
        _editorTrack.PointerPressed += (_, e) => Guard(() =>
        {
            if (_editorTrack is null) return;
            _editorTrack.CapturePointer(e.Pointer);
            EditorScrubTo(e.GetCurrentPoint(_editorTrack).Position.X);
            e.Handled = true;
        });
        _editorTrack.PointerMoved += (_, e) => Guard(() =>
        {
            if (_editorTrack is null || _editorScrubNs is null) return;
            EditorScrubTo(e.GetCurrentPoint(_editorTrack).Position.X);
        });
        _editorTrack.PointerReleased += (_, e) => Guard(() =>
        {
            if (_editorTrack is null || _editorScrubNs is null) return;
            EditorScrubTo(e.GetCurrentPoint(_editorTrack).Position.X);
            _editorScrubNs = null;
            _editorTrack.ReleasePointerCapture(e.Pointer);
        });
        _editorTrack.PointerCaptureLost += (_, _) => Guard(() => _editorScrubNs = null);
        area.Children.Add(_editorTrack);
        Grid.SetRow(area, 3);
        root.Children.Add(area);

        TextBlock hints = Hint(
            "Space play · Left Right frame · J L second · I O in / out · Ctrl+B split · Delete piece · " +
            "Ctrl+Z undo · Ctrl+E export · Esc close");
        hints.Margin = new Thickness(12, 0, 12, 8);
        Grid.SetRow(hints, 4);
        root.Children.Add(hints);
        AutomationProperties_SetName(root, "Video Editor timeline");
        _editorRoot = root;
        RenderEditor(full: true);
        return root;
    }

    private static UIElement EditorDivider() =>
        new Border { Width = 1, Height = 18, Background = Brush(Hairline), Margin = new Thickness(6, 0, 6, 0) };

    private static void Guard(Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            // Never let an exception out of a XAML event: that is a fail-fast.
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static void EditorScrubTo(double x)
    {
        if (_editorTrack is null || !_editorReady || _editorLengthNs <= 0) return;
        double width = Math.Max(1, _editorTrack.ActualWidth);
        long t = (long)(Math.Clamp(x, 0, width) / width * _editorLengthNs);
        _editorScrubNs = t;
        MoveEditorPlayhead();
        Send(Command.EditorSeek, (float)(t / 1_000_000.0));
    }

    // `full`: the pieces, strip or size changed; otherwise only the playhead,
    // the timecode and play / pause move.
    private static void RenderEditor(bool full)
    {
        if (_editorRoot is null) return;
        string timecode = $"{EditorTimecode(EditorShownPlayhead)} / {EditorTimecode(_editorLengthNs)}";
        if (_editorTimecode is not null && _editorTimecode.Text != timecode)
        {
            _editorTimecode.Text = timecode;
            AutomationProperties_SetName(_editorTimecode,
                $"Playhead {EditorTimecode(EditorShownPlayhead)} of {EditorTimecode(_editorLengthNs)}");
        }
        if (_editorPlay is not null) SetButtonText(_editorPlay, _editorPlaying ? "Pause" : "Play");
        if (!full)
        {
            MoveEditorPlayhead();
            return;
        }
        if (_editorDelete is not null) _editorDelete.IsEnabled = _editorPieces.Length > 1;
        if (_editorUndo is not null) _editorUndo.IsEnabled = _editorCanUndo;
        if (_editorRedo is not null) _editorRedo.IsEnabled = _editorCanRedo;
        if (_editorExport is not null) _editorExport.IsEnabled = _editorEdited;
        if (_editorExportExact is not null) _editorExportExact.IsEnabled = _editorEdited;
        if (_editorStatus is not null) _editorStatus.Visibility = _editorReady ? Visibility.Collapsed : Visibility.Visible;
        if (_editorAwayName is not null) _editorAwayName.Text = _editorName;
        DrawEditorTrack();
    }

    private static void DrawEditorTrack()
    {
        Canvas? track = _editorTrack;
        if (track is null) return;
        track.Children.Clear();
        _editorPlayheadMark = null;
        track.Visibility = _editorReady ? Visibility.Visible : Visibility.Collapsed;
        double width = track.ActualWidth;
        if (!_editorReady || width < 2) return;
        long length = Math.Max(1, _editorLengthNs);
        double X(long t) => (double)t / length * width;

        DrawEditorRuler(track, width, length);
        long start = 0;
        for (int i = 0; i < _editorPieces.Length; i++)
        {
            (long pin, long pout) = _editorPieces[i];
            long len = pout - pin;
            double x0 = X(start);
            double w = Math.Max(1, X(start + len) - x0 - 2);
            DrawEditorPiece(track, i, pin, pout, x0, RulerH + TrackGap, w);
            DrawEditorWave(track, pin, pout, x0, RulerH + TrackGap + VideoH + TrackGap, w);
            start += len;
        }
        AutomationProperties_SetName(track, $"Timeline, {_editorPieces.Length} pieces");

        // The playhead, on top; the tick moves it without a redraw.
        var mark = new Canvas { IsHitTestVisible = false };
        mark.Children.Add(new Rectangle
        {
            Width = 2,
            Height = track.Height,
            Fill = Brush(Colors.Red),
        });
        var head = new Polygon { Fill = Brush(Colors.Red) };
        head.Points.Add(new Point(-5, 0));
        head.Points.Add(new Point(7, 0));
        head.Points.Add(new Point(1, 8));
        mark.Children.Add(head);
        track.Children.Add(mark);
        _editorPlayheadMark = mark;
        MoveEditorPlayhead();
    }

    private static void MoveEditorPlayhead()
    {
        if (_editorPlayheadMark is null || _editorTrack is null) return;
        double width = _editorTrack.ActualWidth;
        long length = Math.Max(1, _editorLengthNs);
        XamlCanvas.SetLeft(_editorPlayheadMark, (double)Math.Clamp(EditorShownPlayhead, 0, length) / length * width - 1);
        if (_editorTimecode is not null)
        {
            _editorTimecode.Text = $"{EditorTimecode(EditorShownPlayhead)} / {EditorTimecode(_editorLengthNs)}";
        }
    }

    private static void DrawEditorRuler(Canvas track, double width, long length)
    {
        // A tick every 1, 2, 5 … 600 s, whichever keeps labels 60 DIP or more apart.
        double seconds = length / 1e9;
        double[] steps = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = steps.FirstOrDefault(s => seconds / s * 60 <= width, 600);
        for (double t = 0; t <= seconds; t += step)
        {
            double x = t / seconds * width;
            var tick = new Rectangle { Width = 1, Height = 6, Fill = Brush(Hairline) };
            XamlCanvas.SetLeft(tick, x);
            XamlCanvas.SetTop(tick, RulerH - 6);
            track.Children.Add(tick);
            TextBlock label = Text(EditorTimecode((long)(t * 1e9)).Split('.')[0], Body, UiFontSize - 6);
            label.TextWrapping = TextWrapping.NoWrap;
            XamlCanvas.SetLeft(label, x + 2);
            XamlCanvas.SetTop(label, -2);
            track.Children.Add(label);
        }
    }

    private static void DrawEditorPiece(Canvas track, int index, long pin, long pout, double x, double y, double w)
    {
        // Thumbnails of this piece, each at its own source time, tiled to fill it.
        var tiles = new Canvas
        {
            Width = w,
            Height = VideoH,
            Clip = new RectangleGeometry { Rect = new Rect(0, 0, w, VideoH) },
        };
        double len = Math.Max(1, pout - pin);
        double tx = 0;
        while (tx < w && EditorThumbs.Count > 0)
        {
            long at = pin + (long)(tx / w * len);
            var thumb = EditorThumbs[0];
            foreach (var t in EditorThumbs)
            {
                if (t.ShownNs <= at) thumb = t;
                else break;
            }
            double tw = VideoH * thumb.Width / Math.Max(1, thumb.Height);
            var image = new Image { Source = thumb.Image, Width = tw, Height = VideoH, Stretch = Stretch.Fill };
            XamlCanvas.SetLeft(image, tx);
            tiles.Children.Add(image);
            tx += Math.Max(8, tw);
        }
        var frame = new Border
        {
            Width = w,
            Height = VideoH,
            CornerRadius = new CornerRadius(5),
            Background = Brush(ChromeColour.Surface),
            Child = tiles,
        };
        XamlCanvas.SetLeft(frame, x);
        XamlCanvas.SetTop(frame, y);
        track.Children.Add(frame);
        bool selected = index == _editorSelected;
        var outline = new Border
        {
            Width = w,
            Height = VideoH,
            CornerRadius = new CornerRadius(5),
            BorderBrush = Brush(selected ? TrimAccent : Hairline),
            BorderThickness = new Thickness(selected ? 3 : 1),
            IsHitTestVisible = false,
        };
        XamlCanvas.SetLeft(outline, x);
        XamlCanvas.SetTop(outline, y);
        track.Children.Add(outline);
    }

    private static void DrawEditorWave(Canvas track, long pin, long pout, double x, double y, double w)
    {
        var box = new Border
        {
            Width = w,
            Height = AudioH,
            CornerRadius = new CornerRadius(4),
            Background = Brush(ChromeColour.Surface),
        };
        XamlCanvas.SetLeft(box, x);
        XamlCanvas.SetTop(box, y);
        track.Children.Add(box);
        if (_editorPeaks.Length == 0 || _editorSourceNs <= 0)
        {
            TextBlock none = Text("No audio", Body, UiFontSize - 5);
            XamlCanvas.SetLeft(none, x + 8);
            XamlCanvas.SetTop(none, y + AudioH / 2 - 9);
            track.Children.Add(none);
            return;
        }
        // An envelope polygon: the peaks along the top, mirrored back along the bottom.
        int n = _editorPeaks.Length;
        double mid = y + AudioH / 2;
        int cols = Math.Max(1, (int)(w / 2));
        var top = new Point[cols];
        for (int c = 0; c < cols; c++)
        {
            long s = pin + (long)((double)c / cols * (pout - pin));
            int b = Math.Clamp((int)((double)s / _editorSourceNs * n), 0, n - 1);
            double h = Math.Max(0.5, _editorPeaks[b] * (AudioH / 2 - 2));
            top[c] = new Point(x + c * 2, mid - h);
        }
        var wave = new Polygon { Fill = Brush(TrimAccent), Opacity = 0.75, IsHitTestVisible = false };
        foreach (Point p in top) wave.Points.Add(p);
        for (int c = cols - 1; c >= 0; c--) wave.Points.Add(new Point(top[c].X, 2 * mid - top[c].Y));
        track.Children.Add(wave);
    }

    // ---- the card on the viewer's window ---------------------------------------------

    private static UIElement BuildEditorAway()
    {
        var root = new Grid { Background = Brush(Canvas), RequestedTheme = ElementTheme.Default };
        var col = new StackPanel
        {
            Spacing = 10,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        TextBlock head = Text("Editing in the Video Editor window", Body, UiFontSize);
        head.HorizontalAlignment = HorizontalAlignment.Center;
        col.Children.Add(head);
        _editorAwayName = Text(_editorName, Body, UiFontSize - 3, maxLines: 1);
        _editorAwayName.HorizontalAlignment = HorizontalAlignment.Center;
        col.Children.Add(_editorAwayName);
        StackPanel buttons = Row(
            EditButton("Show editor", () => SendEditor(EditorActions.Show), tip: "Bring the Video Editor forward"),
            EditButton("Done", () => SendEditor(EditorActions.Close), tip: "Close the editor and put the picture back"));
        buttons.HorizontalAlignment = HorizontalAlignment.Center;
        col.Children.Add(buttons);
        root.Children.Add(col);
        AutomationProperties_SetName(root, "Editing in the Video Editor window");
        return root;
    }
}
