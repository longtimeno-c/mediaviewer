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
/// PR 11: the adjust pane — exposure, contrast, saturation, temperature and
/// tint, a histogram and the clipping readout. The third panel island, on the
/// right edge the metadata pane also uses (native shows one at a time).
/// </summary>
/// <remarks>
/// The sliders post their value as the command's float (one command id per
/// parameter, commands.h); native owns the edit stack and pushes back what the
/// pane shows (<c>shell::adjust_view</c>). Nothing here touches pixels: the
/// canvas redraws from the new uniforms on the render thread, and the
/// histogram is a reduction a worker ran. The sliders stay disabled until the
/// FP16 working image is built — for a RAW, LibRaw's full linear develop
/// (docs/design/07: never the embedded preview). Keyboard-complete: Shift+A shows the
/// pane focused on its first slider (ShowPanel's focus flag), Tab walks the
/// sliders, arrows step, Esc returns to the canvas — a focused pane owns its
/// keys (FocusKind.Pane, docs/design/16 "Pane").
/// </remarks>
public static partial class IslandHost
{
    // shell/adjust_pane.h adjust_view: 11 int/float words, then 4 x 64 uint16.
    internal const int AdjustViewSize = 44 + 2 * 4 * HistogramBins;
    internal const int HistogramBins = 64;

    private static DesktopWindowXamlSource? _adjustPane;
    private static bool _adjustPaneVisible;

    // Mirrors edit::adjust_param order and edit::range_of.
    private sealed record AdjustSpec(string Name, int Command, double Min, double Max, double Step,
                                     Func<double, string> Format);

    private static readonly AdjustSpec[] AdjustSpecs =
    {
        new("Exposure", Command.AdjustExposure, -5, 5, 0.1, v => $"{v:+0.0;-0.0;0.0} EV"),
        new("Contrast", Command.AdjustContrast, -100, 100, 5, v => $"{v:+0;-0;0}"),
        new("Saturation", Command.AdjustSaturation, -100, 100, 5, v => $"{v:+0;-0;0}"),
        new("Temperature", Command.AdjustTemperature, -100, 100, 5, v => $"{v:+0;-0;0}"),
        new("Tint", Command.AdjustTint, -100, 100, 5, v => $"{v:+0;-0;0}"),
    };

    // Last view native pushed.
    private static int _adjustReadiness;  // shell::adjust_readiness
    private static bool _adjustFromRaw;
    private static readonly float[] _adjustValues = new float[5];
    private static float _clipHigh;
    private static float _clipLow;
    private static bool _histValid;
    private static readonly ushort[] _histBins = new ushort[4 * HistogramBins];

