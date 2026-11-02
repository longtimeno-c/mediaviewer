// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// "Show photo" (Space): the whole picture a face was found in (face_thumb),
/// with the face's box outlined, over a sheet, so the user judges more than a
/// small round crop. An overlay in the sheet's own tree, not a dialog (a
/// ContentDialog is one more control this island host may not carry). A click,
/// Space or Esc closes it. Rule 6: the file name is shown, never logged.
/// </summary>
internal sealed class PhotoPreview
{
    private readonly FaceCrops _crops;
    private readonly Look _look;
    private readonly Grid _frame;
    private readonly Image _image;
    private readonly Rectangle _outline;
    private readonly TextBlock _caption;
    private readonly TextBlock _missing;
    private int _generation;

    public readonly Grid Root;

    public bool IsOpen => Root.Visibility == Visibility.Visible;

    public event Action? Hidden;

    public PhotoPreview(AiChrome chrome)
    {
        _crops = chrome.Crops;
        _look = chrome.Look;
        Root = new Grid
        {
            Background = _look.Tint(AddonColour.Canvas, 240),
            Visibility = Visibility.Collapsed,
            Padding = new Thickness(16),
            RowSpacing = 8,
        };
        Root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        AutomationProperties.SetName(Root, "Photo");
        _image = new Image { Stretch = Stretch.Uniform };
        _outline = new Rectangle
        {
            Stroke = _look[AddonColour.Accent],
            StrokeThickness = 2.5,
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            IsHitTestVisible = false,
            Visibility = Visibility.Collapsed,
        };
        _frame = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        _frame.Children.Add(_image);
        _frame.Children.Add(_outline);
        Root.Children.Add(_frame);
        _missing = _look.Text("This photo can't be shown.", 13);
        _missing.HorizontalAlignment = HorizontalAlignment.Center;
        _missing.VerticalAlignment = VerticalAlignment.Center;
        _missing.Visibility = Visibility.Collapsed;
        Root.Children.Add(_missing);
        _caption = _look.Text("", 12, wrap: false);
        _caption.HorizontalAlignment = HorizontalAlignment.Center;
        Grid.SetRow(_caption, 1);
        Root.Children.Add(_caption);
        Root.Tapped += (_, e) =>
        {
            Hide();
            e.Handled = true;
        };
    }

    /// <summary>The picture <paramref name="face"/> was found in, <paramref name="box"/> (0..1 of it) outlined.</summary>
    public void Show(ulong face, double[] box, string path, long ptsMs, string caption)
    {
        int g = ++_generation;
        string file = System.IO.Path.GetFileName(path);
        string where = ptsMs >= 0 ? $"{file} at {Look.Moment(ptsMs)}" : file;
        _caption.Text = caption.Length > 0 ? $"{where} · {caption}" : where;
        AutomationProperties.SetName(Root, caption.Length > 0 ? $"Photo, {where}, {caption}" : $"Photo, {where}");
        _image.Source = null;
        _image.Stretch = Stretch.Uniform;
        _outline.Visibility = Visibility.Collapsed;
        _missing.Visibility = Visibility.Collapsed;
        _frame.Width = _frame.Height = double.NaN;
        Root.Visibility = Visibility.Visible;
        _crops.Picture(face, Root.DispatcherQueue, picture =>
        {
            if (g != _generation || !IsOpen) return;
            if (picture.Length == 0)
            {
                _missing.Visibility = Visibility.Visible;
                return;
            }
            try
            {
                var bitmap = new BitmapImage();
                bitmap.ImageOpened += (_, _) =>
                {
                    if (g != _generation) return;
                    Fit(bitmap.PixelWidth, bitmap.PixelHeight, box);
                };
                bitmap.ImageFailed += (_, _) =>
                {
                    if (g == _generation) _missing.Visibility = Visibility.Visible;
                };
                bitmap.UriSource = new Uri(picture);
                _image.Source = bitmap;
            }
            catch (Exception ex) when (ex is UriFormatException or ArgumentException)
            {
                _missing.Visibility = Visibility.Visible;
            }
        });
    }

    // The picture as large as the sheet allows, the box drawn in its pixels.
    private void Fit(int pw, int ph, double[] box)
    {
        if (pw <= 0 || ph <= 0) return;
        double maxW = Math.Max(80, Root.ActualWidth - 32);
        double maxH = Math.Max(80, Root.ActualHeight - 32 - 28);
        double s = Math.Min(maxW / pw, maxH / ph);
        double w = pw * s, h = ph * s;
        _frame.Width = w;
        _frame.Height = h;
        _image.Stretch = Stretch.Fill;
        if (box.Length != 4) return;
        _outline.Margin = new Thickness(box[0] * w, box[1] * h, 0, 0);
        _outline.Width = Math.Max(4, box[2] * w);
        _outline.Height = Math.Max(4, box[3] * h);
        _outline.Visibility = Visibility.Visible;
    }

    public void Hide()
    {
        if (!IsOpen) return;
        ++_generation;
        Root.Visibility = Visibility.Collapsed;
        _image.Source = null;
        Hidden?.Invoke();
    }
}
