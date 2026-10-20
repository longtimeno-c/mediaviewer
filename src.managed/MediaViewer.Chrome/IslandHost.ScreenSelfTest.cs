// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// A developer's soak of Driftwire's window (#508), off unless
/// <c>MV_SCREEN_SELFTEST=&lt;folder&gt;</c> is set: open the window with the files
/// of <c>MV_SCREEN_SELFTEST_FILES</c>, turn the internet on, make a code, send
/// those files to that code (this computer receives them into
/// <c>&lt;folder&gt;\received</c>), close; <c>MV_SCREEN_SELFTEST_CYCLES</c> times
/// (20). Each step is written to <c>&lt;folder&gt;\log.txt</c> and the window is
/// captured with PrintWindow at each step of the first cycle and the last.
/// Nothing here runs in a normal start.
/// </summary>
public static partial class IslandHost
{
    private static bool _screenSelfTestStarted;
    private static readonly string ScreenSelfTestDir = Environment.GetEnvironmentVariable("MV_SCREEN_SELFTEST") ?? "";

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);

    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfoHeader
    {
        public int Size, Width, Height;
        public short Planes, BitCount;
        public int Compression, SizeImage, XPelsPerMeter, YPelsPerMeter, ClrUsed, ClrImportant;
    }

    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr hdc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateDIBSection(IntPtr hdc, ref BitmapInfoHeader bmi, uint usage, out IntPtr bits, IntPtr section, uint offset);
    [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr hdc);

    private static void MaybeStartScreenSelfTest()
    {
        if (_screenSelfTestStarted || ScreenSelfTestDir.Length == 0) return;
        string dir = ScreenSelfTestDir;
        _screenSelfTestStarted = true;
        Directory.CreateDirectory(dir);
        string files = Environment.GetEnvironmentVariable("MV_SCREEN_SELFTEST_FILES") ?? "";
        int cycles = int.TryParse(Environment.GetEnvironmentVariable("MV_SCREEN_SELFTEST_CYCLES"), out int c) && c > 0 ? c : 20;
        _ = Task.Run(() => RunScreenSelfTest(dir, files, cycles));
    }

    private static Task<T> OnUi<T>(Func<T> f)
    {
        var done = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        DispatcherQueueControllerTryEnqueue(() =>
        {
            try { done.SetResult(f()); }
            catch (Exception ex) { done.SetException(ex); }
        });
        return done.Task;
    }

    private static readonly ScreenAt SelfTestAt = new(DriftwireId, "driftwire");

    // The tree's node of this id, as JSON text, or null.
    private static JsonElement? SelfTestNode(JsonElement n, string id)
    {
        if (n.ValueKind != JsonValueKind.Object) return null;
        if (JStr(n, "id") == id) return n;
        foreach (JsonElement c in JArr(n, "children"))
        {
            if (SelfTestNode(c, id) is { } hit) return hit;
        }
        return null;
    }

    private static (bool Open, string? Text, HashSet<string> Ids) SelfTestTree(string textOf)
    {
        var ids = new HashSet<string>(StringComparer.Ordinal);
        string json = SlotNative.ScreensJson();
        if (json.Length == 0) return (false, null, ids);
        using JsonDocument doc = JsonDocument.Parse(json);
        foreach (JsonElement s in doc.RootElement.EnumerateArray())
        {
            if (JStr(s, "addon") != SelfTestAt.Addon || JStr(s, "id") != SelfTestAt.Screen) continue;
            if (!s.TryGetProperty("tree", out JsonElement tree)) return (true, null, ids);
            void Walk(JsonElement n)
            {
                if (n.ValueKind != JsonValueKind.Object) return;
                if (JStr(n, "id") is { Length: > 0 } id && !JFlag(n, "disabled")) ids.Add(id);
                foreach (JsonElement c in JArr(n, "children")) Walk(c);
            }
            Walk(tree);
            string? text = SelfTestNode(tree, textOf) is { } t ? JStr(t, "text") : null;
            return (true, text, ids);
        }
        return (false, null, ids);
    }

    private static async Task RunScreenSelfTest(string dir, string filesDir, int cycles)
    {
        Directory.CreateDirectory(dir);
        string received = Path.Combine(dir, "received");
        Directory.CreateDirectory(received);
        string logPath = Path.Combine(dir, "log.txt");
        var log = new System.Text.StringBuilder();
        void Log(string line)
        {
            log.AppendLine($"{DateTime.Now:HH:mm:ss.fff} {line}");
            try { File.WriteAllText(logPath, log.ToString()); } catch (IOException) { }
        }
        List<string> files = Directory.Exists(filesDir)
            ? Directory.EnumerateFiles(filesDir).OrderBy(f => f, StringComparer.Ordinal).Take(3).ToList()
            : new List<string>();
        Log($"start: {cycles} cycles, {files.Count} files");
        var wait = System.Diagnostics.Stopwatch.StartNew();
        while (!await OnUi(() => DriftwireReady))
        {
            if (wait.ElapsedMilliseconds > 60000)
            {
                Log("Driftwire never became ready: " + await OnUi(() =>
                    string.Join(", ", SlotAddonRows.Select(r => $"{r.Id} {r.State}")) +
                    $"; loaded [{string.Join(", ", SlotAddonsLoaded)}]; screens [{string.Join(", ", SlotWindowScreens.Keys)}]"));
                return;
            }
            await Task.Delay(250);
        }
        Log($"ready after {wait.ElapsedMilliseconds} ms");

        async Task Capture(string name)
        {
            IntPtr hwnd = await OnUi(() => ScreenWindows.TryGetValue(SelfTestAt, out var w)
                ? WinRT.Interop.WindowNative.GetWindowHandle(w.Window) : IntPtr.Zero);
            if (hwnd == IntPtr.Zero) return;
            await Task.Delay(400);  // the last rebuild laid out and drawn
            if (!SlotGetWindowRect(hwnd, out SlotRect r)) return;
            int width = r.Right - r.Left, height = r.Bottom - r.Top;
            if (width <= 0 || height <= 0) return;
            IntPtr mem = CreateCompatibleDC(IntPtr.Zero);
            var info = new BitmapInfoHeader
            {
                Size = 40, Width = width, Height = -height, Planes = 1, BitCount = 32,
            };
            IntPtr dib = CreateDIBSection(mem, ref info, 0, out IntPtr bits, IntPtr.Zero, 0);
            IntPtr old = SelectObject(mem, dib);
            PrintWindow(hwnd, mem, 2);  // PW_RENDERFULLCONTENT: the composed XAML
            byte[] px = new byte[width * height * 4];
            Marshal.Copy(bits, px, 0, px.Length);
            SelectObject(mem, old);
            DeleteObject(dib);
            DeleteDC(mem);
            using var f = new BinaryWriter(File.Create(Path.Combine(dir, name + ".bmp")));
            f.Write((ushort)0x4D42); f.Write(54 + px.Length); f.Write(0); f.Write(54);
            f.Write(40); f.Write(width); f.Write(-height); f.Write((ushort)1); f.Write((ushort)32);
            f.Write(0); f.Write(px.Length); f.Write(2835); f.Write(2835); f.Write(0); f.Write(0);
            f.Write(px);
        }

        async Task<bool> Until(Func<HashSet<string>, bool> ok, int ms)
        {
            var clock = System.Diagnostics.Stopwatch.StartNew();
            while (clock.ElapsedMilliseconds < ms)
            {
                var (_, _, ids) = await OnUi(() => SelfTestTree(""));
                if (ok(ids)) return true;
                await Task.Delay(150);
            }
            return false;
        }

        bool Ev(string node, uint kind, string value) => SlotNative.Event(SelfTestAt.Addon, SelfTestAt.Screen, node, kind, value);

        int sent = 0;
        for (int i = 1; i <= cycles; ++i)
        {
            bool shoot = i == 1 || i == cycles;
            string tag = $"c{i:00}";
            try
            {
                await OnUi(() => OpenDriftwire(files));
                bool open = await Until(ids => ids.Contains("send"), 8000) &&
                            await OnUi(() => ScreenWindows.ContainsKey(SelfTestAt));
                Log($"{tag} open: {open}");
                if (!open) continue;
                if (shoot) await Capture($"{tag}-1-idle");
                await OnUi(() => Ev("internet", SlotNative.EventChange, "true"));
                bool on = await Until(ids => ids.Contains("make_code") || ids.Contains("code_text"), 5000);
                Log($"{tag} internet on: {on}");
                if (shoot) await Capture($"{tag}-2-internet");
                await OnUi(() => Ev("make_code", SlotNative.EventClick, ""));
                bool made = await Until(ids => ids.Contains("code_text"), 30000);
                string code = (await OnUi(() => SelfTestTree("code_text"))).Text ?? "";
                Log($"{tag} code made: {made}");
                if (shoot) await Capture($"{tag}-3-code");
                if (made && files.Count > 0)
                {
                    await OnUi(() => Ev("send_code", SlotNative.EventChange, JsonString(code)));
                    await OnUi(() => Ev("send_to_code", SlotNative.EventClick, ""));
                    // Both halves are this window: confirm the name, then accept into `received`.
                    var clock = System.Diagnostics.Stopwatch.StartNew();
                    bool confirmed = false, accepted = false, done = false, shotSend = false;
                    while (clock.ElapsedMilliseconds < 45000 && !done)
                    {
                        var (_, _, ids) = await OnUi(() => SelfTestTree(""));
                        if (ids.Contains("out_confirm") && !confirmed)
                        {
                            if (shoot) await Capture($"{tag}-4-confirm");
                            await OnUi(() => Ev("out_confirm", SlotNative.EventClick, ""));
                            confirmed = true;
                        }
                        if (ids.Contains("destination") && !accepted)
                        {
                            await OnUi(() => Ev("destination", SlotNative.EventChange, JsonString(received)));
                            await Task.Delay(300);
                            if (shoot) await Capture($"{tag}-5-offer");
                            await OnUi(() => Ev("accept", SlotNative.EventClick, ""));
                            accepted = true;
                        }
                        if (accepted && !shotSend && (ids.Contains("in_cancel") || ids.Contains("out_cancel")))
                        {
                            if (shoot) await Capture($"{tag}-6-moving");
                            shotSend = true;
                        }
                        if (ids.Contains("in_dismiss")) done = true;
                        // Ended without an offer (the code refused): nothing more will come.
                        if (clock.ElapsedMilliseconds > 2000 && !accepted && !ids.Contains("out_cancel") &&
                            !ids.Contains("decline")) break;
                        await Task.Delay(100);
                    }
                    Log($"{tag} send: confirmed {confirmed}, accepted {accepted}, done {done}");
                    if (!done)
                    {
                        if (shoot) await Capture($"{tag}-4-send-ended");
                        foreach (string note in new[] { "send_code_note", "out_note", "out_reaching", "in_note", "in_title" })
                        {
                            if ((await OnUi(() => SelfTestTree(note))).Text is { Length: > 0 } t) Log($"{tag}   {note}: {t}");
                        }
                        await OnUi(() => Ev("out_cancel", SlotNative.EventClick, ""));
                    }
                    if (done)
                    {
                        ++sent;
                        if (shoot) await Capture($"{tag}-7-received");
                        await OnUi(() => Ev("in_dismiss", SlotNative.EventClick, ""));
                    }
                }
                // Scroll to the bottom, flip a switch: the rebuilt window must stay there.
                await OnUi(() => ScrollRootTo(double.MaxValue));
                await Task.Delay(300);  // the scroll has landed, as a person's has
                double before = await OnUi(() => ScrollRootTo(-1));
                // Two revisions back to back: the second must keep the first's place.
                await OnUi(() => Ev("handover", SlotNative.EventChange, "true"));
                await OnUi(() => Ev("relay", SlotNative.EventChange, "true"));
                await Task.Delay(400);
                await OnUi(() => Ev("relay", SlotNative.EventChange, "false"));
                await Task.Delay(800);
                double after = await OnUi(() => ScrollRootTo(-1));
                Log($"{tag} scroll kept across a rebuild: {before:0} -> {after:0}");
                if (shoot) await Capture($"{tag}-8-bottom");
                await OnUi(() => Ev("handover", SlotNative.EventChange, "false"));
                await OnUi(() => Ev("internet", SlotNative.EventChange, "false"));
                await Task.Delay(300);
                await OnUi(() => SlotNative.CloseScreen(SelfTestAt.Addon, SelfTestAt.Screen));
                bool closed = await Until(ids => ids.Count == 0, 5000) &&
                              !await OnUi(() => ScreenWindows.ContainsKey(SelfTestAt));
                Log($"{tag} closed: {closed}");
                await Task.Delay(300);
            }
            catch (Exception ex)
            {
                Log($"{tag} exception: {ex.GetType().Name}: {ex.Message}");
            }
        }
        Log($"end: {sent} of {cycles} sends received");
    }

    // UI thread: scrolls the open screen's root to `to` (MaxValue: the end; < 0:
    // stays) and returns where it is.
    private static double ScrollRootTo(double to)
    {
        if (!ScreenWindows.TryGetValue(SelfTestAt, out var w) || w.Window.Content is not UIElement content ||
            !ScreenIdsOf.TryGetValue(content, out var ids) || !ids.TryGetValue("root", out FrameworkElement? e) ||
            e is not ScrollViewer sv)
        {
            return -1;
        }
        if (to >= 0) sv.ChangeView(null, Math.Min(to, sv.ScrollableHeight), null, disableAnimation: true);
        return to >= 0 ? Math.Min(to, sv.ScrollableHeight) : sv.VerticalOffset;
    }
}
