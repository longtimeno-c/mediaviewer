# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Keeps the WinUI controls that cannot load in this host out of src.managed.
#
# The chrome is WinUI hosted as islands in a native exe, not a WinUI app: there
# is no XamlControlsResources, and the controls whose default style lives in
# the Windows App SDK's controls library (the WinUI 2 lineage) have no style
# here. Such a control fail-fasts the process (0xC000027B, "Cannot load
# DefaultStyleResourceUri ... generic.xaml") the moment it enters the XAML tree
# -- collapsed or not, at launch or the first time its parent is shown.
# Measured 2026-09-28 on 0.1.19: ProgressBar, ProgressRing and DropDownButton
# crash; Button, ToggleSwitch and ComboBox load. It shipped three times: PR 30's
# export rows, the 0.1.14-0.1.18 launch crash (the update bar's ProgressBar),
# and the Local search indexing pill.
#
# TextBox too, by another route: its default context flyout is a
# TextCommandBarFlyout (a controls-library type), looked up when the box's
# template changes, which cannot be created here. Measured the same day: a
# TextBox added to the live tree crashes, on 0.1.19 and 0.1.20 alike; it was the
# Local search panel's Ctrl+F crash. The editors built on it go with it.
#
# Use the plain stand-ins instead: IslandHost.JobBar / Shared\FlatBar.cs for a
# progress bar, an Ellipse or text for activity, a Button with a Flyout for a
# drop-down, Shared\FakeInput.cs for text entry. Comment lines are not checked.
#
# Exit 0 clean, 1 on a violation.

[CmdletBinding()]
param(
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) {
    $SourceRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\src.managed'
}

# Measured crashing: ProgressBar, ProgressRing, DropDownButton, TextBox. The rest
# are the same family (styled in, or built on, the controls library).
$banned = @(
    'TextBox', 'PasswordBox', 'RichEditBox', 'AutoSuggestBox',
    'ProgressBar', 'ProgressRing', 'DropDownButton', 'SplitButton', 'ToggleSplitButton',
    'InfoBar', 'InfoBadge', 'NumberBox', 'Expander', 'TeachingTip', 'TabView',
    'TreeView', 'NavigationView', 'BreadcrumbBar', 'RatingControl', 'PipsPager',
    'ColorPicker', 'PersonPicture', 'RadioButtons', 'RadioMenuFlyoutItem', 'MenuBar',
    'CommandBarFlyout', 'AnimatedIcon', 'AnimatedVisualPlayer', 'ScrollView', 'ItemsView',
    'TwoPaneView', 'SwipeControl', 'RefreshContainer', 'ParallaxView', 'SelectorBar',
    'ImageIcon', 'AnnotatedScrollBar', 'TitleBar'
)
$pattern = '\b(' + ($banned -join '|') + ')\b'

$violations = @()
Get-ChildItem -Path $SourceRoot -Recurse -Filter *.cs |
    Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' } |
    ForEach-Object {
        $file = $_
        $n = 0
        foreach ($line in Get-Content -LiteralPath $file.FullName) {
            $n++
            $code = ($line -replace '//.*$', '')
            if ($code -match $pattern) {
                $violations += "{0}:{1}: {2}" -f $file.FullName, $n, $line.Trim()
            }
        }
    }

if ($violations.Count -gt 0) {
    Write-Host "WinUI controls that fail-fast in this island host (use JobBar / FlatBar / a Button with a Flyout):"
    $violations | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "check-winui-controls: clean"
exit 0
