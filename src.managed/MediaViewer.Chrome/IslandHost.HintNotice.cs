// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
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
/// A windowed <see cref="Popup"/> of the bar island, as the resume notice is:
/// not light-dismissed, never takes focus, so the viewer keeps the keyboard. It
/// stacks above the resume notice while that is up. The twin of the Mac's
/// HintNoticeView.swift.
/// </remarks>
public static partial class IslandHost
{
    private const double HintNoticeWidth = 300;
    private static readonly TimeSpan HintNoticeLife = TimeSpan.FromSeconds(8);

    private static Popup? _hintNotice;
    private static FrameworkElement? _hintNoticeCard;
    private static TextBlock? _hintNoticeText;
    private static DispatcherQueueTimer? _hintNoticeTimer;

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
        if (_source?.Content is not FrameworkElement bar || bar.XamlRoot is null) return;
        bool fresh = _hintNotice is null || _hintNoticeText?.Text != text;
        if (_hintNotice is null)
        {
            _hintNoticeCard = BuildHintNoticeCard(text);
            _hintNotice = new Popup
            {
                Child = _hintNoticeCard,
                XamlRoot = bar.XamlRoot,
                ShouldConstrainToRootBounds = false,
                IsLightDismissEnabled = false,
            };
        }
        else if (_hintNoticeText is not null)
        {
            _hintNoticeText.Text = text;
            AutomationProperties.SetName(_hintNoticeCard!, text);
        }
        PlaceHintNotice();
        _hintNotice.IsOpen = true;
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
    /// (PlaceResumeNotice's maths).</summary>
    private static void PlaceHintNotice()
    {
        if (_hintNotice is null || _hintNoticeCard is null || _hintNotice.XamlRoot is null) return;
        if (_themeWindow == IntPtr.Zero || !GetClientRect(_themeWindow, out NativeRect client)) return;
        double scale = _hintNotice.XamlRoot.RasterizationScale;
        if (scale <= 0) scale = 1;
        _hintNoticeCard.Measure(new Size(HintNoticeWidth, double.PositiveInfinity));
        double height = _hintNoticeCard.DesiredSize.Height;
        int bottomPx = _filmstripTop > 0 ? _filmstripTop : client.Bottom - client.Top;
        double bottom = (bottomPx - _barTop) / scale - ResumeNoticeMargin;
        if (_resumeNotice is not null && _resumeNoticeCard is not null)
        {
            bottom -= _resumeNoticeCard.DesiredSize.Height + ResumeNoticeMargin;
        }
        _hintNotice.HorizontalOffset = Math.Max(0, (client.Right - client.Left) / scale - HintNoticeWidth - ResumeNoticeMargin);
        _hintNotice.VerticalOffset = Math.Max(0, bottom - height);
    }

    private static void HintNoticeLearned()
    {
        CloseHintNotice(restoreFocus: true);
        Send(Command.HintLearned);
    }

    // Only the click hands the keyboard back: fading or native hiding it never
    // moves focus from wherever the user has it.
    private static void CloseHintNotice(bool restoreFocus = false)
    {
        _hintNoticeTimer?.Stop();
        Popup? popup = _hintNotice;
        _hintNotice = null;
        _hintNoticeCard = null;
        _hintNoticeText = null;
        if (popup is null) return;
        try { popup.IsOpen = false; }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        if (restoreFocus) RestoreCanvasFocus();
    }
}
