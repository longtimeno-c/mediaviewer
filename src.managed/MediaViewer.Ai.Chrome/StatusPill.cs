// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Numerics;
using MediaViewer.Interop;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// A small ring (the Mac's AIProgressRing, AITable.swift): the running pass's
/// progress as a solid accent arc over a faint track and, while work runs, a
/// soft comet that sweeps the track. The comet is a fading tail, not a second
/// hard arc, so it never reads as progress.
/// </summary>
/// <remarks>
/// Ellipse and Path arcs, not the system progress ring (that control has no
/// default style in these island hosts and fail-fasts). The sweep is a
/// compositor rotation: no UI-thread work per frame, and it is stopped, not
/// hidden, whenever nothing turns, so a still panel costs nothing. Reduce
/// motion leaves the arc alone and never sweeps.
/// </remarks>
internal sealed class StatusRing
{
    private readonly Look _look;
    private readonly double _size;
    private readonly double _line;
    private readonly Path _arc;
    private readonly Path _comet;
    private double _progress = -1;
    private bool _spinning;
    private bool _running;

    public Grid Root { get; }

    public StatusRing(Look look, double size, double lineWidth)
    {
        _look = look;
        _size = size;
        _line = lineWidth;
        Root = new Grid { Width = size, Height = size, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false };
        double inset = lineWidth / 2;
        Root.Children.Add(new Ellipse
        {
            Width = size - lineWidth,
            Height = size - lineWidth,
            Margin = new Thickness(inset),
            Stroke = look.LiveTint(AddonColour.Body, 46),
            StrokeThickness = lineWidth,
        });
        // The comet: 0.32 of the circle, its tail fading to nothing.
        _comet = new Path
        {
            Data = ArcGeometry(0.32),
            StrokeThickness = lineWidth,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            Opacity = 0,
        };
        Root.Children.Add(_comet);
        _arc = new Path
        {
            Stroke = look[AddonColour.Accent],
            StrokeThickness = lineWidth,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        };
        Root.Children.Add(_arc);
        Root.Unloaded += (_, _) => Stop();
        Root.Loaded += (_, _) => Apply();
        AutomationProperties.SetAccessibilityView(Root, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
    }

    /// <summary>0…1 of the pass the line names; nothing drawn under 0.2 %.</summary>
    public void SetProgress(double fraction)
    {
        fraction = Math.Clamp(fraction, 0, 1);
        if (Math.Abs(fraction - _progress) < 0.001) return;
        _progress = fraction;
        _arc.Data = fraction > 0.002 ? ArcGeometry(fraction) : null;
    }

    /// <summary>Sweeps while true (and motion is on); still otherwise.</summary>
    public void SetSpinning(bool spinning)
    {
        if (spinning == _spinning) return;
        _spinning = spinning;
        Apply();
    }

    private void Apply()
    {
        bool run = _spinning && _look.Motion && Root.IsLoaded;
        _comet.Opacity = _spinning && _look.Motion ? 1 : 0;
        if (run == _running) return;
        if (run) Start();
        else Stop();
    }

    private void Start()
    {
        // In the accent of the moment: the theme may have changed since the last sweep.
        _comet.Stroke = CometBrush();
        Visual visual = ElementCompositionPreview.GetElementVisual(_comet);
        Compositor compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)_size / 2, (float)_size / 2, 0);
        ScalarKeyFrameAnimation spin = compositor.CreateScalarKeyFrameAnimation();
        spin.InsertKeyFrame(0, 0);
        spin.InsertKeyFrame(1, 360, compositor.CreateLinearEasingFunction());
        spin.Duration = TimeSpan.FromMilliseconds(1400);
        spin.IterationBehavior = AnimationIterationBehavior.Forever;
        visual.StartAnimation("RotationAngleInDegrees", spin);
        _running = true;
    }

    private void Stop()
    {
        if (!_running) return;
        ElementCompositionPreview.GetElementVisual(_comet).StopAnimation("RotationAngleInDegrees");
        _running = false;
    }

    // A tail from transparent to 70 % accent along the arc's chord (the arc
    // starts at 12 o'clock and runs clockwise to about 4 o'clock).
    private Brush CometBrush()
    {
        Windows.UI.Color accent = _look[AddonColour.Accent].Color;
        var brush = new LinearGradientBrush
        {
            StartPoint = new Point(0.5, 0),
            EndPoint = new Point(0.9, 0.8),
        };
        brush.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(0, accent.R, accent.G, accent.B), Offset = 0 });
        brush.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(179, accent.R, accent.G, accent.B), Offset = 1 });
        return brush;
    }

    /// <summary>An arc from 12 o'clock, clockwise, over <paramref name="fraction"/> of the circle.</summary>
    private Geometry ArcGeometry(double fraction)
    {
        double r = (_size - _line) / 2;
        double c = _size / 2;
        if (fraction >= 0.999)
        {
            return new EllipseGeometry { Center = new Point(c, c), RadiusX = r, RadiusY = r };
        }
        double angle = fraction * 2 * Math.PI;
        var start = new Point(c, c - r);
        var end = new Point(c + r * Math.Sin(angle), c - r * Math.Cos(angle));
        var figure = new PathFigure { StartPoint = start, IsClosed = false, IsFilled = false };
        figure.Segments.Add(new ArcSegment
        {
            Point = end,
            Size = new Size(r, r),
            IsLargeArc = fraction > 0.5,
            SweepDirection = SweepDirection.Clockwise,
        });
        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        return geometry;
    }
}

