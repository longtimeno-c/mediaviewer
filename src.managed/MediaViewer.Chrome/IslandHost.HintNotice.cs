// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;

namespace MediaViewer.Chrome;

/// <summary>
/// The "Press T" filmstrip hint (shell/filmstrip_hint.h, docs/design/16
/// "Filmstrip hint") as a small card in the window's bottom-right corner, above
/// the filmstrip: the resume notice's place and look (IslandHost.ResumeNotice.cs),
/// so it is seen, where the title's status line was not. Native decides when it
/// is due and sends the text (ShowHint); the card fades after eight seconds,
/// and "Got it" learns the hint for good (Command.HintLearned), as T does.
/// </summary>
/// <remarks>
/// A small island of its own over the canvas corner, as the resume notice and
/// the AI badge are. It was a windowed <c>Popup</c> of the bar island, the
/// resume notice's old build, which never showed on Windows (issue #446). The
/// island is made on the first show and parked off the client area when the
/// card fades, not disposed: native shows and hides it as items change. It
/// does not take focus, so the viewer keeps the keyboard, and stacks above the
/// resume notice while that is up. The twin of the Mac's HintNoticeView.swift.
/// </remarks>
public static partial class IslandHost
{
    private const double HintNoticeWidth = 300;
    private static readonly TimeSpan HintNoticeLife = TimeSpan.FromSeconds(8);

    private static DesktopWindowXamlSource? _hintNotice;
    private static FrameworkElement? _hintNoticeCard;
    // The card's words and button: what fades in and out (NoticeCard below).
    private static FrameworkElement? _hintNoticeContent;
    private static TextBlock? _hintNoticeText;
    private static DispatcherQueueTimer? _hintNoticeTimer;
    // The card is up (placed in the corner), not parked.
    private static bool _hintNoticeShown;

