// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace MediaViewer.Ai.Chrome;

/// <summary>What a face tile shows: the crop, and around it what the sheet knows.</summary>
internal sealed record TileFace(ulong Id, string Path, long PtsMs, double[] Box, bool Pinned = false, string Caption = "");

/// <summary>
/// One face in a sheet (a person's faces, unknown faces, "Might also be"):
/// the round crop, a selection ring, a pin glyph for a face the user placed,
/// a caption under it (why it is worth a look, or who it may be), the hover ✕
/// ("Not this person") and an optional button under it ("Yes", "Merge").
/// Rule 6: the file name is a tooltip and the screen reader's name, never logged.
/// </summary>
internal sealed class FaceTile
{
    public const double Face = 72, Cell = 84, CaptionHeight = 32, ActionHeight = 30;

    public readonly Grid Root;
    public readonly TileFace Data;
    private readonly Ellipse _ring;
    private readonly Button? _reject;
    private readonly string _name;
    private bool _selected;

    /// <param name="reject">The hover ✕ ("Not this person"); null: none.</param>
    /// <param name="action">A button under the face, its label and what it does; null: none.</param>
    public FaceTile(AiChrome chrome, TileFace face, double width, bool captioned, Action<FaceTile> tap,
                    Action? reject = null, (string Label, string Help, Action Click)? action = null)
    {
        Look look = chrome.Look;
        Data = face;
        double height = Cell + (captioned ? CaptionHeight : 0) + (action is null ? 0 : ActionHeight);
        Root = new Grid { Width = width, Height = height, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        var column = new StackPanel { Spacing = 2, HorizontalAlignment = HorizontalAlignment.Center };
        Root.Children.Add(column);

        var disc = new Grid { Width = Cell, Height = Cell, HorizontalAlignment = HorizontalAlignment.Center };
        disc.Children.Add(chrome.Crops.Crop(face.Id, face.Box, Face, "", Root.DispatcherQueue));
        _ring = new Ellipse
        {
            Width = Face + 4,
            Height = Face + 4,
            StrokeThickness = 2.5,
            Stroke = look[AddonColour.Accent],
            Visibility = Visibility.Collapsed,
            IsHitTestVisible = false,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        disc.Children.Add(_ring);
        if (face.Pinned)
        {
            // The user put this face here: a small pin, bottom left.
            var pin = new Grid
            {
                Width = 20,
                Height = 20,
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Bottom,
                Margin = new Thickness(4, 0, 0, 4),
                IsHitTestVisible = false,
            };
            pin.Children.Add(new Ellipse { Fill = look.Tint(AddonColour.Canvas, 230) });
            pin.Children.Add(new TextBlock
            {
                Text = "",  // Pin
                FontFamily = Look.IconFont,
                FontSize = 11,
                Foreground = look[AddonColour.Accent],
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            });
            AutomationProperties.SetAccessibilityView(pin, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
            disc.Children.Add(pin);
        }
        if (reject is not null)
        {
            _reject = new Button
            {
                Content = "✕",
                Width = 22,
                Height = 22,
                Padding = new Thickness(0),
                FontSize = 11,
                CornerRadius = new CornerRadius(11),
                BorderThickness = new Thickness(0),
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(0, 2, 4, 0),
                Visibility = Visibility.Collapsed,
            };
            // Its dark disc on hover and press too, never WinUI's grey.
            Look.HoldFill(_reject, new SolidColorBrush(Windows.UI.Color.FromArgb(166, 0, 0, 0)),
                          new SolidColorBrush(Windows.UI.Color.FromArgb(210, 0, 0, 0)),
                          new SolidColorBrush(Windows.UI.Color.FromArgb(235, 0, 0, 0)),
                          new SolidColorBrush(Microsoft.UI.Colors.White));
            ToolTipService.SetToolTip(_reject, "Not this person");
            AutomationProperties.SetName(_reject, "Not this person");
            _reject.Click += (_, _) => reject();
            disc.Children.Add(_reject);
            Root.PointerEntered += (_, _) => _reject.Visibility = Visibility.Visible;
            Root.PointerExited += (_, _) => _reject.Visibility = Visibility.Collapsed;
            Root.PointerCanceled += (_, _) => _reject.Visibility = Visibility.Collapsed;
        }
        column.Children.Add(disc);

        if (captioned)
        {
            TextBlock caption = look.Text(face.Caption, 11);
            caption.TextAlignment = TextAlignment.Center;
            caption.HorizontalAlignment = HorizontalAlignment.Center;
            caption.MaxLines = 2;
            caption.TextTrimming = TextTrimming.CharacterEllipsis;
            caption.Width = width - 4;
            caption.Height = CaptionHeight;
            column.Children.Add(caption);
        }
        Button? button = null;
        if (action is { } a)
        {
            button = look.Button(a.Label, a.Click);
            button.HorizontalAlignment = HorizontalAlignment.Center;
            button.Padding = new Thickness(10, 2, 10, 3);
            button.FontSize = 12;
            ToolTipService.SetToolTip(button, a.Help);
            AutomationProperties.SetName(button, a.Help);
            column.Children.Add(button);
        }

        string file = System.IO.Path.GetFileName(face.Path);
        string where = face.PtsMs >= 0 ? $"{file} at {Look.Moment(face.PtsMs)}" : file;
        ToolTipService.SetToolTip(Root, face.Caption.Length > 0 ? $"{where}\n{face.Caption}" : where);
        // The caption is read with the face: why it is in "Check these", or who it may be.
        _name = face.Caption.Length > 0 ? $"{where}, {face.Caption}" : where;
        AutomationProperties.SetName(Root, _name);
        UpdateStatus();
        Root.Tapped += (_, e) =>
        {
            var d = e.OriginalSource as DependencyObject;
            if ((_reject is not null && IsInside(d, _reject)) || (button is not null && IsInside(d, button))) return;
            tap(this);
            e.Handled = true;
        };
    }

    public ulong Id => Data.Id;

    private static bool IsInside(DependencyObject? d, DependencyObject ancestor)
    {
        for (; d is not null; d = VisualTreeHelper.GetParent(d))
        {
            if (ReferenceEquals(d, ancestor)) return true;
        }
        return false;
    }

    public void SetSelected(bool on)
    {
        if (_selected == on) return;
        _selected = on;
        _ring.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        UpdateStatus();
    }

    private void UpdateStatus()
    {
        string status = string.Join(", ", new[] { _selected ? "Selected" : "", Data.Pinned ? "Pinned" : "" }.Where(s => s.Length > 0));
        AutomationProperties.SetItemStatus(Root, status);
    }

    /// <summary>The cursor moved here: scroll it into view.</summary>
    public void Reveal() => Root.StartBringIntoView(new BringIntoViewOptions { VerticalAlignmentRatio = 0.5 });
}

/// <summary>An ItemsRepeater's tiles, made by <paramref name="make"/> and remembered by face.</summary>
internal sealed class TileFactory(Func<object, FaceTile> make, Dictionary<ulong, FaceTile> tiles) : IElementFactory
{
    public UIElement GetElement(ElementFactoryGetArgs args)
    {
        FaceTile tile = make(args.Data);
        tiles[tile.Id] = tile;
        return tile.Root;
    }

    public void RecycleElement(ElementFactoryRecycleArgs args)
    {
        foreach ((ulong id, FaceTile tile) in tiles)
        {
            if (!ReferenceEquals(tile.Root, args.Element)) continue;
            tiles.Remove(id);
            break;
        }
    }
}
