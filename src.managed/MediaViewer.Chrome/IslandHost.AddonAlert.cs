// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// An add-on that will not load or start says so in the window, with why
/// (owner, 2026-10-07: "show a user facing alert on any failures and why").
/// Before, the reason went only to Settings' status line, which nobody saw
/// unless Settings was open, and Ctrl+F quietly became a file-name search.
/// </summary>
/// <remarks>
/// A bar button beside Import's card hint and the indexing pill: "Local search
/// couldn't start". It opens a flyout with the reason and Open Settings. One
/// alert at a time (the latest); it goes when dismissed, when the add-on loads,
/// or when another window hosts the add-ons. The text never holds a path (rule 6):
/// the native reasons are written without one (addon/loader_win.cpp).
/// </remarks>
public static partial class IslandHost
{
    private static Button? _addonAlert;
    // The button's warning sign (Segoe Fluent's, not a "⚠" text glyph) and its words.
    private static FontIcon? _addonAlertIcon;
    private static TextBlock? _addonAlertText;
    private static Flyout? _addonAlertFlyout;
    private static AddonSlot? _addonAlertSlot;
    private static string _addonAlertTitle = "";
    private static string _addonAlertBody = "";
    // #509: where a failure came from (ExceptionFrames), for a bug report; "" when none.
    private static string _addonAlertDetails = "";

    private static Button BuildAddonAlert()
    {
        _addonAlertIcon = new FontIcon
        {
            Glyph = "\uE7BA",  // Warning
            FontFamily = IconFont,
            FontSize = TypeSize(TypeSmall),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _addonAlertText = ButtonLabel("", UiFontSize);
        _addonAlertText.VerticalAlignment = VerticalAlignment.Center;
        var label = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = Space(6),
            Children = { _addonAlertIcon, _addonAlertText },
        };
        _addonAlert = ChromeButton(label, ButtonPadding, () => OpenAddonAlert());
        _addonAlert.Visibility = Visibility.Collapsed;
        return _addonAlert;
    }

