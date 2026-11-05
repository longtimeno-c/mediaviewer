// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/plans/edit-refresh.md S3 (§4.2): the canvas tools' context bar, the
/// twin of the Mac's ToolBarView.swift.
/// </summary>
/// <remarks>
/// While a canvas tool is armed on a still, the transport's island (a still has
/// no transport) holds this bar instead, in the band native reserves below the
/// photo (shell/canvas_tool.h kToolBarBand), so nothing covers the picture.
/// Native owns the tool (main.cpp, shell/canvas_tool.h) and pushes what to show
/// with <see cref="SetToolBar"/> (chrome_tool_bar_args); the bar sends its
/// buttons back as Command.CanvasTool (-(action + 1)), its size slider as
/// Command.ToolSizeSet and Paint / Erase as Command.ToolEraseSet. Buttons,
/// Slider and TextBlock only (tools/check-winui-controls.ps1).
/// </remarks>
public static partial class IslandHost
{
    // Mirrors chrome_tool_bar_args (chrome_host.h) and shell::tool_bar.
    [StructLayout(LayoutKind.Sequential)]
    private struct ChromeToolBarArgs
    {
        public int Visible;
        public int Tool;      // shell::canvas_tool: 1 brush, 2 red-eye, 3 white-balance picker, 5 Extend
        public float SizePx;  // the ring's diameter, DIP
        public int Erase;
        public int Strokes;
        public int Spots;
        public int CanApply;
        public int Busy;
        public int ProgressPermille;
        public int ExtendW;
        public int ExtendH;
        public int Full;
        public int CanRedo;
        public int Eyes;
        public int CanUndo;  // S4a: a stroke or a Clear to undo
        public int ExtendGrowth;  // S6: Extend's readout, the growth in permille
        public int ExtendWarn;    // S6: a side past the 25 % warning
        public int ExtendPreset;  // S6: the chip the frame is (0 Original .. 6), -1 the person's own
    }
    private const int ToolBarArgsSize = 72;

    // shell::canvas_tool and shell::tool_bar_action (canvas_tool.h).
    private static class CanvasTools
    {
        public const int Brush = 1;
        public const int RedEye = 2;
        public const int WhiteBalance = 3;
        public const int Faces = 4;  // docs/plans/edit-refresh.md S8
        public const int Extend = 5;  // S6
    }
    private static class ToolBarAction
    {
        public const int Done = 0;
        public const int Undo = 1;
        public const int Redo = 2;
        public const int Clear = 3;
        public const int Apply = 4;
        public const int FindEyes = 5;
        // S8: the Faces bar, on the chosen faces.
        public const int FacesOpenEyes = 6;
        public const int FacesImprove = 7;
        public const int FacesDoAll = 8;
        public const int ExtendPreset = 16;  // S6: chip i is ExtendPreset + i
    }
    // S6: Extend's chips (shell::extend_preset_label: the Enhance add-on's
    // presets after Original).
    private static readonly string[] ExtendPresets = { "Original", "Wider", "Taller", "All round", "1:1", "16:9", "4:5" };

    private static ChromeToolBarArgs _toolBar;
    // The bar as built, for an update in place; null while the island holds
    // the transport (or nothing).
    private static Border? _toolBarRoot;
    private static int _toolBarBuiltFor;
    private static Button? _toolPaint, _toolErase, _toolUndo, _toolRedo, _toolClear, _toolApply, _toolFind;
    private static Button? _toolFacesEyes, _toolFacesImprove;
    private static Slider? _toolSize;
    private static TextBlock? _toolSizeText, _toolStatus;
    private static Spinner? _toolSpinner;
    private static bool _updatingToolBar;
    private static Button[]? _toolChips;  // S6: Extend's presets

    // Segoe Fluent Icons / MDL2 Assets code points.
    private const string GlyphBrush = "";
    private const string GlyphEraser = "";
    private const string GlyphUndo = "";
    private const string GlyphRedo = "";
    private const string GlyphClear = "";

    private const double ToolMinPx = 4, ToolMaxPx = 400;

