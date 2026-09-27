// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 8 telemetry consent (plan/13 Part 3): the first-run screen, and the one
/// Settings row that turns it off again.
/// </summary>
/// <remarks>
/// <para>The rules this file exists to hold, all from plan/13:</para>
/// <list type="bullet">
/// <item>Default off. The screen opens with NEITHER answer selected - there is
/// no pre-ticked box and no default button, so closing it without reading
/// leaves telemetry off.</item>
/// <item>One screen, once. It is shown while native reports the Asked bit
/// clear, and answering it either way sets that bit for good.</item>
/// <item>No second modal after it. plan/13 puts the PR 15 default-viewer
/// prompt after this choice, on a later launch - not stacked behind it.</item>
/// <item>Honest about the update check being a network call of its own, with
/// the setting that disables it named on the same screen.</item>
/// </list>
/// <para>Native owns the value (settings.ini [telemetry]); this is a view of
/// it, pushed back by ApplySettings, exactly like every other settings row.
/// Not a command-table entry (plan/16): no key, nothing in <c>?</c>.</para>
/// </remarks>
public static partial class IslandHost
{
    private static ToggleSwitch? _telemetry;
    private static Flyout? _consentFlyout;
    private static bool _consentShown;
    private static FrameworkElement? _telemetryAnchor;

    private const string ConsentBody =
        "MediaViewer can send anonymous diagnostics: which file formats fail to decode, "
        + "crash-free session rate, frame timing by graphics vendor, and whether hardware "
        + "video decoding was available.\n\n"
        + "It never sends anything about your files - no paths, no filenames, no folder "
        + "names, no thumbnails or pixels, and no EXIF. Camera model is the one exception, "
        + "and it is read on purpose rather than forwarded with the rest of the metadata.\n\n"
        + "Separately, MediaViewer asks GitHub for a newer version at launch. That request "
        + "reveals your IP address and app version whatever you choose here; Settings has a "
        + "switch that turns it off.\n\n"
        + "You can change this any time in Settings. Turning it off deletes the random "
        + "install id and the diagnostics waiting to be sent.";

    /// <summary>
    /// Shown once, when native says the choice has not been made. Called after
    /// the bar is built and after every ApplySettings, so a fresh profile gets
    /// it on the first push rather than needing a restart.
    /// </summary>
    private static void MaybeShowConsent(FrameworkElement anchor)
    {
        if (_consentShown || HasFlag(SettingFlag.TelemetryAsked)) return;
        _consentShown = true;
        try
        {
            _consentFlyout = new Flyout
            {
                ShouldConstrainToRootBounds = false,
                FlyoutPresenterStyle = FlyoutPresenterStyle(),
                Content = BuildConsentContent(),
                // LightDismiss would let a stray click count as an answer.
                // It cannot: dismissing changes nothing, and the Asked bit
                // stays clear until a button is pressed.
                Placement = FlyoutPlacementMode.Bottom,
            };
            _consentFlyout.ShowAt(anchor);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static FrameworkElement BuildConsentContent()
    {
        var root = new StackPanel { Spacing = 10, Width = 420, Padding = new Thickness(4) };
        root.Children.Add(new TextBlock
        {
            Text = "Help improve MediaViewer?",
            FontFamily = UiFont,
            FontSize = UiFontSize + 3,
            Foreground = Brush(Title),
        });
        root.Children.Add(new TextBlock
        {
            Text = ConsentBody,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Body),
            TextWrapping = TextWrapping.Wrap,
        });

        // Two buttons, equal weight, neither of them the default. "No thanks"
        // is first because it is the state the app is already in.
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Right,
        };
        Button no = TextButton("No thanks", () => AnswerConsent(false));
        Button yes = TextButton("Send anonymous diagnostics", () => AnswerConsent(true));
        no.IsTabStop = true;
        yes.IsTabStop = true;
        no.AllowFocusOnInteraction = true;
        yes.AllowFocusOnInteraction = true;
        buttons.Children.Add(no);
        buttons.Children.Add(yes);
        root.Children.Add(buttons);
        return root;
    }

    private static void AnswerConsent(bool on)
    {
        // The Asked bit rides with the answer: native only changes consent for
        // a word that carries it, so no other settings push can flip telemetry.
        int next = _settingFlags | SettingFlag.TelemetryAsked;
        next = on ? next | SettingFlag.Telemetry : next & ~SettingFlag.Telemetry;
        Send(Command.SetSettings, next);
        try { _consentFlyout?.Hide(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        _consentFlyout = null;
    }

    private static void AddTelemetrySettingsRow(StackPanel view)
    {
        _telemetry = new ToggleSwitch
        {
            Header = "Send anonymous diagnostics",
            IsOn = HasFlag(SettingFlag.Telemetry),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
        };
        _telemetry.Toggled += (_, _) =>
        {
            if (_updatingSettingsUi) return;
            // Same rule as the first-run screen: an explicit answer, carrying
            // the Asked bit.
            int next = _settingFlags | SettingFlag.TelemetryAsked;
            next = _telemetry.IsOn ? next | SettingFlag.Telemetry : next & ~SettingFlag.Telemetry;
            Send(Command.SetSettings, next);
        };
        ToolTipService.SetToolTip(_telemetry,
            "Decode failures by format, crash-free sessions, frame timing and hardware-decode "
            + "availability. Never a path, filename, thumbnail or EXIF. Turning it off deletes "
            + "the install id and anything not yet sent.");
        view.Children.Add(SettingsRow("Send anonymous diagnostics", "Optional. Turning this off deletes the install ID and unsent diagnostics.", _telemetry));
    }

    private static void RefreshTelemetrySettingsRow()
    {
        if (_telemetry is not null) _telemetry.IsOn = HasFlag(SettingFlag.Telemetry);
    }
}

