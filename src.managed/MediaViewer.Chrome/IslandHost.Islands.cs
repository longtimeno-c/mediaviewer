// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Hosting;

namespace MediaViewer.Chrome;

/// <summary>
/// Issue #227: the plumbing every child island shares. The gallery, filmstrip,
/// panes and transport each attach the same way (the native context, its
/// command callback, maybe the session, a parked island) and show the same
/// way (build on show, drop the tree and park on hide).
/// </summary>
public static partial class IslandHost
{
    private const int InvalidArg = unchecked((int)0x80070057);

    /// <summary>
    /// The start of every Attach*: reads the args, takes the native context and
    /// command callback. Returns 0, or the HRESULT to hand back.
    /// </summary>
    private static int BeginAttach(IntPtr arg, int sizeBytes, out ChromeFilmstripArgs args, out IntPtr parent)
    {
        args = default;
        parent = IntPtr.Zero;
        if (arg == IntPtr.Zero || sizeBytes < FilmstripArgsSize) return InvalidArg;
        args = Marshal.PtrToStructure<ChromeFilmstripArgs>(arg);
        parent = checked((IntPtr)args.ParentHwnd);
        if (parent == IntPtr.Zero) return InvalidArg;

        EnsureApp();
        _context = checked((IntPtr)args.Context);
        if (args.OnCommand != 0)
        {
            _onCommand = Marshal.GetDelegateForFunctionPointer<NativeCommand>(
                checked((IntPtr)args.OnCommand));
        }
        return 0;
    }

    /// <summary>
    /// The filmstrip normally attaches first and owns the session and the
    /// drain. Another island borrows only if it did not, so a --no-filmstrip
    /// layout still lists.
    /// </summary>
    private static void BorrowSessionIfNone(in ChromeFilmstripArgs args)
    {
        if (_folderSession is not null || args.Session == 0) return;
        _folderSession = MediaViewerSession.Borrow(checked((IntPtr)args.Session));
        StartDrain();
    }

    /// <summary>
    /// Replaces <paramref name="slot"/> with a new island on <paramref name="parent"/>,
    /// with no content: the caller parks it, since a default full-client island
    /// would flash over the canvas for a frame.
    /// </summary>
    private static DesktopWindowXamlSource NewIsland(ref DesktopWindowXamlSource? slot, IntPtr parent,
                                                     bool takeFocus = false)
    {
        DisposeSource(ref slot);
        var source = new DesktopWindowXamlSource();
        EnsureFocusHook();
        source.Initialize(Win32Interop.GetWindowIdFromWindow(parent));
        if (takeFocus) source.TakeFocusRequested += OnTakeFocusRequested;
        slot = source;
        return source;
    }

    /// <summary>
    /// Show is "move the bridge and build the tree", hide is "drop the tree and
    /// park the bridge". <paramref name="place"/> moves it either way (native
    /// sends the parked rectangle with a hide). <paramref name="rebuild"/>
    /// builds on every show (the gallery and filmstrip: a repeater squeezed to
    /// nothing does not reliably realise again); otherwise only when empty.
    /// </summary>
    private static void ShowOrPark(DesktopWindowXamlSource source, bool visible, Action place,
                                   Func<UIElement> build, bool rebuild, Action onShown, Action onHidden)
    {
        if (!visible)
        {
            onHidden();
            source.Content = null;
            place();
            return;
        }
        place();
        if (!rebuild && source.Content is not null) return;
        source.Content = build();
        onShown();
    }
}
