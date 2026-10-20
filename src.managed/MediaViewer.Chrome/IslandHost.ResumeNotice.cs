// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Windows.Foundation;

namespace MediaViewer.Chrome;

/// <summary>
/// Issue #299 (docs/design/17 "Unfinished roots wait for Resume"): an index left
/// unfinished last session (a quit or a kill part-way through a large folder)
/// does not carry on by itself. Once the window is up, a small notice in the
/// bottom-right corner asks "Resume indexing Holiday?". Resume loads the pack
/// (as the first search does) and continues from the last committed frame;
/// Dismiss leaves it paused, and the next launch asks again. Not a modal.
/// </summary>
/// <remarks>
/// The question comes from the pack's <c>data/resume.json</c>, read on the
/// worker that verifies the installed add-ons at launch (StartAddons): the pack
/// itself is not loaded, so no runtime or model session opens until Resume.
/// <para>
/// The notice is a small island of its own over the canvas corner, as the AI
/// badge is (IslandHost.AiBadge.cs): chrome over the swapchain, never drawn into
/// it. Issue #443: it was a windowed <c>Popup</c> of the command-bar island,
/// which never showed on Windows. It is placed from the window's client
/// rectangle, above the filmstrip, and moved on every resize. It does not take
/// focus: the viewer keeps the keyboard. A launch read that lands before the
/// bar is up is kept and shown when the bar attaches, as the Mac's store waits
/// for its host.
/// </para>
/// The twin of the Mac's ResumeNoticeView.swift.
/// </remarks>
public static partial class IslandHost
{
    private const double ResumeNoticeWidth = 340;
    private const double ResumeNoticeMargin = 12;

    private static DesktopWindowXamlSource? _resumeNotice;
    private static FrameworkElement? _resumeNoticeCard;
    // The launch read's roots, kept while the window cannot show them yet.
    private static List<UnfinishedRoot>? _resumeRootsWaiting;
    // The notice's Resume started (or joined) the pack's load: every held root
    // resumes once it attaches.
    private static bool _aiResumePending;
    // The attached pack's mv.ai.1 table (AttachAddonChrome), for Resume while it is loaded.
    private static IntPtr _aiTable;
    // The bar island's top, and the filmstrip's while it is up (0: hidden), in
    // physical pixels (Resize, ShowFilmstrip, ResizeFilmstrip).
    private static int _barTop;
    private static int _filmstripTop;

    private sealed record UnfinishedRoot(long Id, string Path, bool Photos);

    /// <summary>[worker] The roots resume.json lists, beside index.db in the AI
    /// pack's data folder (<paramref name="versionDir"/>'s sibling "data").
    /// Empty when there is none, or it cannot be read.</summary>
    private static List<UnfinishedRoot> ReadUnfinishedRoots(string versionDir)
    {
        var roots = new List<UnfinishedRoot>();
        try
        {
            string? addonDir = System.IO.Path.GetDirectoryName(versionDir.TrimEnd('\\', '/'));
            if (string.IsNullOrEmpty(addonDir)) return roots;
            string file = System.IO.Path.Combine(addonDir, "data", "resume.json");
            if (!File.Exists(file) || new FileInfo(file).Length > (1 << 20)) return roots;
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllBytes(file));
            JsonElement top = doc.RootElement;
            if (top.ValueKind != JsonValueKind.Object || !top.TryGetProperty("version", out JsonElement v)
                || v.ValueKind != JsonValueKind.Number || v.GetInt32() != 1
                || !top.TryGetProperty("roots", out JsonElement list) || list.ValueKind != JsonValueKind.Array)
            {
                return roots;
            }
            foreach (JsonElement r in list.EnumerateArray())
            {
                if (!r.TryGetProperty("id", out JsonElement id) || id.ValueKind != JsonValueKind.Number) continue;
                string path = r.TryGetProperty("path", out JsonElement p) ? p.GetString() ?? "" : "";
                if (path.Length == 0 || id.GetInt64() <= 0) continue;
                bool photos = r.TryGetProperty("kind", out JsonElement k) && k.GetString() == "photos";
                roots.Add(new UnfinishedRoot(id.GetInt64(), path, photos));
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException
                                       or InvalidOperationException or FormatException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            roots.Clear();
        }
        return roots;
    }

    // "Photos" for the Photos library (a Mac root, kept for one index format),
    // else the folder's own name. addon/ai_resume.cpp's unfinished_name.
    private static string UnfinishedName(UnfinishedRoot root)
    {
        if (root.Photos) return "Photos";
        string trimmed = root.Path.Length > 1 ? root.Path.TrimEnd('\\', '/') : root.Path;
        string name = System.IO.Path.GetFileName(trimmed);
        return name.Length == 0 ? trimmed : name;
    }

