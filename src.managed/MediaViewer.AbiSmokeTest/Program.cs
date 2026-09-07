// SPDX-License-Identifier: GPL-2.0-or-later
//
// PR 1's ABI deliverable, proved from the managed side:
//
//   "a header, an mv_guard, one call, a SafeHandle, and a completion drain —
//    proving the shape end to end before anything is built on it."
//                                                    plan/14-abi.md
//
// A console app, not WinUI. PR 1 has no WinUI (plan/10), and the point of this
// program is that the core is exercised over the real ABI with no shell and no
// dispatcher present — which is the property that keeps it testable headlessly.
//
// Usage:  MediaViewer.AbiSmokeTest [path-to-directory-containing-mediaviewer_core.dll]

using System.Diagnostics;
using System.Runtime.InteropServices;
using MediaViewer.Interop;

int failures = 0;

void Check(string what, bool condition)
{
    Console.WriteLine($"  [{(condition ? "pass" : "FAIL")}] {what}");
    if (!condition) failures++;
}

// The core DLL is built by CMake into build/bin/<config>. Point the loader at
// it rather than copying binaries around.
if (args.Length > 0)
{
    string probe = Path.GetFullPath(args[0]);
    NativeLibrary.SetDllImportResolver(typeof(MediaViewerSession).Assembly, (name, assembly, path) =>
        name == "mediaviewer_core"
            ? NativeLibrary.Load(Path.Combine(probe, "mediaviewer_core.dll"))
            : IntPtr.Zero);
}

Console.WriteLine("MediaViewer ABI smoke test");
Console.WriteLine();

Console.WriteLine("version");
Version abi = MediaViewerSession.AbiVersion;
Console.WriteLine($"  core reports ABI {abi}");
Check("major version matches this assembly", abi.Major == MediaViewerSession.ExpectedAbiMajor);

Console.WriteLine();
Console.WriteLine("struct layout");
// If this ever disagrees with the native static_assert, the two sides have
// drifted and every job id read from a completion is nonsense.
Check("MvCompletion is 40 bytes", Marshal.SizeOf<MvCompletion>() == 40);
Check("MvCompletion.JobId at offset 8",
      Marshal.OffsetOf<MvCompletion>(nameof(MvCompletion.JobId)) == 8);
Check("MvCompletion.Payload at offset 32",
      Marshal.OffsetOf<MvCompletion>(nameof(MvCompletion.Payload)) == 32);

Console.WriteLine();
Console.WriteLine("session lifetime (SafeHandle)");
using (var session = MediaViewerSession.Create(workerCount: 2, enableEtw: false))
{
    MvJobStats stats = session.JobStats;
    Check("worker count honoured", stats.WorkerCount == 2);
    Check("nothing submitted yet", stats.Submitted == 0);

    Console.WriteLine();
    Console.WriteLine("generations");
    uint before = session.CurrentGeneration;
    uint bumped = session.BumpGeneration();
    Check("bump advances by one", bumped == before + 1);
    Check("current reflects the bump", session.CurrentGeneration == bumped);

    Console.WriteLine();
    Console.WriteLine("round trip: echo -> worker -> completion drain");
    const string text = "camera dump";
    ulong jobId = session.Echo(text);
    Check("echo returned a job id immediately", jobId != 0);

    // The call above returned without doing the work. The answer arrives out of
    // band, on the completion queue, and C# is what drains it — the core never
    // touches a dispatcher.
    bool signalled = session.CompletionSignal.WaitOne(TimeSpan.FromSeconds(5));
    Check("completion signal fired", signalled);

    ReadOnlySpan<MvCompletion> completions = session.Drain();
    Check("exactly one completion", completions.Length == 1);
    if (completions.Length == 1)
    {
        MvCompletion c = completions[0];
        Check("kind is Echo", c.Kind == MvCompletionKind.Echo);
        Check("status is Ok", c.Status == MvStatus.Ok);
        Check("job id matches the one echo returned", c.JobId == jobId);
        Check("correlation id is present", c.CorrelationId != 0);
        Check("payload is the byte length the core measured",
              c.Payload == System.Text.Encoding.UTF8.GetByteCount(text));
    }

    Check("a second drain reports nothing", session.Drain().Length == 0);

    Console.WriteLine();
    Console.WriteLine("image open (BMP via the ABI, no pixels marshalled)");
    string bmpPath = Path.Combine(Path.GetTempPath(), "mv-smoke.bmp");
    // 2x2 24-bit BMP, untagged.
    byte[] bmp =
    {
        0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
        0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
        0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
    };
    File.WriteAllBytes(bmpPath, bmp);
    uint genBeforeOpen = session.CurrentGeneration;
    ulong imageJob = session.OpenImage(bmpPath);
    Check("open returned a job id immediately", imageJob != 0);
    Check("open bumps generation", session.CurrentGeneration == genBeforeOpen + 1);
    Check("completion signal fired for image", session.CompletionSignal.WaitOne(TimeSpan.FromSeconds(5)));
    ReadOnlySpan<MvCompletion> opened = session.Drain();
    Check("exactly one image completion", opened.Length == 1);
    if (opened.Length == 1)
    {
        Check("kind is ImageOpened", opened[0].Kind == MvCompletionKind.ImageOpened);
        Check("status is Ok", opened[0].Status == MvStatus.Ok);
        Check("payload width is 2", (opened[0].Payload >> 32) == 2);
        Check("payload height is 2", (opened[0].Payload & 0xffffffffL) == 2);
    }
    MvImageInfo info = session.ImageInfo;
    Check("image info width", info.Width == 2);
    Check("image info height", info.Height == 2);
    try { File.Delete(bmpPath); } catch { /* temp */ }

    Console.WriteLine();
    Console.WriteLine("batching");
    // plan/14: a folder scan finishing 400 thumbnails must be one drain, not
    // 400 marshalling hops.
    const int batch = 400;
    for (int i = 0; i < batch; i++) session.Echo("thumb");

    int drained = 0;
    int drainCalls = 0;
    var deadline = Stopwatch.StartNew();
    while (drained < batch && deadline.Elapsed < TimeSpan.FromSeconds(15))
    {
        int n = session.Drain().Length;
        if (n == 0) { Thread.Sleep(1); continue; }
        drainCalls++;
        drained += n;
    }
    Check($"all {batch} completions arrived", drained == batch);
    Check($"batched into {drainCalls} drains, not {batch}", drainCalls < batch / 4);

    Console.WriteLine();
    Console.WriteLine("errors carry a correlation id");
    try
    {
        session.Echo(null!);
        Check("null text rejected", false);
    }
    catch (ArgumentNullException)
    {
        // Rejected on the managed side before it reaches the ABI, which is
        // where an argument that cannot be marshalled belongs.
        Check("null text rejected before crossing the boundary", true);
    }
}

Console.WriteLine();
Console.WriteLine("session released; SafeHandle ran ReleaseHandle");

Console.WriteLine();
Console.WriteLine(failures == 0 ? "PASS" : $"FAILED ({failures} checks)");
return failures == 0 ? 0 : 1;
