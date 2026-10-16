// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

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
}
