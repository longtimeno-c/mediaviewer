// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace MediaViewer.Chrome;

/// <summary>
/// Milestone H, the base app's half of Local search (docs/design/17 "The AI pack",
/// "UI and commands"): Settings → Local search (install / remove per piece,
/// the 5 GB family budget, the add-on's own management panel once loaded),
/// the command-bar indexing pill, the match dots over the scrub bar, and the
/// IAddonHost2 services the AI chrome calls.
/// </summary>
/// <remarks>
/// With no piece installed this file adds exactly one thing to the app: the
/// Settings section that offers the install. No key, no bar item, no menu
/// (docs/design/17: "Nothing about it appears elsewhere in the UI until the core pack
/// is installed"). Nothing downloads before the Install click, and the
/// download carries no identifier (rule 6).
/// </remarks>
public static partial class IslandHost
{
    private const string AiFamily = "ai";

    private static StackPanel? _localSearchPanel;
    private static TextBlock? _localSearchStatus;
    private static UIElement? _aiSettingsPanel;
    private static bool? _nvidiaPresent;
    private static ulong _aiUsed, _aiCeiling;
    private static bool _confirmCoreRemove;
    // People, Audio or NVIDIA acceleration whose "Remove…" was pressed: asked
    // first, as the Mac does (LocalSearchView removeConfirm).
    private static AddonSlot? _confirmPieceRemove;
    // Installs run one at a time (no fight over bandwidth, and each piece's
    // 5 GB check counts the one before it); more Install clicks queue here,
    // Core first (owner request, 2026-09-28: Install on all three at once).
    private static readonly List<AddonSlot> _aiQueue = new();
    private static AddonSlot? _aiInstalling;
    // What each install of the current batch said, shown together.
    private static readonly List<string> _aiBatchNotes = new();

    // ---- listing state the AI chrome reads (IslandHost.Filmstrip.cs sets it) ----

    // The last directory the viewer listed; kept while a result list is open,
    // so "Folder & subfolders" still means the folder the search started from.
    private static string _openedFolder = "";
    private static bool _listOpen;
    // Enter (viewer) or Ctrl+Enter (gallery) from the search panel: applied
    // when the list's FolderReady lands, not before, so the grid never shows
    // the previous folder for a frame.
    private static bool? _pendingListGallery;

    private static event Action? HostThemeChanged;

    private static ISearchChrome? SearchChrome => AiSlot.Chrome as ISearchChrome;

    // The path bar's search icon was clicked while the pack was still starting:
    // the panel opens when it attaches.
    private static bool _searchOpenPending;

    // Installed, verified and being loaded, not yet attached.
    private static bool AiStarting() =>
        AiSlot.Chrome is null && AiSlot.Busy && !AiSlot.Removing && _aiInstalling != AiSlot && AiSlot.Usable;

    // Installed and verified at launch, and waiting, unloaded, for the first
    // search (issue #292, docs/design/17 "Starting on demand"). A later window
    // has its own on-demand reader (EnsureReaderStarted).
    private static bool AiOnDemand() =>
        !_addonsElsewhere && AiSlot.Chrome is null && !AiSlot.Busy && !AiSlot.Removing && _aiInstalling != AiSlot
        && AiSlot.Usable;

    private const string AiStartingText = "Starting Local search…";

    // The first search after launch, or Settings' Start now: the pack loads.
    // `open`: its panel opens once it attaches. True when a load is on its way.
    private static bool StartAiOnDemand(bool open)
    {
        if (!AiOnDemand()) return false;
        LoadAddon(AiSlot);
        if (!AiSlot.Busy) return false;
        if (open) _searchOpenPending = true;
        return true;
    }

    // The path bar's search icon: the panel now, or once the pack attaches;
    // without Local search, file search (IslandHost.FileSearch.cs), the
    // gallery coming up first when it is hidden.
    private static void OpenSearchFromPath()
    {
        if (OpenFileSearch() == FileSearchNeedsGallery && !_galleryVisible) Send(Command.ToggleGallery);
    }

    // A load of the AI pack started or failed: the icon follows.
    private static void OnSearchLoadChanged()
    {
        if (SearchChrome is null && !AiStarting()) _searchOpenPending = false;
        UpdatePathSearchButton();
        // Settings says "Starting Local search…" while it does.
        if (_settingsVisible) RefreshLocalSearch();
    }

    // ---- IAddonHost2 services ------------------------------------------------------

