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
    // shell/adjust_pane.h adjust_view: 2 ints, 16 floats, 2 floats, 2 ints,
    // then 4 x 64 uint16.
    internal const int AdjustParamCount = 16;
    internal const int AdjustViewSize = 24 + 4 * AdjustParamCount + 2 * 4 * HistogramBins;
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
    };

    // edit::adjust_section order: name, Segoe Fluent glyph, parameters as shown, Auto.
    private sealed record AdjustSection(string Name, string Glyph, int[] Params, bool HasAuto);

    private static readonly AdjustSection[] AdjustSections =
    {
        new("Light", "\uE706", new[] { 0, 5, 6, 7, 8, 1 }, true),           // Brightness
        new("Colour", "\uE790", new[] { 2, 9, 3, 4 }, true),                // Color
        new("Black & White", "\uE793", new[] { 10, 11, 12 }, true),         // Contrast
        new("Vignette", "\uEA3A", new[] { 13, 14, 15 }, false),             // CircleRing
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
    private static readonly bool[] _sectionOpen = { true, true, false, false };

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
            var raw = new short[_histBins.Length];
            Marshal.Copy(arg + at + 16, raw, 0, raw.Length);
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
            Send(AdjustSpecs[p].Command, (float)e.NewValue);
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
                    if (_sectionOpen[s]) rows.AddRange(AdjustSections[s].Params);
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
        for (int s = 0; s < AdjustSections.Length; s++)
        {
            bool changed = Array.Exists(AdjustSections[s].Params, p => _adjustValues[p] != 0);
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
