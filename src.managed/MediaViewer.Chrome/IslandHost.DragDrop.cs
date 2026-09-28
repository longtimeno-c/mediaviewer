// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
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
/// have to forward drops. Drag out uses StorageFile so Explorer gets CF_HDROP;
/// it needs OLE on the island thread (main.cpp's OleInitialize). Drags are
/// copy-only and read-only, and never dropped back on this window.
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

    // One of our own file drags is in flight: a cell's (set here), or the
    // canvas's (native says so through SetDragPaths). Our own drop targets
    // refuse it instead of reopening the folder it came from; the Mac's cells
    // return an empty operation inside the app. Native keeps its own flag for
    // the canvas's WM_DROPFILES and the WM_COPYDATA forwarding.
    private static bool _ownDrag;

    // Native's answer to Command.DragItems, parked by SetDragPaths during that
    // same (synchronous) call.
    private static string? _dragAnswer;

    private static void WireFileDrag(UIElement tile, FolderItemVm vm)
    {
        tile.CanDrag = true;
        tile.DragStarting += (sender, e) =>
        {
            if (string.IsNullOrEmpty(vm.Path))
            {
                e.Cancel = true;
                return;
            }
            List<string> paths = DragPaths(vm);
            _ownDrag = true;
            DragStartingEventArgs args = e;
            DragOperationDeferral deferral = args.GetDeferral();
            _ = StartFileDragAsync(paths, args, deferral);
        };
        // Dropped anywhere, or cancelled: our targets take drops again.
        tile.DropCompleted += (sender, e) => EndOwnDrag();
    }

    // What a drag of this cell carries (the Mac's FolderStore.dragFiles): a
    // marked cell drags every marked item in listing order, an unmarked one
    // itself, each with its RAW / Live pair. Marks live in native, which
    // answers inside the Send. A listing that moved on under this cell (or a
    // host without the answer) drags the cell alone, as before.
    private static List<string> DragPaths(FolderItemVm vm)
    {
        _dragAnswer = null;
        Send(Command.DragItems, vm.Index);
        string answer = _dragAnswer ?? "";
        _dragAnswer = null;
        var paths = new List<string>(answer.Split('\n', StringSplitOptions.RemoveEmptyEntries));
        if (!paths.Contains(vm.Path, StringComparer.OrdinalIgnoreCase))
        {
            paths.Clear();
            paths.Add(vm.Path);
            if (!string.IsNullOrEmpty(vm.PairPath)) paths.Add(vm.PairPath);
        }
        return paths;
    }

    private static void EndOwnDrag()
    {
        if (!_ownDrag) return;
        _ownDrag = false;
        Send(Command.DragEnded);
    }

    /// <summary>Native answers Command.DragItems (the files to drag, UTF-8, one
    /// path per line) and says whether one of our drags is in flight
    /// (Reserved != 0), which it also does around the canvas's own drag.
    /// In: ChromeTableArgs.</summary>
    public static int SetDragPaths(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < TableArgsSize) return unchecked((int)0x80070057);
            ChromeTableArgs a = Marshal.PtrToStructure<ChromeTableArgs>(arg);
            _dragAnswer = a.Utf8 == 0 || a.Length <= 0
                ? ""
                : Marshal.PtrToStringUTF8(checked((IntPtr)a.Utf8), a.Length) ?? "";
            _ownDrag = a.Reserved != 0;
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The files are resolved off the UI thread by the broker (no File.Exists
    // here: rule 1). One that has gone since the listing is left out; if none
    // is left, the drag is cancelled. A paired stop drags both halves (PR 7),
    // the same rule as copy / move.
    private static async System.Threading.Tasks.Task StartFileDragAsync(
        List<string> paths, DragStartingEventArgs args, DragOperationDeferral deferral)
    {
        try
        {
            var lookups = new System.Threading.Tasks.Task<StorageFile?>[paths.Count];
            for (int i = 0; i < paths.Count; i++) lookups[i] = ResolveFileAsync(paths[i]);
            StorageFile?[] found = await System.Threading.Tasks.Task.WhenAll(lookups);
            var files = new List<IStorageItem>(found.Length);
            foreach (StorageFile? f in found)
            {
                if (f is not null) files.Add(f);
            }
            if (files.Count == 0)
            {
                args.Cancel = true;
                EndOwnDrag();
                return;
            }
            // The originals themselves (CF_HDROP), read in place by Explorer and
            // editors. Copy only: Explorer on the same volume would otherwise
            // move an original out of the folder.
            args.Data.SetStorageItems(files, true);
            args.Data.RequestedOperation = DataPackageOperation.Copy;
            args.AllowedOperations = DataPackageOperation.Copy;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            args.Cancel = true;
            EndOwnDrag();
        }
        finally
        {
            deferral.Complete();
        }
    }

    private static async System.Threading.Tasks.Task<StorageFile?> ResolveFileAsync(string path)
    {
        try
        {
            return await StorageFile.GetFileFromPathAsync(path);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return null;
        }
    }

    private static void WireFileDrop(UIElement target)
    {
        target.AllowDrop = true;
        target.DragOver += (sender, e) =>
        {
            // Our own drag over our own strip or grid: nothing to take.
            e.AcceptedOperation = _ownDrag ? DataPackageOperation.None : DataPackageOperation.Copy;
            e.Handled = true;
        };
        target.Drop += (sender, e) =>
        {
            e.Handled = true;
            if (_ownDrag) return;
            DragEventArgs args = e;
            DragOperationDeferral deferral = args.GetDeferral();
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
