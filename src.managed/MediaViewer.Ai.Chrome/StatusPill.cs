// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// A small ring (the Mac's AIProgressRing, AITable.swift): a faint track, the
/// running pass's progress as an accent arc from 12 o'clock and, while work
/// runs, a short dimmer arc that turns. Ellipse and Path arcs, not the WinUI
/// ProgressRing: that control has no default style in this island host and
/// fail-fasts when it enters the tree. The turn is a RotateTransform
/// storyboard (an independent, compositor-side animation) that runs only
/// while spinning and loaded, so a still or hidden ring costs nothing.
/// </summary>
internal sealed class StatusRing
{
    private readonly double _size;
    private readonly double _line;
    private readonly Microsoft.UI.Xaml.Shapes.Path _arc;
    private readonly Microsoft.UI.Xaml.Shapes.Path _spinner;
    private readonly RotateTransform _turn;
    private Storyboard? _spin;
    private double _fraction = -2;
    private bool _spinning;

    public Grid Root { get; }

    public StatusRing(Look look, double size, double line)
    {
        _size = size;
        _line = line;
        Root = new Grid { Width = size, Height = size, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false };
        Root.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse
        {
            Width = size - line,
            Height = size - line,
            Margin = new Thickness(line / 2),
            Stroke = look.LiveTint(AddonColour.Body, 46),
            StrokeThickness = line,
        });
        _arc = new Microsoft.UI.Xaml.Shapes.Path
        {
            Stroke = look[AddonColour.Accent],
            StrokeThickness = line,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        };
        Root.Children.Add(_arc);
        _turn = new RotateTransform { CenterX = size / 2, CenterY = size / 2 };
        _spinner = new Microsoft.UI.Xaml.Shapes.Path
        {
            Data = Arc(0.3),
            Stroke = look[AddonColour.Accent],
            Opacity = 0.55,
            StrokeThickness = line,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            RenderTransform = _turn,
            Visibility = Visibility.Collapsed,
        };
        Root.Children.Add(_spinner);
        Root.Loaded += (_, _) => Apply();
        Root.Unloaded += (_, _) => Stop();
        AutomationProperties.SetAccessibilityView(Root, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
    }

    /// <param name="fraction">0…1 for the arc; negative for none (a spinner only).</param>
    /// <param name="spinning">Whether the short arc turns (callers pass false under reduce motion).</param>
    public void Set(double fraction, bool spinning)
    {
        if (Math.Abs(fraction - _fraction) >= 0.001)
        {
            _fraction = fraction;
            // Nothing below a sliver, as the Mac's progress > 0.002.
            _arc.Data = fraction <= 0.002 ? null : Arc(Math.Min(1, fraction));
        }
        if (spinning == _spinning) return;
        _spinning = spinning;
        Apply();
    }

    private void Apply()
    {
        _spinner.Visibility = _spinning ? Visibility.Visible : Visibility.Collapsed;
        if (_spinning && Root.IsLoaded) Start();
        else Stop();
    }

    private void Start()
    {
        if (_spin is not null) return;
        try
        {
            var turn = new DoubleAnimation
            {
                From = 0,
                To = 360,
                Duration = new Duration(TimeSpan.FromMilliseconds(1400)),  // the Mac's 1.4 s turn
                RepeatBehavior = RepeatBehavior.Forever,
            };
            Storyboard.SetTarget(turn, _turn);
            Storyboard.SetTargetProperty(turn, "Angle");
            _spin = new Storyboard();
            _spin.Children.Add(turn);
            _spin.Begin();
        }
        catch (Exception ex)
        {
            // A still ring is better than a broken footer.
            System.Diagnostics.Debug.WriteLine(ex);
            _spin = null;
        }
    }

    private void Stop()
    {
        _spin?.Stop();
        _spin = null;
    }

    /// <summary>An arc from 12 o'clock, clockwise, over <paramref name="fraction"/> of the circle.</summary>
    private Geometry Arc(double fraction)
    {
        double r = (_size - _line) / 2;
        double c = _size / 2;
        if (fraction >= 0.999)
        {
            return new EllipseGeometry { Center = new Windows.Foundation.Point(c, c), RadiusX = r, RadiusY = r };
        }
        double angle = fraction * 2 * Math.PI;
        var figure = new PathFigure
        {
            StartPoint = new Windows.Foundation.Point(c, c - r),
            IsClosed = false,
            IsFilled = false,
        };
        figure.Segments.Add(new ArcSegment
        {
            Point = new Windows.Foundation.Point(c + r * Math.Sin(angle), c - r * Math.Cos(angle)),
            Size = new Windows.Foundation.Size(r, r),
            IsLargeArc = fraction > 0.5,
            SweepDirection = SweepDirection.Clockwise,
        });
        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        return geometry;
    }
}