    /// <summary>Native: show the hint's text for a few seconds, or hide the card
    /// when it is empty. In: ChromeTableArgs, UTF-8.</summary>
    public static int ShowHint(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < TableArgsSize) return unchecked((int)0x80070057);
            ChromeTableArgs a = Marshal.PtrToStructure<ChromeTableArgs>(arg);
            string text = a.Utf8 == 0 || a.Length <= 0
                ? ""
                : Marshal.PtrToStringUTF8(checked((IntPtr)a.Utf8), a.Length) ?? "";
            if (text.Length == 0) CloseHintNotice();
            else OpenHintNotice(text);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            CloseHintNotice();
            return unchecked((int)0x80004005);
        }
    }

    private static void OpenHintNotice(string text)
    {
        if (_themeWindow == IntPtr.Zero || _source?.Content is not FrameworkElement) return;
        bool fresh = !_hintNoticeShown || _hintNoticeText?.Text != text;
        if (_hintNotice is null)
        {
            _hintNotice = new DesktopWindowXamlSource();
            _hintNotice.Initialize(Microsoft.UI.Win32Interop.GetWindowIdFromWindow(_themeWindow));
            _hintNotice.TakeFocusRequested += OnTakeFocusRequested;
            // Parked off the client area until it is placed: a default
            // full-client island would flash over the canvas.
            MoveAt(_hintNotice, 0, ClientHeightPx(), 1, 1);
            _hintNoticeCard = BuildHintNoticeCard(text);
            // Measured for real once it is in the island's live tree: placed again then.
            _hintNoticeCard.Loaded += (_, _) => Guarded(PlaceHintNotice);
            // Behind the card's rounded corners, the canvas colour (the AI badge's).
            _hintNotice.Content = new Grid { Background = Brush(Canvas), Children = { _hintNoticeCard } };
        }
        else if (_hintNoticeText is not null)
        {
            _hintNoticeText.Text = text;
            AutomationProperties.SetName(_hintNoticeCard!, text);
        }
        bool wasShown = _hintNoticeShown;
        _hintNoticeShown = true;
        PlaceHintNotice();
        if (!wasShown && _hintNoticeContent is not null) FadeNoticeContent(_hintNoticeContent, show: true);
        if (fresh)
        {
            // Said once as it appears, not only when focused.
            FrameworkElementAutomationPeer.FromElement(_hintNoticeCard!)?.RaiseNotificationEvent(
                AutomationNotificationKind.Other, AutomationNotificationProcessing.ImportantMostRecent,
                text, "filmstrip-hint");
        }
        // Each show (a document arriving, the wheel zooming one) starts the life again.
        if (_dispatcher is null) return;
        _hintNoticeTimer ??= _dispatcher.DispatcherQueue.CreateTimer();
        _hintNoticeTimer.Interval = HintNoticeLife;
        _hintNoticeTimer.IsRepeating = false;
        _hintNoticeTimer.Tick -= OnHintNoticeExpired;
        _hintNoticeTimer.Tick += OnHintNoticeExpired;
        _hintNoticeTimer.Stop();
        _hintNoticeTimer.Start();
    }

    private static void OnHintNoticeExpired(DispatcherQueueTimer sender, object args) => CloseHintNotice();

    private static FrameworkElement BuildHintNoticeCard(string text)
    {
        // The corner cards' look (NoticeCard below): the words and the button
        // 12 apart, even padding all round.
        var grid = new Grid { ColumnSpacing = Space(12) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _hintNoticeText = new TextBlock
        {
            Text = text,
            FontFamily = UiFont,
            FontSize = TypeSize(TypeSmall),
            LineHeight = Math.Round(TypeSize(TypeSmall) * 1.35),
            Foreground = Brush(Title),
            TextWrapping = TextWrapping.Wrap,
            VerticalAlignment = VerticalAlignment.Center,
        };
        grid.Children.Add(_hintNoticeText);
        Button gotIt = NoticeButton("Got it", HintNoticeLearned);
        ToolTipService.SetToolTip(gotIt, "Don't show this again");
        Grid.SetColumn(gotIt, 1);
        grid.Children.Add(gotIt);
        _hintNoticeContent = grid;
        Border card = NoticeCardBorder(HintNoticeWidth, grid);
        AutomationProperties.SetName(card, text);
        return card;
    }

    // ---- the corner cards' one look ------------------------------------------------
    // The hint, resume and restart cards, after docs/design/20 "Craft rules", the
    // twin of the Mac's NoticeCard (NoticeStore.swift): one surface with a hairline
    // where it meets the photo, words 12 in from every edge, a title 4 over its
    // line, the buttons 12 under it (one neutral, the action filled with the
    // accent), and a short entrance and exit instead of a pop. The island is a
    // rectangle of the canvas colour and cannot fade over the swapchain, so
    // what moves is the card's content: its words and buttons fade in with 6 DIP
    // of rise as it appears, and out before it is parked (150 ms). Animations off
    // cut. Nothing runs once a card has settled.

    private const double NoticeFadeMs = 150;
    private const double NoticeRise = 6;
    private static readonly Dictionary<FrameworkElement, Storyboard> NoticeFades = new();

    private static Border NoticeCardBorder(double width, UIElement content) => new()
    {
        Width = width,
        Padding = Pad(12),
        CornerRadius = Corner(RadiusFloating),
        BorderThickness = new Thickness(1),
        BorderBrush = Brush(Hairline),
        Background = Brush(ChromeColour.Surface),
        Child = content,
    };

    /// <summary>A card's title over its line: 14 and 12, 4 apart.</summary>
    private static StackPanel NoticeWords(TextBlock title, TextBlock line)
    {
        title.FontFamily = UiFont;
        title.FontSize = TypeSize(TypeBody);
        title.Foreground = Brush(Title);
        title.TextWrapping = TextWrapping.Wrap;
        line.FontFamily = UiFont;
        line.FontSize = TypeSize(TypeCaption);
        line.LineHeight = Math.Round(TypeSize(TypeCaption) * 1.35);  // small text: at least 1.3
        line.Foreground = Brush(Body);
        line.TextWrapping = TextWrapping.Wrap;
        var words = new StackPanel { Spacing = Space(4) };
        words.Children.Add(title);
        words.Children.Add(line);
        return words;
    }

    /// <summary>A card's buttons, right-aligned 8 apart.</summary>
    private static StackPanel NoticeButtons(params Button[] buttons)
    {
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = Space(8),
            HorizontalAlignment = HorizontalAlignment.Right,
        };
        foreach (Button b in buttons) row.Children.Add(b);
        return row;
    }

    /// <summary>A card's button: a surface at rest (Settings' fill); the
    /// action is filled with the accent. 13 pt, the Mac's NoticeButtonStyle.</summary>
    private static Button NoticeButton(string label, Action click, bool primary = false)
    {
        double size = TypeSize(TypeSmall);
        Button b = ChromeButton(ButtonLabel(label, size), Pad(12, 5, 12, 5), click, fontSize: size);
        b.IsTabStop = true;
        b.AllowFocusOnInteraction = true;
        FillSettingsButton(b, primary);
        b.Padding = Pad(12, 5, 12, 5);
        return b;
    }

    /// <summary>Fades a card's content in (from 6 DIP below) or out, then runs
    /// <paramref name="done"/>; at once with animations off. A fade already
    /// running carries on from where it is.</summary>
    private static void FadeNoticeContent(FrameworkElement content, bool show, Action? done = null)
    {
        if (content.RenderTransform is not TranslateTransform rise)
        {
            rise = new TranslateTransform();
            content.RenderTransform = rise;
        }
        if (NoticeFades.Remove(content, out Storyboard? running))
        {
            // Hold where it got to: the stopped board would put the old values back.
            double opacity = content.Opacity, y = rise.Y;
            running.Stop();
            content.Opacity = opacity;
            rise.Y = y;
        }
        else if (show && content.Opacity >= 1)
        {
            content.Opacity = 0;  // a fresh card: from nothing
            rise.Y = NoticeRise;
        }
        double toOpacity = show ? 1 : 0, toY = show ? 0 : NoticeRise;
        if (!HostAnimationsEnabled())
        {
            content.Opacity = toOpacity;
            rise.Y = 0;
            if (done is not null) Guarded(done);
            return;
        }
        var duration = new Duration(TimeSpan.FromMilliseconds(NoticeFadeMs));
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };
        var fade = new DoubleAnimation { To = toOpacity, Duration = duration, EasingFunction = ease };
        Storyboard.SetTarget(fade, content);
        Storyboard.SetTargetProperty(fade, "Opacity");
        var slide = new DoubleAnimation { To = toY, Duration = duration, EasingFunction = ease };
        Storyboard.SetTarget(slide, rise);
        Storyboard.SetTargetProperty(slide, "Y");
        var board = new Storyboard();
        board.Children.Add(fade);
        board.Children.Add(slide);
        board.Completed += (_, _) =>
        {
            if (!NoticeFades.TryGetValue(content, out Storyboard? current) || !ReferenceEquals(current, board)) return;
            NoticeFades.Remove(content);
            // Settled: the end values become the element's own, and nothing holds an animation.
            board.Stop();
            content.Opacity = toOpacity;
            rise.Y = toY;
            if (done is not null) Guarded(done);
        };
        NoticeFades[content] = board;
        board.Begin();
    }

    /// <summary>The card's bottom-right corner on the window's, above the
    /// filmstrip when it is up and above the resume notice while that is open
    /// (PlaceResumeNotice's maths). The island is a child of the main window,
    /// so its rectangle is client pixels.</summary>
    private static void PlaceHintNotice()
    {
        if (_hintNotice is null || _hintNoticeCard is null || !_hintNoticeShown) return;
        if (_themeWindow == IntPtr.Zero || !GetClientRect(_themeWindow, out NativeRect client)) return;
        uint dpi = GetDpiForWindow(_themeWindow);
        double scale = dpi > 0 ? dpi / 96.0 : 1;
        _hintNoticeCard.Measure(new Size(HintNoticeWidth, double.PositiveInfinity));
        double height = _hintNoticeCard.DesiredSize.Height;
        if (height <= 0) height = 40;  // not measured yet: about one line and the button
        int width = (int)Math.Ceiling(HintNoticeWidth * scale);
        int tall = (int)Math.Ceiling(height * scale);
        int margin = (int)Math.Round(ResumeNoticeMargin * scale);
        int clientWidth = client.Right - client.Left;
        int bottom = (_filmstripTop > 0 ? _filmstripTop : client.Bottom - client.Top) - margin;
        if (_resumeNotice is not null && _resumeNoticeCard is not null)
        {
            bottom -= (int)Math.Ceiling(_resumeNoticeCard.DesiredSize.Height * scale) + margin;
        }
        bottom -= RestartNoticeTakesPx(scale, margin);  // #510: above the restart card too
        MoveAt(_hintNotice, Math.Max(0, clientWidth - width - margin), Math.Max(0, bottom - tall), width, tall);
        // Above the gallery and the panes, as the resume notice is.
        try { _hintNotice.SiteBridge?.MoveInZOrderAtTop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    private static void HintNoticeLearned()
    {
        CloseHintNotice(restoreFocus: true);
        Send(Command.HintLearned);
    }

    // Parked, not disposed (the next item may show it again). Only the click
    // hands the keyboard back: fading or native hiding it never moves focus
    // from wherever the user has it.
    private static void CloseHintNotice(bool restoreFocus = false)
    {
        _hintNoticeTimer?.Stop();
        if (!_hintNoticeShown) return;
        _hintNoticeShown = false;
        if (restoreFocus) RestoreCanvasFocus();
        DesktopWindowXamlSource? island = _hintNotice;
        void Park()
        {
            // Shown again meanwhile, or the window went: leave it.
            if (_hintNoticeShown || island is null || !ReferenceEquals(island, _hintNotice)) return;
            try { MoveAt(island, 0, ClientHeightPx(), 1, 1); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        }
        // Its words fade out, then the card is parked (the top of the corner's
        // stack: nothing moves for it).
        if (_hintNoticeContent is not null) FadeNoticeContent(_hintNoticeContent, show: false, Park);
        else Park();
    }

    /// <summary>Detach (the window is going): no island outlives it.</summary>
    private static void DropHintNotice()
    {
        _hintNoticeTimer?.Stop();
        _hintNoticeShown = false;
        _hintNoticeCard = null;
        _hintNoticeContent = null;
        _hintNoticeText = null;
        NoticeFades.Clear();
        if (_hintNotice is not null) _hintNotice.TakeFocusRequested -= OnTakeFocusRequested;
        DisposeSource(ref _hintNotice);
    }
}
