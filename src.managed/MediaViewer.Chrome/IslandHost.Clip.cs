// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 13 / 14 (plan/08): trim mode on the scrub bar, the clip tools flyout
/// (Ctrl+S on a clip) and the Jobs pane (Ctrl+J).
/// </summary>
/// <remarks>
/// Native owns trim mode (shell/trim_state.h) and submits every job; this file
/// draws what it pushes (<see cref="SetTrim"/>), turns a flyout choice into one
/// packed integer (<c>pack_clip_choice</c>), and lists the core's clip job queue
/// through the ABI (mediaviewer_clip.h). Nothing here reads a file or waits on a
/// job: the pane polls [no-block] calls on its UI timer. Keyboard-complete
/// without XY focus (it fail-fasts in these islands): the pane and the flyout
/// take the arrows themselves, Delete cancels the focused job, Enter reveals its
/// output, R retries, Esc returns to the canvas (plan/08 "Execution &amp; UX").
/// </remarks>
public static partial class IslandHost
{
    internal const int TrimArgsSize = 80;

    // ---- trim on the transport ------------------------------------------------

    private static bool _trimArmed;
    private static bool _trimIndexReady;
    private static bool _trimPreviewing;
    private static long _trimDuration;
    private static long _trimIn = -1, _trimOut = -1, _trimCutIn = -1, _trimCutOut = -1;
    private static long[] _trimKeyframes = Array.Empty<long>();
    private static string _trimLabelText = "";
    private static Microsoft.UI.Xaml.Controls.Canvas? _trimMarks;
    private static StackPanel? _trimBar;
    private static TextBlock? _trimLabel;

    private static readonly Color TrimAccent = ColorHelper.FromArgb(255, 240, 176, 64);
    private static readonly Color TrimKeep = ColorHelper.FromArgb(70, 240, 176, 64);
    private static readonly Color KeyframeTick = ColorHelper.FromArgb(160, 150, 154, 164);

    // The slider's track starts and ends about half a thumb in from its edges.
    private const double SeekWidth = 320;
    private const double SeekInset = 10;

