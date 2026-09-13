// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;

namespace MediaViewer.Chrome;

/// <summary>
/// File drag out of the gallery / filmstrip, and drop onto those islands.
/// The native canvas already accepts WM_DROPFILES; islands cover it, so they
/// have to forward drops. Drag out uses StorageFile so Explorer gets CF_HDROP.
/// </summary>
public static partial class IslandHost
{
    private const uint WmCopyData = 0x004A;
    private static readonly IntPtr DropCopyData = unchecked((IntPtr)0x4D560001);

    [StructLayout(LayoutKind.Sequential)]
    private struct CopyDataStruct
    {
        public IntPtr DwData;
        public int CbData;
        public IntPtr LpData;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam,
                                              ref CopyDataStruct lParam);

    private static void WireFileDrag(UIElement tile, FolderItemVm vm)
    {
        tile.CanDrag = true;
        tile.DragStarting += (s, e) =>
        {
            _ = s;
            if (string.IsNullOrEmpty(vm.Path) || !File.Exists(vm.Path))
            {
                e.Cancel = true;
                return;
            }
            var args = e;
            var deferral = args.GetDeferral();
            _ = StartFileDragAsync(vm.Path, args, deferral);
        };
    }

    private static async System.Threading.Tasks.Task StartFileDragAsync(
        string path, DragStartingEventArgs args, DragOperationDeferral deferral)
    {
        try
        {
            StorageFile file = await StorageFile.GetFileFromPathAsync(path);
            args.Data.SetStorageItems(new[] { file });
            args.Data.RequestedOperation = DataPackageOperation.Copy;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            args.Cancel = true;
        }
        finally
        {
            deferral.Complete();
        }
    }

    private static void WireFileDrop(UIElement target)
    {
        target.AllowDrop = true;
        target.DragOver += (s, e) =>
        {
            _ = s;
            e.AcceptedOperation = DataPackageOperation.Copy;
            e.Handled = true;
        };
        target.Drop += (s, e) =>
        {
            _ = s;
            e.Handled = true;
            var args = e;
            var deferral = args.GetDeferral();
            _ = HandleFileDropAsync(args, deferral);
        };
    }

    private static async System.Threading.Tasks.Task HandleFileDropAsync(
        DragEventArgs args, DragOperationDeferral deferral)
    {
        try
        {
            if (!args.DataView.Contains(StandardDataFormats.StorageItems)) return;
            IReadOnlyList<IStorageItem> items = await args.DataView.GetStorageItemsAsync();
            List<string> paths = new();
            foreach (IStorageItem item in items)
            {
                if (!string.IsNullOrEmpty(item.Path)) paths.Add(item.Path);
            }
            if (paths.Count > 0) ForwardDropToNative(paths);
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

    private static void ForwardDropToNative(List<string> paths)
    {
        DesktopWindowXamlSource? source = _source ?? _gallery ?? _filmstrip ?? _transport;
        if (source?.SiteBridge is null) return;
        IntPtr hwnd = Win32Interop.GetWindowFromWindowId(source.SiteBridge.WindowId);
        IntPtr root = GetAncestor(hwnd, GaRoot);
        if (root == IntPtr.Zero) return;
        string blob = string.Join("\n", paths);
        IntPtr mem = Marshal.StringToHGlobalUni(blob);
        try
        {
            CopyDataStruct cds = new()
            {
                DwData = DropCopyData,
                CbData = (blob.Length + 1) * 2,
                LpData = mem,
            };
            SendMessageW(root, WmCopyData, IntPtr.Zero, ref cds);
        }
        finally
        {
            Marshal.FreeHGlobal(mem);
        }
    }
}
