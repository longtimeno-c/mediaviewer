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
    // 4 ints (curve_n), 2 x 4 x 8 floats (curve x, y), 2 ints (picking,
    // spot_count), 128 bytes of prompt note (UTF-8, NUL-terminated), then
    // 4 x 64 uint16.
    internal const int AdjustParamCount = 56;
    internal const int CurvePoints = 8;
    internal const int AdjustViewSize =
        24 + 4 * AdjustParamCount + 16 + 2 * 4 * 4 * CurvePoints + 8 + PromptNoteBytes + 2 * 4 * HistogramBins;
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
    private static readonly bool[] _sectionOpen = { true, true, false, false, false, false, false, false, false };
    // Slice 3: the band Selective Colour shows, the pick mode and fixes from
    // native, the red-eye tool's size, and their controls.
    private static int _band;
    private static string _promptNote = "";
    private static TextBlock? _promptNoteText;
    private static int _picking;
    private static int _spotCount;
    private static double _redEyeSize;
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

    private static void DropAdjustUi()
    {
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
        _wbPick = null;
        _promptNoteText = null;
        _redEyePick = null;
        _redEyeCount = null;
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
            _promptNote = Marshal.PtrToStringUTF8(arg + pick + 8) ?? "";
            var raw = new short[_histBins.Length];
            Marshal.Copy(arg + pick + 8 + PromptNoteBytes, raw, 0, raw.Length);
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

        _histCanvas = new Canvas { Height = 96, Background = Brush(ChromeColour.Surface) };
        // Clipped to radius 4, as the Mac's clipShape (issue #198).
        col.Children.Add(new Border { CornerRadius = Corner(RadiusButton), Child = _histCanvas });
        _adjustClip = Text("", Body, UiFontSize - 2);
        col.Children.Add(_adjustClip);
        _adjustStatus = Text("", Body);
        col.Children.Add(_adjustStatus);
        // "Preparing" has no fraction: an indeterminate bar (the Mac's
        // ProgressView; ProgressRing cannot load in these islands).
        _adjustPreparing = new JobBar { IsIndeterminate = true };
        _adjustPreparing.Root.Visibility = Visibility.Collapsed;
        col.Children.Add(_adjustPreparing.Root);

        // Slice 5b: say what you want; the sliders it names move (one undo
        // step). Not a TextBox: that control fail-fasts in these islands
        // (see FakeInput, tools/check-winui-controls.ps1).
        var request = new FakeInput("Describe a change — a bit warmer, less noise…");
        AutomationProperties_SetName(request, "Describe a change");
        request.Submitted += () =>
        {
            if (request.Text.Trim().Length == 0) return;
            _treePending = request.Text;
            Send(Command.AdjustPrompt);
            request.SetText("");
        };
        col.Children.Add(request);
        _promptNoteText = Text("", Body, UiFontSize - 2);
        col.Children.Add(_promptNoteText);

        _adjustSliders = new Slider[AdjustSpecs.Length];
        _adjustReadouts = new TextBlock[AdjustSpecs.Length];
        _adjustRows = new Border[AdjustSpecs.Length];
        _adjustNames = new TextBlock[AdjustSpecs.Length];
        _sectionBodies = new StackPanel[AdjustSections.Length];
        _sectionChevrons = new FontIcon[AdjustSections.Length];
        _sectionResets = new Button[AdjustSections.Length];
        _sectionAutos = new Button?[AdjustSections.Length];
        int tab = 0;
        for (int s = 0; s < AdjustSections.Length; s++)
        {
            col.Children.Add(BuildAdjustSection(s, ref tab));
        }

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

        body.Children.Add(new ScrollViewer { Content = col, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        RenderAdjust();
        return root;
    }

    // A section's header (chevron, glyph and name to open or close it; its
    // reset; its Auto; Black & White's on switch) over its sliders.
    private static UIElement BuildAdjustSection(int s, ref int tab)
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
        TextBlock name = Text(section.Name, Title, UiFontSize + 2);
        name.VerticalAlignment = VerticalAlignment.Center;
        title.Children.Add(name);
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
        }, CompactButtonPadding, () => Send(Command.AdjustResetSection, index),
            name: $"Reset {section.Name}", tip: $"Reset {section.Name}");
        reset.TabIndex = tab++;
        Grid.SetColumn(reset, 1);
        head.Children.Add(reset);
        _sectionResets![s] = reset;

        if (section.HasAuto)
        {
            Button auto = EditButton("AUTO", () => Send(Command.AdjustAuto, index), tip: $"Auto {section.Name}");
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
            Spacing = Space(2),
            Padding = Pad(14, 0, 0, 0),
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
            var size = new Slider { Minimum = -100, Maximum = 100, StepFrequency = 5, Value = _redEyeSize, TabIndex = tab++ };
            AutomationProperties_SetName(size, "Red-eye size");
            size.ValueChanged += (_, e) =>
            {
                _redEyeSize = e.NewValue;
                if (_picking == 2) SendPick(2);
            };
            bodyStack.Children.Add(Text("Size", Body));
            bodyStack.Children.Add(size);
            _redEyeCount = Text("", Body, UiFontSize - 2);
            bodyStack.Children.Add(_redEyeCount);
        }
        if (s == CurvesSection)
        {
            _curveCanvas = new Canvas { Height = 220, Background = Brush(ChromeColour.Surface) };
            AutomationProperties_SetName(_curveCanvas, "Curve");
            _curveCanvas.PointerPressed += OnCurvePressed;
            _curveCanvas.PointerMoved += OnCurveMoved;
            _curveCanvas.PointerReleased += (_, _) => _curveDrag = -1;
            _curveCanvas.SizeChanged += (_, _) => RenderCurve();
            bodyStack.Children.Add(new Border { CornerRadius = Corner(RadiusButton), Child = _curveCanvas });
            bodyStack.Children.Add(Text("Drag the line to add a point · drag a point off the square to remove it",
                                        Body, UiFontSize - 3));
        }
        foreach (int p in section.Params) bodyStack.Children.Add(BuildAdjustRow(p, tab++));
        _sectionBodies![s] = bodyStack;
        outer.Children.Add(bodyStack);
        return outer;
    }

    private static Border BuildAdjustRow(int p, int tabIndex)
    {
        AdjustSpec spec = AdjustSpecs[p];
        var rowStack = new StackPanel { Spacing = Space(2) };
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        // The focused row's name is Title, the others Body (AdjustView.swift).
        TextBlock name = Text(spec.Name, Body);
        head.Children.Add(name);
        _adjustNames![p] = name;
        TextBlock readout = Text(spec.Format(0), Body);
        Grid.SetColumn(readout, 1);
        head.Children.Add(readout);
        _adjustReadouts![p] = readout;
        rowStack.Children.Add(head);

        var slider = new Slider
        {
            Minimum = spec.Min,
            Maximum = spec.Max,
            StepFrequency = spec.Step,
            SmallChange = spec.Step,
            LargeChange = spec.Step * 10,
            IsThumbToolTipEnabled = false,
            TabIndex = tabIndex,
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
        // Double-click a slider: that parameter back to 0.
        slider.DoubleTapped += (_, _) => slider.Value = 0;
        _adjustSliders![p] = slider;
        rowStack.Children.Add(slider);
        // The focused row carries a wash (the Mac's primary at 0.08,
        // radius 6), so the keys below have a visible target.
        var row = new Border
        {
            Child = rowStack,
            Padding = Pad(4, 2, 4, 2),
            CornerRadius = Corner(6),
            Background = Brush(Colors.Transparent),
        };
        slider.GotFocus += (_, _) => FocusAdjustRow(p, true);
        slider.LostFocus += (_, _) => FocusAdjustRow(p, false);
        _adjustRows![p] = row;
        return row;
    }

    private static void ToggleSection(int s)
    {
        _sectionOpen[s] = !_sectionOpen[s];
        if (_sectionBodies is not null)
            _sectionBodies[s].Visibility = _sectionOpen[s] ? Visibility.Visible : Visibility.Collapsed;
        if (_sectionChevrons is not null) _sectionChevrons[s].Glyph = _sectionOpen[s] ? "\uE70D" : "\uE76C";
    }

    // Slice 3: what the next click on the photo does (shell::adjust_pick).
    private static void SendPick(int mode) =>
        Send(Command.AdjustPick, mode * 1000 + (float)Math.Round(_redEyeSize + 100));

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

    private static Point CurveUnit(Point screen)
    {
        double w = _curveCanvas!.ActualWidth, h = _curveCanvas.ActualHeight;
        return new Point(screen.X / Math.Max(w, 1), 1 - screen.Y / Math.Max(h, 1));
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
            _curveDrag = -1;
            SendCurve(pts);
            return;
        }
        double lo = d > 0 ? pts[d - 1].X + 1.0 / 64 : 0;
        double hi = d < pts.Count - 1 ? pts[d + 1].X - 1.0 / 64 : 1;
        double x = end ? pts[d].X : Math.Clamp(u.X, lo, hi);
        pts[d] = new Point(x, Math.Clamp(u.Y, 0, 1));
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
        double w = _curveCanvas.ActualWidth > 1 ? _curveCanvas.ActualWidth : 260;
        double h = _curveCanvas.ActualHeight > 1 ? _curveCanvas.ActualHeight : 220;
        Color[] tints =
        {
            ColorHelper.FromArgb(255, 210, 212, 218),
            ColorHelper.FromArgb(255, 235, 80, 80),
            ColorHelper.FromArgb(255, 90, 210, 110),
            ColorHelper.FromArgb(255, 90, 140, 240),
        };
        if (_histValid)
        {
            int c = _curvesChannel == 0 ? 3 : _curvesChannel - 1;
            var hp = new PointCollection { new Point(0, h) };
            for (int i = 0; i < HistogramBins; i++)
            {
                hp.Add(new Point(w * (i + 0.5) / HistogramBins,
                                 h - h * Math.Min(_histBins[c * HistogramBins + i], (ushort)1000) / 1000.0));
            }
            hp.Add(new Point(w, h));
            _curveCanvas.Children.Add(new Polygon { Points = hp, Fill = Brush(Body), Opacity = 0.2 });
        }
        for (int k = 1; k < 4; k++)
        {
            _curveCanvas.Children.Add(new Line { X1 = w * k / 4, X2 = w * k / 4, Y1 = 0, Y2 = h, Stroke = Brush(Body), Opacity = 0.2 });
            _curveCanvas.Children.Add(new Line { X1 = 0, X2 = w, Y1 = h * k / 4, Y2 = h * k / 4, Stroke = Brush(Body), Opacity = 0.2 });
        }
        List<Point> pts = CurvePointsOf(_curvesChannel);
        var line = new PointCollection();
        for (int s = 0; s <= 96; s++)
        {
            double x = s / 96.0;
            line.Add(new Point(x * w, (1 - EvaluateCurve(pts, x)) * h));
        }
        _curveCanvas.Children.Add(new Polyline { Points = line, Stroke = Brush(tints[_curvesChannel]), StrokeThickness = 1.5 });
        foreach (Point p in pts)
        {
            var dot = new Ellipse { Width = 8, Height = 8, Fill = Brush(tints[_curvesChannel]) };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(dot, p.X * w - 4);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(dot, (1 - p.Y) * h - 4);
            _curveCanvas.Children.Add(dot);
        }
    }

    private static int SectionOf(int param) =>
        Array.FindIndex(AdjustSections, s => Array.IndexOf(s.Params, param) >= 0);

    private static void FocusAdjustRow(int index, bool focused)
    {
        if (_adjustRows is null || _adjustNames is null || index >= _adjustRows.Length) return;
        _adjustRows[index].Background = focused ? Brush(ChromeColour.HoverWash) : Brush(Colors.Transparent);
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
                // The sliders of the open sections, top to bottom.
                var rows = new List<int>();
                for (int s = 0; s < AdjustSections.Length; s++)
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
                : Array.Exists(AdjustSections[s].Params, p => _adjustValues[p] != 0);
            if (_sectionResets?[s] is Button reset)
            {
                reset.Visibility = changed ? Visibility.Visible : Visibility.Collapsed;
                reset.IsEnabled = ready;
            }
            if (_sectionAutos?[s] is Button auto)
            {
                // While a section's Auto runs it reads "…" and no Auto takes a
                // second click (the Mac shows a spinner).
                auto.IsEnabled = ready && _autoSection < 0;
                auto.Content = ButtonLabel(_autoSection == s ? "…" : "AUTO", UiFontSize - 2, trim: true);
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
        if (!_histValid) return;
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
