// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Shared;

/// <summary>
/// A progress bar as two plain borders, for the add-on chromes (linked into
/// each assembly; the main chrome's twin is IslandHost.JobBar). Not a WinUI
/// ProgressBar: that control has no default style in this island host and
/// fail-fasts (0xC000027B) the moment it enters the tree.
/// tools/check-winui-controls.ps1 keeps the whole family out of src.managed.
/// </summary>
internal sealed class FlatBar
{
    public readonly Grid Root;
    private readonly Border _fill;
    private double _value;
    private bool _indeterminate;

    public FlatBar(Brush track, Brush fill, double height = 4)
    {
        _fill = new Border
        {
            Background = fill,
            HorizontalAlignment = HorizontalAlignment.Left,
            CornerRadius = new CornerRadius(height / 2),
        };
        Root = new Grid { Height = height, Background = track, CornerRadius = new CornerRadius(height / 2) };
        Root.Children.Add(_fill);
        Root.SizeChanged += (_, _) => Layout();
    }

    /// <summary>0 to 1.</summary>
    public double Value
    {
        get => _value;
        set
        {
            _value = Math.Clamp(value, 0, 1);
            Layout();
        }
    }

    /// <summary>Running with no fraction yet: a fixed third, not an animation.</summary>
    public bool IsIndeterminate
    {
        get => _indeterminate;
        set
        {
            _indeterminate = value;
            Layout();
        }
    }

    public Brush Fill
    {
        set => _fill.Background = value;
    }

    private void Layout() => _fill.Width = Math.Max(0, (_indeterminate ? 0.33 : _value) * Root.ActualWidth);
}