    // "Resume indexing Holiday?", "... Holiday and Cards?", "... Holiday, Cards
    // and 2 more?" (addon/ai_resume.cpp's resume_question).
    private static string ResumeQuestion(List<UnfinishedRoot> roots)
    {
        if (roots.Count == 0) return "";
        string names = UnfinishedName(roots[0]);
        if (roots.Count == 2) names += " and " + UnfinishedName(roots[1]);
        else if (roots.Count > 2) names += $", {UnfinishedName(roots[1])} and {roots.Count - 2} more";
        return $"Resume indexing {names}?";
    }

    /// <summary>UI thread, once at launch (StartAddons): the notice, when the
    /// verified pack left roots unfinished. Not in a later window, which indexes nothing.</summary>
    private static void MaybeShowResumeNotice(List<UnfinishedRoot> roots)
    {
        if (roots.Count == 0 || _addonsElsewhere || !AiSlot.Usable || _resumeNotice is not null) return;
        // The bar (and the window under it) not up yet: asked once it is (Attach).
        if (_themeWindow == IntPtr.Zero || _source?.Content is not FrameworkElement)
        {
            _resumeRootsWaiting = roots;
            return;
        }
        _resumeRootsWaiting = null;
        string question = ResumeQuestion(roots);
        try
        {
            _resumeNotice = new DesktopWindowXamlSource();
            _resumeNotice.Initialize(Microsoft.UI.Win32Interop.GetWindowIdFromWindow(_themeWindow));
            _resumeNotice.TakeFocusRequested += OnTakeFocusRequested;
            // Parked off the client area until it is placed: a default
            // full-client island would flash over the canvas.
            MoveAt(_resumeNotice, 0, ClientHeightPx(), 1, 1);
            _resumeNoticeCard = BuildResumeNoticeCard(question);
            // Measured for real once it is in the island's live tree: placed again then.
            _resumeNoticeCard.Loaded += (_, _) => Guarded(PlaceCornerCards);
            // Behind the card's rounded corners, the canvas colour (the AI badge's).
            _resumeNotice.Content = new Grid { Background = Brush(Canvas), Children = { _resumeNoticeCard } };
            PlaceCornerCards();  // the restart and hint cards stack above it (#510)
            // Said once as it appears, not only when focused.
            FrameworkElementAutomationPeer.FromElement(_resumeNoticeCard)?.RaiseNotificationEvent(
                AutomationNotificationKind.ActionCompleted, AutomationNotificationProcessing.ImportantMostRecent,
                question, "ai-resume");
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            CloseResumeNotice();
        }
    }

    /// <summary>Attach (the bar is up): a launch read that came first is shown now.</summary>
    private static void ShowWaitingResumeNotice()
    {
        if (_resumeRootsWaiting is List<UnfinishedRoot> roots) MaybeShowResumeNotice(roots);
    }

