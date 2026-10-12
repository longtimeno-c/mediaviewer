// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;

namespace MediaViewer.Chrome;

/// <summary>
/// Screen-reader plumbing for the chrome that takes keys on a panel rather
/// than a field (issue #223): the go-to, find, export and clip-tools popups
/// type onto TextBlocks, and filmstrip / gallery tiles are not focusable, so
/// nothing changes focus for Narrator to follow. These speak the change.
/// </summary>
public static partial class IslandHost
{
    /// <summary>Marks a line Narrator reads politely when <see cref="RaiseLive"/> says it changed.</summary>
    private static void MarkLive(FrameworkElement element) =>
        AutomationProperties.SetLiveSetting(element, AutomationLiveSetting.Polite);

    private static void RaiseLive(FrameworkElement element)
    {
        try
        {
            FrameworkElementAutomationPeer.CreatePeerForElement(element)?
                .RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    private static void Announce(FrameworkElement source, string text, string activityId)
    {
        try
        {
            FrameworkElementAutomationPeer.CreatePeerForElement(source)?.RaiseNotificationEvent(
                AutomationNotificationKind.ActionCompleted, AutomationNotificationProcessing.MostRecent,
                text, activityId);
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    /// <summary>
    /// The selected item, spoken from the gallery while it is up (it owns the
    /// arrows even with the canvas focused) or from the filmstrip while it has
    /// keyboard focus. Arrowing on the canvas alone stays quiet, as before.
    /// </summary>
    private static void AnnounceSelection(int index)
    {
        if (index < 0 || index >= Items.Count) return;
        FrameworkElement? source = _galleryVisible ? _galleryRoot
                                 : _filmstrip is { HasFocus: true } ? _filmstripRoot
                                 : null;
        if (source is null) return;
        Announce(source, $"{Items[index].Name}, {index + 1} of {Items.Count}", "selection");
    }
}
