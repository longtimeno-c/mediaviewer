// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 15 (plan/16 View): Ctrl+Shift+S opens Windows Share with the marked
/// files, else the current one. The share sheet is a WinRT object bound to the
/// native window through <c>IDataTransferManagerInterop</c>, the same desktop
/// contract SMTC uses (IslandHost.Video.cs).
/// </summary>
public static partial class IslandHost
{
    private static DataTransferManager? _share;
    private static IntPtr _shareHwnd;
    private static string[] _sharePaths = Array.Empty<string>();

    /// <summary>
    /// Native, on the UI thread. In: { int64 hwnd; int32 bytes; int32 reserved;
    /// UTF-8 JSON array of paths }. 0 when the sheet was asked for.
    /// </summary>
    public static int ShareFiles(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 16) return unchecked((int)0x80070057);
            IntPtr hwnd = checked((IntPtr)Marshal.ReadInt64(arg));
            int len = Marshal.ReadInt32(arg, 8);
            if (hwnd == IntPtr.Zero || len <= 0 || 16 + len > sizeBytes) return unchecked((int)0x80070057);
            byte[] bytes = new byte[len];
            Marshal.Copy(arg + 16, bytes, 0, len);
            using (JsonDocument doc = JsonDocument.Parse(bytes))
            {
                _sharePaths = doc.RootElement.EnumerateArray().Select(e => e.GetString() ?? "")
                    .Where(s => s.Length > 0).ToArray();
            }
            if (_sharePaths.Length == 0) return 1;
            if (_share is null || _shareHwnd != hwnd)
            {
                if (_share is not null) _share.DataRequested -= OnShareRequested;
                _share = DataTransferManagerInterop.GetForWindow(hwnd);
                _shareHwnd = hwnd;
                _share.DataRequested += OnShareRequested;
            }
            DataTransferManagerInterop.ShowShareUIForWindow(hwnd);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static async void OnShareRequested(DataTransferManager sender, DataRequestedEventArgs args)
    {
        DataRequest request = args.Request;
        DataRequestDeferral deferral = request.GetDeferral();
        try
        {
            string[] paths = _sharePaths;
            var files = new List<IStorageItem>(paths.Length);
            foreach (string p in paths)
            {
                try { files.Add(await StorageFile.GetFileFromPathAsync(p)); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            }
            if (files.Count == 0)
            {
                request.FailWithDisplayText("The file is no longer there.");
                return;
            }
            // The sheet's heading: the one file's name, else a count. Local UI only.
            request.Data.Properties.Title = files.Count == 1 ? files[0].Name : $"{files.Count} files";
            request.Data.SetStorageItems(files, true);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
        finally
        {
            deferral.Complete();
        }
    }
}
