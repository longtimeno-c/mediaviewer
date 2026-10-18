// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Foundation;

namespace MediaViewer.Chrome;

/// <summary>
/// What the filmstrip, gallery and folder tile factories share (#227): putting a
/// disk thumbnail or folder cover into a tile, and following the tile's view
/// model only while the repeater has the tile realised. Each factory used to
/// carry its own copy, and the filmstrip's had drifted (it never unsubscribed,
/// #215).
/// </summary>
public static partial class IslandHost
{
    /// <summary>
    /// Shows the cached thumbnail at <paramref name="path"/> in
    /// <paramref name="image"/>, decoded at <paramref name="decodeWidth"/> (a
    /// 2x-DPI tile, not the full JPEG-512 disk thumb). An empty path clears it.
    /// With a <paramref name="placeholder"/>, the image shows only while it has
    /// a source and the placeholder otherwise (folder covers). The thumbnail is
    /// written before its path is published, so the decode is XAML's own, off
    /// this thread; a bad path leaves the tile empty rather than throwing.
    /// </summary>
    private static void ShowTileImage(Image image, string? path, int decodeWidth, UIElement? placeholder = null)
    {
        bool shown = false;
        if (string.IsNullOrEmpty(path))
        {
            image.Source = null;
        }
        else
        {
            try
            {
                image.Source = new BitmapImage(new Uri(path)) { DecodePixelWidth = decodeWidth };
                shown = true;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                image.Source = null;
            }
        }
        if (placeholder is null) return;
        image.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
        placeholder.Visibility = shown ? Visibility.Collapsed : Visibility.Visible;
    }

    /// <summary>
    /// Follows <paramref name="vm"/> for as long as <paramref name="tile"/> is
    /// realised: <see cref="UnbindTile"/> from the factory's RecycleElement
    /// unsubscribes. Without it every realisation leaks a handler, keeping each
    /// tile scrolled or arrowed past alive, and each thumb completion walks a
    /// growing list of dead elements.
    /// </summary>
    private static void BindTile(Border tile, INotifyPropertyChanged vm, PropertyChangedEventHandler onChanged,
                                 Action? alsoUnsubscribe = null)
    {
        vm.PropertyChanged += onChanged;
        tile.Tag = alsoUnsubscribe is null
            ? (Action)(() => vm.PropertyChanged -= onChanged)
            : (Action)(() =>
            {
                vm.PropertyChanged -= onChanged;
                alsoUnsubscribe();
            });
    }

    /// <summary>The other half of <see cref="BindTile"/>, for RecycleElement.</summary>
    private static void UnbindTile(UIElement element)
    {
        if (element is not Border { Tag: Action unsubscribe } tile) return;
        unsubscribe();
        tile.Tag = null;
    }
    /// <summary>
    /// The Mac's thumbnail square (FilmstripCell, GalleryCell, issue #201): the
    /// placeholder fill until the thumbnail arrives, <paramref name="content"/>
    /// over it with the corners clipped, then an optional wash and the accent
    /// outline drawn over the thumbnail alone, never around the name below it.
    /// The outline starts at zero thickness; <see cref="SetOutline"/> lights it.
    /// </summary>
    private static Grid ThumbBox(double size, double radius, UIElement content, UIElement placeholder,
                                 out Border outline, Border? wash = null)
    {
        var inner = new Grid();
        inner.Children.Add(placeholder);
        inner.Children.Add(content);
        var box = new Grid { Width = size, Height = size };
        box.Children.Add(new Border { CornerRadius = Corner(radius), Child = inner });
        if (wash is not null)
        {
            wash.CornerRadius = Corner(radius);
            wash.IsHitTestVisible = false;
            box.Children.Add(wash);
        }
        outline = new Border
        {
            CornerRadius = Corner(radius),
            BorderBrush = Brush(ChromeColour.Accent),  // the Mac's accent outline
            IsHitTestVisible = false,
        };
        box.Children.Add(outline);
        return box;
    }

    /// <summary>The Mac's <c>Rectangle().fill(.quaternary)</c> while a tile waits.</summary>
    private static Border TilePlaceholder() => new() { Background = Brush(ChromeColour.Placeholder) };

    private static void SetOutline(Border outline, bool on) =>
        outline.BorderThickness = new Thickness(on ? 2 : 0);

    /// <summary>
    /// The Mac's <c>.truncationMode(.middle)</c>: "IMG_20…_0042.JPG" keeps the
    /// end of a camera name, where files differ. XAML only trims the end, so
    /// the text is measured once and, only when it overflows, cut to fit from
    /// an estimate (usually one or two more measures). The full name stays the
    /// accessible name.
    /// </summary>
    private static void SetMiddleTrimmed(TextBlock block, string text, double maxWidth)
    {
        block.Text = text;
        AutomationProperties_SetName(block, text);
        if (text.Length < 5 || maxWidth <= 0) return;
        var unbounded = new Size(double.PositiveInfinity, double.PositiveInfinity);
        // A MaxWidth caps DesiredSize, so the overflow would never show: measure
        // without it.
        double cap = block.MaxWidth;
        block.MaxWidth = double.PositiveInfinity;
        try
        {
            block.Measure(unbounded);
            double extra = block.Margin.Left + block.Margin.Right;
            double full = block.DesiredSize.Width - extra;
            if (full <= maxWidth) return;
            int keep = Math.Min(text.Length - 1, (int)(text.Length * maxWidth / full));
            for (; keep >= 2; --keep)
            {
                int head = (keep + 1) / 2;
                int tail = keep - head;
                // Never split a surrogate pair.
                if (head > 0 && char.IsHighSurrogate(text[head - 1])) --head;
                if (tail > 0 && char.IsLowSurrogate(text[^tail])) --tail;
                block.Text = string.Concat(text.AsSpan(0, head), "…", text.AsSpan(text.Length - tail));
                block.Measure(unbounded);
                if (block.DesiredSize.Width - extra <= maxWidth) return;
            }
        }
        catch (Exception ex)
        {
            // A measure that cannot run leaves XAML's end ellipsis.
            System.Diagnostics.Debug.WriteLine(ex);
            block.Text = text;
        }
        finally
        {
            block.MaxWidth = cap;
        }
    }
}
