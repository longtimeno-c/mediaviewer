// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
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
        _hintNoticeShown = true;
        PlaceHintNotice();
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
        var grid = new Grid { ColumnSpacing = 8 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _hintNoticeText = new TextBlock
        {
            Text = text,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            TextWrapping = TextWrapping.Wrap,
            VerticalAlignment = VerticalAlignment.Center,
        };
        grid.Children.Add(_hintNoticeText);
        Button gotIt = TextButton("Got it", HintNoticeLearned);
        ToolTipService.SetToolTip(gotIt, "Don't show this again");
        gotIt.IsTabStop = true;
        gotIt.AllowFocusOnInteraction = true;
        Grid.SetColumn(gotIt, 1);
        grid.Children.Add(gotIt);
        var card = new Border
        {
            Width = HintNoticeWidth,
            Padding = new Thickness(14, 8, 6, 8),
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(1),
            BorderBrush = Brush(Hairline),
            Background = Brush(ChromeColour.Surface),
            Child = grid,
        };
        AutomationProperties.SetName(card, text);
        return card;
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
        try { if (_hintNotice is not null) MoveAt(_hintNotice, 0, ClientHeightPx(), 1, 1); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        if (restoreFocus) RestoreCanvasFocus();
    }

    /// <summary>Detach (the window is going): no island outlives it.</summary>
    private static void DropHintNotice()
    {
        _hintNoticeTimer?.Stop();
        _hintNoticeShown = false;
        _hintNoticeCard = null;
        _hintNoticeText = null;
        if (_hintNotice is not null) _hintNotice.TakeFocusRequested -= OnTakeFocusRequested;
        DisposeSource(ref _hintNotice);
    }
}