    /// <summary>Shows <paramref name="slot"/>'s failure in the bar, and opens its
    /// flyout when <paramref name="open"/> (the person just asked for it: Ctrl+F).</summary>
    /// <remarks><paramref name="warn"/> false: a note, not a failure (a later window's
    /// reader with nothing indexed yet), shown without the warning sign.
    /// <paramref name="details"/>: the exception's type and methods (ExceptionFrames),
    /// shown small under the reason; never a message or a path (#509).</remarks>
    private static void ShowAddonAlert(AddonSlot slot, string title, string body, bool open = false, bool warn = true,
                                       string details = "")
    {
        _addonAlertSlot = slot;
        _addonAlertTitle = title;
        _addonAlertBody = body;
        _addonAlertDetails = details;
        if (_addonAlert is null) return;
        SolidColorBrush ink = Brush(warn ? ChromeColour.TrimAccent : Body);
        if (_addonAlertText is not null)
        {
            _addonAlertText.Text = title;
            _addonAlertText.Foreground = ink;
        }
        if (_addonAlertIcon is not null)
        {
            _addonAlertIcon.Visibility = warn ? Visibility.Visible : Visibility.Collapsed;
            _addonAlertIcon.Foreground = ink;
        }
        AutomationProperties.SetName(_addonAlert, $"{title}. {body}");
        ToolTipService.SetToolTip(_addonAlert, body);
        _addonAlert.Visibility = Visibility.Visible;
        // Narrated once, as it appears, not only when focused.
        try
        {
            FrameworkElementAutomationPeer.FromElement(_addonAlert)?.RaiseNotificationEvent(
                AutomationNotificationKind.ActionCompleted, AutomationNotificationProcessing.ImportantMostRecent,
                $"{title}. {body}", "addon-alert");
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        if (open) OpenAddonAlert();
    }

    /// <summary>Clears the alert when it is <paramref name="slot"/>'s (it loaded after all).</summary>
    private static void ClearAddonAlert(AddonSlot slot)
    {
        if (_addonAlertSlot != slot) return;
        _addonAlertSlot = null;
        try { _addonAlertFlyout?.Hide(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        if (_addonAlert is not null) _addonAlert.Visibility = Visibility.Collapsed;
    }

    private static void OpenAddonAlert()
    {
        if (_addonAlert is null || _addonAlertSlot is null) return;
        try
        {
            _addonAlertFlyout?.Hide();
            _addonAlertFlyout = new Flyout
            {
                ShouldConstrainToRootBounds = false,
                FlyoutPresenterStyle = FlyoutPresenterStyle(),
                Content = BuildAddonAlertContent(),
                Placement = FlyoutPlacementMode.Bottom,
            };
            _addonAlertFlyout.ShowAt(_addonAlert);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static FrameworkElement BuildAddonAlertContent()
    {
        var root = new StackPanel { Spacing = Space(10), Width = 420, Padding = Pad(4) };
        // The Mac's popover (AddonsView.swift AddonAlertItem): the title at the
        // bar's size, the reason at 13, Dismiss and the filled Open Settings.
        root.Children.Add(new TextBlock
        {
            Text = _addonAlertTitle,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            TextWrapping = TextWrapping.Wrap,
        });
        root.Children.Add(new TextBlock
        {
            Text = _addonAlertBody,
            FontFamily = UiFont,
            FontSize = TypeSize(TypeSmall),
            LineHeight = Math.Round(TypeSize(TypeSmall) * 1.35),
            Foreground = Brush(Body),
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,  // to copy into a bug report
        });
        if (_addonAlertDetails.Length > 0)
        {
            root.Children.Add(new TextBlock
            {
                Text = "Details: " + _addonAlertDetails,
                FontFamily = UiFont,
                FontSize = TypeSize(TypeCaption),
                LineHeight = Math.Round(TypeSize(TypeCaption) * 1.35),
                Foreground = Brush(Body),
                TextWrapping = TextWrapping.Wrap,
                IsTextSelectionEnabled = true,
            });
        }
        AddonSlot? slot = _addonAlertSlot;
        Button dismiss = NoticeButton("Dismiss", () =>
        {
            if (slot is not null) ClearAddonAlert(slot);
        });
        Button settings = NoticeButton("Open Settings", () =>
        {
            try { _addonAlertFlyout?.Hide(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            if (slot == ImportSlot)
            {
                // #510: Import's own page in Settings → Add-ons.
                if (!_settingsVisible)
                {
                    _addonPageWanted = ImportPageId;
                    Send(Command.OpenSettings);
                }
                else
                {
                    OpenAddonPage(ImportPageId);
                }
            }
            else
            {
                HostShowLocalSearchSettings();
            }
        }, primary: true);
        root.Children.Add(NoticeButtons(dismiss, settings));
        return root;
    }

    /// <summary>Why an add-on did not load, in words a person can act on.</summary>
    private static string AddonLoadReason(AddonSlot slot, MediaViewerException ex) => ex.Status switch
    {
        MvStatus.UnsupportedFormat =>
            $"It needs an update to work with this MediaViewer. Update it in Settings → {SettingsSection(slot)}.",
        MvStatus.Corrupt =>
            $"Its files changed since it was installed, so it was not loaded. Remove it and install it again in Settings → {SettingsSection(slot)}.",
        // Verified, but Windows would not load it: the core says why (addon/loader_win.cpp).
        MvStatus.Io when ex.Detail.Length > 0 && !ex.Detail.StartsWith("mv_", StringComparison.Ordinal) && ex.Detail != "IO" =>
            ex.Detail,
        MvStatus.OutOfMemory => "There is not enough memory to start it. Close other apps, then restart MediaViewer.",
        MvStatus.PermissionDenied => "Windows denied access to its files. Security software may be blocking it.",
        _ => $"Something went wrong ({ex.Status}, reference {ex.CorrelationId}).",
    };

    private static string SettingsSection(AddonSlot slot) => slot == ImportSlot ? "Add-ons" : "Local search";

    private static string AddonFailTitle(AddonSlot slot) => $"{slot.Name} couldn't start";
}