    public static int SetToolBar(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < ToolBarArgsSize) return unchecked((int)0x80070057);
            _toolBar = Marshal.PtrToStructure<ChromeToolBarArgs>(arg);
            // Shown: refresh in place (a new tool rebuilds). Hidden: native's
            // ShowTransport that follows drops the content.
            if (_toolBar.Visible != 0 && _toolBarRoot is not null && _transport?.Content == _toolBarRoot)
            {
                if (_toolBarBuiltFor != ToolBarKind) _transport.Content = BuildToolBar();
                else if (ToolBarKind != EnhanceRunBar) UpdateToolBar();
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ShowTransport's visible branch while a tool is armed (or a run shows its line).
    private static bool ToolBarWanted => _toolBar.Visible != 0;

    // One-click Enhance: while a run shows its line, the bar is that line
    // ("Enhancing… 40 % · 1 min left · Cancel"), whatever tool is armed.
    private const int EnhanceRunBar = -1;
    private static int ToolBarKind => EnhanceRunActive ? EnhanceRunBar : _toolBar.Tool;
    // The run's line: the one progress language (IslandHost.Progress.cs).
    private static ProgressLine? _runProgress;
    private static Button? _runDismiss;

    // The run node changed: the bar follows, rebuilt when it turns into or out
    // of the run's line, else refreshed in place.
    private static void RefreshEnhanceRunBar()
    {
        if (_toolBarRoot is null || _transport?.Content != _toolBarRoot) return;
        if (_toolBarBuiltFor != ToolBarKind) _transport.Content = BuildToolBar();
        else if (ToolBarKind == EnhanceRunBar) UpdateEnhanceRunBar();
    }

    // docs/design/20 "Craft rules": the bar and "Enhancing · 40 % · about 1
    // min left" with Cancel (Esc too); then, in the same slot, the result
    // with Move to Recycle Bin, or why it stopped with Try again, and a quiet ×.
    // A fixed width, so the bar never reflows as the words change. The Mac
    // twin is EnhanceRunRow (EnhanceRun.swift).
    private static UIElement BuildEnhanceRunBar()
    {
        DropToolBarUi();
        _toolBarBuiltFor = EnhanceRunBar;
        var row = new Grid
        {
            ColumnSpacing = Space(4),
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Center,
            Width = 380,
        };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _runProgress = new ProgressLine(("Cancel", EnhanceRunCancel));
        row.Children.Add(_runProgress.Root);
        _runDismiss = PaneIconButton(Glyph.Close, "Dismiss", null, EnhanceRunDismiss);
        Grid.SetColumn(_runDismiss, 1);
        row.Children.Add(_runDismiss);
        _toolBarRoot = new Border
        {
            RequestedTheme = IslandTheme,
            Background = Brush(Canvas),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            Height = TransportDip,
            Padding = new Thickness(Space(12), 0, Space(12), 0),
            Child = row,
        };
        AutomationProperties.SetName(_toolBarRoot, "Enhance");
        UpdateEnhanceRunBar();
        return _toolBarRoot;
    }

    private static void UpdateEnhanceRunBar()
    {
        if (_runProgress is null) return;
        bool working = EnhanceRunWorking;
        string verb = _enhanceRunVerb.Length > 0 ? _enhanceRunVerb
                    : _enhanceRunLine.Split('…', '·')[0].Trim() is { Length: > 0 } first ? first : "Working";
        switch (_enhanceRunState)
        {
            case "done":
                _runProgress.ShowOutcome(true, _enhanceRunLine,
                                         _enhanceRunDiscard.Length > 0 ? _enhanceRunDiscard : null,
                                         _enhanceRunDiscard.Length > 0 ? EnhanceRunDiscard : null);
                break;
            case "failed":
                bool retry = _enhanceRunTool is not ("" or "remove" or "extend");  // those run from the bar
                _runProgress.ShowOutcome(false, _enhanceRunLine.Length > 0 ? _enhanceRunLine : "It did not finish.",
                                         retry ? "Try again" : null, retry ? EnhanceRunRetry : null);
                break;
            case "cancelled":
                _runProgress.ShowOutcome(null, _enhanceRunLine.Length > 0 ? _enhanceRunLine : "Cancelled. The photo is unchanged.");
                break;
            default:
                _runProgress.Show(verb, _enhanceRunFraction >= 0 ? Math.Min(1, _enhanceRunFraction) : null,
                                  key: "enhance-run-" + _enhanceRunId, timed: working);
                break;
        }
        if (_runDismiss is not null) _runDismiss.Visibility = working ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void DropToolBarUi()
    {
        _toolBarRoot = null;
        _runProgress = null;
        _runDismiss = null;
        _toolPaint = _toolErase = _toolUndo = _toolRedo = _toolClear = _toolApply = _toolFind = null;
        _toolFacesEyes = _toolFacesImprove = null;
        _toolSize = null;
        _toolSizeText = _toolStatus = null;
        _toolSpinner = null;
        _toolChips = null;
        _toolBarBuiltFor = 0;
    }

    private static UIElement BuildToolBar()
    {
        if (EnhanceRunActive) return BuildEnhanceRunBar();
        DropToolBarUi();
        _toolBarBuiltFor = _toolBar.Tool;
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = Space(8),
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        _toolStatus = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = TypeSmall,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            // One width per tool, whatever it says: "Brush over what to remove"
            // -> "1 stroke" must not re-centre the row and move every button
            // (the Mac's ToolBarView.statusSlot; 2026-11-05). The full text is
            // the tooltip.
            Width = _toolBar.Tool switch
            {
                CanvasTools.Brush => 168,
                CanvasTools.RedEye => 160,
                _ => double.NaN,
            },
        };
        Button done = EditButton("Done", () => SendToolAction(ToolBarAction.Done), tip: "Put the tool down  Esc");

        switch (_toolBar.Tool)
        {
            case CanvasTools.Brush:
                // Paint / Erase · Size · Undo · Redo · Clear · "3 strokes" · Remove ↵ · Done.
                _toolPaint = ToolModeButton(GlyphBrush, "Paint", () => Send(Command.ToolEraseSet, 0));
                _toolErase = ToolModeButton(GlyphEraser, "Erase", () => Send(Command.ToolEraseSet, 1));
                ToolTipService.SetToolTip(_toolErase, "Erase (hold Alt to erase while you brush)");
                row.Children.Add(_toolPaint);
                row.Children.Add(_toolErase);
                row.Children.Add(BuildToolSize());
                _toolUndo = IconButton(GlyphUndo, WithKey("Undo stroke", Command.ToolUndo),
                                       () => SendToolAction(ToolBarAction.Undo));
                _toolRedo = IconButton(GlyphRedo, WithKey("Redo stroke", Command.ToolRedo),
                                       () => SendToolAction(ToolBarAction.Redo));
                _toolClear = IconButton(GlyphClear, WithKey("Clear strokes", Command.ToolClear),
                                        () => SendToolAction(ToolBarAction.Clear));
                row.Children.Add(_toolUndo);
                row.Children.Add(_toolRedo);
                row.Children.Add(_toolClear);
                row.Children.Add(_toolStatus);
                _toolApply = AccentButton("Remove ↵", () => SendToolAction(ToolBarAction.Apply));
                ToolTipService.SetToolTip(_toolApply, WithKey("Remove what is brushed", Command.ToolApply));
                row.Children.Add(_toolApply);
                break;
            case CanvasTools.RedEye:
                // "Click a red pupil · 2 fixed" · Size · Find eyes · Undo · Done.
                row.Children.Add(_toolStatus);
                row.Children.Add(BuildToolSize());
                _toolSpinner = new Spinner(16);
                // A fixed slot: the spinner collapses when idle, the row does not.
                var spinSlot = new Grid { Width = 16, VerticalAlignment = VerticalAlignment.Center };
                spinSlot.Children.Add(_toolSpinner.Root);
                row.Children.Add(spinSlot);
                _toolFind = AccentButton("Find eyes", () => SendToolAction(ToolBarAction.FindEyes));
                row.Children.Add(_toolFind);
                _toolUndo = IconButton(GlyphUndo, WithKey("Undo the last fix", Command.ToolUndo),
                                       () => SendToolAction(ToolBarAction.Undo));
                row.Children.Add(_toolUndo);
                break;
            case CanvasTools.Faces:
                // S8: "3 faces · 2 chosen" · Open eyes · Improve · Do all · Done, on
                // the chosen faces (the Faces panel has each face's own).
                _toolSpinner = new Spinner(16);
                row.Children.Add(_toolSpinner.Root);
                row.Children.Add(_toolStatus);
                _toolFacesEyes = ToolModeButton("\uE7B3", "Open eyes", () => SendToolAction(ToolBarAction.FacesOpenEyes));
                ToolTipService.SetToolTip(_toolFacesEyes, "Open the chosen faces' closed eyes from another photo of them (a new file)");
                _toolFacesImprove = ToolModeButton("\uE734", "Improve", () => SendToolAction(ToolBarAction.FacesImprove));
                ToolTipService.SetToolTip(_toolFacesImprove, "A clearly better photo of each chosen face, from your photos (a new file)");
                row.Children.Add(_toolFacesEyes);
                row.Children.Add(_toolFacesImprove);
                _toolApply = AccentButton("Do all", () => SendToolAction(ToolBarAction.FacesDoAll));
                ToolTipService.SetToolTip(_toolApply, "Each chosen face's best fix, in one new file");
                row.Children.Add(_toolApply);
                break;
            case CanvasTools.Extend:
                // S6: preset chips · "+18 % · 6048×4032" · Fill ↵ · Cancel.
                _toolChips = new Button[ExtendPresets.Length];
                var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(2) };
                for (int i = 0; i < ExtendPresets.Length; i++)
                {
                    int chip = i;
                    Button b = EditButton(ExtendPresets[i], () => SendToolAction(ToolBarAction.ExtendPreset + chip),
                                          tip: i == 0 ? "No new edges" : $"{ExtendPresets[i]} (the Enhance add-on's frame)");
                    _toolChips[i] = b;
                    chips.Children.Add(b);
                }
                row.Children.Add(chips);
                row.Children.Add(_toolStatus);
                _toolApply = AccentButton("Fill ↵", () => SendToolAction(ToolBarAction.Apply));
                ToolTipService.SetToolTip(_toolApply, WithKey("Fill the new edges (a frame inside the photo crops it)",
                                                              Command.ToolApply));
                row.Children.Add(_toolApply);
                done = EditButton("Cancel", () => SendToolAction(ToolBarAction.Done), tip: "Drop the new frame  Esc");
                break;
            default:
                // The white-balance picker: one click, then it puts itself down.
                _toolStatus.Text = "Click something that should be neutral grey";
                row.Children.Add(_toolStatus);
                break;
        }
        row.Children.Add(done);

        var ground = new Grid();
        ThemeField.Lay(ground);
        ground.Children.Add(row);
        // The transport's look (a floating bar, hairline all round; the island
        // window is rectangular, so no rounding).
        _toolBarRoot = new Border
        {
            RequestedTheme = IslandTheme,
            Background = Brush(Canvas),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            Height = TransportDip,
            Padding = new Thickness(Space(10), 0, Space(10), 0),
            Child = ground,
        };
        AutomationProperties.SetName(_toolBarRoot, _toolBar.Tool switch
        {
            CanvasTools.Brush => "Remove brush",
            CanvasTools.RedEye => "Red-eye tool",
            CanvasTools.Faces => "Faces tool",
            CanvasTools.Extend => "Extend edges",
            _ => "White balance picker",
        });
        ToolTipService.SetToolTip(_toolBarRoot, ToolHints());
        UpdateToolBar();
        return _toolBarRoot;
    }

