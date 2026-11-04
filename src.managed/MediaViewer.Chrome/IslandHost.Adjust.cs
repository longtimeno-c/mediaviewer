// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 11: the adjust pane, grown into the full Adjust list (docs/plans/
/// edit-adjust.md slice 1): Light, Colour, Black &amp; White and Vignette
/// sections, each collapsible with its own reset and (but the vignette) its
/// own Auto; a histogram and the clipping readout. The third panel island, on
/// the right edge the metadata pane also uses (native shows one at a time).
/// </summary>
/// <remarks>
/// The sliders post their value as the command's float (one command id per
/// parameter, commands.h); Auto and a section's reset post the section.
/// Native owns the edit stack and pushes back what the pane shows
/// (<c>shell::adjust_view</c>). Nothing here touches pixels: the canvas
/// redraws from the new uniforms on the render thread, and the histogram and
/// Auto are worker jobs. The sliders stay disabled until the FP16 working
/// image is built — for a RAW, LibRaw's full linear develop (docs/design/07:
/// never the embedded preview). Keyboard-complete: Shift+A shows the pane
/// focused on its first slider (ShowPanel's focus flag), Tab walks the
/// controls, ↑ ↓ the sliders of open sections, arrows step, A runs the
/// focused slider's section's Auto, Esc returns to the canvas — a focused
/// pane owns its keys (FocusKind.Pane, docs/design/16 "Pane"). The twin of
/// AdjustView.swift.
/// </remarks>
public static partial class IslandHost
{
    // shell/adjust_pane.h adjust_view: 2 ints, 56 floats, 2 floats, 2 ints,
    // 4 ints (curve_n), 2 x 4 x 8 floats (curve x, y), 4 ints (picking,
    // spot_count, brush_strokes, brush_full), 128 bytes of prompt note
    // (UTF-8, NUL-terminated), then 4 x 64 uint16.
    internal const int AdjustParamCount = 56;
    internal const int CurvePoints = 8;
    internal const int AdjustViewSize =
        24 + 4 * AdjustParamCount + 16 + 2 * 4 * 4 * CurvePoints + 16 + PromptNoteBytes + 2 * 4 * HistogramBins;
    private const int PromptNoteBytes = 128;
    private const int SelectiveSection = 6;
    private const int RedEyeSection = 7;
    private static readonly string[] BandNames = { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta" };
    private static readonly Color[] BandColours =
    {
        ColorHelper.FromArgb(255, 230, 60, 60), ColorHelper.FromArgb(255, 240, 150, 40),
        ColorHelper.FromArgb(255, 235, 215, 50), ColorHelper.FromArgb(255, 70, 190, 80),
        ColorHelper.FromArgb(255, 60, 200, 210), ColorHelper.FromArgb(255, 60, 110, 230),
        ColorHelper.FromArgb(255, 150, 80, 220), ColorHelper.FromArgb(255, 225, 80, 170),
    };
    private const int LevelsSection = 4;
    private const int CurvesSection = 5;
    private static readonly string[] ChannelNames = { "Image", "Red", "Green", "Blue" };
    internal const int HistogramBins = 64;
    private const int MonoParam = 10;

    private static DesktopWindowXamlSource? _adjustPane;
    private static bool _adjustPaneVisible;

    // Mirrors edit::adjust_param order and edit::range_of.
    private sealed record AdjustSpec(string Name, int Command, double Min, double Max, double Step,
                                     Func<double, string> Format);

    private static readonly Func<double, string> Signed = v => $"{v:+0;-0;0}";

    private static readonly AdjustSpec[] AdjustSpecs =
    {
        new("Exposure", Command.AdjustExposure, -5, 5, 0.1, v => $"{v:+0.0;-0.0;0.0} EV"),
        new("Contrast", Command.AdjustContrast, -100, 100, 5, Signed),
        new("Saturation", Command.AdjustSaturation, -100, 100, 5, Signed),
        new("Temperature", Command.AdjustTemperature, -100, 100, 5, Signed),
        new("Tint", Command.AdjustTint, -100, 100, 5, Signed),
        new("Highlights", Command.AdjustHighlights, -100, 100, 5, Signed),
        new("Shadows", Command.AdjustShadows, -100, 100, 5, Signed),
        new("Whites", Command.AdjustWhites, -100, 100, 5, Signed),
        new("Blacks", Command.AdjustBlacks, -100, 100, 5, Signed),
        new("Vibrance", Command.AdjustVibrance, -100, 100, 5, Signed),
        new("Intensity", Command.AdjustMono, 0, 100, 5, v => $"{v:0}"),
        new("Filter", Command.AdjustMonoFilter, -100, 100, 5, Signed),
        new("Tone", Command.AdjustMonoTone, -100, 100, 5, Signed),
        new("Strength", Command.AdjustVignette, -100, 100, 5, Signed),
        new("Radius", Command.AdjustVignetteRadius, -100, 100, 5, Signed),
        new("Softness", Command.AdjustVignetteSoftness, -100, 100, 5, Signed),
        // Slice 2: Levels for the image, red, green and blue.
        new("Black", Command.AdjustLevelsBlack + 0, 0, 100, 1, v => $"{v:0}"),
        new("Mid-tones", Command.AdjustLevelsBlack + 1, -100, 100, 2, Signed),
        new("White", Command.AdjustLevelsBlack + 2, -100, 0, 1, v => $"{v:0}"),
        new("Black", Command.AdjustLevelsBlack + 3, 0, 100, 1, v => $"{v:0}"),
        new("Mid-tones", Command.AdjustLevelsBlack + 4, -100, 100, 2, Signed),
        new("White", Command.AdjustLevelsBlack + 5, -100, 0, 1, v => $"{v:0}"),
        new("Black", Command.AdjustLevelsBlack + 6, 0, 100, 1, v => $"{v:0}"),
        new("Mid-tones", Command.AdjustLevelsBlack + 7, -100, 100, 2, Signed),
        new("White", Command.AdjustLevelsBlack + 8, -100, 0, 1, v => $"{v:0}"),
        new("Black", Command.AdjustLevelsBlack + 9, 0, 100, 1, v => $"{v:0}"),
        new("Mid-tones", Command.AdjustLevelsBlack + 10, -100, 100, 2, Signed),
        new("White", Command.AdjustLevelsBlack + 11, -100, 0, 1, v => $"{v:0}"),
        // Slice 3: selective colour, eight bands; no command each (-1: parked
        // through AdjustParam).
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        new("Hue", -1, -100, 100, 5, Signed), new("Saturation", -1, -100, 100, 5, Signed), new("Luminance", -1, -100, 100, 5, Signed),
        // Slice 4: Detail, parked through AdjustParam too.
        new("Definition", -1, -100, 100, 5, Signed), new("Brilliance", -1, -100, 100, 5, Signed),
        new("Sharpen", -1, 0, 100, 5, v => $"{v:0}"), new("Noise reduction", -1, 0, 100, 5, v => $"{v:0}"),
    };

    // edit::adjust_section order: name, Segoe Fluent glyph, parameters as shown, Auto.
    private sealed record AdjustSection(string Name, string Glyph, int[] Params, bool HasAuto);

    private static readonly AdjustSection[] AdjustSections =
    {
        new("Light", "\uE706", new[] { 0, 5, 6, 7, 8, 1 }, true),           // Brightness
        new("Colour", "\uE790", new[] { 2, 9, 3, 4 }, true),                // Color
        new("Black & White", "\uE793", new[] { 10, 11, 12 }, true),         // Contrast
        new("Vignette", "\uEA3A", new[] { 13, 14, 15 }, false),             // CircleRing
        new("Levels", "\uE9D2", new[] { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 }, true),  // AreaChart
        new("Curves", "\uE9D9", Array.Empty<int>(), false),                // Diagnostic
        new("Selective Colour", "\uE790", Enumerable.Range(28, 24).ToArray(), false),  // Color
        new("Red-Eye", "\uE7B3", Array.Empty<int>(), false),               // RedEye
        new("Detail", "\uE9F5", new[] { 52, 53, 54, 55 }, false),          // Processing
        new("Faces", "\uE77B", Array.Empty<int>(), false),                 // Contact
        new("Remove", "\uE75C", Array.Empty<int>(), false),                // EraseTool (slice 7)
    };

    // Last view native pushed.
    private static int _adjustReadiness;  // shell::adjust_readiness
    private static bool _adjustFromRaw;
    private static readonly float[] _adjustValues = new float[AdjustParamCount];
    private static float _clipHigh;
    private static float _clipLow;
    private static bool _histValid;
    private static int _autoSection = -1;
    private static readonly ushort[] _histBins = new ushort[4 * HistogramBins];
    // Which sections are open; kept while the pane is rebuilt.
    private static readonly bool[] _sectionOpen = { true, true, false, false, false, false, false, false, false, false, false };
    private const int FacesSection = 9;
    private const int RemoveSection = 10;
    // Slice 7: the Remove brush from native and its controls (S3: its size is
    // the context bar's, IslandHost.ToolBar.cs).
    private static int _brushStrokes;
    private static bool _brushFull;
    private static Button? _brushPick;
    private static Button? _brushUndo;
    private static Button? _brushClear;
    private static Button? _brushRemove;
    private static TextBlock? _brushNote;
    // Slice 3: the band Selective Colour shows, the pick mode and fixes from
    // native, and their controls.
    private static int _band;
    private static string _promptNote = "";
    private static TextBlock? _promptNoteText;
    private static int _picking;
    private static int _spotCount;
    private static Button? _wbPick;
    private static Button? _redEyePick;
    private static TextBlock? _redEyeCount;
    private static Border[]? _bandPicks;
    // Slice 2: the curves from native (image, red, green, blue) and the channel
    // each picker shows.
    private static readonly List<Point>[] _curves = { new(), new(), new(), new() };
    private static int _levelsChannel;
    private static int _curvesChannel;
    private static Canvas? _curveCanvas;
    private static int _curveDrag = -1;
    // docs/plans/edit-refresh.md §2.6: the curve's plot sits 6 DIP inside its
    // square, so the end points and edge lines are whole and hittable; drawing
    // and hit-testing share that inset. The last press on a point, for a
    // double-click that removes it, and the "in -> out" readout while dragging.
    private const double CurveInset = 6;
    private const double CurveMaxSide = 340;  // shell/pane_width.h curve_side
    private static int _curveLastPress = -1;
    private static long _curveLastPressMs;
    private static TextBlock? _curveReadout;
    // A resize redraws the curve and histogram once, on the next frame, not
    // per SizeChanged (a column drag sends many).
    private static bool _adjustRedrawQueued;
    private static bool _histMeasured;
    private static Button[]? _levelsPicks;
    private static Button[]? _curvesPicks;

    private static Slider[]? _adjustSliders;  // by parameter
    private static TextBlock[]? _adjustReadouts;
    private static TextBlock? _adjustStatus;
    private static TextBlock? _adjustClip;
    private static Canvas? _histCanvas;
    private static Button? _adjustResetButton;
    private static JobBar? _adjustPreparing;
    private static Border[]? _adjustRows;
    private static TextBlock[]? _adjustNames;
    private static StackPanel[]? _sectionBodies;
    private static FontIcon[]? _sectionChevrons;
    private static Button[]? _sectionResets;
    private static Button?[]? _sectionAutos;
    private static ToggleSwitch? _monoSwitch;
    private static bool _updatingAdjust;
    // docs/plans/edit-refresh.md S2 (§2.5): the layout by the inspector's width class.
    // The section cards are built once and moved (never rebuilt) between one
    // column and two (Light, Colour, B&W, Vignette, Detail | Levels, Curves,
    // Selective Colour, Red-Eye, then Faces and Remove); at xwide the histogram
    // and the Curves square move into a scopes row side by side.
    private static readonly int[] AdjustLeftSections = { 0, 1, 2, 3, 8 };
    private static readonly int[] AdjustRightSections = { 4, 5, 6, 7, 9, 10 };
    private static int _adjustWidthClass = -1;
    private static Border[]? _sectionCards;
    private static Border[]? _sectionDots;
    private static StackPanel? _adjustOneColumn;
    private static Grid? _adjustTwoColumns;
    private static StackPanel? _adjustLeft;
    private static StackPanel? _adjustRight;
    private static Grid? _adjustHistFrame;   // the histogram with its clip triangles
    private static Grid? _adjustCurveFrame;  // the Curves square with its readout
    private static StackPanel? _adjustHistSlot;   // where the histogram sits outside xwide
    private static StackPanel? _adjustCurveSlot;  // where the curve sits in its section
    private static Grid? _adjustScopes;      // xwide: histogram | curve
    private static Grid[]? _adjustRowGrids;
    private static Button[]? _adjustValueButtons;

    private static void DropAdjustUi()
    {
        _adjustWidthClass = -1;
        _sectionCards = null;
        _sectionDots = null;
        _adjustOneColumn = null;
        _adjustTwoColumns = null;
        _adjustLeft = null;
        _adjustRight = null;
        _adjustHistFrame = null;
        _adjustCurveFrame = null;
        _adjustHistSlot = null;
        _adjustCurveSlot = null;
        _adjustScopes = null;
        _adjustRowGrids = null;
        _adjustValueButtons = null;
        _adjustSliders = null;
        _adjustReadouts = null;
        _adjustStatus = null;
        _adjustClip = null;
        _histCanvas = null;
        _adjustResetButton = null;
        _adjustPreparing = null;
        _adjustRows = null;
        _adjustNames = null;
        _sectionBodies = null;
        _sectionChevrons = null;
        _sectionResets = null;
        _sectionAutos = null;
        _monoSwitch = null;
        _curveCanvas = null;
        _curveReadout = null;
        _curveDrag = -1;
        _clipLowMark = null;
        _clipHighMark = null;
        _histMeasured = false;
        _wbPick = null;
        _promptNoteText = null;
        _redEyePick = null;
        _redEyeCount = null;
        _brushPick = null;
        _brushUndo = null;
        _brushClear = null;
        _brushRemove = null;
        _brushNote = null;
        _bandPicks = null;
        _levelsPicks = null;
        _curvesPicks = null;
    }

    public static int ShowAdjustPane(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _adjustPane, BuildAdjustPane,
        onShown: () => _adjustPaneVisible = true,
        onHidden: () =>
        {
            _adjustPaneVisible = false;
            DropAdjustUi();
        });

    /// <summary>
    /// Native pushes readiness, values, histogram, clipping and the running
    /// Auto whenever any of them changes. In: shell::adjust_view (blittable,
    /// AdjustViewSize bytes).
    /// </summary>
    public static int SetAdjustView(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < AdjustViewSize) return unchecked((int)0x80070057);
            _adjustReadiness = Marshal.ReadInt32(arg, 0);
            _adjustFromRaw = Marshal.ReadInt32(arg, 4) != 0;
            for (int i = 0; i < AdjustParamCount; i++)
            {
                _adjustValues[i] = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 8 + 4 * i));
            }
            int at = 8 + 4 * AdjustParamCount;
            _clipHigh = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, at));
            _clipLow = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, at + 4));
            _histValid = Marshal.ReadInt32(arg, at + 8) != 0;
            _autoSection = Marshal.ReadInt32(arg, at + 12);
            int curves = at + 16;
            for (int ch = 0; ch < 4; ch++)
            {
                int n = Math.Clamp(Marshal.ReadInt32(arg, curves + 4 * ch), 0, CurvePoints);
                _curves[ch].Clear();
                for (int i = 0; i < n; i++)
                {
                    float x = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, curves + 16 + 4 * (ch * CurvePoints + i)));
                    float y = BitConverter.Int32BitsToSingle(
                        Marshal.ReadInt32(arg, curves + 16 + 4 * 4 * CurvePoints + 4 * (ch * CurvePoints + i)));
                    _curves[ch].Add(new Point(x, y));
                }
            }
            int pick = curves + 16 + 2 * 4 * 4 * CurvePoints;
            _picking = Marshal.ReadInt32(arg, pick);
            _spotCount = Marshal.ReadInt32(arg, pick + 4);
            _brushStrokes = Marshal.ReadInt32(arg, pick + 8);
            _brushFull = Marshal.ReadInt32(arg, pick + 12) != 0;
            _promptNote = Marshal.PtrToStringUTF8(arg + pick + 16) ?? "";
            var raw = new short[_histBins.Length];
            Marshal.Copy(arg + pick + 16 + PromptNoteBytes, raw, 0, raw.Length);
            for (int i = 0; i < raw.Length; i++) _histBins[i] = unchecked((ushort)raw[i]);
            if (_adjustPaneVisible) RenderAdjust();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static UIElement BuildAdjustPane()
    {
        Grid root = PanelShell("Adjust", Command.AdjustPane, out Grid body);
        var col = new StackPanel { Spacing = Space(10), Padding = Pad(14, 4, 14, 14) };

        // Slice 5b: say what you want; the sliders it names move (one undo
        // step). Not a TextBox: that control fail-fasts in these islands
        // (see FakeInput, tools/check-winui-controls.ps1). It and the header
        // stay put; everything under them scrolls (docs/plans/edit-refresh.md
        // §2.5 "Height rule"), as on the Mac.
        var request = new FakeInput("Describe a change — a bit warmer, less noise…");
        AutomationProperties_SetName(request, "Describe a change");
        request.Submitted += () =>
        {
            if (request.Text.Trim().Length == 0) return;
            _treePending = request.Text;
            Send(Command.AdjustPrompt);
            request.SetText("");
        };
        _promptNoteText = Text("", Body, UiFontSize - 2);
        col.Children.Add(_promptNoteText);

        // The histogram with its clipping triangles (a click shows the
        // blinkies, `C`), redrawn on a resize once per frame.
        _histCanvas = new Canvas { Height = 96, Background = Brush(ChromeColour.Surface) };
        _histCanvas.SizeChanged += (_, _) =>
        {
            _histMeasured = true;
            QueueAdjustRedraw();
        };
        var hist = new Grid();
        // Clipped to radius 4, as the Mac's clipShape (issue #198).
        hist.Children.Add(new Border
        {
            CornerRadius = Corner(6), Child = _histCanvas, BorderBrush = Brush(Hairline), BorderThickness = new Thickness(1),
        });
        hist.Children.Add(ClipTriangle(left: true));
        hist.Children.Add(ClipTriangle(left: false));
        _adjustHistFrame = hist;
        // The histogram's place outside xwide; at xwide it moves into the
        // scopes row beside the Curves square (ApplyAdjustWidthClass).
        _adjustHistSlot = new StackPanel();
        _adjustHistSlot.Children.Add(hist);
        col.Children.Add(_adjustHistSlot);
        _adjustScopes = new Grid { ColumnSpacing = Space(12), Visibility = Visibility.Collapsed };
        _adjustScopes.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _adjustScopes.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        col.Children.Add(_adjustScopes);
        _adjustClip = Text("", Body, UiFontSize - 2);
        col.Children.Add(_adjustClip);
        _adjustStatus = Text("", Body);
        col.Children.Add(_adjustStatus);
        // "Preparing" has no fraction: an indeterminate bar (the Mac's
        // ProgressView; ProgressRing cannot load in these islands).
        _adjustPreparing = new JobBar { IsIndeterminate = true };
        _adjustPreparing.Root.Visibility = Visibility.Collapsed;
        col.Children.Add(_adjustPreparing.Root);

        _adjustSliders = new Slider[AdjustSpecs.Length];
        _adjustReadouts = new TextBlock[AdjustSpecs.Length];
        _adjustRows = new Border[AdjustSpecs.Length];
        _adjustNames = new TextBlock[AdjustSpecs.Length];
        _sectionBodies = new StackPanel[AdjustSections.Length];
        _sectionChevrons = new FontIcon[AdjustSections.Length];
        _sectionResets = new Button[AdjustSections.Length];
        _sectionAutos = new Button?[AdjustSections.Length];
        _sectionCards = new Border[AdjustSections.Length];
        _sectionDots = new Border[AdjustSections.Length];
        _adjustRowGrids = new Grid[AdjustSpecs.Length];
        _adjustValueButtons = new Button[AdjustSpecs.Length];
        int tab = 0;
        for (int s = 0; s < AdjustSections.Length; s++)
        {
            _sectionCards[s] = BuildAdjustSection(s, ref tab);
        }
        // One column, or two independent stacks (a section opening on one side
        // never moves the other). Both exist; the cards move between them.
        _adjustOneColumn = new StackPanel { Spacing = Space(8) };
        _adjustLeft = new StackPanel { Spacing = Space(8) };
        _adjustRight = new StackPanel { Spacing = Space(8) };
        Grid.SetColumn(_adjustRight, 1);
        _adjustTwoColumns = new Grid { ColumnSpacing = Space(10), Visibility = Visibility.Collapsed };
        _adjustTwoColumns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _adjustTwoColumns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _adjustTwoColumns.Children.Add(_adjustLeft);
        _adjustTwoColumns.Children.Add(_adjustRight);
        col.Children.Add(_adjustOneColumn);
        col.Children.Add(_adjustTwoColumns);

        _adjustResetButton = EditButton("Reset adjustments", () => Send(Command.AdjustReset), compact: false);
        _adjustResetButton.TabIndex = tab;
        col.Children.Add(_adjustResetButton);
        col.Children.Add(Text("↑ ↓ choose · ← → change (Shift ×10) · 0 zero · A auto · R reset · Esc photo · Shift+A closes",
                              Body, UiFontSize - 3));
        // The Mac's keys (AdjustView.swift), on a focused slider: ↑ ↓ choose
        // a row rather than nudge it, Shift+← → step ten, 0 zeroes the row,
        // A runs its section's Auto and R resets every slider. A focused pane
        // owns its keys (key_router: focus_kind::pane), so these reach XAML.
        col.PreviewKeyDown += OnAdjustKey;

        var stack = new Grid();
        stack.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        stack.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var ask = new Border { Padding = Pad(14, 4, 14, 4), Child = request };
        stack.Children.Add(ask);
        var scroll = new ScrollViewer { Content = col, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Grid.SetRow(scroll, 1);
        stack.Children.Add(scroll);
        body.Children.Add(stack);
        // The width class follows the pane; the cards move only when it changes.
        root.SizeChanged += (_, e) => ApplyAdjustWidthClass(e.NewSize.Width);
        ApplyAdjustWidthClass(0);
        RenderAdjust();
        return root;
    }

    // compact (< 340): one column, labels above the sliders, histogram 72.
    // regular: one column, labels inline, histogram 96. wide (560+): two
    // columns, labels above. xwide (720+): two columns, labels inline, and
    // the histogram beside a square Curves. The same controls, moved.
    private static void ApplyAdjustWidthClass(double width)
    {
        int cls = width <= 0 ? WidthRegular : WidthClassOf(width);
        if (cls == _adjustWidthClass || _sectionCards is null || _adjustOneColumn is null || _adjustLeft is null ||
            _adjustRight is null || _adjustTwoColumns is null)
        {
            return;
        }
        _adjustWidthClass = cls;
        bool two = cls >= WidthWide;
        static void Detach(UIElement e)
        {
            if (VisualTreeHelper.GetParent(e) is Panel p) p.Children.Remove(e);
        }
        foreach (Border card in _sectionCards) Detach(card);
        if (two)
        {
            foreach (int s in AdjustLeftSections) _adjustLeft.Children.Add(_sectionCards[s]);
            foreach (int s in AdjustRightSections) _adjustRight.Children.Add(_sectionCards[s]);
        }
        else
        {
            for (int s = 0; s < _sectionCards.Length; s++) _adjustOneColumn.Children.Add(_sectionCards[s]);
        }
        _adjustOneColumn.Visibility = two ? Visibility.Collapsed : Visibility.Visible;
        _adjustTwoColumns.Visibility = two ? Visibility.Visible : Visibility.Collapsed;
        // The scopes row (xwide): the histogram and the curve side by side.
        bool scopes = cls == WidthXWide;
        if (_adjustHistFrame is not null && _adjustCurveFrame is not null && _adjustScopes is not null &&
            _adjustHistSlot is not null && _adjustCurveSlot is not null)
        {
            Detach(_adjustHistFrame);
            Detach(_adjustCurveFrame);
            if (scopes)
            {
                Grid.SetColumn(_adjustCurveFrame, 1);
                Grid.SetColumn(_adjustHistFrame, 0);
                _adjustScopes.Children.Add(_adjustHistFrame);
                _adjustScopes.Children.Add(_adjustCurveFrame);
            }
            else
            {
                Grid.SetColumn(_adjustCurveFrame, 0);
                _adjustHistSlot.Children.Add(_adjustHistFrame);
                _adjustCurveSlot.Children.Add(_adjustCurveFrame);
            }
            _adjustScopes.Visibility = scopes ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_histCanvas is not null && !scopes) _histCanvas.Height = cls == WidthCompact ? 72 : 96;
        // Labels inline (regular, xwide) or above the slider (compact, wide).
        bool inline = cls == WidthRegular || cls == WidthXWide;
        if (_adjustRowGrids is not null)
        {
            foreach (Grid g in _adjustRowGrids) LayAdjustRow(g, inline);
        }
        QueueAdjustRedraw();
    }

    // A slider row: name, slider, value. Inline: one row, the name 92 wide.
    // Above: the name and value on top, the slider under them.
    private static void LayAdjustRow(Grid? g, bool inline)
    {
        if (g is null || g.Children.Count < 3 || g.ColumnDefinitions.Count < 3) return;
        var name = (FrameworkElement)g.Children[0];
        var slider = (FrameworkElement)g.Children[1];
        var value = (FrameworkElement)g.Children[2];
        g.ColumnDefinitions[0].Width = inline ? new GridLength(92) : GridLength.Auto;
        Grid.SetRow(name, 0);
        Grid.SetColumn(name, 0);
        Grid.SetColumnSpan(name, inline ? 1 : 2);
        Grid.SetRow(value, 0);
        Grid.SetColumn(value, 2);
        Grid.SetRow(slider, inline ? 0 : 1);
        Grid.SetColumn(slider, inline ? 1 : 0);
        Grid.SetColumnSpan(slider, inline ? 1 : 3);
    }

    // Shadows (left) or highlights (right) clipped: the corner triangle lights;
    // a click is `C` (the blinkies on the photo).
    private static Button ClipTriangle(bool left)
    {
        var shape = new Polygon
        {
            Points = left
                ? new PointCollection { new Point(0, 0), new Point(12, 0), new Point(0, 12) }
                : new PointCollection { new Point(0, 0), new Point(12, 0), new Point(12, 12) },
            Fill = Brush(Body),
            Opacity = 0.35,
        };
        var b = new Button
        {
            Content = shape,
            Padding = new Thickness(3),
            MinWidth = 0,
            MinHeight = 0,
            Background = Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            HorizontalAlignment = left ? HorizontalAlignment.Left : HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Top,
            Tag = left ? "low" : "high",
        };
        b.Click += (_, _) => Send(Command.Clipping);
        string what = left ? "Crushed shadows" : "Clipped highlights";
        ToolTipService.SetToolTip(b, WithKey(what + ": show clipping on the photo", Command.Clipping));
        AutomationProperties_SetName(b, left ? "Show crushed shadows" : "Show clipped highlights");
        if (left) _clipLowMark = shape; else _clipHighMark = shape;
        return b;
    }
    private static Polygon? _clipLowMark;
    private static Polygon? _clipHighMark;

    // One redraw of the curve and histogram on the next frame, however many
    // SizeChanged a column drag sends before it.
    private static void QueueAdjustRedraw()
    {
        if (_adjustRedrawQueued) return;
        _adjustRedrawQueued = true;
        Microsoft.UI.Xaml.Media.CompositionTarget.Rendering += AdjustRedrawTick;
    }

    private static void AdjustRedrawTick(object? sender, object e)
    {
        Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= AdjustRedrawTick;
        _adjustRedrawQueued = false;
        FitCurveSquare();
        RenderCurve();
        RenderHistogram();
    }

    // The curve is a square as wide as the pane's content, at most 340
    // (docs/plans/edit-refresh.md §2.6), never a fixed height.
    private static void FitCurveSquare()
    {
        if (_curveCanvas is null) return;
        double w = _curveCanvas.ActualWidth;
        if (w <= 1) return;
        // In the scopes row (xwide) the curve is half the pane, and the
        // histogram beside it is as tall.
        double side = _adjustWidthClass == WidthXWide ? w : Math.Min(w, CurveMaxSide);
        if (Math.Abs(_curveCanvas.Height - side) > 0.5) _curveCanvas.Height = side;
        if (_adjustWidthClass == WidthXWide && _histCanvas is not null && Math.Abs(_histCanvas.Height - side) > 0.5)
            _histCanvas.Height = side;
    }

    // A section's header (chevron, glyph and name to open or close it; its
    // reset; its Auto; Black & White's on switch) over its sliders.
    private static Border BuildAdjustSection(int s, ref int tab)
    {
        AdjustSection section = AdjustSections[s];
        var outer = new StackPanel { Spacing = Space(2) };

        var head = new Grid { ColumnSpacing = Space(6) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var title = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        var chevron = new FontIcon
        {
            Glyph = _sectionOpen[s] ? "\uE70D" : "\uE76C",  // ChevronDown / ChevronRight
            FontFamily = SymbolFont,
            FontSize = UiFontSize - 3,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _sectionChevrons![s] = chevron;
        title.Children.Add(chevron);
        title.Children.Add(new FontIcon
        {
            Glyph = section.Glyph,
            FontFamily = SymbolFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            VerticalAlignment = VerticalAlignment.Center,
        });
        TextBlock name = Text(section.Name, Title, UiFontSize);
        name.VerticalAlignment = VerticalAlignment.Center;
        title.Children.Add(name);
        // The changed dot: a slider of this section is away from 0.
        var changedDot = new Border
        {
            Width = 6, Height = 6, CornerRadius = new CornerRadius(3), Background = Brush(ChromeColour.Accent),
            VerticalAlignment = VerticalAlignment.Center, Visibility = Visibility.Collapsed,
        };
        _sectionDots![s] = changedDot;
        title.Children.Add(changedDot);
        int index = s;
        Button toggle = ChromeButton(title, Pad(2, 4, 2, 4), () => ToggleSection(index),
                                     align: HorizontalAlignment.Stretch, contentAlign: HorizontalAlignment.Left,
                                     name: section.Name);
        toggle.TabIndex = tab++;
        head.Children.Add(toggle);

        Button reset = ChromeButton(new FontIcon
        {
            Glyph = "\uE7A7",  // Undo
            FontFamily = SymbolFont,
            FontSize = UiFontSize - 2,
            Foreground = Brush(Body),
        }, CompactButtonPadding, () =>
        {
            // Remove's reset takes every stroke off (it has no parameters).
            if (index == RemoveSection) Send(Command.AdjustBrush, 1);
            else Send(Command.AdjustResetSection, index);
        },
            name: $"Reset {section.Name}", tip: $"Reset {section.Name}");
        reset.TabIndex = tab++;
        Grid.SetColumn(reset, 1);
        head.Children.Add(reset);
        _sectionResets![s] = reset;

        if (section.HasAuto)
        {
            // A small capsule (the Mac's AutoPill), its outline the hairline.
            Button auto = ChromeButton(ButtonLabel("AUTO", UiFontSize - 5), Pad(7, 2, 7, 2),
                                       () => Send(Command.AdjustAuto, index), tip: $"Auto {section.Name}: one undo step");
            auto.CornerRadius = new CornerRadius(999);
            auto.BorderBrush = Brush(Hairline);
            auto.BorderThickness = new Thickness(1);
            AutomationProperties_SetName(auto, $"Auto {section.Name}");
            auto.TabIndex = tab++;
            Grid.SetColumn(auto, 2);
            head.Children.Add(auto);
            _sectionAutos![s] = auto;
        }
        if (s == 2)
        {
            _monoSwitch = new ToggleSwitch
            {
                OnContent = "",
                OffContent = "",
                MinWidth = 0,
                Width = 48,
                TabIndex = tab++,
            };
            AutomationProperties_SetName(_monoSwitch, "Black and white");
            _monoSwitch.Toggled += (_, _) =>
            {
                if (_updatingAdjust || _monoSwitch is null) return;
                Send(AdjustSpecs[MonoParam].Command, _monoSwitch.IsOn ? 100f : 0f);
            };
            Grid.SetColumn(_monoSwitch, 3);
            head.Children.Add(_monoSwitch);
        }
        outer.Children.Add(head);

        var bodyStack = new StackPanel
        {
            Spacing = Space(4),
            Padding = Pad(2, 4, 0, 0),
            Visibility = _sectionOpen[s] ? Visibility.Visible : Visibility.Collapsed,
        };
        if (s == LevelsSection || s == CurvesSection)
        {
            var picks = new Button[4];
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
            for (int ch = 0; ch < 4; ch++)
            {
                int channel = ch;
                picks[ch] = EditButton(ChannelNames[ch], () => PickChannel(index, channel));
                picks[ch].TabIndex = tab++;
                row.Children.Add(picks[ch]);
            }
            if (s == LevelsSection) _levelsPicks = picks; else _curvesPicks = picks;
            bodyStack.Children.Add(row);
        }
        if (s == 1)
        {
            _wbPick = EditButton("Pick white balance", () => SendPick(_picking == 1 ? 0 : 1),
                                 tip: "Click a neutral part of the photo to set temperature and tint");
            _wbPick.TabIndex = tab++;
            bodyStack.Children.Add(_wbPick);
        }
        if (s == SelectiveSection)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
            _bandPicks = new Border[8];
            for (int b = 0; b < 8; b++)
            {
                int band = b;
                var dot = new Border
                {
                    Width = 18,
                    Height = 18,
                    CornerRadius = new CornerRadius(9),
                    Background = Brush(BandColours[b]),
                    BorderBrush = Brush(Title),
                };
                Button pickBand = ChromeButton(dot, Pad(2, 2, 2, 2), () => { _band = band; RenderAdjust(); },
                                               name: BandNames[b], tip: BandNames[b]);
                pickBand.TabIndex = tab++;
                _bandPicks[b] = dot;
                row.Children.Add(pickBand);
            }
            bodyStack.Children.Add(row);
        }
        if (s == RedEyeSection)
        {
            _redEyePick = EditButton("Fix red eyes", () => SendPick(_picking == 2 ? 0 : 2));
            _redEyePick.TabIndex = tab++;
            bodyStack.Children.Add(_redEyePick);
            // docs/plans/edit-refresh.md S3: the size, Find eyes and Done are the
            // context bar's, below the photo, while the tool is armed.
            bodyStack.Children.Add(Text("Size, Find eyes and Done are in the bar below the photo", Body, UiFontSize - 3));
            _redEyeCount = Text("", Body, UiFontSize - 2);
            bodyStack.Children.Add(_redEyeCount);
        }
        if (s == RemoveSection)
        {
            // Slice 7: brush on the photo; Enhance fills it in, a new file.
            _brushPick = EditButton("Brush over what to remove", () => SendPick(_picking == 3 ? 0 : 3));
            _brushPick.TabIndex = tab++;
            bodyStack.Children.Add(_brushPick);
            // S3: the size (and Erase) are the context bar's, below the photo.
            bodyStack.Children.Add(Text("Size and Erase are in the bar below the photo", Body, UiFontSize - 3));
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
            _brushUndo = EditButton("Undo stroke", () => Send(Command.AdjustBrush, 0));
            _brushUndo.TabIndex = tab++;
            _brushClear = EditButton("Clear", () => Send(Command.AdjustBrush, 1));
            _brushClear.TabIndex = tab++;
            _brushRemove = EditButton("Remove…", () => Send(Command.AdjustBrush, 2),
                                      tip: "Enhance fills in what you brushed from its surroundings and saves a new file.");
            _brushRemove.TabIndex = tab++;
            row.Children.Add(_brushUndo);
            row.Children.Add(_brushClear);
            row.Children.Add(_brushRemove);
            bodyStack.Children.Add(row);
            _brushNote = Text("", Body, UiFontSize - 3);
            bodyStack.Children.Add(_brushNote);
        }
        if (s == FacesSection)
        {
            // Slice 6: real pixels from a frame of the same moment, a new file.
            Button eyes = EditButton("Open closed eyes", () => Send(Command.AdjustOpenEyes),
                                     tip: "Eyes shut? Use the same person's open eyes from a photo taken moments before or after. Saves a new file.");
            eyes.TabIndex = tab++;
            bodyStack.Children.Add(eyes);
            bodyStack.Children.Add(Text("Uses People in Local search to find the same person in the photos taken within 10 seconds.",
                                        Body, UiFontSize - 3));
        }
        if (s == CurvesSection)
        {
            // Its slot: the curve sits here, or (xwide) in the scopes row.
            _adjustCurveSlot = new StackPanel();
            bodyStack.Children.Add(_adjustCurveSlot);
            // A square (FitCurveSquare): its width is the pane's content, at
            // most 340; its height follows on the next frame. A Canvas measures
            // to no width of its own, so it stretches across its frame's one
            // column (capped below), never left-aligned to nothing.
            _curveCanvas = new Canvas
            {
                Height = 260,
                Background = Brush(ChromeColour.Surface),
            };
            AutomationProperties_SetName(_curveCanvas, "Curve");
            _curveCanvas.PointerPressed += OnCurvePressed;
            _curveCanvas.PointerMoved += OnCurveMoved;
            _curveCanvas.PointerReleased += (_, e) =>
            {
                EndCurveDrag();
                _curveCanvas?.ReleasePointerCapture(e.Pointer);
            };
            _curveCanvas.PointerCaptureLost += (_, _) => EndCurveDrag();
            _curveCanvas.SizeChanged += (_, _) => QueueAdjustRedraw();
            // The frame is a border, not a clip: a point on a corner is whole.
            // One star column capped at 340: the frame is the content's width up
            // to that, on the left (the Mac's .frame(maxWidth: 340, alignment: .leading)).
            var curveFrame = new Grid();
            curveFrame.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(1, GridUnitType.Star),
                MaxWidth = CurveMaxSide,
            });
            curveFrame.Children.Add(new Border
            {
                CornerRadius = Corner(RadiusButton),
                BorderBrush = Brush(Hairline),
                BorderThickness = new Thickness(1),
                Child = _curveCanvas,
            });
            _curveReadout = Text("", Body, UiFontSize - 3);
            _curveReadout.Margin = new Thickness(CurveInset + 2);
            _curveReadout.IsHitTestVisible = false;
            _curveReadout.HorizontalAlignment = HorizontalAlignment.Left;
            _curveReadout.VerticalAlignment = VerticalAlignment.Top;
            curveFrame.Children.Add(_curveReadout);
            _adjustCurveFrame = curveFrame;
            _adjustCurveSlot.Children.Add(curveFrame);
            bodyStack.Children.Add(Text("Drag the line to add a point · double-click or drag a point off the square to remove it",
                                        Body, UiFontSize - 3));
        }
        foreach (int p in section.Params) bodyStack.Children.Add(BuildAdjustRow(p, tab++));
        _sectionBodies![s] = bodyStack;
        outer.Children.Add(bodyStack);
        // The section's card (the Mac's: a raised surface, radius 9).
        var card = new Border
        {
            Child = outer,
            Padding = Pad(10, 6, 8, 8),
            CornerRadius = Corner(9),
            Background = Brush(ChromeColour.Surface2),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(0.5),
        };
        return card;
    }

    private static Border BuildAdjustRow(int p, int tabIndex)
    {
        AdjustSpec spec = AdjustSpecs[p];
        // name | slider | value, laid inline or with the name above
        // (LayAdjustRow, by the width class): three cells of one Grid, moved,
        // never rebuilt.
        var grid = new Grid { ColumnSpacing = Space(8), RowSpacing = Space(0) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        // The focused row's name is Title, the others Body (AdjustView.swift).
        TextBlock name = Text(spec.Name, Body, UiFontSize - 2, maxLines: 1);
        name.TextTrimming = TextTrimming.CharacterEllipsis;
        name.VerticalAlignment = VerticalAlignment.Center;
        _adjustNames![p] = name;

        var slider = new Slider
        {
            Minimum = spec.Min,
            Maximum = spec.Max,
            StepFrequency = spec.Step,
            SmallChange = spec.Step,
            LargeChange = spec.Step * 10,
            IsThumbToolTipEnabled = false,
            TabIndex = tabIndex,
            VerticalAlignment = VerticalAlignment.Center,
        };
        AutomationProperties_SetName(slider, spec.Name);
        slider.ValueChanged += (_, e) =>
        {
            _adjustReadouts![p].Text = AdjustSpecs[p].Format(e.NewValue);
            if (_updatingAdjust) return;
            _adjustValues[p] = (float)e.NewValue;
            if (AdjustSpecs[p].Command >= 0)
            {
                Send(AdjustSpecs[p].Command, (float)e.NewValue);
            }
            else
            {
                var inv = System.Globalization.CultureInfo.InvariantCulture;
                _treePending = p.ToString(inv) + "\t" + e.NewValue.ToString("0.###", inv);
                Send(Command.AdjustParam);
            }
        };
        // Double-click a slider (or its row): that parameter back to 0.
        slider.DoubleTapped += (_, _) => slider.Value = 0;
        GradientTrack(slider, p);
        _adjustSliders![p] = slider;

        // The value: click it to type a number (FakeInput; Enter sets it, Esc
        // or leaving it puts the readout back).
        TextBlock readout = Text(spec.Format(0), Body, UiFontSize - 2, maxLines: 1);
        readout.TextAlignment = TextAlignment.Right;
        _adjustReadouts![p] = readout;
        Button value = ChromeButton(readout, Pad(4, 1, 4, 1), () => BeginValueEntry(p),
                                    align: HorizontalAlignment.Right, contentAlign: HorizontalAlignment.Right,
                                    name: $"{spec.Name} value", tip: "Click to type a value");
        value.MinWidth = 64;
        _adjustValueButtons![p] = value;

        grid.Children.Add(name);
        grid.Children.Add(slider);
        grid.Children.Add(value);
        _adjustRowGrids![p] = grid;
        LayAdjustRow(grid, _adjustWidthClass == WidthRegular || _adjustWidthClass == WidthXWide || _adjustWidthClass < 0);
        // The focused row carries a wash (the Mac's selection wash, radius 6), so
        // the keys below have a visible target.
        var row = new Border
        {
            Child = grid,
            Padding = Pad(6, 2, 4, 2),
            CornerRadius = Corner(6),
            Background = Brush(Colors.Transparent),
        };
        row.DoubleTapped += (_, e) =>
        {
            if (e.OriginalSource is DependencyObject d && IsInside(d, value)) return;  // the value's own clicks
            if (slider.IsEnabled) slider.Value = 0;
        };
        ToolTipService.SetToolTip(name, $"{spec.Name}: double-click the row to set it back to 0");
        slider.GotFocus += (_, _) => FocusAdjustRow(p, true);
        slider.LostFocus += (_, _) => FocusAdjustRow(p, false);
        _adjustRows![p] = row;
        return row;
    }

    private static bool IsInside(DependencyObject d, DependencyObject ancestor)
    {
        for (DependencyObject? at = d; at is not null; at = VisualTreeHelper.GetParent(at))
            if (ReferenceEquals(at, ancestor)) return true;
        return false;
    }

    // The value readout becomes a field in place; Enter sets the slider (which
    // sends it, clamped and snapped by native), Esc or leaving puts it back.
    private static void BeginValueEntry(int p)
    {
        if (_adjustValueButtons is null || _adjustSliders is null || _adjustRowGrids is null) return;
        Button value = _adjustValueButtons[p];
        Slider slider = _adjustSliders[p];
        if (!slider.IsEnabled || value.Parent is not Grid g) return;
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        double v = slider.Value;
        var field = new FakeInput("", width: 72);
        field.SetText(Math.Abs(Math.Round(v) - v) < 1e-4 ? Math.Round(v).ToString(inv) : v.ToString("0.0", inv));
        Grid.SetRow(field, Grid.GetRow(value));
        Grid.SetColumn(field, Grid.GetColumn(value));
        AutomationProperties_SetName(field, $"{AdjustSpecs[p].Name}, type a value");
        bool done = false;
        void Finish(bool apply)
        {
            if (done) return;
            done = true;
            if (apply)
            {
                string t = field.Text.Replace("EV", "", StringComparison.OrdinalIgnoreCase).Replace(',', '.').Trim();
                if (double.TryParse(t, System.Globalization.NumberStyles.Float, inv, out double typed))
                    slider.Value = Math.Clamp(typed, slider.Minimum, slider.Maximum);
            }
            g.Children.Remove(field);
            value.Visibility = Visibility.Visible;
        }
        field.Submitted += () => Guarded(() => Finish(true));
        field.LostFocus += (_, _) => Guarded(() => Finish(false));
        field.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape) return;
            e.Handled = true;
            Guarded(() => Finish(false));
            slider.Focus(FocusState.Keyboard);
        };
        value.Visibility = Visibility.Collapsed;
        g.Children.Add(field);
        field.Focus(FocusState.Programmatic);
        field.SelectAll();
    }

    // Sliders whose track shows what they do to the picture: exposure dark to
    // light, saturation and vibrance grey to colour, temperature cool to warm,
    // tint green to magenta, a selective band's hue its neighbours either side.
    // Lightweight resources on the one Slider (never a ControlTemplate).
    private static void GradientTrack(Slider slider, int p)
    {
        Color[]? stops = p switch
        {
            0 => new[] { ColorHelper.FromArgb(255, 20, 20, 20), ColorHelper.FromArgb(255, 242, 242, 242) },
            2 or 9 => new[] { ColorHelper.FromArgb(255, 140, 140, 140), TrackWarm, TrackTintM, TrackCool },
            3 => new[] { TrackCool, ColorHelper.FromArgb(255, 217, 217, 217), TrackWarm },
            4 => new[] { TrackTintG, ColorHelper.FromArgb(255, 217, 217, 217), TrackTintM },
            _ when p >= 28 && p < 52 && (p - 28) % 3 == 0 => new[]
            {
                BandColours[((p - 28) / 3 + 7) % 8], BandColours[(p - 28) / 3], BandColours[((p - 28) / 3 + 1) % 8],
            },
            _ => null,
        };
        if (stops is null) return;
        var brush = new LinearGradientBrush { StartPoint = new Point(0, 0.5), EndPoint = new Point(1, 0.5) };
        for (int i = 0; i < stops.Length; i++)
            brush.GradientStops.Add(new GradientStop { Color = stops[i], Offset = (double)i / (stops.Length - 1) });
        foreach (string key in new[] { "SliderTrackFill", "SliderTrackFillPointerOver", "SliderTrackFillPressed" })
            slider.Resources[key] = brush;
        // The gradient is the track; no value fill over it.
        foreach (string key in new[] { "SliderTrackValueFill", "SliderTrackValueFillPointerOver",
                                       "SliderTrackValueFillPressed" })
            slider.Resources[key] = Brush(Colors.Transparent);
    }

    private static void ToggleSection(int s)
    {
        _sectionOpen[s] = !_sectionOpen[s];
        if (_sectionBodies is not null)
            _sectionBodies[s].Visibility = _sectionOpen[s] ? Visibility.Visible : Visibility.Collapsed;
        if (_sectionChevrons is not null) _sectionChevrons[s].Glyph = _sectionOpen[s] ? "\uE70D" : "\uE76C";
    }

    // Slice 3: what the next click on the photo does (shell::adjust_pick).
    // The size is the red-eye tool's, or the Remove brush's (mode 3).
    // The size travels apart now (the context bar, [ ]): native ignores it.
    private static void SendPick(int mode) => Send(Command.AdjustPick, mode * 1000 + 100);

    private static void PickChannel(int section, int channel)
    {
        if (section == LevelsSection) _levelsChannel = channel; else _curvesChannel = channel;
        RenderAdjust();
    }

    // Levels shows the picked channel's three sliders only.
    private static bool RowShown(int param) =>
        param < 16 || param >= 52 || (param < 28 ? (param - 16) / 3 == _levelsChannel : (param - 28) / 3 == _band);

    private static List<Point> CurvePointsOf(int ch) =>
        _curves[ch].Count >= 2 ? new List<Point>(_curves[ch]) : new List<Point> { new(0, 0), new(1, 1) };

    // edit::evaluate: a monotone cubic (Fritsch–Carlson), flat past the ends.
    private static double EvaluateCurve(List<Point> pts, double p)
    {
        int n = pts.Count;
        if (n < 2) return p;
        if (p <= pts[0].X) return pts[0].Y;
        if (p >= pts[n - 1].X) return pts[n - 1].Y;
        var d = new double[n - 1];
        for (int i = 0; i < n - 1; i++) d[i] = (pts[i + 1].Y - pts[i].Y) / (pts[i + 1].X - pts[i].X);
        var m = new double[n];
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (int i = 1; i < n - 1; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
        for (int i = 0; i < n - 1; i++)
        {
            if (d[i] == 0) { m[i] = 0; m[i + 1] = 0; continue; }
            double a = m[i] / d[i], b = m[i + 1] / d[i], h2 = a * a + b * b;
            if (h2 > 9) { double t0 = 3 / Math.Sqrt(h2); m[i] = t0 * a * d[i]; m[i + 1] = t0 * b * d[i]; }
        }
        int k = 0;
        while (k + 2 < n && p > pts[k + 1].X) k++;
        double h = pts[k + 1].X - pts[k].X, t = (p - pts[k].X) / h, tt = t * t, ttt = tt * t;
        return (2 * ttt - 3 * tt + 1) * pts[k].Y + (ttt - 2 * tt + t) * h * m[k]
               + (-2 * ttt + 3 * tt) * pts[k + 1].Y + (ttt - tt) * h * m[k + 1];
    }

    // The plot rect: the canvas less CurveInset on every side.
    private static Point CurveUnit(Point screen)
    {
        double w = _curveCanvas!.ActualWidth - 2 * CurveInset, h = _curveCanvas.ActualHeight - 2 * CurveInset;
        return new Point((screen.X - CurveInset) / Math.Max(w, 1), 1 - (screen.Y - CurveInset) / Math.Max(h, 1));
    }

    private static void EndCurveDrag()
    {
        _curveDrag = -1;
        if (_curveReadout is not null) _curveReadout.Text = "";
    }

    private static void ShowCurveReadout(Point p)
    {
        if (_curveReadout is null) return;
        _curveReadout.Text = $"{Math.Round(p.X * 255)} → {Math.Round(p.Y * 255)}";
    }

    private static void OnCurvePressed(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (_curveCanvas is null || _adjustReadiness != 2) return;
        Point u = CurveUnit(e.GetCurrentPoint(_curveCanvas).Position);
        List<Point> pts = CurvePointsOf(_curvesChannel);
        int near = -1;
        double best = 0.06;
        for (int i = 0; i < pts.Count; i++)
        {
            double dist = Math.Sqrt((pts[i].X - u.X) * (pts[i].X - u.X) + (pts[i].Y - u.Y) * (pts[i].Y - u.Y));
            if (dist < best) { best = dist; near = i; }
        }
        // A double-click on an inner point removes it.
        long nowMs = Environment.TickCount64;
        if (near > 0 && near < pts.Count - 1 && near == _curveLastPress &&
            nowMs - _curveLastPressMs <= DoubleClickMs())
        {
            _curveLastPress = -1;
            pts.RemoveAt(near);
            SendCurve(pts);
            return;
        }
        _curveLastPress = near;
        _curveLastPressMs = nowMs;
        if (near < 0)
        {
            if (pts.Count >= CurvePoints || u.X <= 0.02 || u.X >= 0.98) return;
            var added = new Point(u.X, EvaluateCurve(pts, u.X));
            pts.Add(added);
            pts.Sort((a, b) => a.X.CompareTo(b.X));
            near = pts.IndexOf(added);
            SendCurve(pts);
        }
        _curveDrag = near;
        _curveCanvas.CapturePointer(e.Pointer);
        if (near < pts.Count) ShowCurveReadout(pts[near]);
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern uint GetDoubleClickTime();

    private static long DoubleClickMs()
    {
        try { return GetDoubleClickTime(); }
        catch (Exception) { return 500; }
    }

    private static void OnCurveMoved(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (_curveCanvas is null || _curveDrag < 0) return;
        Point u = CurveUnit(e.GetCurrentPoint(_curveCanvas).Position);
        List<Point> pts = CurvePointsOf(_curvesChannel);
        int d = _curveDrag;
        if (d >= pts.Count) return;
        bool end = d == 0 || d == pts.Count - 1;
        if (!end && (u.Y < -0.08 || u.Y > 1.08 || u.X < -0.08 || u.X > 1.08))
        {
            pts.RemoveAt(d);
            EndCurveDrag();
            SendCurve(pts);
            return;
        }
        double lo = d > 0 ? pts[d - 1].X + 1.0 / 64 : 0;
        double hi = d < pts.Count - 1 ? pts[d + 1].X - 1.0 / 64 : 1;
        double x = end ? pts[d].X : Math.Clamp(u.X, lo, hi);
        pts[d] = new Point(x, Math.Clamp(u.Y, 0, 1));
        ShowCurveReadout(pts[d]);
        SendCurve(pts);
    }

    // Parked "channel\tx,y;x,y" (chrome_cmd_adjust_curve); shown at once, as a
    // slider is, without waiting for native's echo.
    private static void SendCurve(List<Point> pts)
    {
        _curves[_curvesChannel].Clear();
        _curves[_curvesChannel].AddRange(pts);
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        _treePending = _curvesChannel.ToString(inv) + "\t" +
                       string.Join(";", pts.Select(p => p.X.ToString("0.#####", inv) + "," + p.Y.ToString("0.#####", inv)));
        Send(Command.AdjustCurve);
        RenderCurve();
    }

    private static void RenderCurve()
    {
        if (_curveCanvas is null) return;
        _curveCanvas.Children.Clear();
        double cw = _curveCanvas.ActualWidth > 1 ? _curveCanvas.ActualWidth : 260;
        double ch = _curveCanvas.ActualHeight > 1 ? _curveCanvas.ActualHeight : 260;
        // The plot: CurveInset inside the canvas on every side (CurveUnit's rect).
        double ox = CurveInset, oy = CurveInset;
        double w = Math.Max(cw - 2 * CurveInset, 1), h = Math.Max(ch - 2 * CurveInset, 1);
        Color[] tints =
        {
            ColorHelper.FromArgb(255, 210, 212, 218),
            ColorHelper.FromArgb(255, 235, 80, 80),
            ColorHelper.FromArgb(255, 90, 210, 110),
            ColorHelper.FromArgb(255, 90, 140, 240),
        };
        if (_histValid)
        {
            // The channel's own histogram, in its tint.
            int c = _curvesChannel == 0 ? 3 : _curvesChannel - 1;
            var hp = new PointCollection { new Point(ox, oy + h) };
            for (int i = 0; i < HistogramBins; i++)
            {
                hp.Add(new Point(ox + w * (i + 0.5) / HistogramBins,
                                 oy + h - h * Math.Min(_histBins[c * HistogramBins + i], (ushort)1000) / 1000.0));
            }
            hp.Add(new Point(ox + w, oy + h));
            _curveCanvas.Children.Add(new Polygon
            {
                Points = hp,
                Fill = _curvesChannel == 0 ? Brush(Body) : Brush(tints[_curvesChannel]),
                Opacity = _curvesChannel == 0 ? 0.2 : 0.25,
            });
        }
        for (int k = 1; k < 4; k++)
        {
            _curveCanvas.Children.Add(new Line { X1 = ox + w * k / 4, X2 = ox + w * k / 4, Y1 = oy, Y2 = oy + h, Stroke = Brush(Body), Opacity = 0.2 });
            _curveCanvas.Children.Add(new Line { X1 = ox, X2 = ox + w, Y1 = oy + h * k / 4, Y2 = oy + h * k / 4, Stroke = Brush(Body), Opacity = 0.2 });
        }
        _curveCanvas.Children.Add(new Rectangle { Width = w, Height = h, Stroke = Brush(Body), StrokeThickness = 0.5, Opacity = 0.35 });
        Microsoft.UI.Xaml.Controls.Canvas.SetLeft(_curveCanvas.Children[^1], ox);
        Microsoft.UI.Xaml.Controls.Canvas.SetTop(_curveCanvas.Children[^1], oy);
        List<Point> pts = CurvePointsOf(_curvesChannel);
        var line = new PointCollection();
        for (int s = 0; s <= 96; s++)
        {
            double x = s / 96.0;
            line.Add(new Point(ox + x * w, oy + (1 - EvaluateCurve(pts, x)) * h));
        }
        _curveCanvas.Children.Add(new Polyline { Points = line, Stroke = Brush(tints[_curvesChannel]), StrokeThickness = 1.5 });
        foreach (Point p in pts)
        {
            var dot = new Ellipse { Width = 8, Height = 8, Fill = Brush(tints[_curvesChannel]) };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(dot, ox + p.X * w - 4);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(dot, oy + (1 - p.Y) * h - 4);
            _curveCanvas.Children.Add(dot);
        }
    }

    private static int SectionOf(int param) =>
        Array.FindIndex(AdjustSections, s => Array.IndexOf(s.Params, param) >= 0);

    private static void FocusAdjustRow(int index, bool focused)
    {
        if (_adjustRows is null || _adjustNames is null || index >= _adjustRows.Length) return;
        _adjustRows[index].Background = focused ? Brush(ChromeColour.SelectionWash) : Brush(Colors.Transparent);
        _adjustNames[index].Foreground = Brush(focused ? Title : Body);
    }

    private static void OnAdjustKey(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs e)
    {
        if (_adjustSliders is null) return;
        int param = Array.FindIndex(_adjustSliders, s => s.FocusState != FocusState.Unfocused);
        if (param < 0) return;
        Slider slider = _adjustSliders[param];
        bool shift = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        switch (e.Key)
        {
            case Windows.System.VirtualKey.Up:
            case Windows.System.VirtualKey.Down:
            {
                // The sliders of the open sections, top to bottom: in two
                // columns the left one, then the right (docs/plans/edit-refresh.md §2.5).
                var rows = new List<int>();
                IEnumerable<int> order = _adjustWidthClass >= WidthWide
                    ? AdjustLeftSections.Concat(AdjustRightSections)
                    : Enumerable.Range(0, AdjustSections.Length);
                foreach (int s in order)
                {
                    if (_sectionOpen[s]) rows.AddRange(AdjustSections[s].Params.Where(RowShown));
                }
                int at = rows.IndexOf(param);
                if (at < 0 || rows.Count == 0) return;
                int n = rows.Count;
                int next = (at + (e.Key == Windows.System.VirtualKey.Up ? n - 1 : 1)) % n;
                _adjustSliders[rows[next]].Focus(FocusState.Keyboard);
                e.Handled = true;
                break;
            }
            case Windows.System.VirtualKey.Left when shift:
            case Windows.System.VirtualKey.Right when shift:
                if (!slider.IsEnabled) return;
                slider.Value += (e.Key == Windows.System.VirtualKey.Left ? -10 : 10) * AdjustSpecs[param].Step;
                e.Handled = true;
                break;
            case Windows.System.VirtualKey.Number0:
            case Windows.System.VirtualKey.NumberPad0:
                if (!slider.IsEnabled) return;
                slider.Value = 0;
                e.Handled = true;
                break;
            case Windows.System.VirtualKey.A:
            {
                int s = SectionOf(param);
                if (_adjustReadiness == 2 && s >= 0 && AdjustSections[s].HasAuto) Send(Command.AdjustAuto, s);
                e.Handled = true;
                break;
            }
            case Windows.System.VirtualKey.R:
                if (_adjustReadiness == 2) Send(Command.AdjustReset);
                e.Handled = true;
                break;
        }
    }

    // Accessibility name without pulling another using into every partial.
    private static void AutomationProperties_SetName(DependencyObject o, string name) =>
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(o, name);

    private static void RenderAdjust()
    {
        if (_adjustSliders is null || _adjustReadouts is null || _adjustStatus is null) return;
        bool ready = _adjustReadiness == 2;
        _adjustStatus.Text = _adjustReadiness switch
        {
            0 => "Open a photo to adjust it.",
            1 => "Preparing the full-resolution image…",
            3 => "This file could not be prepared for editing.",
            _ => _adjustFromRaw ? "Editing the RAW's linear data." : "",
        };
        _updatingAdjust = true;
        try
        {
            for (int i = 0; i < _adjustSliders.Length; i++)
            {
                _adjustSliders[i].IsEnabled = ready;
                if (_adjustValueButtons?[i] is Button vb) vb.IsEnabled = ready;
                if (Math.Abs(_adjustSliders[i].Value - _adjustValues[i]) > 1e-4)
                {
                    _adjustSliders[i].Value = _adjustValues[i];
                }
                _adjustReadouts[i].Text = AdjustSpecs[i].Format(_adjustValues[i]);
            }
            if (_monoSwitch is not null)
            {
                _monoSwitch.IsEnabled = ready;
                _monoSwitch.IsOn = _adjustValues[MonoParam] > 0;
            }
        }
        finally
        {
            _updatingAdjust = false;
        }
        if (_adjustRows is not null)
        {
            for (int p = 0; p < _adjustRows.Length; p++)
                _adjustRows[p].Visibility = RowShown(p) ? Visibility.Visible : Visibility.Collapsed;
        }
        for (int ch = 0; ch < 4; ch++)
        {
            if (_levelsPicks is not null) _levelsPicks[ch].Opacity = ch == _levelsChannel ? 1.0 : 0.55;
            if (_curvesPicks is not null) _curvesPicks[ch].Opacity = ch == _curvesChannel ? 1.0 : 0.55;
        }
        RenderCurve();
        if (_wbPick is not null)
        {
            _wbPick.Content = ButtonLabel(_picking == 1 ? "Click something white or grey…" : "Pick white balance",
                                          UiFontSize - 2, trim: true);
            _wbPick.IsEnabled = ready;
        }
        if (_redEyePick is not null)
        {
            _redEyePick.Content = ButtonLabel(_picking == 2 ? "Click each red eye — click here when done" : "Fix red eyes",
                                              UiFontSize - 2, trim: true);
            _redEyePick.IsEnabled = ready;
        }
        if (_redEyeCount is not null) _redEyeCount.Text = _spotCount > 0 ? $"{_spotCount} fixed" : "";
        if (_brushPick is not null)
        {
            _brushPick.Content = ButtonLabel(_picking == 3 ? "Brushing — click here when done" : "Brush over what to remove",
                                             UiFontSize - 2, trim: true);
            _brushPick.IsEnabled = ready;
        }
        if (_brushUndo is not null) _brushUndo.IsEnabled = ready && _brushStrokes > 0;
        if (_brushClear is not null) _brushClear.IsEnabled = ready && _brushStrokes > 0;
        if (_brushRemove is not null) _brushRemove.IsEnabled = ready && _brushStrokes > 0;
        if (_brushNote is not null) _brushNote.Text = _brushFull ? "The brush is full: remove this, then brush more." : "";
        if (_promptNoteText is not null) _promptNoteText.Text = _promptNote;
        if (_bandPicks is not null)
        {
            for (int b = 0; b < _bandPicks.Length; b++) _bandPicks[b].BorderThickness = new Thickness(b == _band ? 2 : 0);
        }
        for (int s = 0; s < AdjustSections.Length; s++)
        {
            bool changed = s == CurvesSection
                ? Array.Exists(_curves, c => c.Exists(p => p.X != p.Y))
                : s == RedEyeSection ? _spotCount > 0
                : s == RemoveSection ? _brushStrokes > 0
                : Array.Exists(AdjustSections[s].Params, p => _adjustValues[p] != 0);
            if (_sectionResets?[s] is Button reset)
            {
                reset.Visibility = changed ? Visibility.Visible : Visibility.Collapsed;
                reset.IsEnabled = ready;
            }
            if (_sectionDots?[s] is Border dot) dot.Visibility = changed ? Visibility.Visible : Visibility.Collapsed;
            if (_sectionAutos?[s] is Button auto)
            {
                // While a section's Auto runs it reads "…" and no Auto takes a
                // second click (the Mac shows a spinner).
                auto.IsEnabled = ready && _autoSection < 0;
                auto.Content = ButtonLabel(_autoSection == s ? "…" : "AUTO", UiFontSize - 5);
            }
        }
        if (_adjustResetButton is not null) _adjustResetButton.IsEnabled = ready;
        if (_adjustPreparing is not null)
            _adjustPreparing.Root.Visibility = _adjustReadiness == 1 ? Visibility.Visible : Visibility.Collapsed;
        RenderHistogram();
    }

    // R, G, B and luma as four translucent filled polygons, bins 0..1000 of the
    // tallest non-end bin (edit::pack_histogram — the Mac pane draws the same).
    private static void RenderHistogram()
    {
        if (_histCanvas is null) return;
        _histCanvas.Children.Clear();
        if (_adjustClip is not null)
        {
            _adjustClip.Text = _histValid
                ? $"Clipped highlights {_clipHigh * 100:0.0} %   ·   crushed shadows {_clipLow * 100:0.0} %"
                : "";
        }
        if (_clipLowMark is not null)
        {
            bool lit = _histValid && _clipLow > 0;
            _clipLowMark.Fill = lit ? Brush(ColorHelper.FromArgb(255, 90, 140, 240)) : Brush(Body);
            _clipLowMark.Opacity = lit ? 1.0 : 0.35;
        }
        if (_clipHighMark is not null)
        {
            bool lit = _histValid && _clipHigh > 0;
            _clipHighMark.Fill = lit ? Brush(ColorHelper.FromArgb(255, 235, 80, 80)) : Brush(Body);
            _clipHighMark.Opacity = lit ? 1.0 : 0.35;
        }
        if (!_histValid) return;
        // The 300 is only for the first pass, before the canvas was measured;
        // after that its SizeChanged redraws at the real width.
        if (_histCanvas.ActualWidth <= 1 && _histMeasured) return;
        double w = _histCanvas.ActualWidth > 1 ? _histCanvas.ActualWidth : 300;
        double h = _histCanvas.Height;
        Color[] colours =
        {
            ColorHelper.FromArgb(110, 235, 80, 80),
            ColorHelper.FromArgb(110, 90, 210, 110),
            ColorHelper.FromArgb(110, 90, 140, 240),
            ColorHelper.FromArgb(150, 210, 212, 218),
        };
        for (int c = 0; c < 4; c++)
        {
            var points = new PointCollection { new Point(0, h) };
            for (int i = 0; i < HistogramBins; i++)
            {
                double x = w * (i + 0.5) / HistogramBins;
                double y = h - h * Math.Min(_histBins[c * HistogramBins + i], (ushort)1000) / 1000.0;
                points.Add(new Point(x, y));
            }
            points.Add(new Point(w, h));
            _histCanvas.Children.Add(new Polygon
            {
                Points = points,
                Fill = c == 3 ? Brush(Title) : Brush(colours[c]),
                Opacity = c == 3 ? 0.6 : 1.0,
            });
        }
    }
}