/// <summary>
/// The status pill (the Mac's StatusPill, AITable.swift): ring (or a check
/// when idle), the status line, the other passes as stage chips while any
/// runs, a detail line (why the GPU fell back, the OneDrive wait), the compute
/// badge, and the owner's buttons (Index anyway, Pause). The search footer and
/// Settings → Local search both show one.
/// </summary>
internal sealed class StatusPill
{
    private const string GlyphCheck = "";        // CheckMark
    private const string GlyphDoneCircle = "";   // Completed

    private readonly Look _look;
    private readonly StatusRing _ring;
    private readonly FontIcon _idle;
    private readonly TextBlock _text;
    private readonly StackPanel _stages;
    private readonly Stage[] _stageChips;
    private readonly TextBlock _detail;
    private readonly Border _badge;
    private readonly TextBlock _badgeText;
    private bool _active = true;

    public Border Root { get; }

    public StatusPill(Look look, params UIElement[] actions)
    {
        _look = look;
        _ring = new StatusRing(look, 18, 2.5);
        _idle = new FontIcon
        {
            Glyph = GlyphDoneCircle,
            FontFamily = Look.IconFont,
            FontSize = 16,
            Foreground = look[AddonColour.Body],
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        var mark = new Grid { Width = 18, VerticalAlignment = VerticalAlignment.Center };
        mark.Children.Add(_ring.Root);
        mark.Children.Add(_idle);

        _text = look.Text("", 13, AddonColour.Title, wrap: false);
        _stages = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Visibility = Visibility.Collapsed };
        _stageChips = new[] { new Stage(look, "Pictures"), new Stage(look, "Sound"), new Stage(look, "Speech") };
        foreach (Stage chip in _stageChips) _stages.Children.Add(chip.Root);
        _detail = look.Text("", 12);
        _detail.MaxLines = 2;
        _detail.TextTrimming = TextTrimming.CharacterEllipsis;
        _detail.Visibility = Visibility.Collapsed;
        var lines = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        lines.Children.Add(_text);
        lines.Children.Add(_stages);
        lines.Children.Add(_detail);

        _badgeText = look.Text("", 12, AddonColour.Body, wrap: false);
        _badge = new Border
        {
            Child = _badgeText,
            Padding = new Thickness(7, 2, 7, 2),
            CornerRadius = new CornerRadius(10),
            BorderBrush = look[AddonColour.Hairline],
            BorderThickness = new Thickness(1),
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };

        var row = new Grid { ColumnSpacing = 8 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(mark);
        Grid.SetColumn(lines, 1);
        row.Children.Add(lines);
        Grid.SetColumn(_badge, 2);
        row.Children.Add(_badge);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        foreach (UIElement a in actions) buttons.Children.Add(a);
        Grid.SetColumn(buttons, 3);
        row.Children.Add(buttons);

        Root = new Border
        {
            Child = row,
            Padding = new Thickness(12, 6, 8, 6),
            CornerRadius = new CornerRadius(18),
            Background = look.LiveTint(AddonColour.Body, 13),
        };
        AutomationProperties.SetName(Root, "Local search status");
        AutomationProperties.SetLiveSetting(_text, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
    }

    /// <summary>Whether the stage chips are showing (the owner's own sound line steps back then).</summary>
    public bool ShowsStages => _stages.Visibility == Visibility.Visible;

    /// <summary>The line's text, for a tooltip the owner adds (a later window's note).</summary>
    public TextBlock Text => _text;

    /// <summary>
    /// Whether the owner is on screen: the ring sweeps only then. A hidden
    /// search window or a closed Settings page leaves it still.
    /// </summary>
    public bool Active
    {
        get => _active;
        set
        {
            _active = value;
            if (!value) _ring.SetSpinning(false);
        }
    }

    public void ShowNotRunning()
    {
        _text.Text = "Local search is not running.";
        _ring.Root.Visibility = Visibility.Collapsed;
        _idle.Visibility = Visibility.Visible;
        _ring.SetSpinning(false);
        _stages.Visibility = Visibility.Collapsed;
        _detail.Visibility = Visibility.Collapsed;
        _badge.Visibility = Visibility.Collapsed;
    }

    public void Update(in MvAiStatus s)
    {
        _text.Text = Look.StatusLine(s);
        bool idle = s.State == MvAiState.Idle;
        _ring.Root.Visibility = idle ? Visibility.Collapsed : Visibility.Visible;
        _idle.Visibility = idle ? Visibility.Visible : Visibility.Collapsed;
        _ring.SetSpinning(_active && Look.Busy(s));
        // The ring follows the pass the line names.
        ulong pictures = PicturesDone(s);
        _ring.SetProgress(Phase(s) switch
        {
            1 => Fraction(s.SoundDone, s.SoundTotal),
            2 => Fraction(s.SpeechDone, s.SpeechTotal),
            _ => Fraction(pictures, s.AssetsTotal),
        });

        // While any pass runs, the others sit beside the line as stages.
        bool stages = !idle && (s.SoundTotal > 0 || s.SpeechTotal > 0) &&
                      (pictures < s.AssetsTotal || s.SoundDone < s.SoundTotal || s.SpeechDone < s.SpeechTotal);
        int shown = s.State == MvAiState.Indexing ? Phase(s) : -1;
        bool any = false;
        (ulong Done, ulong Total)[] passes = { (pictures, s.AssetsTotal), (s.SoundDone, s.SoundTotal), (s.SpeechDone, s.SpeechTotal) };
        for (int i = 0; i < 3; ++i)
        {
            bool on = stages && i != shown && passes[i].Total > 0;
            _stageChips[i].Root.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            if (on) _stageChips[i].Update(passes[i].Done, passes[i].Total);
            any |= on;
        }
        _stages.Visibility = any ? Visibility.Visible : Visibility.Collapsed;

        var notes = new List<string>(2);
        if (Look.FallbackReason(s) is string why) notes.Add(why);
        if (s.CloudFilesLeft > 0 && Look.CloudWait(s) is string wait) notes.Add(wait);
        _detail.Text = string.Join(" · ", notes);
        _detail.Visibility = notes.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

        _badgeText.Text = Look.ComputeBadge(s);
        _badge.Visibility = Visibility.Visible;
        ToolTipService.SetToolTip(_badge, s.ModelText.Length > 0 ? s.ModelText : null);
    }

    /// <summary>Pictures are handled once indexed, failed, unavailable, or waiting on OneDrive.</summary>
    private static ulong PicturesDone(in MvAiStatus s) =>
        s.AssetsDone + s.AssetsFailed + s.AssetsUnavailable + s.CloudFilesLeft;

    /// <summary>The pass running now (0 pictures, 1 sound, 2 speech): pictures first, then sound, then speech.</summary>
    private static int Phase(in MvAiStatus s) =>
        PicturesDone(s) < s.AssetsTotal ? 0 : s.SoundDone < s.SoundTotal ? 1 : s.SpeechDone < s.SpeechTotal ? 2 : 0;

    private static double Fraction(ulong done, ulong total) => total == 0 ? 0 : Math.Min(1, (double)done / total);

    /// <summary>"Sound 14%" with a small ring, or "Pictures ✓" when its pass is done.</summary>
    private sealed class Stage
    {
        private readonly string _name;
        private readonly StatusRing _ring;
        private readonly FontIcon _check;
        private readonly TextBlock _label;

        public StackPanel Root { get; }

        public Stage(Look look, string name)
        {
            _name = name;
            _ring = new StatusRing(look, 10, 1.5);
            _check = new FontIcon
            {
                Glyph = GlyphCheck,
                FontFamily = Look.IconFont,
                FontSize = 9,
                Foreground = look[AddonColour.Accent],
                VerticalAlignment = VerticalAlignment.Center,
            };
            _label = look.Text(name, 11.5, AddonColour.Body, wrap: false);
            _label.VerticalAlignment = VerticalAlignment.Center;
            Root = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Visibility = Visibility.Collapsed };
            Root.Children.Add(_ring.Root);
            Root.Children.Add(_check);
            Root.Children.Add(_label);
        }

        public void Update(ulong done, ulong total)
        {
            bool finished = total > 0 && done >= total;
            _ring.Root.Visibility = finished ? Visibility.Collapsed : Visibility.Visible;
            _check.Visibility = finished ? Visibility.Visible : Visibility.Collapsed;
            double fraction = total == 0 ? 0 : Math.Min(1, (double)done / total);
            _ring.SetProgress(fraction);
            CultureInfo c = CultureInfo.CurrentCulture;
            _label.Text = finished ? _name : $"{_name} {(int)Math.Floor(fraction * 100)}%";
            string said = finished ? $"{_name}: all {total.ToString("N0", c)} done"
                                   : $"{_name}: {done.ToString("N0", c)} of {total.ToString("N0", c)}";
            ToolTipService.SetToolTip(Root, said);
            AutomationProperties.SetName(Root, finished ? $"{_name} done" : said);
        }
    }
}
