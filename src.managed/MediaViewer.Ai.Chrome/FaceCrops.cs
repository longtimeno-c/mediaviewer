// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;

namespace MediaViewer.Ai.Chrome;

internal sealed record PersonVm(ulong Id, string Name, long Faces, ulong CoverFace, double[] Box)
{
    /// <summary>The Mac's words: "Unnamed person" for the screen reader and the sheet's title.</summary>
    public string Label => Name.Length > 0 ? Name : "Unnamed person";

    /// <summary>A merge target in a menu: "Unnamed (3 photos)" when it has no name (the Mac's displayName).</summary>
    public string MenuName => Name.Length > 0 ? Name : $"Unnamed ({Photos})";

    public string Photos => Faces == 1 ? "1 photo" : $"{Faces:N0} photos";
}

internal sealed record FaceVm(ulong Id, string Path, long PtsMs, double[] Box);

/// <summary>
/// Circular face crops, made in the view from the stored box (an ImageBrush
/// transform over the JPEG face_thumb hands out): nothing is ever written to
/// disk. Shared by the People grid in Settings and a person's faces sheet, so
/// a cover seen once is not asked for again. A monogram shows until (or
/// unless) the picture loads, as on the Mac.
/// </summary>
internal sealed class FaceCrops(AiApi api, Look look)
{
    // face id -> the JPEG it was found in (face_thumb); a person's faces are
    // often one photo, and the grids re-realise tiles as they scroll.
    private readonly Dictionary<ulong, string> _thumbPaths = new();
    // A few decodes at once, not one per card: 200 people are 200 face_thumb
    // calls into the pack, each possibly a decode (the Mac's ThumbGate).
    private readonly SemaphoreSlim _gate = new(4);

    public FrameworkElement Crop(ulong face, double[] box, double size, string monogram, DispatcherQueue queue)
    {
        // The placeholder disc, then the picture over it once it decodes.
        var back = new Ellipse { Width = size, Height = size, Fill = look.Tint(AddonColour.Title, 20) };
        var shape = new Ellipse { Width = size, Height = size };
        var initial = look.Text(monogram, size / 4.6, AddonColour.Body, wrap: false);
        initial.HorizontalAlignment = HorizontalAlignment.Center;
        initial.VerticalAlignment = VerticalAlignment.Center;
        var grid = new Grid { Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center };
        grid.Children.Add(back);
        grid.Children.Add(shape);
        grid.Children.Add(initial);
        if (face == 0 || box.Length != 4) return grid;
        double x = box[0], y = box[1], w = Math.Max(0.01, box[2]), h = Math.Max(0.01, box[3]);
        // Pad the face a little so a crop reads as a portrait, not a mask.
        const double Pad = 0.25;
        double px = Math.Max(0, x - w * Pad), py = Math.Max(0, y - h * Pad);
        double pw = Math.Min(1 - px, w * (1 + 2 * Pad)), ph = Math.Min(1 - py, h * (1 + 2 * Pad));
        void Show(string path)
        {
            try
            {
                var bitmap = new BitmapImage { DecodePixelWidth = (int)Math.Min(1024, size * 2 / pw) };
                bitmap.UriSource = new Uri(path);
                var brush = new ImageBrush
                {
                    ImageSource = bitmap,
                    Stretch = Stretch.Fill,
                    // Relative coordinates: the box [px, px+pw] maps to [0, 1].
                    RelativeTransform = new CompositeTransform
                    {
                        ScaleX = 1 / pw,
                        ScaleY = 1 / ph,
                        TranslateX = -px / pw,
                        TranslateY = -py / ph,
                    },
                };
                // Fill now: a BitmapImage nothing in the live tree uses is never
                // decoded, so waiting for ImageOpened to fill left every crop
                // blank. The monogram stays on top until the picture is there.
                shape.Fill = brush;
                bitmap.ImageOpened += (_, _) => initial.Visibility = Visibility.Collapsed;
            }
            catch (Exception ex) when (ex is UriFormatException or ArgumentException) { }
        }

        if (_thumbPaths.TryGetValue(face, out string? known))
        {
            Show(known);
            return grid;
        }
        _ = Task.Run(async () =>
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            string? path = null;
            try { path = api.FaceThumb(face); }  // [worker-thread]: may make the JPEG
            catch (MediaViewerException) { }
            finally { _gate.Release(); }
            if (string.IsNullOrEmpty(path)) return;
            queue.TryEnqueue(() =>
            {
                _thumbPaths[face] = path;
                Show(path);
            });
        });
        return grid;
    }

    /// <summary>"T" for Tristan; nothing for an unnamed person (the Mac's monogram).</summary>
    public static string Monogram(string name) => name.Length > 0 ? name[..1].ToUpperInvariant() : "";
}