    /// <summary>
    /// Native pushes trim state on every change (shell::chrome_trim_args,
    /// 80 bytes). Pointers are valid for the call only, so the keyframes and
    /// the label are copied here.
    /// </summary>
    public static int SetTrim(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < TrimArgsSize) return unchecked((int)0x80070057);
            _trimArmed = Marshal.ReadInt32(arg, 0) != 0;
            _trimIndexReady = Marshal.ReadInt32(arg, 4) != 0;
            _trimDuration = Marshal.ReadInt64(arg, 8);
            _trimIn = Marshal.ReadInt64(arg, 16);
            _trimOut = Marshal.ReadInt64(arg, 24);
            _trimCutIn = Marshal.ReadInt64(arg, 32);
            _trimCutOut = Marshal.ReadInt64(arg, 40);
            IntPtr keyframes = checked((IntPtr)Marshal.ReadInt64(arg, 48));
            int count = Marshal.ReadInt32(arg, 56);
            int labelLen = Marshal.ReadInt32(arg, 60);
            IntPtr label = checked((IntPtr)Marshal.ReadInt64(arg, 64));
            _trimPreviewing = Marshal.ReadInt32(arg, 72) != 0;
            if (count > 0 && keyframes != IntPtr.Zero)
            {
                _trimKeyframes = new long[count];
                Marshal.Copy(keyframes, _trimKeyframes, 0, count);
            }
            else
            {
                _trimKeyframes = Array.Empty<long>();
            }
            _trimLabelText = labelLen > 0 && label != IntPtr.Zero
                ? Marshal.PtrToStringUTF8(label, labelLen) ?? ""
                : "";
            RenderTrim();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>The scrubber with the trim overlay on top (BuildTransport).</summary>
    private static UIElement WrapSeekForTrim(Slider seek)
    {
        _trimMarks = new Microsoft.UI.Xaml.Controls.Canvas
        {
            Width = SeekWidth,
            IsHitTestVisible = false,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        var grid = new Grid { Width = SeekWidth, VerticalAlignment = VerticalAlignment.Center };
        grid.Children.Add(seek);
        grid.Children.Add(_trimMarks);
        return grid;
    }

    /// <summary>The label and the two save buttons, shown while trim is armed.</summary>
    private static UIElement BuildTrimBar()
    {
        _trimLabel = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = 12,
            Foreground = Brush(TrimAccent),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _trimBar = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        _trimBar.Children.Add(_trimLabel);
        // The keyed commands, for the mouse: Enter / Shift+Enter in trim mode.
        _trimBar.Children.Add(TextButton("Save (instant)", () => Send(Command.TrimKeyframe)));
        _trimBar.Children.Add(TextButton("Save exact (slower)", () => Send(Command.TrimReencode)));
        return _trimBar;
    }

    private static void DropTrimUi()
    {
        _trimMarks = null;
        _trimBar = null;
        _trimLabel = null;
    }

    private static double TrimX(long t)
    {
        if (_trimDuration <= 0) return SeekInset;
        double f = Math.Clamp((double)t / _trimDuration, 0, 1);
        return SeekInset + f * (SeekWidth - 2 * SeekInset);
    }

    private static void RenderTrim()
    {
        if (_trimBar is not null)
        {
            _trimBar.Visibility = _trimArmed ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_trimLabel is not null)
        {
            _trimLabel.Text = _trimPreviewing ? _trimLabelText + " · previewing" : _trimLabelText;
        }
        if (_trimMarks is null) return;
        _trimMarks.Children.Clear();
        if (!_trimArmed || _trimDuration <= 0) return;
        double h = _trimMarks.ActualHeight > 1 ? _trimMarks.ActualHeight : 32;

        // What Path 1 keeps, shaded; the grid under it so snapping is visible
        // before it happens (plan/08: "show the keyframe grid on the timeline").
        if (_trimCutIn >= 0 && _trimCutOut > _trimCutIn)
        {
            double x0 = TrimX(_trimCutIn), x1 = TrimX(_trimCutOut);
            var keep = new Rectangle { Width = Math.Max(1, x1 - x0), Height = h, Fill = Brush(TrimKeep) };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(keep, x0);
            _trimMarks.Children.Add(keep);
        }
        if (_trimIndexReady)
        {
            double last = -10;
            foreach (long k in _trimKeyframes)
            {
                double x = TrimX(k);
                if (x - last < 2) continue;  // one tick per two pixels on a long clip
                last = x;
                var tick = new Rectangle { Width = 1, Height = 6, Fill = Brush(KeyframeTick) };
                Microsoft.UI.Xaml.Controls.Canvas.SetLeft(tick, x);
                Microsoft.UI.Xaml.Controls.Canvas.SetTop(tick, h - 6);
                _trimMarks.Children.Add(tick);
            }
        }
        foreach (long marker in new[] { _trimIn, _trimOut })
        {
            if (marker < 0) continue;
            var line = new Rectangle { Width = 2, Height = h, Fill = Brush(TrimAccent) };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(line, TrimX(marker) - 1);
            _trimMarks.Children.Add(line);
        }
    }

    // ---- the clip tools flyout (Ctrl+S on a clip) --------------------------------

    // Mirrors shell/trim_state.h: kClipFlag* and pack_clip_choice (op | option << 8).
    private const int ClipFlagHasRange = 1;
    private const int ClipFlagHasAudio = 2;

    private sealed record ClipTool(string Name, int Op, int Option, int Needs);

    private static readonly ClipTool[] ClipTools =
    {
        new("Rotate 90° right (lossless)", (int)MvClipOp.Rotate, 1, 0),
        new("Rotate 90° left (lossless)", (int)MvClipOp.Rotate, 2, 0),
        new("Rotate 180° (lossless)", (int)MvClipOp.Rotate, 3, 0),
        new("Split at the playhead", (int)MvClipOp.Split, 0, 0),
        new("Save this frame as PNG", (int)MvClipOp.Frame, 1, 0),
        new("Save this frame as JPEG", (int)MvClipOp.Frame, 2, 0),
        new("Extract audio (copy, no re-encode)", (int)MvClipOp.Audio, 1, ClipFlagHasAudio),
        new("Extract audio as WAV", (int)MvClipOp.Audio, 2, ClipFlagHasAudio),
        new("Extract audio as FLAC", (int)MvClipOp.Audio, 3, ClipFlagHasAudio),
        new("Convert to MP4 (remux, no re-encode)", (int)MvClipOp.Remux, 1, 0),
        new("Convert to MKV (remux, no re-encode)", (int)MvClipOp.Remux, 2, 0),
        new("GIF of in–out (or the next 5 s)", (int)MvClipOp.Animation, 1, 0),
        new("WebP of in–out (or the next 5 s)", (int)MvClipOp.Animation, 2, 0),
        new("Trim to in–out (keyframe, instant)", (int)MvClipOp.TrimKeyframe, 0, ClipFlagHasRange),
        new("Trim to in–out (re-encode, frame-accurate, slower)", (int)MvClipOp.TrimReencode, 0, ClipFlagHasRange),
        new("Remove in–out", (int)MvClipOp.RemoveMiddle, 0, ClipFlagHasRange),
    };

    private static UIElement BuildClipTools(int flags, out UIElement focusTarget)
    {
        var panel = new StackPanel { Spacing = 2, Margin = new Thickness(12), IsTabStop = true, MinWidth = 380 };
        panel.Children.Add(Label("Clip tools", UiFontSize + 2));
        panel.Children.Add(Label("New files beside the clip; the original is never changed.", UiFontSize, mute: true));
        var rows = new TextBlock[ClipTools.Length];
        bool Enabled(int i) => (ClipTools[i].Needs & flags) == ClipTools[i].Needs;
        int row = 0;
        while (row < ClipTools.Length && !Enabled(row)) ++row;

        void Refresh()
        {
            for (int i = 0; i < rows.Length; ++i)
            {
                rows[i].Text = (i == row ? "▶ " : "  ") + ClipTools[i].Name;
                rows[i].Foreground = Brush(!Enabled(i) ? Hairline : i == row ? Title : Body);
            }
        }
        void Move(int delta)
        {
            for (int n = 0; n < ClipTools.Length; ++n)
            {
                row = (row + delta + ClipTools.Length) % ClipTools.Length;
                if (Enabled(row)) break;
            }
            Refresh();
        }
        void Run(int i)
        {
            if (!Enabled(i)) return;
            int packed = ClipTools[i].Op | (ClipTools[i].Option << 8);
            ClosePopup();
            Send(Command.ClipTool, packed);
        }

        for (int i = 0; i < ClipTools.Length; ++i)
        {
            rows[i] = Label("", UiFontSize);
            int captured = i;
            var b = FlatButton(rows[i], new Thickness(0, 1, 0, 1), tabStop: false);
            b.IsEnabled = Enabled(i);
            b.Click += (_, _) => Run(captured);
            panel.Children.Add(b);
        }
        panel.Children.Add(Label("↑ ↓ choose   Enter run   Esc cancel   ·   jobs: Ctrl+J", UiFontSize, mute: true));
        Refresh();
        panel.KeyDown += (_, e) =>
        {
            switch (e.Key)
            {
                case Windows.System.VirtualKey.Up: Move(-1); e.Handled = true; break;
                case Windows.System.VirtualKey.Down: Move(1); e.Handled = true; break;
                case Windows.System.VirtualKey.Enter: Run(row); e.Handled = true; break;
            }
        };
        focusTarget = panel;
        return panel;
    }

    // ---- the Jobs pane (Ctrl+J) ------------------------------------------------------

    private static DesktopWindowXamlSource? _jobsPane;
    private static bool _jobsPaneVisible;
    private static DispatcherTimer? _jobsTimer;
    private static StackPanel? _jobsList;
    private static TextBlock? _jobsEmpty;
    private static readonly List<JobRow> _jobRows = new();
    private static int _jobCursor;

    private sealed class JobRow
    {
        public ulong Id;
        public MvClipJobState State;
        public Border Root = null!;
        public TextBlock Title = null!;
        public TextBlock Status = null!;
        public ProgressBar Bar = null!;
    }

    public static int ShowJobsPane(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _jobsPane, BuildJobsPane,
        onShown: () =>
        {
            _jobsPaneVisible = true;
            _jobsTimer ??= new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
            _jobsTimer.Tick -= OnJobsTick;
            _jobsTimer.Tick += OnJobsTick;
            _jobsTimer.Start();
            RefreshJobs();
        },
        onHidden: () =>
        {
            _jobsPaneVisible = false;
            _jobsTimer?.Stop();
            DropJobsUi();
        });

    private static void OnJobsTick(object? sender, object e) => RefreshJobs();

    private static void DropJobsUi()
    {
        _jobsList = null;
        _jobsEmpty = null;
        _jobRows.Clear();
    }

    private static UIElement BuildJobsPane()
    {
        Grid root = PanelShell("Jobs", Command.JobsPane, out Grid body);
        var col = new StackPanel { Spacing = 8, Padding = new Thickness(14, 4, 14, 14), IsTabStop = true };
        _jobsEmpty = Text("No clip jobs. On a clip: Ctrl+T trims, Ctrl+S opens the clip tools.", Body);
        col.Children.Add(_jobsEmpty);
        _jobsList = new StackPanel { Spacing = 6 };
        col.Children.Add(_jobsList);
        var clear = new Button { Content = Text("Clear finished", Title), IsTabStop = false };
        clear.Click += (_, _) =>
        {
            _folderSession?.ClipClearFinished();
            RefreshJobs();
        };
        col.Children.Add(clear);
        col.Children.Add(Text("↑ ↓ choose · Delete cancel · R retry · Enter show in Explorer · Esc back",
                              Body, UiFontSize - 3));
        col.KeyDown += OnJobsKey;
        body.Children.Add(new ScrollViewer { Content = col, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        return root;
    }

    private static void OnJobsKey(object sender, KeyRoutedEventArgs e)
    {
        if (_jobRows.Count == 0) return;
        switch (e.Key)
        {
            case Windows.System.VirtualKey.Up:
                _jobCursor = Math.Max(0, _jobCursor - 1);
                break;
            case Windows.System.VirtualKey.Down:
                _jobCursor = Math.Min(_jobRows.Count - 1, _jobCursor + 1);
                break;
            case Windows.System.VirtualKey.Delete:
                _folderSession?.ClipCancel(_jobRows[_jobCursor].Id);
                break;
            case Windows.System.VirtualKey.R:
                _folderSession?.ClipRetry(_jobRows[_jobCursor].Id);
                break;
            case Windows.System.VirtualKey.Enter:
                RevealJob(_jobRows[_jobCursor].Id);
                break;
            default:
                return;
        }
        e.Handled = true;
        RefreshJobs();
        _jobRows[Math.Min(_jobCursor, _jobRows.Count - 1)].Root.StartBringIntoView();
    }

    // Explorer, the output selected (plan/08: "reveal in Explorer").
    private static void RevealJob(ulong id)
    {
        if (_folderSession is null) return;
        string path = _folderSession.ClipJobOutput(id, 0);
        if (path.Length == 0) return;
        try
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = false });
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    // The drain saw a clip completion (IslandHost.Filmstrip.cs DrainFolder).
    private static void OnClipCompletion(MvCompletion c)
    {
        if (c.Kind == MvCompletionKind.ClipIndex)
        {
            // Native reads the keyframes itself; ids are small, so a float is exact.
            float id = c.JobId;
            _dispatcher?.DispatcherQueue.TryEnqueue(() => Send(Command.ClipIndex, id));
        }
        else if (c.Kind == MvCompletionKind.ClipJob && _jobsPaneVisible)
        {
            RefreshJobs();
        }
    }

    private static string StatusOf(in MvClipProgress p)
    {
        static string Clock(long ms) => ms >= 3_600_000
            ? TimeSpan.FromMilliseconds(ms).ToString(@"h\:mm\:ss")
            : TimeSpan.FromMilliseconds(ms).ToString(@"m\:ss");
        return p.State switch
        {
            MvClipJobState.Queued => "Waiting",
            MvClipJobState.Running => p.EtaMs >= 0
                ? $"{p.Fraction * 100:0} % · {Clock(p.EtaMs)} left"
                : $"{p.Fraction * 100:0} %",
            MvClipJobState.Done => p.OutputCount > 1 ? $"Done in {Clock(p.ElapsedMs)} · {p.OutputCount} files" : $"Done in {Clock(p.ElapsedMs)}",
            MvClipJobState.Cancelled => "Cancelled · nothing written",
            _ => p.Error switch
            {
                MvStatus.UnsupportedFormat when p.Op == MvClipOp.TrimReencode =>
                    "Failed · no hardware encoder here; use the instant trim",
                MvStatus.UnsupportedFormat => "Failed · this clip or container cannot do that",
                MvStatus.InvalidArg => "Failed · the range is empty or too long",
                MvStatus.Io => "Failed · could not write beside the clip",
                _ => "Failed",
            },
        };
    }

    private static void RefreshJobs()
    {
        if (_jobsList is null || _folderSession is null) return;
        ulong[] ids;
        try
        {
            ids = _folderSession.ClipJobs();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return;
        }
        // Newest first; rows are kept, not rebuilt, so the list does not flicker.
        Array.Reverse(ids);
        if (_jobRows.Count != ids.Length || _jobRows.Where((r, i) => r.Id != ids[i]).Any())
        {
            _jobsList.Children.Clear();
            var kept = _jobRows.ToDictionary(r => r.Id);
            _jobRows.Clear();
            foreach (ulong id in ids)
            {
                if (!kept.TryGetValue(id, out JobRow? row)) row = MakeJobRow(id);
                _jobRows.Add(row);
                _jobsList.Children.Add(row.Root);
            }
        }
        if (_jobsEmpty is not null) _jobsEmpty.Visibility = ids.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
        _jobCursor = Math.Clamp(_jobCursor, 0, Math.Max(0, _jobRows.Count - 1));
        for (int i = 0; i < _jobRows.Count; ++i)
        {
            JobRow row = _jobRows[i];
            if (!_folderSession.TryClipProgress(row.Id, out MvClipProgress p)) continue;
            row.State = p.State;
            row.Title.Text = $"{p.TitleText} — {p.SourceNameText}";
            row.Status.Text = StatusOf(p);
            row.Bar.Value = p.Fraction;
            row.Bar.IsIndeterminate = p.State == MvClipJobState.Running && p.Fraction <= 0;
            row.Bar.Visibility = p.State is MvClipJobState.Running or MvClipJobState.Queued
                ? Visibility.Visible : Visibility.Collapsed;
            row.Root.BorderBrush = Brush(i == _jobCursor ? TrimAccent : Hairline);
        }
    }

    private static JobRow MakeJobRow(ulong id)
    {
        var row = new JobRow { Id = id };
        row.Title = Text("", Title, UiFontSize - 2, maxLines: 2);
        row.Status = Text("", Body, UiFontSize - 3);
        row.Bar = new ProgressBar { Minimum = 0, Maximum = 1, Height = 4 };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        buttons.Children.Add(TextButton("Cancel", () => { _folderSession?.ClipCancel(id); RefreshJobs(); }));
        buttons.Children.Add(TextButton("Retry", () => { _folderSession?.ClipRetry(id); RefreshJobs(); }));
        buttons.Children.Add(TextButton("Show in Explorer", () => RevealJob(id)));
        var inner = new StackPanel { Spacing = 3 };
        inner.Children.Add(row.Title);
        inner.Children.Add(row.Bar);
        inner.Children.Add(row.Status);
        inner.Children.Add(buttons);
        row.Root = new Border
        {
            Child = inner,
            BorderThickness = new Thickness(1),
            BorderBrush = Brush(Hairline),
            Padding = new Thickness(8, 6, 8, 6),
            CornerRadius = new CornerRadius(4),
        };
        return row;
    }
}
