// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using Windows.Graphics.Imaging;
using Windows.Media.Ocr;

namespace MediaViewer.Chrome;

/// <summary>
/// Copy Text in Image (Preview's; docs/design/16 View): the Windows half of
/// shell/text_in_image.h's port. Windows.Media.Ocr is WinRT, so it lives here
/// rather than in the native host (D9 keeps it out of image/ as well). On
/// device: nothing is sent anywhere (rule 6), and no Store download is ever
/// offered (rule 7) -- without an OCR language the notice says so.
/// </summary>
public static partial class IslandHost
{
    private const int OcrRead = 0, OcrNoLanguage = 1, OcrUnavailable = 2;

    /// <summary>
    /// Native, on a pool worker (never the UI thread): it blocks until the
    /// recognition finishes. In: chrome_text_recognition_args { int64 pixels;
    /// int32 width; int32 height; int64 out_utf8; int32 out_capacity;
    /// int32 out_length }, BGRA8 opaque pixels. Writes the lines read, UTF-8,
    /// one per line, and their length. Back: 0 read (maybe nothing), 1 no OCR
    /// language for the user's profile, 2 no OCR on this system, else failed.
    /// </summary>
    public static int RecognizeText(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 32) return unchecked((int)0x80070057);
            IntPtr pixels = checked((IntPtr)Marshal.ReadInt64(arg));
            int width = Marshal.ReadInt32(arg, 8);
            int height = Marshal.ReadInt32(arg, 12);
            IntPtr output = checked((IntPtr)Marshal.ReadInt64(arg, 16));
            int capacity = Marshal.ReadInt32(arg, 24);
            if (pixels == IntPtr.Zero || output == IntPtr.Zero || width <= 0 || height <= 0 || capacity < 0)
            {
                return unchecked((int)0x80070057);
            }
            if (OcrEngine.AvailableRecognizerLanguages.Count == 0) return OcrNoLanguage;
            OcrEngine? engine = OcrEngine.TryCreateFromUserProfileLanguages();
            if (engine is null) return OcrNoLanguage;
            uint max = OcrEngine.MaxImageDimension;
            if ((uint)width > max || (uint)height > max) return unchecked((int)0x80070057);

            byte[] bytes = new byte[checked(width * height * 4)];
            Marshal.Copy(pixels, bytes, 0, bytes.Length);
            using SoftwareBitmap bitmap = SoftwareBitmap.CreateCopyFromBuffer(
                bytes.AsBuffer(), BitmapPixelFormat.Bgra8, width, height, BitmapAlphaMode.Premultiplied);
            OcrResult result = engine.RecognizeAsync(bitmap).AsTask().GetAwaiter().GetResult();

            var text = new StringBuilder();
            foreach (OcrLine line in result.Lines) text.Append(line.Text).Append('\n');
            byte[] utf8 = Encoding.UTF8.GetBytes(text.ToString());
            if (utf8.Length > capacity) return unchecked((int)0x8007007A);  // ERROR_INSUFFICIENT_BUFFER
            Marshal.Copy(utf8, 0, output, utf8.Length);
            Marshal.WriteInt32(arg, 28, utf8.Length);
            return OcrRead;
        }
        catch (TypeLoadException)
        {
            return OcrUnavailable;
        }
        catch (COMException ex) when (ex.HResult == unchecked((int)0x80040154))  // REGDB_E_CLASSNOTREG
        {
            return OcrUnavailable;
        }
        catch (Exception ex)
        {
            // The type only: no text read from the user's image goes to a log.
            System.Diagnostics.Debug.WriteLine("RecognizeText: " + ex.GetType().Name);
            return unchecked((int)0x80004005);
        }
    }
}
