// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MediaViewer.Interop;

/// <summary>
/// The SafeHandle for <c>mv_session_t</c>.
/// </summary>
/// <remarks>
/// <para>
/// plan/14-abi.md calls this non-negotiable, and the reason is worth restating
/// where the code is: an <see cref="IntPtr"/> that the GC loses is a leaked
/// decoded image — tens or hundreds of MB of VRAM per occurrence, invisible
/// until the app falls over on a long browsing session. A SafeHandle also
/// behaves correctly if the process is torn down mid-call, which a hand-rolled
/// finalizer does not.
/// </para>
/// <para>
/// Handles are reference-counted in the core so the filmstrip holding a
/// thumbnail and the canvas holding the same image do not fight over lifetime.
/// This wrapper never calls <c>retain</c>: it takes ownership of what the
/// factory returned and releases exactly once.
/// </para>
/// </remarks>
public sealed class MvSessionHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private MvSessionHandle() : base(ownsHandle: true)
    {
    }

    internal MvSessionHandle(IntPtr handle) : base(ownsHandle: true)
    {
        SetHandle(handle);
    }

    protected override bool ReleaseHandle()
        => NativeMethods.mv_session_release(handle) == MvStatus.Ok;
}