    // The size: a log slider over 4–400 DIP and "48 px".
    private static UIElement BuildToolSize()
    {
        _toolSize = new Slider
        {
            Minimum = Math.Log(ToolMinPx),
            Maximum = Math.Log(ToolMaxPx),
            StepFrequency = 0.01,
            Width = 110,
            VerticalAlignment = VerticalAlignment.Center,
            IsThumbToolTipEnabled = false,
        };
        AutomationProperties.SetName(_toolSize, "Size");
        // As the scrubber: a drag must not leave the keyboard on the island
        // (the tool's keys are the canvas's).
        _toolSize.AddHandler(UIElement.PointerReleasedEvent,
                             new Microsoft.UI.Xaml.Input.PointerEventHandler((_, _) => RestoreCanvasFocus()), true);
        _toolSize.ValueChanged += (_, e) =>
        {
            if (_updatingToolBar) return;
            double px = Math.Round(Math.Exp(e.NewValue));
            if (_toolSizeText is not null) _toolSizeText.Text = $"{px:0} px";
            Send(Command.ToolSizeSet, (float)px);
        };
        _toolSizeText = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = TypeSmall,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            Width = 48,  // "4 px" .. "400 px" in one width: the row never reflows
        };
        var size = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        size.Children.Add(_toolSize);
        size.Children.Add(_toolSizeText);
        string? smaller = KeyOf(Command.ToolSmaller), bigger = KeyOf(Command.ToolBigger);
        ToolTipService.SetToolTip(size, smaller is null && bigger is null ? "Size" : $"Size  {smaller} {bigger}");
        return size;
    }

    private static Button ToolModeButton(string glyph, string label, Action click)
    {
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
        content.Children.Add(new FontIcon { Glyph = glyph, FontFamily = IconFont, FontSize = TypeBody });
        content.Children.Add(new TextBlock { Text = label, FontFamily = UiFont, FontSize = TypeSmall });
        Button b = ChromeButton(content, CompactButtonPadding, click, name: label, tip: label);
        b.CornerRadius = Corner(RadiusButton);
        return b;
    }

    // What native last pushed, onto the controls already built.
    private static void UpdateToolBar()
    {
        if (_toolBarRoot is null) return;
        _updatingToolBar = true;
        try
        {
            ChromeToolBarArgs b = _toolBar;
            if (_toolSize is not null)
            {
                double px = Math.Clamp(b.SizePx > 0 ? b.SizePx : 40, ToolMinPx, ToolMaxPx);
                double v = Math.Log(px);
                if (Math.Abs(_toolSize.Value - v) > 0.005) _toolSize.Value = v;
                if (_toolSizeText is not null) _toolSizeText.Text = $"{px:0} px";
            }
            if (_toolPaint is not null && _toolErase is not null)
            {
                // The chosen half in the accent on its wash (the Mac's ModeButton).
                bool erase = b.Erase != 0;
                _toolPaint.Background = erase ? Brush(Microsoft.UI.Colors.Transparent) : Brush(ChromeColour.SelectionWash);
                _toolErase.Background = erase ? Brush(ChromeColour.SelectionWash) : Brush(Microsoft.UI.Colors.Transparent);
                _toolPaint.Foreground = Brush(erase ? Body : ChromeColour.Accent);
                _toolErase.Foreground = Brush(erase ? ChromeColour.Accent : Body);
            }
            if (_toolUndo is not null)
                _toolUndo.IsEnabled = b.Tool == CanvasTools.Brush ? b.CanUndo != 0 : b.Spots > 0 || _editCount > 0;
            if (_toolRedo is not null) _toolRedo.IsEnabled = b.CanRedo != 0;
            if (_toolClear is not null) _toolClear.IsEnabled = b.Strokes > 0;
            if (_toolApply is not null) _toolApply.IsEnabled = b.CanApply != 0;
            if (_toolFacesEyes is not null) _toolFacesEyes.IsEnabled = b.CanApply != 0;
            if (_toolFacesImprove is not null) _toolFacesImprove.IsEnabled = b.CanApply != 0;
            if (_toolFind is not null)
            {
                _toolFind.IsEnabled = b.CanApply != 0 && b.Busy == 0;
                ToolTipService.SetToolTip(_toolFind, b.Eyes > 0
                    ? "Fix every red eye on the photo, one undo step"
                    : "Find eyes needs People in Local search (Settings → Add-ons)");
            }
            if (_toolSpinner is not null) _toolSpinner.Active = b.Busy != 0;
            if (_toolChips is not null)
            {
                // The chip the frame is, in the accent on its wash (the Mac's ChipButton).
                for (int i = 0; i < _toolChips.Length; i++)
                {
                    bool on = b.ExtendPreset == i;
                    _toolChips[i].Background = on ? Brush(ChromeColour.SelectionWash) : Brush(Microsoft.UI.Colors.Transparent);
                    _toolChips[i].Foreground = Brush(on ? ChromeColour.Accent : Body);
                }
            }
            if (_toolStatus is not null)
            {
                switch (b.Tool)
                {
                    case CanvasTools.Brush:
                        _toolStatus.Text = b.Full != 0 ? "Brush full: Remove, then brush more"
                                         : b.Strokes == 0 ? "Brush over what to remove"
                                         : b.Strokes == 1 ? "1 stroke" : $"{b.Strokes} strokes";
                        _toolStatus.Foreground = Brush(b.Full != 0 ? ChromeColour.Accent : Body);
                        ToolTipService.SetToolTip(_toolStatus, _toolStatus.Text);
                        break;
                    case CanvasTools.RedEye:
                        _toolStatus.Text = b.Busy != 0 ? "Looking for red eyes…"
                                         : b.Spots == 0 ? "Click a red pupil" : $"Click a red pupil · {b.Spots} fixed";
                        ToolTipService.SetToolTip(_toolStatus, _toolStatus.Text);
                        break;
                    case CanvasTools.Faces:
                        // Eyes: the faces found; Spots: the chosen (shell/canvas_tool.h).
                        _toolStatus.Text = b.Eyes > 0
                            ? $"{(b.Eyes == 1 ? "1 face" : $"{b.Eyes} faces")} · {b.Spots} chosen"
                            : FacesSummary();
                        break;
                    case CanvasTools.Extend:
                        // "+18 % · 6048×4032"; past the 25 % warning in the
                        // overlay's caution amber (the Mac's MVTheme.warm), saying why.
                        int pct = (Math.Abs(b.ExtendGrowth) + 5) / 10;
                        _toolStatus.Text = b.ExtendW > 0
                            ? $"{(b.ExtendGrowth < 0 ? "−" : "+")}{pct} % · {b.ExtendW}×{b.ExtendH}" +
                              (b.ExtendWarn != 0 ? " · may smear" : "")
                            : "Drag the edges past the photo";
                        _toolStatus.Foreground = b.ExtendWarn != 0 ? Brush(TrackWarm) : Brush(Body);
                        ToolTipService.SetToolTip(_toolStatus, b.ExtendWarn != 0 ? "Past 25 % a side, fills may smear"
                                                                                  : "The new picture's size");
                        break;
                }
            }
        }
        finally
        {
            _updatingToolBar = false;
        }
    }

    private static void SendToolAction(int action) => Send(Command.CanvasTool, -(action + 1));

    // "[ ] size · Alt erase · Space pan · Enter remove · Esc done", from the live table.
    private static string ToolHints()
    {
        var parts = new List<string>();
        string? smaller = KeyOf(Command.ToolSmaller), bigger = KeyOf(Command.ToolBigger);
        if (smaller is not null || bigger is not null) parts.Add($"{smaller} {bigger} size");
        if (_toolBar.Tool == CanvasTools.Faces) return "Click a face to choose it · Esc done";
        if (_toolBar.Tool == CanvasTools.Extend)
        {
            // S6: drag the edges; Enter fills, Esc cancels.
            return $"Drag an edge past the photo · {KeyOf(Command.ToolApply) ?? "Enter"} fill · Esc cancel";
        }
        if (_toolBar.Tool == CanvasTools.Brush) parts.Add("Alt erase");
        parts.Add($"{KeyOf(Command.ToolPan) ?? "Space"} pan");
        if (_toolBar.Tool == CanvasTools.Brush && KeyOf(Command.ToolApply) is string apply) parts.Add($"{apply} remove");
        parts.Add("Esc done");
        return string.Join(" · ", parts);
    }
}
