// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using XamlCanvas = Microsoft.UI.Xaml.Controls.Canvas;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 30 (docs/design/21, issue #40; owner 2026-09-26): the Video Editor window's
/// timeline — the WinUI twin of VideoEditorView.swift. The preview above it is
/// the viewer's own swapchain, moved into the window by native (one canvas,
/// one present path, rule 2); this island sits under it: transport, the cut
/// tools, a thumbnail track and a waveform over the edited program, a playhead
/// you can drag (it snaps to joins and marks), piece edges you can drag, a
/// marked range, and Export. A second island on the viewer's window says where
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
    internal const int EditorViewArgsSize = 160;
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
        public const int MarkIn = 14;
        public const int MarkOut = 15;
        public const int ClearMarks = 16;
        public const int TurnLeft = 17;
        public const int TurnRight = 18;
        public const int MirrorH = 19;
        public const int MirrorV = 20;
        public const int CropOriginal = 21;  // 21-26: original, 16:9, 9:16, 1:1, 4:3, 4:5
        public const int SaveFrame = 27;
        public const int AutoLight = 28;
        public const int ResetLight = 29;
        public const int ToggleMute = 30;
        public const int SpeedHalf = 31;  // 31-33: 0.5x, 1x, 2x
    }

    // PR 11's five sliders (edit/adjust.cpp range_of), as on the Mac.
    private static readonly (string Name, double Min, double Max, double Step)[] EditorLightSpecs =
    {
        ("Exposure", -5, 5, 0.1), ("Contrast", -100, 100, 5), ("Saturation", -100, 100, 5),
        ("Temperature", -100, 100, 5), ("Tint", -100, 100, 5),
    };
    private static readonly float[] _editorLight = new float[5];
    // Sound (slice 3): mute, volume 0..2, fades in milliseconds.
    private static bool _editorMuted;
    private static float _editorVolume = 1;
    private static int _editorFadeInMs, _editorFadeOutMs;
    private static float _editorSpeed = 1;  // slice 4: 0.5, 1 or 2
    private static Button? _editorSpeedButton;

    private static readonly string[] EditorCropNames = { "Original", "16:9", "9:16", "1:1", "4:3", "4:5" };

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
    // The look (docs/plans/video-editor-look.md).
    private static int _editorLookCrop;
    private static bool _editorLookCopyable = true;
    private static ulong _editorGeneration = ulong.MaxValue;
    private static string _editorName = "";
    private static (long In, long Out)[] _editorPieces = Array.Empty<(long, long)>();
    private static readonly List<(long ShownNs, WriteableBitmap Image, int Width, int Height)> EditorThumbs = new();
    private static float[] _editorPeaks = Array.Empty<float>();
    private static double _editorFps;
    private static long _editorMarkIn = -1;
    private static long _editorMarkOut = -1;
    // While the playhead is dragged it shows the pointer, not the player.
    private static long? _editorScrubNs;
    // While a piece's edge is dragged: where it was (source ns), where the
    // pointer went down, and the scale then (the program re-fits as it moves).
    private static (long OriginNs, double OriginX, double NsPerDip)? _editorTrim;

    /// The timeline's canvas: a Canvas that can show the resize cursor over a
    /// piece's edge (ProtectedCursor is only settable from a subclass).
    private sealed class EditorTrackCanvas : XamlCanvas
    {
        private bool _resize;

        public void SetResizeCursor(bool on)
        {
            if (on == _resize) return;
            _resize = on;
            ProtectedCursor = on ? InputSystemCursor.Create(InputSystemCursorShape.SizeWestEast) : null;
        }
    }

    private static Grid? _editorRoot;
    private static EditorTrackCanvas? _editorTrack;
    private static Canvas? _editorPlayheadMark;
    private static TextBlock? _editorTimecode;
    // While the clip is read: the tracks at their own size, quiet (no spinner:
    // reading the strip can take seconds). The Mac's TimelineSkeleton.
    private static FrameworkElement? _editorSkeleton;
    private static bool _editorShownReady;
    // What the track last drew, so a change of choice or marks can fade.
    private static int _editorDrawnSelected = -1;
    private static int _editorDrawnPieces;
    private static (long In, long Out)? _editorDrawnMarks;
    // The Light flyout follows a pushed view while it is open (Auto, Reset, undo).
    private static Action? _editorLightSync;
    private static Button? _editorSoundButton;
    private static Button? _editorPlay;
    private static Button? _editorDelete;
    private static Button? _editorUndo;
    private static Button? _editorRedo;
    private static Button? _editorExport;
    private static Button? _editorExportExact;
    private static StackPanel? _editorAddons;  // add-ons' buttons (editor_items): Enhance…
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
                _editorFps = Marshal.ReadInt32(arg, 84) / 1000.0;
                _editorMarkIn = Marshal.ReadInt64(arg, 88);
                _editorMarkOut = Marshal.ReadInt64(arg, 96);
                _editorLookCrop = Marshal.ReadInt32(arg, 112);
                _editorLookCopyable = Marshal.ReadInt32(arg, 116) != 0;
                for (int i = 0; i < _editorLight.Length; i++)
                {
                    _editorLight[i] = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 120 + 4 * i));
                }
                _editorMuted = Marshal.ReadInt32(arg, 140) != 0;
                _editorVolume = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 144));
                _editorFadeInMs = Marshal.ReadInt32(arg, 148);
                _editorFadeOutMs = Marshal.ReadInt32(arg, 152);
                _editorSpeed = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 156));
                if (_editorSpeedButton is not null) SetButtonText(_editorSpeedButton, EditorSpeedName(_editorSpeed));
                _editorLightSync?.Invoke();
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
        _editorSkeleton = null;
        _editorLightSync = null;
        _editorPlay = _editorDelete = _editorUndo = _editorRedo = _editorExport = _editorExportExact = _editorSpeedButton = null;
        _editorSoundButton = null;
        _editorAddons = null;
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
        _editorTrim = null;
        _editorFps = 0;
        _editorMarkIn = _editorMarkOut = -1;
        _editorShownReady = false;
        _editorDrawnSelected = -1;
        _editorDrawnPieces = 0;
        _editorDrawnMarks = null;
    }

    private static void SendEditor(int action) => Send(Command.EditorAction, action);

    // "MM:SS:FF" (or "H:MM:SS:FF"), the frame within the second at the clip's
    // rate; "M:SS.cc" hundredths when the rate is unknown. VideoEditorView.swift's twin.
    private static string EditorTimecode(long ns)
    {
        long t = Math.Max(0, ns);
        if (_editorFps <= 0)
        {
            long cs = t / 10_000_000;
            long sec = cs / 100;
            return sec >= 3600
                ? $"{sec / 3600}:{sec / 60 % 60:00}:{sec % 60:00}.{cs % 100:00}"
                : $"{sec / 60}:{sec % 60:00}.{cs % 100:00}";
        }
        long s = t / 1_000_000_000;
        int perSecond = Math.Max(1, (int)Math.Ceiling(_editorFps));
        int frame = Math.Min(perSecond - 1, (int)((t % 1_000_000_000) / 1e9 * _editorFps + 1e-6));
        return s >= 3600
            ? $"{s / 3600}:{s / 60 % 60:00}:{s % 60:00}:{frame:00}"
            : $"{s / 60:00}:{s % 60:00}:{frame:00}";
    }

    // The ruler's labels: whole seconds.
    private static string EditorRulerLabel(long ns)
    {
        long s = Math.Max(0, ns) / 1_000_000_000;
        return s >= 3600 ? $"{s / 3600}:{s / 60 % 60:00}:{s % 60:00}" : $"{s / 60}:{s % 60:00}";
    }

    private static bool EditorHasMarks => _editorMarkIn >= 0 || _editorMarkOut >= 0;

    private static long EditorShownPlayhead => _editorScrubNs ?? _editorPlayheadNs;

    // ---- the timeline island -------------------------------------------------------

    private static UIElement BuildEditorTimeline()
    {
        var root = new Grid { Background = Brush(Canvas), RequestedTheme = IslandTheme };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // rule
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // toolbar
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });  // key hints
        // The one hairline: where the preview meets the timeline. The bar and
        // the tracks share a surface, so spacing (not a rule) parts them.
        root.Children.Add(new Border { Height = 1, Background = Brush(Hairline) });

        // The bar pulls back by its buttons' inset (8), so the first icon's ink
        // lines up with the tracks and the key line, 12 from the edge.
        var bar = new Grid { Padding = Pad(4, 6, 4, 6) };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _editorPlay = PaneIconButton(Glyph.Play, "Play", "Space", () => SendEditor(EditorActions.TogglePlay));
        _editorTimecode = Text("", Title, UiFontSize - 2);
        _editorTimecode.VerticalAlignment = VerticalAlignment.Center;
        _editorTimecode.Margin = Pad(10, 0, 10, 0);
        _editorDelete = EditButton("Delete", () => SendEditor(EditorActions.Delete),
                                   tip: "Delete the marked range, else the selected piece  Delete");
        _editorUndo = EditButton("Undo", () => SendEditor(EditorActions.Undo), tip: "Undo  Ctrl+Z");
        _editorRedo = EditButton("Redo", () => SendEditor(EditorActions.Redo), tip: "Redo  Ctrl+Shift+Z");
        Button saveFrame = PaneIconButton(EditorGlyphCamera, "Save frame", "Ctrl+Shift+S",
                                          () => SendEditor(EditorActions.SaveFrame));
        ToolTipService.SetToolTip(saveFrame, "Save this frame as a photo  Ctrl+Shift+S");
        var left = Row(
            PaneIconButton(Glyph.Previous, "Previous frame", "Left", () => SendEditor(EditorActions.StepBack)),
            _editorPlay,
            PaneIconButton(Glyph.Next, "Next frame", "Right", () => SendEditor(EditorActions.StepForward)),
            _editorTimecode,
            EditorDivider(),
            EditButton("Split", () => SendEditor(EditorActions.Split), tip: "Split at the playhead  Ctrl+B"),
            _editorDelete,
            EditorDivider(),
            EditButton("Mark in", () => SendEditor(EditorActions.MarkIn), tip: "Mark the start of a range to delete  I"),
            EditButton("Mark out", () => SendEditor(EditorActions.MarkOut), tip: "Mark the end of a range to delete  O"),
            EditorDivider(),
            EditButton("Trim start", () => SendEditor(EditorActions.SetIn), tip: "Cut everything before the playhead  ["),
            EditButton("Trim end", () => SendEditor(EditorActions.SetOut), tip: "Cut everything after the playhead  ]"),
            EditorDivider(),
            _editorUndo,
            _editorRedo,
            EditorDivider(),
            // The look's tools are icons, as on the Mac: each with its tooltip
            // and an accessibility name.
            PaneIconButton(Glyph.Rotate, "Turn left", null, () => SendEditor(EditorActions.TurnLeft),
                           new ScaleTransform { ScaleX = -1 }),
            PaneIconButton(Glyph.Rotate, "Turn right", null, () => SendEditor(EditorActions.TurnRight)),
            EditorCropButton(),
            EditorLightButton(),
            EditorSoundButton(),
            EditorSpeedButton(),
            saveFrame);
        bar.Children.Add(left);
        _editorExport = EditButton("Export", () => SendEditor(EditorActions.Export),
            tip: "Write the edit as a new file, cut on keyframes: instant, no quality loss  Ctrl+E");
        _editorExportExact = EditButton("Export exact", () => SendEditor(EditorActions.ExportExact),
            tip: "Frame-accurate: re-encoded on the hardware encoder, slower  Ctrl+Shift+E");
        _editorAddons = Row();
        RebuildEditorAddons();
        var right = Row(_editorAddons, _editorExport, _editorExportExact, EditorDivider(),
                        EditButton("Done", () => SendEditor(EditorActions.Close), tip: "Close the editor  Esc or Ctrl+W"));
        Grid.SetColumn(right, 2);
        bar.Children.Add(right);
        Grid.SetRow(bar, 1);
        root.Children.Add(bar);

        var area = new Grid { Padding = Pad(12, 10, 12, 10) };
        _editorSkeleton = BuildEditorSkeleton();
        area.Children.Add(_editorSkeleton);
        _editorTrack = new EditorTrackCanvas
        {
            Height = EditorTrackHeight,
            VerticalAlignment = VerticalAlignment.Top,
            Background = Brush(Colors.Transparent),  // hit-testable everywhere
        };
        AutomationProperties_SetName(_editorTrack, "Timeline");
        _editorTrack.SizeChanged += (_, _) => Guard(() => RenderEditor(full: true));
        _editorTrack.PointerPressed += (_, e) => Guard(() =>
        {
            if (_editorTrack is null) return;
            _editorTrack.CapturePointer(e.Pointer);
            Point at = e.GetCurrentPoint(_editorTrack).Position;
            if (at.Y >= RulerH && EditorEdgeAt(at.X) is { } edge && _editorReady && _editorLengthNs > 0)
            {
                // Left of a join is the outgoing piece's out, right of it the incoming piece's in.
                _editorTrim = (edge.SourceNs, at.X, (double)_editorLengthNs / Math.Max(1, _editorTrack.ActualWidth));
                Send(Command.EditorTrimGrab, edge.Index * 2 + (edge.InEdge ? 0 : 1));
            }
            else
            {
                EditorScrubTo(at.X);
            }
            e.Handled = true;
        });
        _editorTrack.PointerMoved += (_, e) => Guard(() =>
        {
            if (_editorTrack is null) return;
            Point at = e.GetCurrentPoint(_editorTrack).Position;
            if (_editorTrim is { } trim)
            {
                long to = trim.OriginNs + (long)((at.X - trim.OriginX) * trim.NsPerDip);
                Send(Command.EditorTrimTo, (float)(Math.Max(0, to) / 1_000_000.0));
                return;
            }
            if (_editorScrubNs is null)
            {
                _editorTrack.SetResizeCursor(at.Y >= RulerH && EditorEdgeAt(at.X) is not null);
                return;
            }
            EditorScrubTo(at.X);
        });
        _editorTrack.PointerReleased += (_, e) => Guard(() =>
        {
            if (_editorTrack is null) return;
            if (_editorTrim is not null)
            {
                _editorTrim = null;
                Send(Command.EditorTrimGrab, -1);
            }
            else if (_editorScrubNs is not null)
            {
                EditorScrubTo(e.GetCurrentPoint(_editorTrack).Position.X);
                _editorScrubNs = null;
            }
            _editorTrack.ReleasePointerCapture(e.Pointer);
        });
        _editorTrack.PointerExited += (_, _) => Guard(() => _editorTrack?.SetResizeCursor(false));
        _editorTrack.PointerCaptureLost += (_, _) => Guard(() =>
        {
            _editorScrubNs = null;
            if (_editorTrim is not null)
            {
                _editorTrim = null;
                Send(Command.EditorTrimGrab, -1);
            }
        });
        area.Children.Add(_editorTrack);
        Grid.SetRow(area, 2);
        root.Children.Add(area);

        TextBlock hints = Hint(
            "Space play · Left Right frame · J K L shuttle · I O mark · X clear · [ ] trim · Ctrl+B split · " +
            "Delete · Ctrl+Z undo · Ctrl+Shift+S save frame · Ctrl+E export · Esc close");
        hints.Margin = Pad(12, 0, 12, 10);
        Grid.SetRow(hints, 3);
        root.Children.Add(hints);
        AutomationProperties_SetName(root, "Video Editor timeline");
        _editorRoot = root;
        RenderEditor(full: true);
        ThemeField.Lay(root);
        return root;
    }

    private static UIElement EditorDivider() =>
        new Border { Width = 1, Height = 18, Background = Brush(Hairline), Margin = Pad(6, 0, 6, 0) };

    // Segoe Fluent / MDL2 code points for the look's tools (the Mac's crop,
    // sun.max.fill and camera.fill); the rest are the shared Glyph table's.
    private const string EditorGlyphCrop = "\uE7A8";
    private const string EditorGlyphLight = "\uE706";  // Brightness, as Adjust's Light
    private const string EditorGlyphCamera = "\uE722";

    private static double EditorTrackHeight => RulerH + TrackGap + VideoH + TrackGap + AudioH + 4;

    // The tracks at their own size while the clip is read, quiet and still
    // (nothing pulses), with one line saying what is happening.
    private static FrameworkElement BuildEditorSkeleton()
    {
        var col = new StackPanel
        {
            Spacing = TrackGap,
            Height = EditorTrackHeight,
            VerticalAlignment = VerticalAlignment.Top,
            IsHitTestVisible = false,
        };
        col.Children.Add(new Border { Height = RulerH });
        TextBlock reading = Text("Reading the clip…", Body, UiFontSize - 4, maxLines: 1);
        reading.VerticalAlignment = VerticalAlignment.Center;
        reading.Margin = Pad(12, 0, 12, 0);
        col.Children.Add(new Border
        {
            Height = VideoH, CornerRadius = Corner(5), Background = Brush(ChromeColour.Surface), Child = reading,
        });
        col.Children.Add(new Border { Height = AudioH, CornerRadius = Corner(4), Background = Brush(ChromeColour.Surface) });
        AutomationProperties_SetName(col, "Reading the clip");
        return col;
    }

    // A state a click sets fades (150 ms ease out); with Windows' animations
    // off it cuts. `done` runs when it lands.
    private static void EditorFade(UIElement element, double from, double to, Action? done = null)
    {
        if (from == to || !HostAnimationsEnabled())
        {
            element.Opacity = to;
            done?.Invoke();
            return;
        }
        element.Opacity = from;
        var fade = new DoubleAnimation
        {
            From = from,
            To = to,
            Duration = new Duration(TimeSpan.FromMilliseconds(150)),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        };
        Storyboard.SetTarget(fade, element);
        Storyboard.SetTargetProperty(fade, "Opacity");
        var board = new Storyboard();
        board.Children.Add(fade);
        if (done is not null) board.Completed += (_, _) => Guard(done);
        board.Begin();
    }

    // A menu row that shows the current choice with the menu's own check
    // (not a text "✓"); styled as Item.
    private static ToggleMenuFlyoutItem EditorCheckItem(string text, bool on, Action action)
    {
        var item = new ToggleMenuFlyoutItem
        {
            Text = text,
            IsChecked = on,
            Foreground = Brush(Title),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Background = new SolidColorBrush(Colors.Transparent),
            Padding = Pad(12, 8, 12, 8),
            CornerRadius = Corner(RadiusButton),
            AllowFocusOnInteraction = false,
        };
        foreach (string prefix in new[] { "MenuFlyoutItem", "ToggleMenuFlyoutItem" })
        {
            item.Resources[prefix + "BackgroundPointerOver"] = Brush(ChromeColour.HoverWash);
            item.Resources[prefix + "BackgroundPressed"] = Brush(ChromeColour.PressedWash);
            item.Resources[prefix + "ForegroundPointerOver"] = Brush(Title);
            item.Resources[prefix + "ForegroundPressed"] = Brush(Title);
            item.Resources[prefix + "ForegroundDisabled"] = Brush(ChromeColour.Disabled);
        }
        foreach (string state in new[] { "", "PointerOver", "Pressed" })
            item.Resources["ToggleMenuFlyoutItemCheckGlyphForeground" + state] = Brush(Title);
        item.Click += (_, _) => Guarded(action);
        return item;
    }

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

    // A piece edge under `x` (within a few DIPs): left of a join is the outgoing
    // piece's out, right of it the incoming piece's in (as in Final Cut).
    private static (int Index, bool InEdge, long SourceNs)? EditorEdgeAt(double x)
    {
        if (_editorTrack is null || _editorLengthNs <= 0) return null;
        const double slop = 6;
        double width = Math.Max(1, _editorTrack.ActualWidth);
        long start = 0;
        for (int i = 0; i < _editorPieces.Length; i++)
        {
            (long pin, long pout) = _editorPieces[i];
            long len = pout - pin;
            double x0 = (double)start / _editorLengthNs * width;
            double x1 = (double)(start + len) / _editorLengthNs * width;
            if (x >= x0 - 1 && x - x0 <= slop) return (i, true, pin);
            if (x <= x1 + 1 && x1 - x <= slop) return (i, false, pout);
            start += len;
        }
        return null;
    }

    private static void EditorScrubTo(double x)
    {
        if (_editorTrack is null || !_editorReady || _editorLengthNs <= 0) return;
        double width = Math.Max(1, _editorTrack.ActualWidth);
        long t = (long)(Math.Clamp(x, 0, width) / width * _editorLengthNs);
        // The playhead snaps to joins and marks within a few DIPs.
        long slop = (long)(8.0 / width * _editorLengthNs);
        long best = t;
        long start = 0;
        void Consider(long c)
        {
            if (c >= 0 && Math.Abs(c - t) <= slop && Math.Abs(c - t) < Math.Abs(best - t)) best = c;
        }
        foreach ((long pin, long pout) in _editorPieces)
        {
            Consider(start);
            start += pout - pin;
        }
        Consider(start);
        Consider(_editorMarkIn);
        Consider(_editorMarkOut);
        t = best;
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
        if (_editorPlay is not null)
        {
            if (_editorPlay.Content is FontIcon icon) icon.Glyph = _editorPlaying ? Glyph.Pause : Glyph.Play;
            ToolTipService.SetToolTip(_editorPlay, _editorPlaying ? "Pause  Space" : "Play  Space");
            AutomationProperties_SetName(_editorPlay, _editorPlaying ? "Pause" : "Play");
        }
        if (!full)
        {
            MoveEditorPlayhead();
            return;
        }
        if (_editorDelete is not null) _editorDelete.IsEnabled = EditorHasMarks || _editorPieces.Length > 1;
        if (_editorUndo is not null) _editorUndo.IsEnabled = _editorCanUndo;
        if (_editorRedo is not null) _editorRedo.IsEnabled = _editorCanRedo;
        if (_editorExport is not null)
        {
            // Packet copy carries a turn and mute only: the rest needs Export exact.
            _editorExport.IsEnabled = _editorEdited && _editorLookCopyable;
            ToolTipService.SetToolTip(_editorExport, _editorLookCopyable
                ? "Write the edit as a new file, cut on keyframes: instant, no quality loss  Ctrl+E"
                : "Crop, mirror, colour, sound and speed need Export exact");
        }
        if (_editorExportExact is not null) _editorExportExact.IsEnabled = _editorEdited;
        if (_editorSoundButton?.Content is FontIcon sound) sound.Glyph = _editorMuted ? Glyph.Mute : Glyph.Volume;
        if (_editorAddons is not null)
        {
            foreach (UIElement child in _editorAddons.Children)
            {
                if (child is not Button { Tag: string verb } addon) continue;
                addon.IsEnabled = _editorReady;
                ToolTipService.SetToolTip(addon, EditorAddonTip(verb));
            }
        }
        if (_editorAwayName is not null) _editorAwayName.Text = _editorName;
        DrawEditorTrack();
        // The skeleton and the tracks cross-fade in one place, so nothing moves.
        if (_editorReady != _editorShownReady && _editorSkeleton is FrameworkElement skeleton && _editorTrack is not null)
        {
            _editorShownReady = _editorReady;
            if (_editorReady)
            {
                EditorFade(_editorTrack, 0, 1);
                EditorFade(skeleton, 1, 0, () =>
                {
                    if (_editorShownReady) skeleton.Visibility = Visibility.Collapsed;
                });
            }
            else
            {
                skeleton.Visibility = Visibility.Visible;
                skeleton.Opacity = 1;
            }
        }
    }

    // "The original is not changed" is the result notice's to say, once.
    private static string EditorAddonTip(string verb) =>
        _editorEdited ? $"{verb} the edit as a new clip" : $"{verb} the whole clip as a new clip";

    // Add-ons' buttons in the bar (editor_items, contribution API 11), before
    // Export, with a divider after them; none without one. Each runs on the
    // edit: the clip on screen and its kept pieces, or all of it when nothing
    // is cut. The add-on opens its own sheet (centred over this window).
    // Mirror and crop the whole clip: a menu, as on the Mac. The crop shown
    // checked is the one the last view pushed.
    private static Button EditorCropButton()
    {
        Button crop = PaneIconButton(EditorGlyphCrop, "Mirror and crop", null, () => { });
        ToolTipService.SetToolTip(crop, "Mirror and crop the whole clip");
        var menu = new MenuFlyout { MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle() };
        menu.Opening += (_, _) =>
        {
            menu.Items.Clear();
            menu.Items.Add(Item("Mirror left to right", null, () => SendEditor(EditorActions.MirrorH)));
            menu.Items.Add(Item("Mirror upside down", null, () => SendEditor(EditorActions.MirrorV)));
            menu.Items.Add(new MenuFlyoutSeparator());
            for (int i = 0; i < EditorCropNames.Length; i++)
            {
                int ratio = i;
                menu.Items.Add(EditorCheckItem(EditorCropNames[ratio], ratio == _editorLookCrop,
                                               () => SendEditor(EditorActions.CropOriginal + ratio)));
            }
        };
        crop.Flyout = menu;
        return crop;
    }

    // A slider row of the Light and Sound flyouts, as the Mac's popovers: the
    // name, its value right of it in the body ink, and the slider 2 below.
    private static (StackPanel Row, TextBlock Readout) EditorSliderRow(string name, Slider slider)
    {
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(Text(name, Title, UiFontSize - 3));
        TextBlock readout = Text("", Body, UiFontSize - 4);
        readout.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(readout, 1);
        head.Children.Add(readout);
        var row = new StackPanel { Spacing = Space(2) };
        row.Children.Add(head);
        row.Children.Add(slider);
        return (row, readout);
    }

    private static string EditorLightValue(int index, double v) =>
        index == 0 ? $"{v:+0.0;-0.0;0.0} EV" : $"{v:+0;-0;0}";  // Edit's own words (AdjustStore)

    // Light and colour for the whole clip (docs/plans/video-editor-look.md
    // slice 2): the five sliders, Auto and Reset in a flyout. Each move goes
    // to native at once (the preview follows); a flyout session is one undo
    // step (chrome_cmd_editor_light_drag on open and close).
    private static Button EditorLightButton()
    {
        Button light = PaneIconButton(EditorGlyphLight, "Light and colour", null, () => { });
        ToolTipService.SetToolTip(light, "Light and colour for the whole clip");
        var flyout = new Flyout { FlyoutPresenterStyle = FlyoutPresenterStyle() };
        bool updating = false, open = false;
        var sliders = new Slider[EditorLightSpecs.Length];
        var readouts = new TextBlock[EditorLightSpecs.Length];
        // The presenter's 4 and this 8: words 12 from the flyout's edge.
        var col = new StackPanel { Spacing = Space(8), Width = 280, Padding = Pad(8) };
        Button reset = EditButton("Reset", () => SendEditor(EditorActions.ResetLight), tip: "Every slider back to 0");
        for (int i = 0; i < EditorLightSpecs.Length; i++)
        {
            var spec = EditorLightSpecs[i];
            var slider = new Slider
            {
                Minimum = spec.Min,
                Maximum = spec.Max,
                StepFrequency = spec.Step,
                SmallChange = spec.Step,
                LargeChange = spec.Step * 10,
                IsThumbToolTipEnabled = false,
            };
            AutomationProperties_SetName(slider, spec.Name);
            (StackPanel row, readouts[i]) = EditorSliderRow(spec.Name, slider);
            int index = i;
            slider.ValueChanged += (_, e) =>
            {
                readouts[index].Text = EditorLightValue(index, e.NewValue);
                if (updating) return;
                _editorLight[index] = (float)e.NewValue;
                reset.IsEnabled = _editorLight.Any(v => v != 0);
                Send(Command.EditorLightExposure + index, (float)e.NewValue);
            };
            slider.DoubleTapped += (_, _) => slider.Value = 0;
            sliders[i] = slider;
            col.Children.Add(row);
        }
        // Auto at the start, Reset at the end, as on the Mac.
        var foot = new Grid();
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.Children.Add(EditButton("Auto", () => SendEditor(EditorActions.AutoLight),
                                     tip: "Exposure and contrast from the frame on screen"));
        Grid.SetColumn(reset, 1);
        foot.Children.Add(reset);
        col.Children.Add(foot);
        flyout.Content = col;
        // The sliders say what the edit holds: on opening, and while open when
        // a pushed view changes it (Auto, Reset, an undo). A value already
        // there (the one being dragged) is left alone.
        void Sync()
        {
            updating = true;
            for (int i = 0; i < sliders.Length; i++)
            {
                if (Math.Abs(sliders[i].Value - _editorLight[i]) > EditorLightSpecs[i].Step / 2)
                    sliders[i].Value = _editorLight[i];
                readouts[i].Text = EditorLightValue(i, sliders[i].Value);
            }
            updating = false;
            reset.IsEnabled = _editorLight.Any(v => v != 0);
        }
        _editorLightSync = () => { if (open) Sync(); };
        flyout.Opening += (_, _) =>
        {
            open = true;
            Sync();
            Send(Command.EditorLightDrag, 1);
        };
        flyout.Closed += (_, _) =>
        {
            open = false;
            Send(Command.EditorLightDrag, 0);
        };
        light.Flyout = flyout;
        return light;
    }

    // The edit's sound (slice 3): Mute, Volume (0-200 %) and fades (0-5 s) in a
    // flyout; a flyout session is one undo step, as Light's. Muted, the sliders
    // rest (as the Mac's).
    private static Button EditorSoundButton()
    {
        Button sound = PaneIconButton(_editorMuted ? Glyph.Mute : Glyph.Volume, "Sound", null, () => { });
        ToolTipService.SetToolTip(sound, "Sound for the whole clip");
        _editorSoundButton = sound;
        var flyout = new Flyout { FlyoutPresenterStyle = FlyoutPresenterStyle() };
        bool updating = false;
        var col = new StackPanel { Spacing = Space(8), Width = 260, Padding = Pad(8) };
        var mute = new CheckBox
        {
            Content = "Mute", FontFamily = UiFont, FontSize = UiFontSize - 3, Foreground = Brush(Title),
        };
        col.Children.Add(mute);
        var sliders = new List<Slider>();
        (Slider Slider, Action Say) Add(string name, double min, double max, double step, int command,
                                        Func<double, string> format)
        {
            var slider = new Slider
            {
                Minimum = min, Maximum = max, StepFrequency = step, SmallChange = step, LargeChange = step * 10,
                IsThumbToolTipEnabled = false,
            };
            AutomationProperties_SetName(slider, name);
            (StackPanel row, TextBlock readout) = EditorSliderRow(name, slider);
            slider.ValueChanged += (_, e) =>
            {
                readout.Text = format(e.NewValue);
                if (!updating) Send(command, (float)e.NewValue);
            };
            col.Children.Add(row);
            sliders.Add(slider);
            return (slider, () => readout.Text = format(slider.Value));
        }
        var volume = Add("Volume", 0, 2, 0.05, Command.EditorSoundVolume, v => $"{v * 100:0} %");
        var fadeIn = Add("Fade in", 0, 5000, 100, Command.EditorSoundFadeIn, v => $"{v / 1000:0.0} s");
        var fadeOut = Add("Fade out", 0, 5000, 100, Command.EditorSoundFadeOut, v => $"{v / 1000:0.0} s");
        void Rest()
        {
            foreach (Slider s in sliders) s.IsEnabled = mute.IsChecked != true;
        }
        mute.Click += (_, _) =>
        {
            Rest();
            SendEditor(EditorActions.ToggleMute);
        };
        flyout.Content = col;
        flyout.Opening += (_, _) =>
        {
            updating = true;
            mute.IsChecked = _editorMuted;
            volume.Slider.Value = _editorVolume;
            fadeIn.Slider.Value = _editorFadeInMs;
            fadeOut.Slider.Value = _editorFadeOutMs;
            updating = false;
            volume.Say();
            fadeIn.Say();
            fadeOut.Say();
            Rest();
            Send(Command.EditorLightDrag, 1);
        };
        flyout.Closed += (_, _) => Send(Command.EditorLightDrag, 0);
        sound.Flyout = flyout;
        return sound;
    }

    private static string EditorSpeedName(float s) => s < 0.75f ? "0.5×" : s < 1.5f ? "1×" : "2×";

    // The edit's speed (slice 4): 0.5x, 1x, 2x, for playback and Export exact.
    private static Button EditorSpeedButton()
    {
        _editorSpeedButton = EditButton(EditorSpeedName(_editorSpeed), () => { },
                                        tip: "Speed of the whole clip: playback and Export exact");
        var menu = new MenuFlyout { MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle() };
        menu.Opening += (_, _) =>
        {
            menu.Items.Clear();
            float[] speeds = { 0.5f, 1f, 2f };
            for (int i = 0; i < speeds.Length; i++)
            {
                int code = EditorActions.SpeedHalf + i;
                menu.Items.Add(EditorCheckItem(EditorSpeedName(speeds[i]), speeds[i] == _editorSpeed,
                                               () => SendEditor(code)));
            }
        };
        _editorSpeedButton.Flyout = menu;
        return _editorSpeedButton;
    }

    private static void RebuildEditorAddons()
    {
        StackPanel? panel = _editorAddons;
        if (panel is null) return;
        panel.Children.Clear();
        List<SlotItem> items = _slotItems.Where(i => i.Slot == "editor").ToList();
        foreach (SlotItem item in items)
        {
            string verb = item.Label.Replace("…", "");
            Button addon = EditButton(item.Label, () => RunEditorAddon(item), tip: EditorAddonTip(verb));
            addon.Tag = verb;  // RenderEditor re-says the tip as the edit changes
            addon.IsEnabled = _editorReady;
            panel.Children.Add(addon);
        }
        if (items.Count > 0) panel.Children.Add(EditorDivider());
        panel.Visibility = items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static void RunEditorAddon(SlotItem item)
    {
        if (!_editorReady || _selectedIndex < 0 || _selectedIndex >= Items.Count) return;
        SlotNative.EditorItem(item.Addon, item.Id, Items[_selectedIndex].Path,
                              _editorEdited ? _editorPieces : Array.Empty<(long, long)>());
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
        bool silent = _editorPeaks.Length == 0 || _editorSourceNs <= 0;
        bool saidSilent = false;
        // Lift, don't ring: with a piece chosen (Delete's target), the others
        // step back under a wash of the field, fading as the choice moves.
        bool lifted = _editorPieces.Length > 1 && _editorSelected >= 0;
        bool samePieces = _editorDrawnPieces == _editorPieces.Length;
        bool wasLifted = samePieces && _editorPieces.Length > 1 && _editorDrawnSelected >= 0;
        long start = 0;
        for (int i = 0; i < _editorPieces.Length; i++)
        {
            (long pin, long pout) = _editorPieces[i];
            long len = pout - pin;
            double x0 = X(start);
            double w = Math.Max(1, X(start + len) - x0 - 2);
            double audioY = RulerH + TrackGap + VideoH + TrackGap;
            DrawEditorPiece(track, pin, pout, x0, RulerH + TrackGap, w);
            DrawEditorWave(track, pin, pout, x0, audioY, w);
            // "No audio" is said once, in the first piece wide enough for it.
            if (silent && !saidSilent && w >= 88)
            {
                saidSilent = true;
                TextBlock none = Text("No audio", Body, UiFontSize - 5, maxLines: 1);
                XamlCanvas.SetLeft(none, x0 + 12);
                XamlCanvas.SetTop(none, audioY + AudioH / 2 - 9);
                track.Children.Add(none);
            }
            double to = lifted && i != _editorSelected ? 1 : 0;
            double from = samePieces ? (wasLifted && i != _editorDrawnSelected ? 1 : 0) : to;
            if (from > 0 || to > 0)
            {
                var wash = new Canvas { IsHitTestVisible = false, Opacity = from };
                foreach ((double y, double h, double r) in new[] { (RulerH + TrackGap, VideoH, 5.0), (audioY, AudioH, 4.0) })
                {
                    var part = new Border
                    {
                        Width = w, Height = h, CornerRadius = Corner(r), Background = Brush(Canvas), Opacity = 0.45,
                    };
                    XamlCanvas.SetLeft(part, x0);
                    XamlCanvas.SetTop(part, y);
                    wash.Children.Add(part);
                }
                track.Children.Add(wash);
                EditorFade(wash, from, to);
            }
            start += len;
        }
        _editorDrawnSelected = _editorSelected;
        _editorDrawnPieces = _editorPieces.Length;
        // Marks fade in and out; a cleared range fades from where it was.
        (long In, long Out)? marks = EditorHasMarks ? (_editorMarkIn, _editorMarkOut) : null;
        if (marks is { } now)
        {
            UIElement drawn = DrawEditorMarks(track, width, length, now.In, now.Out);
            if (_editorDrawnMarks is null) EditorFade(drawn, 0, 1);
        }
        else if (_editorDrawnMarks is { } was)
        {
            UIElement ghost = DrawEditorMarks(track, width, length, was.In, was.Out);
            EditorFade(ghost, 1, 0);
        }
        _editorDrawnMarks = marks;
        AutomationProperties_SetName(track, EditorHasMarks
            ? $"Timeline, {_editorPieces.Length} pieces, marked {EditorTimecode(Math.Max(0, _editorMarkIn))} to " +
              $"{EditorTimecode(_editorMarkOut >= 0 ? _editorMarkOut : _editorLengthNs)}"
            : $"Timeline, {_editorPieces.Length} pieces");

        // The playhead, on top; the tick moves it without a redraw. The title
        // ink on a thin halo of the field, so it reads over any frame without
        // borrowing a status colour (the Mac's EditorPlayhead).
        var mark = new Canvas { IsHitTestVisible = false };
        var halo = new Rectangle { Width = 4, Height = track.Height, Fill = Brush(Canvas) };
        XamlCanvas.SetLeft(halo, -1);
        mark.Children.Add(halo);
        mark.Children.Add(new Rectangle
        {
            Width = 2,
            Height = track.Height,
            Fill = Brush(Title),
        });
        var head = new Polygon { Fill = Brush(Title) };
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
            TextBlock label = Text(EditorRulerLabel((long)(t * 1e9)), Body, UiFontSize - 5);
            label.TextWrapping = TextWrapping.NoWrap;
            XamlCanvas.SetLeft(label, x + 3);
            XamlCanvas.SetTop(label, -1);
            track.Children.Add(label);
        }
    }

    // The marked range: a band over both tracks, and a bracket at each end that
    // is set (shape, not only colour, says "marked"). One layer, so it fades
    // as a whole.
    private static UIElement DrawEditorMarks(Canvas track, double width, long length, long markIn, long markOut)
    {
        double X(long t) => (double)Math.Clamp(t, 0, length) / length * width;
        double a = X(Math.Max(0, markIn));
        double b = X(markOut >= 0 ? markOut : length);
        double top = RulerH + TrackGap;
        double h = track.Height - top;
        var layer = new Canvas { IsHitTestVisible = false };
        var band = new Rectangle
        {
            Width = Math.Max(1, b - a),
            Height = h,
            Fill = Brush(TrimAccent),
            Opacity = 0.18,
        };
        XamlCanvas.SetLeft(band, a);
        XamlCanvas.SetTop(band, top);
        layer.Children.Add(band);
        foreach ((long t, bool isIn) in new[] { (markIn, true), (markOut, false) })
        {
            if (t < 0) continue;
            double x = X(t);
            double arm = isIn ? 6 : -6;
            var bracket = new Polyline { Stroke = Brush(TrimAccent), StrokeThickness = 2 };
            bracket.Points.Add(new Point(x + arm, top));
            bracket.Points.Add(new Point(x, top));
            bracket.Points.Add(new Point(x, top + h - 2));
            bracket.Points.Add(new Point(x + arm, top + h - 2));
            layer.Children.Add(bracket);
        }
        track.Children.Add(layer);
        return layer;
    }

    // One surface per piece: its fill and its frames, no outline (the 2 DIP
    // join already parts it from the next). The chosen one is lifted by the
    // wash over the others (DrawEditorTrack), not ringed in the accent.
    private static void DrawEditorPiece(Canvas track, long pin, long pout, double x, double y, double w)
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
            CornerRadius = Corner(5),
            Background = Brush(ChromeColour.Surface),
            Child = tiles,
        };
        XamlCanvas.SetLeft(frame, x);
        XamlCanvas.SetTop(frame, y);
        track.Children.Add(frame);
    }

    // The envelope in the body ink: the accent is kept for what you marked.
    private static void DrawEditorWave(Canvas track, long pin, long pout, double x, double y, double w)
    {
        var box = new Border
        {
            Width = w,
            Height = AudioH,
            CornerRadius = Corner(4),
            Background = Brush(ChromeColour.Surface),
        };
        XamlCanvas.SetLeft(box, x);
        XamlCanvas.SetTop(box, y);
        track.Children.Add(box);
        if (_editorPeaks.Length == 0 || _editorSourceNs <= 0) return;  // DrawEditorTrack says "No audio" once
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
        var wave = new Polygon { Fill = Brush(Body), IsHitTestVisible = false };
        foreach (Point p in top) wave.Points.Add(p);
        for (int c = cols - 1; c >= 0; c--) wave.Points.Add(new Point(top[c].X, 2 * mid - top[c].Y));
        track.Children.Add(wave);
    }

    // ---- the card on the viewer's window ---------------------------------------------

    private static UIElement BuildEditorAway()
    {
        var root = new Grid { Background = Brush(Canvas), RequestedTheme = IslandTheme };
        // Hierarchy from one ink and one size step: the line in the title ink,
        // the clip's name under it 4 apart, the two buttons 12 below.
        var col = new StackPanel
        {
            Spacing = Space(12),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var words = new StackPanel { Spacing = Space(4) };
        TextBlock head = Text("Editing in the Video Editor window", Title, UiFontSize - 1);
        head.HorizontalAlignment = HorizontalAlignment.Center;
        words.Children.Add(head);
        _editorAwayName = Text(_editorName, Body, UiFontSize - 3, maxLines: 1);
        _editorAwayName.HorizontalAlignment = HorizontalAlignment.Center;
        _editorAwayName.TextTrimming = TextTrimming.CharacterEllipsis;
        words.Children.Add(_editorAwayName);
        col.Children.Add(words);
        StackPanel buttons = Row(
            EditButton("Show editor", () => SendEditor(EditorActions.Show), tip: "Bring the Video Editor forward"),
            EditButton("Done", () => SendEditor(EditorActions.Close), tip: "Close the editor and put the picture back"));
        buttons.Spacing = Space(8);
        buttons.HorizontalAlignment = HorizontalAlignment.Center;
        col.Children.Add(buttons);
        root.Children.Add(col);
        AutomationProperties_SetName(root, "Editing in the Video Editor window");
        ThemeField.Lay(root);
        return root;
    }
}