    private static AddonCurrentItem HostCurrentItem()
    {
        string path = _selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex].Path : "";
        if (_folderSession is null || path.Length == 0) return new AddonCurrentItem(path, false, -1, true);
        if (!_videoActive) return new AddonCurrentItem(path, false, -1, true);
        try
        {
            long ms = Math.Max(0, _folderSession.VideoPosition) / 1_000_000;
            return new AddonCurrentItem(path, true, ms, _folderSession.VideoState != 1);
        }
        catch (MediaViewerException)
        {
            return new AddonCurrentItem(path, false, -1, true);
        }
    }

    private static void HostOpenList(string title, IReadOnlyList<string> paths, IReadOnlyList<long>? momentsMs,
                                     int selectIndex, bool gallery)
    {
        if (_folderSession is null || paths.Count == 0) return;
        try
        {
            _pendingListGallery = gallery;
            _folderSession.OpenList(title, paths, momentsMs, (uint)Math.Clamp(selectIndex, 0, paths.Count - 1));
        }
        catch (Exception ex) when (ex is MediaViewerException or EntryPointNotFoundException)
        {
            // An older core without 0.14 result listings.
            _pendingListGallery = null;
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return;
        }
        // A list opened from Settings or People (a person's photos) is what
        // the person asked to see: Settings steps aside (owner report,
        // 2026-09-27). open_settings toggles.
        if (_settingsVisible) Send(Command.OpenSettings);
        ActivateMainWindow();
    }

    // FolderReady for a result list (ReloadItems): the view the panel asked for.
    private static void ApplyPendingListView()
    {
        if (_pendingListGallery is not bool gallery || !_listOpen) return;
        _pendingListGallery = null;
        if (gallery != _galleryVisible) Send(gallery ? Command.ToggleGallery : Command.CloseGallery);
    }

    private static void HostSeekVideo(long ms, bool exact)
    {
        if (_folderSession is null || !_videoActive) return;
        try { _folderSession.VideoSeek(Math.Max(0, ms) * 1_000_000, exact); }
        catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
    }

    private static void HostShowLocalSearchSettings()
    {
        _settingsKeyboard = false;
        // #510: Local search's own page (PresentSettings opens it when Settings shows).
        if (!_settingsVisible)
        {
            _addonPageWanted = LocalSearchPageId;
            Send(Command.OpenSettings);
            return;
        }
        _showGeneralSettings?.Invoke();
        OpenAddonPage(LocalSearchPageId);
    }

    private static uint HostColour(AddonColour role)
    {
        ChromeColour c = role switch
        {
            AddonColour.Canvas => ChromeColour.Canvas,
            AddonColour.PanelBg => ChromeColour.PanelBg,
            AddonColour.Surface => ChromeColour.Surface,
            AddonColour.Title => ChromeColour.Title,
            AddonColour.Body => ChromeColour.Body,
            AddonColour.Hairline => ChromeColour.Hairline,
            AddonColour.Selection => ChromeColour.Selection,
            _ => ChromeColour.TrimAccent,
        };
        if (!ThemeBrushes.TryGetValue(c, out SolidColorBrush? brush)) return 0xFF808080;
        Color v = brush.Color;
        return (uint)(v.A << 24 | v.R << 16 | v.G << 8 | v.B);
    }

    private static string HostUiFontSource() => UiFont.Source;

    private static bool HostAnimationsEnabled()
    {
        try { return _themeSettings?.AnimationsEnabled ?? true; }
        catch (COMException) { return true; }
    }

    // RefreshTheme (IslandHost.Theme.cs) calls this after the brushes move.
    private static void RaiseHostThemeChanged()
    {
        try { HostThemeChanged?.Invoke(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    private static void ActivateMainWindow()
    {
        if (_themeWindow == IntPtr.Zero) return;
        SetForegroundWindow(_themeWindow);
        SetFocus(_themeWindow);
    }

    private static void OnSearchChromeAttached()
    {
        _aiSettingsPanel = null;
        // #355: audio files are indexed only while File types "Show audio files" is on.
        try { SearchChrome?.OnShowAudioChanged(HasFlag(SettingFlag.ShowAudio)); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        // #354: documents likewise, behind "Show documents".
        try { SearchChrome?.OnShowDocumentsChanged(HasFlag(SettingFlag.ShowDocuments)); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        if (_openedFolder.Length > 0)
        {
            try { SearchChrome?.OnFolderChanged(_openedFolder); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        }
        // The folder trail's search icon (IslandHost.Gallery.cs).
        UpdatePathSearchButton();
        if (_searchOpenPending)
        {
            _searchOpenPending = false;
            try { SearchChrome?.RunCommand(SearchCommand.Open); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        }
    }

    private static void OnSearchChromeDetached()
    {
        _aiSettingsPanel = null;
        HostSetIndexingPill(null, false, -1, false);
        HostSetScrubMarkers(Array.Empty<long>(), -1);
        _pendingListGallery = null;
        _searchOpenPending = false;
        UpdatePathSearchButton();
    }

    // From the drain, after a listing or a selection landed.
    private static void NotifySearchFolder(string dir)
    {
        try { SearchChrome?.OnFolderChanged(dir); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    private static void NotifySearchItem()
    {
        // A different item: its markers belong to the chrome to redraw.
        HostSetScrubMarkers(Array.Empty<long>(), -1);
        if (SearchChrome is not ISearchChrome search) return;
        string? path = _selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex].Path : null;
        try { search.OnItemChanged(path); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    // ---- the command-bar pill ---------------------------------------------------------

    private static Button? _indexingPill;
    private static IndexingRing? _indexingRing;
    private static TextBlock? _indexingText;
    private static bool _indexingShown;
    private static bool _indexingOnBattery;
    private static Storyboard? _indexingFade;

    private static UIElement BuildIndexingPill()
    {
        _indexingRing = new IndexingRing(14, 2);
        _indexingText = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = 13,
            Foreground = Brush(Title),
            VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 360,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(7) };
        row.Children.Add(_indexingRing.Root);
        row.Children.Add(_indexingText);
        // Starting (issue #292) or indexing: what Ctrl+F does.
        _indexingPill = TextButton("", OpenSearchFromPath);
        _indexingPill.Content = new Border
        {
            Child = row,
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(12),  // a pill (the Mac's Capsule), not a theme radius
            Padding = Pad(10, 3, 12, 3),
        };
        _indexingPill.Padding = Pad(4, 0, 4, 0);
        _indexingPill.Visibility = Visibility.Collapsed;
        _indexingPill.Opacity = 0;
        _indexingShown = false;
        // The Mac's context menu: Open search, and Index anyway while it waits
        // on battery. A plain MenuFlyout (it loads here; the bar's own menus
        // are MenuFlyouts), filled as it opens so it says what is true now.
        var menu = new MenuFlyout();
        menu.Opening += (_, _) => Guarded(() => FillIndexingMenu(menu));
        _indexingPill.ContextFlyout = menu;
        AutomationProperties.SetName(_indexingPill, "Local search indexing. Open search");
        RefreshIndexingTip();
        return _indexingPill;
    }

    private static void FillIndexingMenu(MenuFlyout menu)
    {
        menu.Items.Clear();
        var open = new MenuFlyoutItem { Text = "Open search" };
        open.Click += (_, _) => Guarded(() => SearchChrome?.RunCommand(SearchCommand.Open));
        menu.Items.Add(open);
        if (_indexingOnBattery)
        {
            var anyway = new MenuFlyoutItem { Text = "Index anyway, on battery" };
            anyway.Click += (_, _) => Guarded(() => SearchChrome?.RunCommand(SearchCommand.IndexAnyway));
            menu.Items.Add(anyway);
        }
    }

    /// <param name="fraction">0…1 for the ring's arc; negative from a chrome
    /// that sends none (IAddonHost2.SetIndexingStatus), which draws no arc.</param>
    private static void HostSetIndexingPill(string? text, bool busy, double fraction, bool onBattery)
    {
        NoteIndexingBusy(text is not null && busy);  // #510: Restart now waits for it
        if (_indexingPill is null || _indexingText is null || _indexingRing is null) return;
        _indexingOnBattery = text is not null && onBattery;
        if (text is not null && text != _indexingText.Text)
        {
            _indexingText.Text = text;
            RefreshIndexingTip();
            AutomationProperties.SetName(_indexingPill, text + ". Open search");
        }
        if (text is not null) _indexingText.Text = text;
        _indexingRing.Set(fraction, busy && HostAnimationsEnabled());
        FadeIndexingPill(text is not null);
    }

    // The Mac fades the pill in and out (LocalSearchBarItem's transition): a
    // short opacity storyboard, 150 ms with Windows' animations off as the
    // Mac's Reduce Motion one. Hidden means collapsed, so a hidden pill's ring
    // is unloaded and stops.
    private static void FadeIndexingPill(bool show)
    {
        if (_indexingPill is not Button pill || show == _indexingShown) return;
        _indexingShown = show;
        _indexingFade?.Stop();
        _indexingFade = null;
        if (show) pill.Visibility = Visibility.Visible;
        try
        {
            var fade = new DoubleAnimation
            {
                To = show ? 1 : 0,
                Duration = new Duration(TimeSpan.FromMilliseconds(HostAnimationsEnabled() ? 220 : 150)),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
            };
            Storyboard.SetTarget(fade, pill);
            Storyboard.SetTargetProperty(fade, "Opacity");
            var board = new Storyboard();
            board.Children.Add(fade);
            board.Completed += (_, _) =>
            {
                if (_indexingFade != board) return;
                _indexingFade = null;
                pill.Opacity = show ? 1 : 0;
                if (!show) pill.Visibility = Visibility.Collapsed;
            };
            _indexingFade = board;
            board.Begin();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            _indexingFade = null;
            pill.Opacity = show ? 1 : 0;
            pill.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    /// <summary>
    /// The pill's ring (the Mac's ProgressRing, LocalSearchView.swift): a faint
    /// track, the pass's progress as an accent arc from 12 o'clock and, while
    /// work runs, a short dimmer arc that turns. Ellipse and Path arcs, not the
    /// WinUI control: that one has no default style in this island host and
    /// crashed the app the first time this pill showed. The turn is a
    /// RotateTransform storyboard that runs only while busy and loaded, so a
    /// paused or hidden pill costs nothing; with animations off it never turns.
    /// </summary>
    private sealed class IndexingRing
    {
        private readonly double _size;
        private readonly double _line;
        private readonly Microsoft.UI.Xaml.Shapes.Path _arc;
        private readonly Microsoft.UI.Xaml.Shapes.Path _spinner;
        private readonly RotateTransform _turn;
        private Storyboard? _spin;
        private double _fraction = -2;
        private bool _spinning;

        public Grid Root { get; }

        public IndexingRing(double size, double line)
        {
            _size = size;
            _line = line;
            Root = new Grid { Width = size, Height = size, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false };
            Root.Children.Add(new Ellipse
            {
                Width = size - line,
                Height = size - line,
                Margin = new Thickness(line / 2),
                Stroke = Brush(Hairline),
                StrokeThickness = line,
            });
            _arc = new Microsoft.UI.Xaml.Shapes.Path
            {
                Stroke = Brush(ChromeColour.TrimAccent),
                StrokeThickness = line,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
            };
            Root.Children.Add(_arc);
            _turn = new RotateTransform { CenterX = size / 2, CenterY = size / 2 };
            _spinner = new Microsoft.UI.Xaml.Shapes.Path
            {
                Data = Arc(0.18),
                Stroke = Brush(ChromeColour.TrimAccent),
                Opacity = 0.55,
                StrokeThickness = line,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
                RenderTransform = _turn,
                Visibility = Visibility.Collapsed,
            };
            Root.Children.Add(_spinner);
            Root.Loaded += (_, _) => Apply();
            Root.Unloaded += (_, _) => Stop();
            AutomationProperties.SetAccessibilityView(Root, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        }

        public void Set(double fraction, bool spinning)
        {
            if (Math.Abs(fraction - _fraction) >= 0.001)
            {
                _fraction = fraction;
                // Never empty while shown, as the Mac's trim(to: max(0.04, p)).
                _arc.Data = fraction < 0 ? null : Arc(Math.Max(0.04, Math.Min(1, fraction)));
            }
            if (spinning == _spinning) return;
            _spinning = spinning;
            Apply();
        }

        private void Apply()
        {
            _spinner.Visibility = _spinning ? Visibility.Visible : Visibility.Collapsed;
            if (_spinning && Root.IsLoaded) Start();
            else Stop();
        }

        private void Start()
        {
            if (_spin is not null) return;
            try
            {
                var turn = new DoubleAnimation
                {
                    From = 0,
                    To = 360,
                    Duration = new Duration(TimeSpan.FromMilliseconds(1100)),
                    RepeatBehavior = RepeatBehavior.Forever,
                };
                Storyboard.SetTarget(turn, _turn);
                Storyboard.SetTargetProperty(turn, "Angle");
                _spin = new Storyboard();
                _spin.Children.Add(turn);
                _spin.Begin();
            }
            catch (Exception ex)
            {
                // A still ring is better than a broken bar.
                System.Diagnostics.Debug.WriteLine(ex);
                _spin = null;
            }
        }

        private void Stop()
        {
            _spin?.Stop();
            _spin = null;
        }

        /// <summary>An arc from 12 o'clock, clockwise, over <paramref name="fraction"/> of the circle.</summary>
        private Geometry Arc(double fraction)
        {
            double r = (_size - _line) / 2;
            double c = _size / 2;
            if (fraction >= 0.999)
            {
                return new EllipseGeometry { Center = new Windows.Foundation.Point(c, c), RadiusX = r, RadiusY = r };
            }
            double angle = fraction * 2 * Math.PI;
            var figure = new PathFigure
            {
                StartPoint = new Windows.Foundation.Point(c, c - r),
                IsClosed = false,
                IsFilled = false,
            };
            figure.Segments.Add(new ArcSegment
            {
                Point = new Windows.Foundation.Point(c + r * Math.Sin(angle), c - r * Math.Cos(angle)),
                Size = new Windows.Foundation.Size(r, r),
                IsLargeArc = fraction > 0.5,
                SweepDirection = SweepDirection.Clockwise,
            });
            var geometry = new PathGeometry();
            geometry.Figures.Add(figure);
            return geometry;
        }
    }

    // Names the live search key; RefreshKeyTips calls this again after a remap.
    private static void RefreshIndexingTip()
    {
        if (_indexingPill is null || _indexingText is null) return;
        // The pack's line ("Paused on battery", Look.StatusLine): say where the override is.
        bool battery = _indexingOnBattery || _indexingText.Text.Contains("battery", StringComparison.OrdinalIgnoreCase);
        string key = KeyNote(Command.SearchOpen);
        ToolTipService.SetToolTip(_indexingPill, _indexingText.Text == AiStartingText
            ? $"Local search is starting. Click to search{key}; the panel opens once it is ready."
            : battery
            ? (_indexingOnBattery
                ? $"Local search waits on battery. Click to search{key}; right-click to index anyway."
                : $"Local search waits on battery. Click to search{key}; Index anyway is in the panel's footer.")
            : $"Local search is indexing in the background. Click to search{key}");
    }

    // ---- matches over the scrub bar -----------------------------------------------------

    private static Microsoft.UI.Xaml.Controls.Canvas? _matchMarks;
    private static long[] _matchMs = Array.Empty<long>();
    private static int _matchCurrent = -1;
    private static long _matchDurationMs;

    /// <summary>The dots' layer over the scrubber (WrapSeekForTrim).</summary>
    private static Microsoft.UI.Xaml.Controls.Canvas BuildMatchMarks()
    {
        _matchMarks = new Microsoft.UI.Xaml.Controls.Canvas
        {
            IsHitTestVisible = false,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        _matchDurationMs = 0;
        return _matchMarks;
    }

    private static void HostSetScrubMarkers(IReadOnlyList<long> ms, int currentIndex)
    {
        bool fresh = _matchMs.Length == 0 && ms.Count > 0;
        _matchMs = ms.ToArray();
        _matchCurrent = currentIndex;
        RenderMatchMarks(fresh);
    }

    // UpdateVideoControls: the dots are placed against the clip's duration,
    // which lands a beat after the markers do.
    private static void UpdateMatchDuration(long durationMs)
    {
        if (durationMs == _matchDurationMs) return;
        _matchDurationMs = durationMs;
        RenderMatchMarks(false);
    }

    private static void RenderMatchMarks(bool fadeIn)
    {
        if (_matchMarks is null) return;
        _matchMarks.Children.Clear();
        if (_matchMs.Length == 0 || _matchDurationMs <= 0) return;
        double last = -10;
        for (int i = 0; i < _matchMs.Length; ++i)
        {
            bool current = i == _matchCurrent;
            double f = Math.Clamp((double)_matchMs[i] / _matchDurationMs, 0, 1);
            double x = SeekInset + f * (_seekWidth - 2 * SeekInset);
            if (!current && x - last < 3) continue;  // a dense clip: one dot per three pixels
            last = x;
            double d = current ? 8 : 5;
            var dot = new Ellipse
            {
                Width = d,
                Height = d,
                Fill = Brush(ChromeColour.TrimAccent),
                Opacity = current ? 1 : 0.85,
            };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(dot, x - d / 2);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(dot, current ? 0 : 1.5);
            _matchMarks.Children.Add(dot);
        }
        if (!fadeIn) return;
        // ~200 ms fade (docs/design/17 brief); a fade is also the reduced-motion form.
        var fade = new DoubleAnimation
        {
            From = 0,
            To = 1,
            Duration = new Duration(TimeSpan.FromMilliseconds(200)),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        };
        Storyboard.SetTarget(fade, _matchMarks);
        Storyboard.SetTargetProperty(fade, "Opacity");
        var board = new Storyboard();
        board.Children.Add(fade);
        board.Begin();
    }

    // ---- Settings → Local search ------------------------------------------------------

    private static void AddLocalSearchSettings(StackPanel view)
    {
        // #510: the body of Local search's page in Settings → Add-ons, whose
        // heading names it.
        _localSearchPanel = new StackPanel { Spacing = Space(10) };
        _localSearchStatus = Note("");
        view.Children.Add(_localSearchPanel);
        view.Children.Add(FirstPartyPageHost(AiSlot, FacesSlot, AudioSlot, DocsSlot, LargeSlot, CudaSlot));  // PR 56
        view.Children.Add(_localSearchStatus);
        EnsureReaderStarted();  // a later window: what is installed, read now
        _localSearchRestartButton = AddonRestartButton();
        view.Children.Add(_localSearchRestartButton);
        RefreshLocalSearch();
        // Opening Settings asks the channel for every piece, like Import: the
        // person is looking at what can be installed or updated. The GETs
        // carry nothing about them.
        foreach (AddonSlot piece in new[] { AiSlot, FacesSlot, AudioSlot, DocsSlot })
        {
            if (piece.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(piece);
        }
        // Large pictures only once installed, for its updates: the picker installs it.
        if (LargeSlot.State.Installed && LargeSlot.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(LargeSlot);
        StartNvidiaProbe();
        RefreshFamilyUsage();
    }

    private static void SetLocalSearchStatus(string text)
    {
        if (_localSearchStatus is not null) _localSearchStatus.Text = text;
    }

    // Worker.
    private static void RefreshFamilyUsage()
    {
        _ = Task.Run(() =>
        {
            (ulong used, ulong ceiling) = (0, 0);
            try { (used, ceiling) = AddonNative.FamilyUsage(AiFamily); }
            catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _aiUsed = used;
                _aiCeiling = ceiling;
                RefreshLocalSearch();
            });
        });
    }

    private static void StartNvidiaProbe()
    {
        if (_nvidiaPresent is not null) return;
        _ = Task.Run(() =>
        {
            bool nvidia = HasNvidiaAdapter();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _nvidiaPresent = nvidia;
                if (nvidia && !CudaSlot.State.Installed && CudaSlot.Offer.Kind == OfferKind.Unknown) ProbeOffer(CudaSlot);
                RefreshLocalSearch();
            });
        });
    }

    private static void RefreshLocalSearch()
    {
        if (_localSearchPanel is null) return;
        RefreshAddonList();  // #510: its row in the list
        _localSearchPanel.Children.Clear();

        if (!_addonStatesRead)
        {
            // What is installed is still being read (it verifies every file):
            // quiet, and nothing offered for download until it is known.
            _localSearchPanel.Children.Add(Note("Checking installed add-ons…"));
            return;
        }
        if (_addonsElsewhere)
        {
            // A later window (IslandHost.Addons.cs): it searches through the
            // reader; the first window indexes, installs and removes.
            _localSearchPanel.Children.Add(Note(!AiSlot.State.Installed
                ? "Local search is not installed. " + AddonsElsewhereText
                : AddonsElsewhereText));
            AddAiSettingsPanel();  // the pack's own, in its read-only form
            return;
        }

        // The intro and its Install: only until Core is installed (the Mac's introCard).
        var intro = new StackPanel { Spacing = Space(8) };
        var card = new Border
        {
            Child = intro,
            Padding = Pad(14),
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = Corner(RadiusSheet),
        };
        TextBlock what = Note("Find photos and moments in videos by describing them: “dog on a beach”, “birthday cake”.");
        what.Foreground = Brush(Title);
        intro.Children.Add(what);
        intro.Children.Add(Note("It runs entirely on this computer. Nothing about your files, and no search, ever leaves it."));
        if (!AiSlot.State.Installed && !AiQueuedOrInstalling(AiSlot) &&
            InstallFailureRow(AiSlot, "Local search", false, () => StartPieceInstall(AiSlot), () => StartPieceReinstall(AiSlot))
                is FrameworkElement coreFailed)
        {
            intro.Children.Add(coreFailed);
            _localSearchPanel.Children.Add(card);
        }
        else if (!AiSlot.State.Installed)
        {
            AddonOffer offer = AiSlot.Offer;
            switch (offer.Kind)
            {
                case OfferKind.Available:
                    string label = offer.InstalledSize > 0
                        ? $"Install local search — downloads ~{MediaViewer.Shared.ByteSize.Text(offer.ArchiveSize)}, uses ~{MediaViewer.Shared.ByteSize.Text(offer.InstalledSize)}"
                        : $"Install local search — downloads ~{MediaViewer.Shared.ByteSize.Text(offer.ArchiveSize)}";
                    Button install = SettingsButton(label, () => StartPieceInstall(AiSlot));
                    install.IsEnabled = !AiQueuedOrInstalling(AiSlot);
                    install.HorizontalAlignment = HorizontalAlignment.Left;
                    intro.Children.Add(install);
                    // Core, People, Audio and Documents in one click, one after another.
                    // Offered while none is installed; NVIDIA acceleration is
                    // never installed unasked (docs/design/17).
                    AddonSlot[] all = InstallAllSlots();
                    if (!AnyAiPending() && all.Length > 1 && !FacesSlot.State.Installed && !AudioSlot.State.Installed && !DocsSlot.State.Installed)
                    {
                        long archive = all.Sum(s => s.Offer.ArchiveSize);
                        long uses = all.Sum(s => s.Offer.InstalledSize);
                        string names = string.Join(", ", all.Select(s => s == AiSlot ? "Core" : s.Name));
                        Button installAll = SettingsButton(
                            uses > 0 ? $"Install all ({names}) — downloads ~{MediaViewer.Shared.ByteSize.Text(archive)}, uses ~{MediaViewer.Shared.ByteSize.Text(uses)}"
                                     : $"Install all ({names}) — downloads ~{MediaViewer.Shared.ByteSize.Text(archive)}",
                            () =>
                            {
                                foreach (AddonSlot each in InstallAllSlots()) StartPieceInstall(each);
                            });
                        installAll.HorizontalAlignment = HorizontalAlignment.Left;
                        intro.Children.Add(installAll);
                    }
                    intro.Children.Add(Small("Nothing downloads until you click Install. The download carries no identifier."));
                    break;
                default:
                    AddOfferNote(intro, AiSlot, "Local search");
                    break;
            }
            _localSearchPanel.Children.Add(card);
        }

        // Every piece with a newer version, in one click, Core first.
        AddonSlot[] updates = UpdateAllSlots();
        if (AiSlot.State.Installed && !AnyAiPending() && updates.Length > 1)
        {
            long archive = updates.Sum(s => s.Offer.ArchiveSize);
            string names = string.Join(", ", updates.Select(s => s == AiSlot ? "Core" : s.Name));
            string[] versions = updates.Select(s => s.Offer.Version).Distinct().ToArray();
            string head = versions.Length == 1 ? $"Update all to {versions[0]}" : "Update all";
            Button updateAll = SettingsButton($"{head} ({names}) — downloads ~{MediaViewer.Shared.ByteSize.Text(archive)}", () =>
            {
                foreach (AddonSlot each in UpdateAllSlots()) StartPieceInstall(each);
            });
            updateAll.HorizontalAlignment = HorizontalAlignment.Left;
            ToolTipService.SetToolTip(updateAll, "Core first, then the others, one after another.");
            _localSearchPanel.Children.Add(updateAll);
        }

        // The pieces, each with its size and its own Install / Remove, in one
        // card with a hairline between them (the Mac's).
        var pieces = new StackPanel();
        void Piece(FrameworkElement row)
        {
            if (pieces.Children.Count > 0) pieces.Children.Add(new Border { Height = 1, Background = Brush(Hairline) });
            pieces.Children.Add(row);
        }
        Piece(PieceRow(AiSlot, "Core", "Search by description. Required."));
        Piece(PieceRow(FacesSlot, "People", "Find people in your photos. Optional; off until you turn it on."));
        Piece(PieceRow(AudioSlot, "Audio",
            "Sounds and speech in videos — find “dog barking” or what someone said. Optional."));
        Piece(PieceRow(DocsSlot, "Documents",
            "Search PDF and Word files by what they're about, not just their words."));
        // #482: listed once the model picker has asked for it (Maxxed) or it is installed;
        // never offered on its own.
        if (LargeSlot.State.Installed || LargeSlot.Removing || AiQueuedOrInstalling(LargeSlot))
        {
            Piece(PieceRow(LargeSlot, "Large pictures", "The Maxxed tier's ViT-H/14 picture tower."));
        }
        if (_nvidiaPresent == true || CudaSlot.State.Installed)
        {
            Piece(PieceRow(CudaSlot, "NVIDIA acceleration", "Index faster on this computer's NVIDIA graphics. Needs NVIDIA's CUDA 13 and cuDNN 9, installed separately."));
        }
        _localSearchPanel.Children.Add(new Border
        {
            Child = pieces,
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = Corner(RadiusCard),
        });

        if (_confirmCoreRemove) _localSearchPanel.Children.Add(BuildCoreRemoveConfirm());
        else if (_confirmPieceRemove is AddonSlot asked && asked.State.Installed && !asked.Removing)
            _localSearchPanel.Children.Add(BuildPieceRemoveConfirm(asked));
        // Installed, verified and being loaded: say so (the Mac's).
        if (AiStarting()) _localSearchPanel.Children.Add(Note("Starting Local search…"));

        if (AiSlot.State.Installed && AiSlot.State.State != "ok")
        {
            _localSearchPanel.Children.Add(Note(AiSlot.State.State == "needs_update"
                ? $"Local search {AiSlot.State.Version} needs an update to work with this MediaViewer."
                : "The installed local search did not pass verification and is not loaded."));
            // #525: the one next step, unless Core's row already offers it after a failure.
            if (AiSlot.Failure is null && !AiQueuedOrInstalling(AiSlot))
            {
                Button reinstall = SettingsButton(InstallWords.Reinstall, () => StartPieceReinstall(AiSlot));
                ToolTipService.SetToolTip(reinstall, InstallWords.ReinstallTip);
                reinstall.HorizontalAlignment = HorizontalAlignment.Left;
                _localSearchPanel.Children.Add(reinstall);
            }
        }

        AddAiSettingsPanel();
    }

    // The add-on's own management panel, once it is loaded (docs/design/17 PR 23).
    // Before that (issue #292): "Starting Local search…" while it loads, or
    // Start now while it waits for the first search. Opening Settings alone
    // does not start it.
    private static void AddAiSettingsPanel()
    {
        if (_localSearchPanel is null) return;
        if (SearchChrome is null && AiStarting())
        {
            _localSearchPanel.Children.Add(WrappedLabel(AiStartingText));
            return;
        }
        if (AiOnDemand())
        {
            _localSearchPanel.Children.Add(WrappedLabel(
                "Local search starts the first time you search. Start it now to see its folders and settings."));
            Button start = SettingsButton("Start now", () => StartAiOnDemand(open: false));
            start.HorizontalAlignment = HorizontalAlignment.Left;
            _localSearchPanel.Children.Add(start);
            return;
        }
        if (SearchChrome is ISearchChrome search)
        {
            try { _aiSettingsPanel ??= search.BuildSettingsPanel() as UIElement; }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                _aiSettingsPanel = null;
            }
            if (_aiSettingsPanel is not null)
            {
                if (_aiSettingsPanel is FrameworkElement fe && fe.Parent is Panel old) old.Children.Remove(fe);
                _localSearchPanel.Children.Add(_aiSettingsPanel);
            }
        }
    }

    private static TextBlock Small(string text)
    {
        TextBlock t = WrappedLabel(text);
        t.FontSize = TypeCaption;
        return t;
    }

    // A note or status line in Settings → Add-ons: 13 (the Mac's).
    private static TextBlock Note(string text)
    {
        TextBlock t = WrappedLabel(text);
        t.FontSize = TypeNote;
        return t;
    }

    private static FrameworkElement PieceRow(AddonSlot slot, string title, string detail)
    {
        var row = new Grid
        {
            Padding = Pad(12),
            ColumnSpacing = Space(16),
        };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var labels = new StackPanel { Spacing = Space(2), VerticalAlignment = VerticalAlignment.Center };
        TextBlock name = Label(title);
        name.Foreground = Brush(Title);
        labels.Children.Add(name);
        labels.Children.Add(Small(detail));
        row.Children.Add(labels);

        long size = slot.State.Installed ? slot.State.Size : slot.Offer.InstalledSize;
        var sizeText = Label(size <= 0 ? "" : slot.State.Installed ? $"{slot.State.Version} · {MediaViewer.Shared.ByteSize.Text(size)}" : MediaViewer.Shared.ByteSize.Text(size));
        sizeText.FontSize = 12;
        sizeText.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(sizeText, 1);
        row.Children.Add(sizeText);

        FrameworkElement action;
        if (_aiInstalling == slot)
        {
            // Installing: "412 MB of 1.08 GB" and the percentage, then the
            // checking and installing steps (the Mac's AddonProgressView).
            action = ProgressFor(slot, 200);
        }
        else if (_aiQueue.Contains(slot))
        {
            // Waiting its turn; it can be taken out before it starts.
            var queued = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            queued.Children.Add(Small(slot.Parent is not null && !AiSlot.State.Installed ? "Queued, after Core" : "Queued"));
            Button cancel = SettingsButton("Cancel", () => CancelQueuedPiece(slot));
            AutomationProperties.SetName(cancel, $"Cancel installing {title}");
            queued.Children.Add(cancel);
            action = queued;
        }
        else if (slot.Removing)
        {
            // Removing: said at once, not a greyed Remove (owner report,
            // 2026-09-27); Install comes back when it has gone.
            var removing = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            removing.Children.Add(Small("Removing…"));
            AutomationProperties.SetName(removing, $"Removing {title}");
            action = removing;
        }
        else if (slot.Failure is InstallFailure failure && !slot.Busy)
        {
            // #525: the failure's one next step; its sentence is the panel's status line.
            bool broken = slot.State.Installed && slot.State.State != "ok";
            InstallNext next = InstallWords.Next(failure, broken);
            Button go = SettingsButton(InstallWords.NextLabel(next),
                () => { if (next == InstallNext.Reinstall) StartPieceReinstall(slot); else StartPieceInstall(slot); });
            if (next == InstallNext.Reinstall) ToolTipService.SetToolTip(go, InstallWords.ReinstallTip);
            AutomationProperties.SetName(go, $"{InstallWords.NextLabel(next)}: {title}");
            action = go;
        }
        else if (slot.State.Installed)
        {
            // Every piece asks first (the Mac's "Remove…").
            Button remove = SettingsButton("Remove…", () =>
            {
                _confirmCoreRemove = slot == AiSlot;
                _confirmPieceRemove = slot == AiSlot ? null : slot;
                RefreshLocalSearch();
            });
            // Only Remove waits for the queue and any load or removal to finish.
            remove.IsEnabled = !AnyAiPending();
            AutomationProperties.SetName(remove, "Remove " + title);
            action = remove;
            if (UpdateVersion(slot) is string newer)
            {
                Button update = SettingsButton($"Update to {newer}", () => StartPieceInstall(slot));
                update.IsEnabled = !slot.Removing;
                if (slot != AiSlot && CoreBehind()) ToolTipService.SetToolTip(update, "Core updates first.");
                AutomationProperties.SetName(update, $"Update {title} to {newer}");
                var both = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
                both.Children.Add(update);
                both.Children.Add(remove);
                action = both;
            }
        }
        else if (slot.Parent is not null && !CoreComing())
        {
            action = Small("Install Core first");
        }
        else if (slot.Offer.Kind == OfferKind.Available)
        {
            // Accepted at once, queued behind any install in progress.
            Button install = SettingsButton("Install", () => StartPieceInstall(slot));
            install.IsEnabled = !slot.Busy;
            AutomationProperties.SetName(install, "Install " + title);
            action = install;
        }
        else
        {
            action = Small(slot.Offer.Kind switch
            {
                OfferKind.Checking or OfferKind.Unknown => "Checking…",
                OfferKind.Unreachable => "Offline",
                OfferKind.NeedsNewerApp => "Needs a newer MediaViewer",
                _ => "Not published yet",
            });
        }
        action.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(action, 2);
        row.Children.Add(action);
        return row;
    }

    private static bool AnyAiBusy() => AiSlot.Busy || FacesSlot.Busy || AudioSlot.Busy || DocsSlot.Busy || LargeSlot.Busy || CudaSlot.Busy;

    // Busy (installing, loading, removing) or waiting in the install queue.
    private static bool AnyAiPending() => AnyAiBusy() || _aiQueue.Count > 0;

    private static bool AnyAiRemoving() => AiSlot.Removing || FacesSlot.Removing || AudioSlot.Removing || DocsSlot.Removing || LargeSlot.Removing || CudaSlot.Removing;

    private static bool AiQueuedOrInstalling(AddonSlot slot) => _aiInstalling == slot || _aiQueue.Contains(slot);

    // Core is installed or on its way in this batch: a piece may queue behind it.
    private static bool CoreComing() => AiSlot.State.Installed || AiQueuedOrInstalling(AiSlot);

    // What "Install all" fetches: Core, People, Audio and Documents where not installed
    // and offered (never NVIDIA acceleration unasked).
    private static AddonSlot[] InstallAllSlots() => new[] { AiSlot, FacesSlot, AudioSlot, DocsSlot }
        .Where(s => !s.State.Installed && !s.Removing && !AiQueuedOrInstalling(s) && s.Offer.Kind == OfferKind.Available)
        .ToArray();

    // What "Update all" fetches: every installed piece the channel has a newer
    // version of; the queue puts Core first. Owner, 2026-10-03: the pieces had
    // been updated one by one and the 1 GB Core left behind, so a piece ran
    // ahead of the engine it was built with.
    private static AddonSlot[] UpdateAllSlots() => new[] { AiSlot, FacesSlot, AudioSlot, DocsSlot, LargeSlot, CudaSlot }
        .Where(s => UpdateVersion(s) is not null && !s.Removing && !AiQueuedOrInstalling(s))
        .ToArray();

    // Core is behind the channel and not on its way: a piece's install or
    // update queues Core first.
    private static bool CoreBehind() => UpdateVersion(AiSlot) is not null && !AiQueuedOrInstalling(AiSlot) && !AiSlot.Removing;

    private static void AddAiNote(string text)
    {
        if (text.Length == 0) return;
        _aiBatchNotes.Add(text);
        SetLocalSearchStatus(string.Join(" ", _aiBatchNotes));
    }

    // Takes a piece out of the queue before it starts. Without Core on its
    // way, the pieces waiting for it go too.
    private static void CancelQueuedPiece(AddonSlot slot)
    {
        _aiQueue.Remove(slot);
        if (!CoreComing() && _aiQueue.Any(q => q.Parent is not null))
        {
            _aiQueue.RemoveAll(q => q.Parent is not null);
            AddAiNote("The queued pieces were cancelled: they need Core.");
        }
        RefreshLocalSearch();
    }

    // Starts the next queued piece once nothing installs or is being removed.
    // A Core load in flight does not hold it up.
    private static void StartNextPiece()
    {
        while (_aiInstalling is null && !AnyAiRemoving() && _aiQueue.Count > 0)
        {
            AddonSlot next = _aiQueue[0];
            _aiQueue.RemoveAt(0);
            if (next.Parent is not null && !AiSlot.State.Installed)
            {
                AddAiNote($"{next.Name} was not installed: it needs Core.");
                continue;
            }
            // The 5 GB rule, now that the pieces before it in the batch have
            // landed (checked again against the signed size before download).
            if (BudgetRefusal(next, next.Offer.InstalledSize) is string refused)
            {
                AddAiNote(refused);
                continue;
            }
            BeginPieceInstall(next);
            return;
        }
        RefreshLocalSearch();
    }

    private static FrameworkElement BuildCoreRemoveConfirm()
    {
        var box = ConfirmBox();
        TextBlock ask = Note("Remove local search? People, Audio, Documents and NVIDIA acceleration go with it.");
        ask.Foreground = Brush(Title);
        box.Children.Add(ask);
        box.Children.Add(Note("Also delete the search index? Keeping it means a reinstall searches at once, without indexing again."));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        // Keeping the index is the default (accent), and where Enter lands; Esc cancels.
        Button keep = SettingsButton("Remove, keep the index", () => RemoveCore(keepData: true), accent: true);
        buttons.Children.Add(keep);
        buttons.Children.Add(SettingsButton("Remove and delete the index", () => RemoveCore(keepData: false)));
        buttons.Children.Add(SettingsButton("Cancel", () => CancelLocalSearchConfirm()));
        box.Children.Add(buttons);
        _dispatcher?.DispatcherQueue.TryEnqueue(() => keep.Focus(FocusState.Keyboard));
        return ConfirmCard(box);
    }

    // "Remove People?" and the like (the Mac's removeConfirm): Remove is the
    // default, Esc or Cancel keeps it.
    private static FrameworkElement BuildPieceRemoveConfirm(AddonSlot slot)
    {
        var box = ConfirmBox();
        TextBlock ask = Note(slot == AudioSlot
            ? "Remove Audio? Searching videos by sound and speech stops until it is installed again."
            : slot == FacesSlot
                ? "Remove People? Face search stops until it is installed again."
                : slot == DocsSlot
                    ? "Remove Documents? Document words match only as typed until it is installed again."
                    : slot == LargeSlot
                        ? "Remove Large pictures? The Maxxed tier cannot search pictures until it is installed again; choose another tier under Models."
                        : $"Remove {slot.Name}? Indexing runs on the CPU until it is installed again.");
        ask.Foreground = Brush(Title);
        box.Children.Add(ask);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        Button remove = SettingsButton("Remove", () =>
        {
            _confirmPieceRemove = null;
            RemovePiece(slot, keepData: true);
        }, accent: true);
        remove.IsEnabled = !AnyAiPending();
        AutomationProperties.SetName(remove, "Remove " + slot.Name);
        buttons.Children.Add(remove);
        buttons.Children.Add(SettingsButton("Cancel", () => CancelLocalSearchConfirm()));
        box.Children.Add(buttons);
        _dispatcher?.DispatcherQueue.TryEnqueue(() => remove.Focus(FocusState.Keyboard));
        return ConfirmCard(box);
    }

    private static StackPanel ConfirmBox() => new() { Spacing = Space(8) };

    private static Border ConfirmCard(UIElement child) => new()
    {
        Child = child,
        Padding = Pad(12),
        Background = Brush(ChromeColour.Surface),
        BorderBrush = Brush(Hairline),
        BorderThickness = new Thickness(1),
        CornerRadius = Corner(RadiusCard),
    };

    /// <summary>Esc or Cancel on a Local search "Remove …?": false when none is open.</summary>
    private static bool CancelLocalSearchConfirm()
    {
        if (!_confirmCoreRemove && _confirmPieceRemove is null) return false;
        _confirmCoreRemove = false;
        _confirmPieceRemove = null;
        RefreshLocalSearch();
        return true;
    }

    private static void RemoveCore(bool keepData)
    {
        if (AnyAiPending()) return;
        _confirmCoreRemove = false;
        AiSlot.Busy = true;
        // Removing Core removes its pieces: their rows say so too.
        foreach (AddonSlot gone in new[] { AiSlot, FacesSlot, AudioSlot, DocsSlot, LargeSlot, CudaSlot })
        {
            if (gone == AiSlot || gone.State.Installed) gone.Removing = true;
        }
        UnloadAddonChrome(AiSlot);
        SetLocalSearchStatus("Removing local search…");
        RefreshLocalSearch();
        _ = Task.Run(() =>
        {
            string done = "Local search removed.";
            foreach (AddonSlot slot in new[] { CudaSlot, LargeSlot, DocsSlot, AudioSlot, FacesSlot, AiSlot })
            {
                try
                {
                    // The pieces' data is the Core's (index.db, faces.db).
                    AddonNative.Remove(slot.Id, slot == AiSlot ? keepData : true);
                }
                catch (MediaViewerException)
                {
                    done = "Local search will finish uninstalling the next time MediaViewer starts.";
                }
            }
            FinishAiChange(AiSlot, done, load: false);
        });
    }

    private static void RemovePiece(AddonSlot slot, bool keepData)
    {
        if (AnyAiPending()) return;
        slot.Busy = true;
        slot.Removing = true;
        SetLocalSearchStatus($"Removing {slot.Name}…");
        RefreshLocalSearch();
        _ = Task.Run(() =>
        {
            string done;
            try
            {
                AddonNative.Remove(slot.Id, keepData);
                done = $"{slot.Name} removed. Install it again here whenever you like.";
            }
            catch (MediaViewerException)
            {
                done = $"{slot.Name} will finish uninstalling the next time MediaViewer starts.";
            }
            FinishAiChange(slot, done, load: false);
        });
    }

    // Worker in, UI out: re-read what is installed and the family's room,
    // then start the next queued install (its 5 GB check sees this one).
    private static void FinishAiChange(AddonSlot slot, string message, bool load, bool install = false)
    {
        Dictionary<string, AddonState> states = ReadAddonStates();
        (ulong used, ulong ceiling) = (0, 0);
        try { (used, ceiling) = AddonNative.FamilyUsage(AiFamily); }
        catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        DispatcherQueueControllerTryEnqueue(() =>
        {
            slot.Busy = false;
            slot.Phase = null;
            if (_aiInstalling == slot) _aiInstalling = null;
            if (!install)
            {
                foreach (AddonSlot gone in new[] { AiSlot, FacesSlot, AudioSlot, DocsSlot, LargeSlot, CudaSlot }) gone.Removing = false;
            }
            ApplyAddonStates(states);
            _aiUsed = used;
            _aiCeiling = ceiling;
            if (!install)
            {
                SetLocalSearchStatus(message);
            }
            else if (slot == AiSlot && !AiSlot.State.Installed && _aiQueue.Any(q => q.Parent is not null))
            {
                // Pieces clicked with Core cannot install without it.
                string dropped = string.Join(" and ", _aiQueue.Where(q => q.Parent is not null).Select(q => q.Name));
                _aiQueue.RemoveAll(q => q.Parent is not null);
                AddAiNote(message);
                AddAiNote(dropped.Contains(" and ")
                    ? $"{dropped} were not installed: they need Core."
                    : $"{dropped} was not installed: it needs Core.");
            }
            else
            {
                AddAiNote(message);
            }
            // A piece just removed offers Install again: its size must be known.
            foreach (AddonSlot piece in new[] { AiSlot, FacesSlot, AudioSlot, DocsSlot })
            {
                if (!piece.State.Installed && piece.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(piece);
            }
            RefreshLocalSearch();
            if (load && AiSlot.Usable && AiSlot.Chrome is null) LoadAddon(AiSlot);
            // A piece came or went under a loaded pack: it picks People up at
            // once; NVIDIA acceleration waits for a restart (its panel says so).
            if (slot.Parent is not null && SearchChrome is ISearchChrome search)
            {
                try { search.OnPiecesChanged(); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
            StartNextPiece();
        });
    }

    /// <summary>Refused before anything is downloaded (docs/design/17: 5 GB).</summary>
    private sealed class OverBudgetException(string sentence) : Exception(sentence);

    private static string? BudgetRefusal(AddonSlot slot, long installedSize)
    {
        if (_aiCeiling == 0 || installedSize <= 0) return null;
        long current = slot.State.Installed ? slot.State.Size : 0;
        long after = (long)_aiUsed - current + installedSize;
        if (after <= (long)_aiCeiling) return null;
        long free = Math.Max(0, (long)_aiCeiling - (long)_aiUsed + current);
        return $"{slot.Name} needs {MediaViewer.Shared.ByteSize.Text(installedSize)}, but only {MediaViewer.Shared.ByteSize.Text(free)} of the {MediaViewer.Shared.ByteSize.Text((long)_aiCeiling)} " +
               "for local search is free. Remove another piece first; nothing was downloaded.";
    }

    // An Install / Update / Reinstall click: queued and accepted at once. The
    // queue runs one install at a time, Core first; a piece clicked before
    // Core is installed waits for Core and is dropped with a note if Core
    // does not install.
    private static void StartPieceInstall(AddonSlot slot)
    {
        if (!_addonStatesRead || slot.Removing || AiQueuedOrInstalling(slot)) return;
        if (slot.Parent is not null && !CoreComing())
        {
            SetLocalSearchStatus("Install Core first.");
            return;
        }
        if (BudgetRefusal(slot, slot.Offer.InstalledSize) is string refused)
        {
            SetLocalSearchStatus(refused);
            return;
        }
        if (!AnyAiPending()) _aiBatchNotes.Clear();
        // A piece behind a Core that is itself behind: Core first (its 5 GB
        // check runs when its turn comes, like every queued piece's).
        if (slot != AiSlot && CoreBehind()) _aiQueue.Insert(0, AiSlot);
        if (slot == AiSlot) _aiQueue.Insert(0, slot);
        else _aiQueue.Add(slot);
        StartNextPiece();
    }

    /// <summary>
    /// The model picker's Download (#482, IAddonPieces): the pieces the chosen
    /// tier and its checkboxes need, queued as their Install buttons would be.
    /// Only People, Audio, Documents and Large pictures; any other id is
    /// ignored (NVIDIA acceleration is never installed unasked), and an
    /// installed or already queued piece is skipped.
    /// </summary>
    private static void HostInstallPieces(IReadOnlyList<string> ids)
    {
        AddonSlot[] allowed = { FacesSlot, AudioSlot, DocsSlot, LargeSlot };
        bool any = false;
        foreach (string id in ids.Distinct(StringComparer.Ordinal))
        {
            AddonSlot? slot = Array.Find(allowed, s => s.Id == id);
            if (slot is null || slot.State.Installed || AiQueuedOrInstalling(slot)) continue;
            StartPieceInstall(slot);
            any = true;
        }
        if (any) RefreshLocalSearch();
    }

    private static void BeginPieceInstall(AddonSlot slot)
    {
        _aiInstalling = slot;
        string what = slot == AiSlot ? "local search" : slot.Name;
        string name = PieceName(slot);
        bool reinstall = _aiReinstall.Remove(slot);
        // An update installs beside the running copy (the store keeps it until
        // the next start). A new Core takes over then; a new piece at once.
        string? update = reinstall ? null : UpdateVersion(slot);
        bool coreRunning = slot == AiSlot && AiSlot.Chrome is not null;
        SetLocalSearchStatus(string.Join(" ", _aiBatchNotes.Append(InstallWords.Downloading(what, update))));
        IProgress<AddonPhase> progress = BeginInstall(slot);
        RefreshLocalSearch();
        long ceiling = (long)_aiCeiling;
        _ = Task.Run(async () =>
        {
            // #525: the shared flow (IslandHost.InstallFlow.cs), with Local
            // search's own rule: the family's room, checked again on the
            // signed size before the archive is requested.
            InstallOutcome outcome = await RunInstall(slot, progress, reinstall, manifest =>
            {
                long size = manifest.TryGetProperty("installed_size", out JsonElement s) ? s.GetInt64() : 0;
                (ulong used, ulong cap) = AddonNative.FamilyUsage(AiFamily);
                _aiUsed = used;
                _aiCeiling = cap;
                if (BudgetRefusal(slot, size) is string no) throw new OverBudgetException(no);
            }, ex => ex switch
            {
                OverBudgetException over => over.Message,
                // mv_addon_install refuses it a third time.
                MediaViewerException { Status: MvStatus.UnsupportedFormat } when ceiling > 0 =>
                    $"This piece would take local search over its {MediaViewer.Shared.ByteSize.Text(ceiling)}, so it was not installed.",
                _ => null,
            }).ConfigureAwait(false);
            DispatcherQueueControllerTryEnqueue(() => EndInstall(slot, outcome));
            string message = outcome.Refused ?? (!outcome.Ok ? InstallWords.Failure(outcome.Failure ?? InstallFailure.Failed, name)
                : reinstall ? InstallWords.Reinstalled(name)
                : update is null ? InstallWords.Installed(name)
                : coreRunning ? ""  // the restart card says it
                : InstallWords.Updated(name, update));
            if (outcome.NotPublished) message = "";
            if (outcome.Ok && update is not null && coreRunning)
                DispatcherQueueControllerTryEnqueue(() => AddonRestartNeeded("Local search", update));
            FinishAiChange(slot, message, load: slot == AiSlot, install: true);
        });
    }

    // Local search's pieces by the name a person reads: Core is "Local search".
    private static string PieceName(AddonSlot slot) => slot == AiSlot ? "Local search" : slot.Name;

    // #525: queued with Remove and install again (BeginPieceInstall takes it out).
    private static readonly HashSet<AddonSlot> _aiReinstall = new();

    private static void StartPieceReinstall(AddonSlot slot)
    {
        if (!_addonStatesRead || slot.Removing || AiQueuedOrInstalling(slot)) return;
        if (slot == AiSlot) UnloadAddonChrome(AiSlot);
        _aiReinstall.Add(slot);
        StartPieceInstall(slot);
        if (!AiQueuedOrInstalling(slot)) _aiReinstall.Remove(slot);  // refused (room, Core first)
    }

    // ---- NVIDIA detection -----------------------------------------------------------------

    private static readonly Guid IidDxgiFactory1 = new("770aae78-f26f-4dba-a829-253c83d1b387");

    [DllImport("dxgi.dll")]
    private static extern int CreateDXGIFactory1(in Guid riid, out IntPtr factory);

    /// <summary>
    /// Worker. Whether any adapter is NVIDIA's (DXGI vendor 0x10DE), for the
    /// "NVIDIA acceleration" piece: offered on hardware that can use it, never
    /// auto-installed (docs/design/17). Raw vtable calls; the shell owns no DXGI
    /// wrapper and this is the only question it asks.
    /// </summary>
    private static unsafe bool HasNvidiaAdapter()
    {
        const uint NvidiaVendor = 0x10DE;
        IntPtr factory;
        try
        {
            if (CreateDXGIFactory1(IidDxgiFactory1, out factory) < 0 || factory == IntPtr.Zero) return false;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
        IntPtr* fvt = *(IntPtr**)factory;
        var release = (delegate* unmanaged[Stdcall]<IntPtr, uint>)fvt[2];
        // IDXGIFactory::EnumAdapters is slot 7 (IUnknown 3 + IDXGIObject 4).
        var enumAdapters = (delegate* unmanaged[Stdcall]<IntPtr, uint, IntPtr*, int>)fvt[7];
        byte* desc = stackalloc byte[512];
        try
        {
            for (uint i = 0; i < 16; ++i)
            {
                IntPtr adapter;
                if (enumAdapters(factory, i, &adapter) < 0 || adapter == IntPtr.Zero) break;
                IntPtr* avt = *(IntPtr**)adapter;
                // IDXGIAdapter::GetDesc is slot 8; DXGI_ADAPTER_DESC.VendorId
                // follows the 128-WCHAR description (offset 256).
                var getDesc = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int>)avt[8];
                var adapterRelease = (delegate* unmanaged[Stdcall]<IntPtr, uint>)avt[2];
                bool nvidia = getDesc(adapter, desc) >= 0 && *(uint*)(desc + 256) == NvidiaVendor;
                adapterRelease(adapter);
                if (nvidia) return true;
            }
            return false;
        }
        finally
        {
            release(factory);
        }
    }
}