    private static FrameworkElement BuildResumeNoticeCard(string question)
    {
        var stack = new StackPanel { Spacing = Space(8) };
        stack.Children.Add(new TextBlock
        {
            Text = question,
            FontFamily = UiFont,
            FontSize = UiFontSize + 1,
            Foreground = Brush(Title),
            TextWrapping = TextWrapping.Wrap,
        });
        stack.Children.Add(new TextBlock
        {
            Text = "Local search stopped part-way last time. It stays paused until you resume it.",
            FontFamily = UiFont,
            FontSize = UiFontSize - 2,
            Foreground = Brush(Body),
            TextWrapping = TextWrapping.Wrap,
        });
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = Space(8),
            HorizontalAlignment = HorizontalAlignment.Right,
        };
        Button dismiss = TextButton("Dismiss", DismissResumeNotice);
        ToolTipService.SetToolTip(dismiss, "Leave it paused. You can resume it from Settings → Local search.");
        Button resume = TextButton("Resume", ResumeFromNotice);
        foreach (Button b in new[] { dismiss, resume })
        {
            b.IsTabStop = true;
            b.AllowFocusOnInteraction = true;
            buttons.Children.Add(b);
        }
        stack.Children.Add(buttons);
        var card = new Border
        {
            Width = ResumeNoticeWidth,
            Padding = Pad(14),
            CornerRadius = Corner(8),
            BorderThickness = new Thickness(1),
            BorderBrush = Brush(Hairline),
            Background = Brush(ChromeColour.Surface),
            Child = stack,
        };
        AutomationProperties.SetName(card, question);
        return card;
    }

    /// <summary>The notice's bottom-right corner on the window's, above the
    /// filmstrip when it is up. The island is a child of the main window, so
    /// its rectangle is client pixels.</summary>
    private static void PlaceResumeNotice()
    {
        if (_resumeNotice is null || _resumeNoticeCard is null) return;
        if (_themeWindow == IntPtr.Zero || !GetClientRect(_themeWindow, out NativeRect client)) return;
        uint dpi = GetDpiForWindow(_themeWindow);
        double scale = dpi > 0 ? dpi / 96.0 : 1;
        _resumeNoticeCard.Measure(new Size(ResumeNoticeWidth, double.PositiveInfinity));
        double height = _resumeNoticeCard.DesiredSize.Height;
        if (height <= 0) height = 140;  // not measured yet: about the card's two lines and buttons
        int width = (int)Math.Ceiling(ResumeNoticeWidth * scale);
        int tall = (int)Math.Ceiling(height * scale);
        int margin = (int)Math.Round(ResumeNoticeMargin * scale);
        int clientWidth = client.Right - client.Left;
        int bottom = _filmstripTop > 0 ? _filmstripTop : client.Bottom - client.Top;
        MoveAt(_resumeNotice, Math.Max(0, clientWidth - width - margin), Math.Max(0, bottom - margin - tall), width, tall);
        // Above the gallery and the panes, as the Mac's is ("last in").
        try { _resumeNotice.SiteBridge?.MoveInZOrderAtTop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    private static int ClientHeightPx() =>
        _themeWindow != IntPtr.Zero && GetClientRect(_themeWindow, out NativeRect r) ? r.Bottom - r.Top : 0;

    // ShowFilmstrip / ResizeFilmstrip: where the strip's top now is (0 hidden),
    // so the notice sits above it.
    private static void NoteFilmstripTop(bool visible, int y)
    {
        _filmstripTop = visible ? Math.Max(0, y) : 0;
        PlaceCornerCards();
    }

    private static void ResumeFromNotice()
    {
        CloseResumeNotice();
        if (_addonsElsewhere) return;
        if (AiSlot.Chrome is not null)
        {
            ResumeHeldRoots(_aiTable);
            return;
        }
        // The pack loads as at the first search (no panel opens); the held roots
        // resume once it attaches (AttachAddonChrome → OnAiAttachedForResume).
        if (AiStarting() || StartAiOnDemand(open: false)) _aiResumePending = true;
    }

    private static void DismissResumeNotice() => CloseResumeNotice();

    private static void CloseResumeNotice()
    {
        _resumeRootsWaiting = null;
        DesktopWindowXamlSource? island = _resumeNotice;
        _resumeNotice = null;
        _resumeNoticeCard = null;
        if (island is null) return;
        // Parked at once; disposed after the click that closed it has returned
        // (an island disposed inside its own button's Click is not safe).
        try { MoveAt(island, 0, ClientHeightPx(), 1, 1); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        island.TakeFocusRequested -= OnTakeFocusRequested;
        RestoreCanvasFocus();
        DispatcherQueueControllerTryEnqueue(() =>
        {
            DesktopWindowXamlSource? gone = island;
            DisposeSource(ref gone);
        });
        PlaceRestartNotice();  // the cards stacked above it come down
        PlaceHintNotice();
    }

    /// <summary>Detach (the window is going): no island outlives it.</summary>
    private static void DropResumeNotice()
    {
        _resumeRootsWaiting = null;
        _resumeNoticeCard = null;
        if (_resumeNotice is not null) _resumeNotice.TakeFocusRequested -= OnTakeFocusRequested;
        DisposeSource(ref _resumeNotice);
    }

    /// <summary>AttachAddonChrome, the pack attached: the notice's Resume, if it asked.</summary>
    private static void OnAiAttachedForResume(IntPtr table)
    {
        _aiTable = table;
        if (!_aiResumePending) return;
        _aiResumePending = false;
        ResumeHeldRoots(table);
    }

    private static void OnAiDetachedForResume()
    {
        _aiTable = IntPtr.Zero;
        _aiResumePending = false;
    }

    // mv.ai.1 set_setting "resume_root" 0: every held root, from its last committed frame.
    private static void ResumeHeldRoots(IntPtr table)
    {
        if (table == IntPtr.Zero) return;
        try { new AiApi(table).SetSetting("resume_root", "0"); }
        catch (Exception ex) when (ex is MediaViewerException or InvalidOperationException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left, Top, Right, Bottom;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(IntPtr hWnd, out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hWnd);
}