/// <summary>
/// The status pill's contents (the Mac's StatusPill, AITable.swift), shared by
/// the search footer and Settings → Local search: the ring while work is in
/// hand (turning while it runs) or a check when idle, the status line, the
/// other passes as stage chips ("Pictures ✓", "Sound 14%"), and a detail line
/// for what the line does not say (a provider fallback, an upgrade, a full
/// index, a OneDrive wait).
/// </summary>
internal sealed class StatusPill
{
    private readonly Look _look;
    private readonly bool _inSettings;
    private readonly StatusRing _ring;
    private readonly FontIcon _done;
    private readonly TextBlock _text;
    private readonly StackPanel _stages;
    private readonly TextBlock _detail;
    private string _stagesKey = "";

    public FrameworkElement Root { get; }

    /// <summary>The status line itself, for a tooltip (a read-only window's note).</summary>
    public TextBlock Line => _text;

    /// <param name="inSettings">Settings says the compute and the OneDrive wait in rows of
    /// their own: the detail line leaves them out there.</param>
    public StatusPill(Look look, double textSize, AddonColour textRole, bool inSettings)
    {
        _look = look;
        _inSettings = inSettings;
        _ring = new StatusRing(look, 18, 2.5);
        _ring.Root.Visibility = Visibility.Collapsed;
        _done = new FontIcon
        {
            Glyph = "",  // Completed: the Mac's checkmark.circle
            FontFamily = Look.IconFont,
            FontSize = 16,
            Foreground = look[AddonColour.Body],
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        AutomationProperties.SetAccessibilityView(_done, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        var mark = new Grid { Width = 18, VerticalAlignment = VerticalAlignment.Center };
        mark.Children.Add(_ring.Root);
        mark.Children.Add(_done);

        _text = look.Text("", textSize, textRole, wrap: false);
        _stages = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Visibility = Visibility.Collapsed };
        _detail = look.Text("", 12, AddonColour.Body, wrap: true);
        _detail.MaxLines = 2;
        _detail.TextTrimming = TextTrimming.CharacterEllipsis;
        _detail.Visibility = Visibility.Collapsed;
        var lines = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        lines.Children.Add(_text);
        lines.Children.Add(_stages);
        lines.Children.Add(_detail);

        var row = new Grid { ColumnSpacing = 8 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.Children.Add(mark);
        Grid.SetColumn(lines, 1);
        row.Children.Add(lines);
        Root = row;
    }

    /// <summary>The engine is not answering: the words only.</summary>
    public void ShowNotRunning()
    {
        _text.Text = "Local search is not running.";
        _ring.Root.Visibility = Visibility.Collapsed;
        _ring.Set(-1, false);
        _done.Visibility = Visibility.Collapsed;
        ShowStages(new());
        ShowDetail("");
    }

    public void Show(in MvAiStatus s)
    {
        _text.Text = Look.StatusLine(s);
        bool idle = s.State == MvAiState.Idle;
        _ring.Root.Visibility = idle ? Visibility.Collapsed : Visibility.Visible;
        _done.Visibility = idle ? Visibility.Visible : Visibility.Collapsed;
        _ring.Set(idle ? -1 : Look.Progress(s), !idle && Look.Busy(s) && _look.Motion);
        ShowStages(Stages(s));
        ShowDetail(Detail(s));
    }

    // ---- stages ------------------------------------------------------------------------

    internal readonly record struct Stage(string Name, ulong Done, ulong Total)
    {
        public bool Finished => Done >= Total;
        public double Fraction => Total == 0 ? 1 : Math.Min(1, (double)Done / Total);
    }

    /// <summary>
    /// While any of pictures, sound and speech still runs, the passes beside
    /// the one the line names (the Mac's StatusLine.stages). Empty without
    /// sound work: pictures is then the only pass.
    /// </summary>
    internal static List<Stage> Stages(in MvAiStatus s)
    {
        var shown = new List<Stage>();
        if (s.SoundTotal == 0 && s.SpeechTotal == 0) return shown;
        if (s.State is MvAiState.Idle or MvAiState.Error) return shown;
        ulong pictures = Look.PicturesDone(s);
        if (pictures >= s.AssetsTotal && s.SoundDone >= s.SoundTotal && s.SpeechDone >= s.SpeechTotal) return shown;
        Look.Phase? running = s.State == MvAiState.Indexing ? Look.PhaseOf(s) : null;
        (Look.Phase Phase, Stage Stage)[] all =
        {
            (Look.Phase.Pictures, new Stage("Pictures", pictures, s.AssetsTotal)),
            (Look.Phase.Sound, new Stage("Sound", s.SoundDone, s.SoundTotal)),
            (Look.Phase.Speech, new Stage("Speech", s.SpeechDone, s.SpeechTotal)),
        };
        foreach ((Look.Phase phase, Stage stage) in all)
        {
            if (phase != running && stage.Total > 0) shown.Add(stage);
        }
        return shown;
    }

    private void ShowStages(List<Stage> stages)
    {
        // Rebuilt only when a chip's words change, not at every 4 Hz read.
        string key = string.Join("|", stages.Select(st => st.Finished ? st.Name : $"{st.Name}{Percent(st)}"));
        if (key == _stagesKey) return;
        _stagesKey = key;
        _stages.Children.Clear();
        foreach (Stage st in stages) _stages.Children.Add(Chip(st));
        _stages.Visibility = stages.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static int Percent(Stage st) => (int)Math.Floor(st.Fraction * 100);

    /// <summary>"Sound 14%" with a still ring, or "Pictures ✓".</summary>
    private FrameworkElement Chip(Stage st)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        if (st.Finished)
        {
            row.Children.Add(new FontIcon
            {
                Glyph = "",  // CheckMark
                FontFamily = Look.IconFont,
                FontSize = 10,
                Foreground = _look[AddonColour.Accent],
                VerticalAlignment = VerticalAlignment.Center,
            });
            row.Children.Add(_look.Text(st.Name, 11.5, AddonColour.Body, wrap: false));
        }
        else
        {
            var ring = new StatusRing(_look, 10, 1.5);
            ring.Set(st.Fraction, false);
            row.Children.Add(ring.Root);
            row.Children.Add(_look.Text($"{st.Name} {Percent(st)}%", 11.5, AddonColour.Body, wrap: false));
        }
        string n(ulong v) => v.ToString("N0", CultureInfo.CurrentCulture);
        ToolTipService.SetToolTip(row, st.Finished ? $"{st.Name}: all {n(st.Total)} done" : $"{st.Name}: {n(st.Done)} of {n(st.Total)}");
        AutomationProperties.SetName(row, st.Finished ? $"{st.Name} done" : $"{st.Name} {n(st.Done)} of {n(st.Total)}");
        return row;
    }

    // ---- detail ------------------------------------------------------------------------

    /// <summary>What the line does not say, joined with " · " (the Mac's StatusLine.detail).</summary>
    private string Detail(in MvAiStatus s)
    {
        var notes = new List<string>();
        string n(ulong v) => v.ToString("N0", CultureInfo.CurrentCulture);
        if (!_inSettings && Look.FallbackReason(s) is string why) notes.Add(why);
        // While indexing, the line itself names the upgrade.
        if (s.MigrateTotal > 0 && s.MigrateDone < s.MigrateTotal && s.State != MvAiState.Indexing)
        {
            notes.Add($"Upgrading search quality: {n(s.MigrateDone)} of {n(s.MigrateTotal)}. Searches use the current index until it finishes");
        }
        if ((s.Flags & MvAiStatus.FlagIndexFull) != 0) notes.Add("Index is full — raise the cap or remove a folder");
        if (!_inSettings && Look.CloudWait(s) is string wait) notes.Add(wait.TrimEnd('.'));
        return string.Join(" · ", notes);
    }

    private void ShowDetail(string detail)
    {
        if (_detail.Text == detail) return;
        _detail.Text = detail;
        _detail.Visibility = detail.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        ToolTipService.SetToolTip(_detail, detail.Length > 0 ? detail : null);  // two lines may cut it short
    }
}