    private static Slider[]? _adjustSliders;
    private static TextBlock[]? _adjustReadouts;
    private static TextBlock? _adjustStatus;
    private static TextBlock? _adjustClip;
    private static Canvas? _histCanvas;
    private static Button? _adjustResetButton;
    private static JobBar? _adjustPreparing;
    private static Border[]? _adjustRows;
    private static TextBlock[]? _adjustNames;
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
    /// Native pushes readiness, values, histogram and clipping whenever any of
    /// them changes. In: shell::adjust_view (blittable, AdjustViewSize bytes).
    /// </summary>
    public static int SetAdjustView(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < AdjustViewSize) return unchecked((int)0x80070057);
            _adjustReadiness = Marshal.ReadInt32(arg, 0);
            _adjustFromRaw = Marshal.ReadInt32(arg, 4) != 0;
            for (int i = 0; i < 5; i++)
            {
                _adjustValues[i] = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 8 + 4 * i));
            }
            _clipHigh = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 28));
            _clipLow = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 32));
            _histValid = Marshal.ReadInt32(arg, 36) != 0;
            var raw = new short[_histBins.Length];
            Marshal.Copy(arg + 44, raw, 0, raw.Length);
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
        var col = new StackPanel { Spacing = 10, Padding = new Thickness(14, 4, 14, 14) };

        _histCanvas = new Canvas { Height = 96, Background = Brush(ChromeColour.Surface) };
        // Clipped to radius 4, as the Mac's clipShape (issue #198).
        col.Children.Add(new Border { CornerRadius = new CornerRadius(RadiusButton), Child = _histCanvas });
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
        for (int i = 0; i < AdjustSpecs.Length; i++)
        {
            AdjustSpec spec = AdjustSpecs[i];
            var rowStack = new StackPanel { Spacing = 2 };
            var head = new Grid();
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            // The focused row's name is Title, the others Body (AdjustView.swift).
            TextBlock name = Text(spec.Name, Body);
            head.Children.Add(name);
            _adjustNames[i] = name;
            TextBlock readout = Text(spec.Format(0), Body);
            Grid.SetColumn(readout, 1);
            head.Children.Add(readout);
            _adjustReadouts[i] = readout;
            rowStack.Children.Add(head);

            var slider = new Slider
            {
                Minimum = spec.Min,
                Maximum = spec.Max,
                StepFrequency = spec.Step,
                SmallChange = spec.Step,
                LargeChange = spec.Step * 10,
                IsThumbToolTipEnabled = false,
                TabIndex = i,
            };
            AutomationProperties_SetName(slider, spec.Name);
            int index = i;
            slider.ValueChanged += (_, e) =>
            {
                _adjustReadouts![index].Text = AdjustSpecs[index].Format(e.NewValue);
                if (_updatingAdjust) return;
                _adjustValues[index] = (float)e.NewValue;
                Send(AdjustSpecs[index].Command, (float)e.NewValue);
            };
            // Double-click a slider: that parameter back to 0.
            slider.DoubleTapped += (_, _) => slider.Value = 0;
            _adjustSliders[i] = slider;
            rowStack.Children.Add(slider);
            // The focused row carries a wash (the Mac's primary at 0.08,
            // radius 6), so the keys below have a visible target.
            var row = new Border
            {
                Child = rowStack,
                Padding = new Thickness(4, 2, 4, 2),
                CornerRadius = new CornerRadius(6),
                Background = Brush(Colors.Transparent),
            };
            slider.GotFocus += (_, _) => FocusAdjustRow(index, true);
            slider.LostFocus += (_, _) => FocusAdjustRow(index, false);
            _adjustRows[i] = row;
            col.Children.Add(row);
        }

        _adjustResetButton = EditButton("Reset colour", () => Send(Command.AdjustReset), compact: false);
        _adjustResetButton.TabIndex = AdjustSpecs.Length;
        col.Children.Add(_adjustResetButton);
        col.Children.Add(Text("↑ ↓ choose · ← → change (Shift ×10) · 0 zero · R reset · Esc photo · Shift+A closes · Ctrl+S exports",
                              Body, UiFontSize - 3));
        // The Mac's keys (AdjustView.swift), on a focused slider: ↑ ↓ choose
        // a row rather than nudge it, Shift+← → step ten, 0 zeroes the row
        // and R resets every slider. A focused pane owns its keys
        // (key_router: focus_kind::pane), so these reach XAML.
        col.PreviewKeyDown += OnAdjustKey;

        body.Children.Add(new ScrollViewer { Content = col, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        RenderAdjust();
        return root;
    }

    private static void FocusAdjustRow(int index, bool focused)
    {
        if (_adjustRows is null || _adjustNames is null || index >= _adjustRows.Length) return;
        _adjustRows[index].Background = focused ? Brush(ChromeColour.HoverWash) : Brush(Colors.Transparent);
        _adjustNames[index].Foreground = Brush(focused ? Title : Body);
    }

    private static void OnAdjustKey(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs e)
    {
        if (_adjustSliders is null) return;
        int row = Array.FindIndex(_adjustSliders, s => s.FocusState != FocusState.Unfocused);
        if (row < 0) return;
        Slider slider = _adjustSliders[row];
        bool shift = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        switch (e.Key)
        {
            case Windows.System.VirtualKey.Up:
            case Windows.System.VirtualKey.Down:
            {
                int n = _adjustSliders.Length;
                int next = (row + (e.Key == Windows.System.VirtualKey.Up ? n - 1 : 1)) % n;
                _adjustSliders[next].Focus(FocusState.Keyboard);
                e.Handled = true;
                break;
            }
            case Windows.System.VirtualKey.Left when shift:
            case Windows.System.VirtualKey.Right when shift:
                if (!slider.IsEnabled) return;
                slider.Value += (e.Key == Windows.System.VirtualKey.Left ? -10 : 10) * AdjustSpecs[row].Step;
                e.Handled = true;
                break;
            case Windows.System.VirtualKey.Number0:
            case Windows.System.VirtualKey.NumberPad0:
                if (!slider.IsEnabled) return;
                slider.Value = 0;
                e.Handled = true;
                break;
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
        }
        finally
        {
            _updatingAdjust = false;
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
