// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// #345 (docs/design/06 "The AI badge"): the small "AI" mark in the bottom-right of
/// the canvas when the file on screen says it was AI-made.
/// </summary>
/// <remarks>
/// Its own island, like the transport bar: chrome over the swapchain, never drawn
/// into it, so showing, hiding or hovering it costs the canvas no present. Native
/// decides when it shows, where, and what it says (shell/ai_badge.h,
/// chrome_host::show_ai_badge); this only draws it. Hover or keyboard focus opens
/// a tooltip with "Made with ..." and the evidence. It takes no keys: with it
/// focused the router still runs the viewer's commands, as with the command bar.
/// Only controls that load in this island host (tools/check-winui-controls.ps1).
/// </remarks>
public static partial class IslandHost
{
    internal const int AiBadgeArgsSize = 48;

    private static DesktopWindowXamlSource? _aiBadge;
    private static TextBlock? _aiBadgeTipTitle;
    private static TextBlock? _aiBadgeTipEvidence;
    private static Button? _aiBadgeButton;
    private static ToolTip? _aiBadgeTip;
    private static bool _aiBadgeShown;

    // Called from AttachPanels: parked, no content, until native shows it.
    private static void AttachAiBadge(IntPtr parent, int clientHeight)
    {
        DisposeSource(ref _aiBadge);
        _aiBadge = new DesktopWindowXamlSource();
        _aiBadge.Initialize(Microsoft.UI.Win32Interop.GetWindowIdFromWindow(parent));
        _aiBadge.TakeFocusRequested += OnTakeFocusRequested;
        MoveAt(_aiBadge, 0, clientHeight, 1, 1);
        _aiBadgeShown = false;
    }

    private static void DetachAiBadge()
    {
        if (_aiBadge is not null) _aiBadge.TakeFocusRequested -= OnTakeFocusRequested;
        DropAiBadgeUi();
        DisposeSource(ref _aiBadge);
        _aiBadgeShown = false;
    }

    private static void DropAiBadgeUi()
    {
        if (_aiBadgeTip is not null) _aiBadgeTip.IsOpen = false;
        _aiBadgeButton = null;
        _aiBadgeTip = null;
        _aiBadgeTipTitle = null;
        _aiBadgeTipEvidence = null;
    }

    public static int ShowAiBadge(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < AiBadgeArgsSize) return unchecked((int)0x80070057);
            if (_aiBadge is null) return 1;
            ChromeAiBadgeArgs a = Marshal.PtrToStructure<ChromeAiBadgeArgs>(arg);
            static string Utf8(ulong ptr, int len) =>
                ptr == 0 || len <= 0 ? "" : Marshal.PtrToStringUTF8(checked((IntPtr)ptr), len) ?? "";
            if (a.Visible == 0)
            {
                DropAiBadgeUi();
                _aiBadge.Content = null;
                MoveAt(_aiBadge, a.X, a.Y, a.Width, a.Height);
                _aiBadgeShown = false;
                return 0;
            }
            MoveAt(_aiBadge, a.X, a.Y, a.Width, a.Height);
            if (_aiBadge.Content is null || _aiBadgeButton is null) _aiBadge.Content = BuildAiBadge();
            string title = Utf8(a.Title, a.TitleLen);
            string evidence = Utf8(a.Evidence, a.EvidenceLen);
            _aiBadgeTipTitle!.Text = title;
            _aiBadgeTipEvidence!.Text = evidence;
            _aiBadgeTipEvidence.Visibility = evidence.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            AutomationProperties.SetHelpText(_aiBadgeButton!, evidence.Length > 0 ? title + ". " + evidence : title);
            _aiBadgeShown = true;
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static UIElement BuildAiBadge()
    {
        var label = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 3,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        // U+2726 BLACK FOUR POINTED STAR: the sparkle, in any font.
        label.Children.Add(new TextBlock { Text = "✦", FontSize = 10, Foreground = Brush(Title),
            VerticalAlignment = VerticalAlignment.Center });
        label.Children.Add(new TextBlock { Text = "AI", FontSize = 11, FontWeight = FontWeights.SemiBold,
            Foreground = Brush(Title), VerticalAlignment = VerticalAlignment.Center });

        _aiBadgeTipTitle = new TextBlock { FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
        _aiBadgeTipEvidence = new TextBlock { FontSize = 12, TextWrapping = TextWrapping.Wrap, Opacity = 0.8 };
        var tipBody = new StackPanel { Spacing = 4, MaxWidth = 320 };
        tipBody.Children.Add(_aiBadgeTipTitle);
        tipBody.Children.Add(_aiBadgeTipEvidence);
        _aiBadgeTip = new ToolTip { Content = tipBody };

        // A Button for keyboard focus and the automation peer; pressing it does
        // nothing but hand the keyboard back to the canvas.
        var button = new Button
        {
            Content = label,
            Padding = new Thickness(0),
            MinWidth = 0,
            MinHeight = 0,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Background = Brush(PanelBg),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            IsTabStop = true,
            UseSystemFocusVisuals = true,
        };
        AutomationProperties.SetName(button, "AI-generated");
        ToolTipService.SetToolTip(button, _aiBadgeTip);
        // Keyboard focus shows the same text hover does.
        button.GotFocus += (_, _) => { if (_aiBadgeTip is not null) _aiBadgeTip.IsOpen = true; };
        button.LostFocus += (_, _) => { if (_aiBadgeTip is not null) _aiBadgeTip.IsOpen = false; };
        button.Click += (_, _) => RestoreCanvasFocus();
        _aiBadgeButton = button;
        return new Grid { Background = Brush(Canvas), Children = { button } };
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeAiBadgeArgs
{
    public int Visible;
    public int X;
    public int Y;
    public int Width;
    public int Height;
    public int TitleLen;
    public ulong Title;
    public ulong Evidence;
    public int EvidenceLen;
    public int Reserved;
}
