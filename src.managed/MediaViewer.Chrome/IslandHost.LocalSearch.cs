// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
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
/// Milestone H, the base app's half of Local search (plan/17 "The AI pack",
/// "UI and commands"): Settings → Local search (install / remove per piece,
/// the 3 GB family budget, the add-on's own management panel once loaded),
/// the command-bar indexing pill, the match dots over the scrub bar, and the
/// IAddonHost2 services the AI chrome calls.
/// </summary>
/// <remarks>
/// With no piece installed this file adds exactly one thing to the app: the
/// Settings section that offers the install. No key, no bar item, no menu
/// (plan/17: "Nothing about it appears elsewhere in the UI until the core pack
/// is installed"). Nothing downloads before the Install click, and the
/// download carries no identifier (rule 6).
/// </remarks>
public static partial class IslandHost
{
    private const string AiFamily = "ai";

    private static StackPanel? _localSearchPanel;
    private static TextBlock? _localSearchStatus;
    private static FrameworkElement? _localSearchHeading;
    private static UIElement? _aiSettingsPanel;
    private static bool? _nvidiaPresent;
    private static ulong _aiUsed, _aiCeiling;
    private static bool _confirmCoreRemove;
    // Installs run one at a time (no fight over bandwidth, and each piece's
    // 3 GB check counts the one before it); more Install clicks queue here,
    // Core first (owner request, 2026-09-28: Install on all three at once).
    private static readonly List<AddonSlot> _aiQueue = new();
    private static AddonSlot? _aiInstalling;
    // What each install of the current batch said, shown together.
    private static readonly List<string> _aiBatchNotes = new();

    // ---- listing state the AI chrome reads (IslandHost.Filmstrip.cs sets it) ----

    // The last directory the viewer listed; kept while a result list is open,
    // so "This folder" still means the folder the search started from.
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

    // The path bar's search icon: the panel now, or once the pack attaches.
    private static void OpenSearchFromPath()
    {
        if (SearchChrome is ISearchChrome search)
        {
            search.RunCommand(SearchCommand.Open);
            return;
        }
        if (AiStarting()) _searchOpenPending = true;
    }

    // A load of the AI pack started or failed: the icon follows.
    private static void OnSearchLoadChanged()
    {
        if (SearchChrome is null && !AiStarting()) _searchOpenPending = false;
        UpdatePathSearchButton();
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
        if (!_settingsVisible) Send(Command.OpenSettings);
        // After PresentSettings has built and laid out the screen.
        _dispatcher?.DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low,
            () => _localSearchHeading?.StartBringIntoView(new BringIntoViewOptions { VerticalAlignmentRatio = 0 }));
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
        HostSetIndexingPill(null, false);
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
    private static ProgressRing? _indexingRing;
    private static TextBlock? _indexingText;

    private static UIElement BuildIndexingPill()
    {
        _indexingRing = new ProgressRing
        {
            Width = 14,
            Height = 14,
            MinWidth = 14,
            MinHeight = 14,
            IsActive = false,
            Foreground = Brush(ChromeColour.TrimAccent),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _indexingText = new TextBlock
        {
            FontFamily = UiFont,
            FontSize = 13,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 360,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        row.Children.Add(_indexingRing);
        row.Children.Add(_indexingText);
        _indexingPill = TextButton("", () => SearchChrome?.RunCommand(SearchCommand.Open));
        _indexingPill.Content = new Border
        {
            Child = row,
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(12),
            Padding = new Thickness(10, 3, 12, 3),
        };
        _indexingPill.Padding = new Thickness(4, 0, 4, 0);
        _indexingPill.Visibility = Visibility.Collapsed;
        AutomationProperties.SetName(_indexingPill, "Local search indexing. Open search");
        ToolTipService.SetToolTip(_indexingPill, "Local search is indexing. Click to search (Ctrl+F)");
        return _indexingPill;
    }

    private static void HostSetIndexingPill(string? text, bool busy)
    {
        if (_indexingPill is null || _indexingText is null || _indexingRing is null) return;
        _indexingPill.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
        if (text != _indexingText.Text)
        {
            // The pack's line ("Paused on battery", Look.StatusLine): say where the override is.
            bool battery = text?.Contains("battery", StringComparison.OrdinalIgnoreCase) == true;
            ToolTipService.SetToolTip(_indexingPill, battery
                ? "Local search waits on battery. Click to search (Ctrl+F); Index anyway is in the panel's footer."
                : "Local search is indexing in the background. Click to search (Ctrl+F)");
        }
        _indexingText.Text = text ?? "";
        // The ring spins only while indexing: a still app shows a still bar.
        _indexingRing.IsActive = busy && text is not null;
        _indexingRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
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
            Width = SeekWidth,
            IsHitTestVisible = false,
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
            double x = SeekInset + f * (SeekWidth - 2 * SeekInset);
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
        // ~200 ms fade (plan/17 brief); a fade is also the reduced-motion form.
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
        // An item of Settings → Add-ons, titled like Import.
        TextBlock heading = Label("Local search");
        _localSearchHeading = heading;
        view.Children.Add(heading);
        _localSearchPanel = new StackPanel { Spacing = 8 };
        _localSearchStatus = WrappedLabel("");
        view.Children.Add(_localSearchPanel);
        view.Children.Add(_localSearchStatus);
        RefreshLocalSearch();
        // Opening Settings asks the channel for every piece, like Import: the
        // person is looking at what can be installed or updated. The GETs
        // carry nothing about them.
        foreach (AddonSlot piece in new[] { AiSlot, FacesSlot, AudioSlot })
        {
            if (piece.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(piece);
        }
        StartNvidiaProbe();
        RefreshFamilyUsage();
    }

    private static void SetLocalSearchStatus(string text)
    {
        if (_localSearchStatus is not null) _localSearchStatus.Text = text;
    }

    private static string Gb(long bytes) => bytes >= 1_000_000_000
        ? $"{bytes / 1e9:0.0} GB"
        : $"{Math.Max(1, (long)Math.Round(bytes / 1e6))} MB";

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
        _localSearchPanel.Children.Clear();

        var card = new StackPanel
        {
            Spacing = 6,
            Padding = new Thickness(14),
            Background = Brush(ChromeColour.Surface),
            CornerRadius = new CornerRadius(8),
        };
        TextBlock what = WrappedLabel("Find photos and moments in videos by describing them: “dog on a beach”, “birthday cake”.");
        what.Foreground = Brush(Title);
        card.Children.Add(what);
        card.Children.Add(WrappedLabel("It runs entirely on this computer. Nothing about your files, and no search, ever leaves it."));

        if (!_addonStatesRead)
        {
            // What is installed is still being read (it verifies every file):
            // quiet, and nothing offered for download until it is known.
            card.Children.Add(WrappedLabel("Checking installed add-ons…"));
            _localSearchPanel.Children.Add(card);
            return;
        }
        if (!AiSlot.State.Installed)
        {
            AddonOffer offer = AiSlot.Offer;
            switch (offer.Kind)
            {
                case OfferKind.Available:
                    string label = offer.InstalledSize > 0
                        ? $"Install local search — downloads ~{Gb(offer.ArchiveSize)}, uses ~{Gb(offer.InstalledSize)}"
                        : $"Install local search — downloads ~{Gb(offer.ArchiveSize)}";
                    Button install = SettingsButton(label, () => StartPieceInstall(AiSlot));
                    install.IsEnabled = !AiQueuedOrInstalling(AiSlot);
                    install.HorizontalAlignment = HorizontalAlignment.Left;
                    card.Children.Add(install);
                    // Core, People and Audio in one click, one after another.
                    // Offered while none is installed; NVIDIA acceleration is
                    // never installed unasked (plan/17).
                    AddonSlot[] all = InstallAllSlots();
                    if (!AnyAiPending() && all.Length > 1 && !FacesSlot.State.Installed && !AudioSlot.State.Installed)
                    {
                        long archive = all.Sum(s => s.Offer.ArchiveSize);
                        long uses = all.Sum(s => s.Offer.InstalledSize);
                        string names = string.Join(", ", all.Select(s => s == AiSlot ? "Core" : s.Name));
                        Button installAll = SettingsButton(
                            uses > 0 ? $"Install all ({names}) — downloads ~{Gb(archive)}, uses ~{Gb(uses)}"
                                     : $"Install all ({names}) — downloads ~{Gb(archive)}",
                            () =>
                            {
                                foreach (AddonSlot each in InstallAllSlots()) StartPieceInstall(each);
                            });
                        installAll.HorizontalAlignment = HorizontalAlignment.Left;
                        card.Children.Add(installAll);
                    }
                    card.Children.Add(Small("Nothing downloads until you click Install. The download carries no identifier."));
                    break;
                case OfferKind.NotPublished:
                    card.Children.Add(WrappedLabel("Local search is not published for download yet. It will be offered here once a release carries it."));
                    break;
                case OfferKind.NeedsNewerApp:
                    card.Children.Add(WrappedLabel("The published local search needs a newer MediaViewer. Update MediaViewer, then install it."));
                    break;
                case OfferKind.Unreachable:
                    card.Children.Add(WrappedLabel("Could not reach the download server."));
                    card.Children.Add(SettingsButton("Try again", () => ProbeOffer(AiSlot)));
                    break;
                default:
                    card.Children.Add(WrappedLabel("Checking for local search…"));
                    break;
            }
        }
        _localSearchPanel.Children.Add(card);

        // The pieces, each with its size and its own Install / Remove.
        var pieces = new StackPanel { Spacing = 4 };
        pieces.Children.Add(PieceRow(AiSlot, "Core", "Search by description. Required."));
        pieces.Children.Add(PieceRow(FacesSlot, "People", "Find people in your photos. Optional; off until you turn it on."));
        pieces.Children.Add(PieceRow(AudioSlot, "Audio",
            "Sounds and speech in videos — find “dog barking” or what someone said. Optional."));
        if (_nvidiaPresent == true || CudaSlot.State.Installed)
        {
            pieces.Children.Add(PieceRow(CudaSlot, "NVIDIA acceleration", "Index faster on this computer's NVIDIA graphics."));
        }
        _localSearchPanel.Children.Add(pieces);

        if (_confirmCoreRemove) _localSearchPanel.Children.Add(BuildCoreRemoveConfirm());

        if (AiSlot.State.Installed && AiSlot.State.State != "ok")
        {
            _localSearchPanel.Children.Add(WrappedLabel(AiSlot.State.State == "needs_update"
                ? $"Local search {AiSlot.State.Version} needs an update to work with this MediaViewer."
                : "The installed local search did not pass verification and is not loaded. Reinstall it."));
            _localSearchPanel.Children.Add(SettingsButton("Reinstall", () => StartPieceInstall(AiSlot)));
        }

        // The add-on's own management panel, once it is loaded (plan/17 PR 23).
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
        t.FontSize = 12;
        return t;
    }

    private static FrameworkElement PieceRow(AddonSlot slot, string title, string detail)
    {
        var row = new Grid
        {
            Padding = new Thickness(14, 10, 14, 10),
            ColumnSpacing = 16,
            Background = Brush(ChromeColour.Surface),
            CornerRadius = new CornerRadius(8),
        };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var labels = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        TextBlock name = Label(title);
        name.Foreground = Brush(Title);
        labels.Children.Add(name);
        labels.Children.Add(Small(detail));
        row.Children.Add(labels);

        long size = slot.State.Installed ? slot.State.Size : slot.Offer.InstalledSize;
        var sizeText = Label(size <= 0 ? "" : slot.State.Installed ? $"{slot.State.Version} · {Gb(size)}" : Gb(size));
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
            var queued = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
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
            var removing = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            removing.Children.Add(new ProgressRing { IsActive = true, Width = 16, Height = 16 });
            removing.Children.Add(Small("Removing…"));
            AutomationProperties.SetName(removing, $"Removing {title}");
            action = removing;
        }
        else if (slot.State.Installed)
        {
            Button remove = SettingsButton("Remove", () =>
            {
                if (slot == AiSlot)
                {
                    _confirmCoreRemove = true;
                    RefreshLocalSearch();
                }
                else
                {
                    RemovePiece(slot, keepData: true);
                }
            });
            // Only Remove waits for the queue and any load or removal to finish.
            remove.IsEnabled = !AnyAiPending();
            AutomationProperties.SetName(remove, "Remove " + title);
            action = remove;
            if (UpdateVersion(slot) is string newer)
            {
                Button update = SettingsButton($"Update to {newer}", () => StartPieceInstall(slot));
                update.IsEnabled = !slot.Removing;
                AutomationProperties.SetName(update, $"Update {title} to {newer}");
                var both = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
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

    private static bool AnyAiBusy() => AiSlot.Busy || FacesSlot.Busy || AudioSlot.Busy || CudaSlot.Busy;

    // Busy (installing, loading, removing) or waiting in the install queue.
    private static bool AnyAiPending() => AnyAiBusy() || _aiQueue.Count > 0;

    private static bool AnyAiRemoving() => AiSlot.Removing || FacesSlot.Removing || AudioSlot.Removing || CudaSlot.Removing;

    private static bool AiQueuedOrInstalling(AddonSlot slot) => _aiInstalling == slot || _aiQueue.Contains(slot);

    // Core is installed or on its way in this batch: a piece may queue behind it.
    private static bool CoreComing() => AiSlot.State.Installed || AiQueuedOrInstalling(AiSlot);

    // What "Install all" fetches: Core, People and Audio where not installed
    // and offered (never NVIDIA acceleration unasked).
    private static AddonSlot[] InstallAllSlots() => new[] { AiSlot, FacesSlot, AudioSlot }
        .Where(s => !s.State.Installed && !s.Removing && !AiQueuedOrInstalling(s) && s.Offer.Kind == OfferKind.Available)
        .ToArray();

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
            // The 3 GB rule, now that the pieces before it in the batch have
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
        var box = new StackPanel
        {
            Spacing = 8,
            Padding = new Thickness(14),
            Background = Brush(ChromeColour.Surface),
            CornerRadius = new CornerRadius(8),
        };
        box.Children.Add(WrappedLabel("Remove local search? People, Audio and NVIDIA acceleration go with it."));
        box.Children.Add(WrappedLabel("Also delete the search index? Keeping it means a reinstall searches at once, without indexing again."));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        Button keep = SettingsButton("Remove, keep the index", () => RemoveCore(keepData: true));
        buttons.Children.Add(keep);
        buttons.Children.Add(SettingsButton("Remove and delete the index", () => RemoveCore(keepData: false)));
        buttons.Children.Add(SettingsButton("Cancel", () =>
        {
            _confirmCoreRemove = false;
            RefreshLocalSearch();
        }));
        box.Children.Add(buttons);
        // Keeping the index is the default, and where Enter lands.
        _dispatcher?.DispatcherQueue.TryEnqueue(() => keep.Focus(FocusState.Keyboard));
        return box;
    }

    private static void RemoveCore(bool keepData)
    {
        if (AnyAiPending()) return;
        _confirmCoreRemove = false;
        AiSlot.Busy = true;
        // Removing Core removes its pieces: their rows say so too.
        foreach (AddonSlot gone in new[] { AiSlot, FacesSlot, AudioSlot, CudaSlot })
        {
            if (gone == AiSlot || gone.State.Installed) gone.Removing = true;
        }
        UnloadAddonChrome(AiSlot);
        SetLocalSearchStatus("Removing local search…");
        RefreshLocalSearch();
        _ = Task.Run(() =>
        {
            string done = "Local search removed.";
            foreach (AddonSlot slot in new[] { CudaSlot, AudioSlot, FacesSlot, AiSlot })
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
    // then start the next queued install (its 3 GB check sees this one).
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
                foreach (AddonSlot gone in new[] { AiSlot, FacesSlot, AudioSlot, CudaSlot }) gone.Removing = false;
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
            foreach (AddonSlot piece in new[] { AiSlot, FacesSlot, AudioSlot })
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

    /// <summary>Refused before anything is downloaded (plan/17: 3 GB).</summary>
    private sealed class OverBudgetException(string sentence) : Exception(sentence);

    private static string? BudgetRefusal(AddonSlot slot, long installedSize)
    {
        if (_aiCeiling == 0 || installedSize <= 0) return null;
        long current = slot.State.Installed ? slot.State.Size : 0;
        long after = (long)_aiUsed - current + installedSize;
        if (after <= (long)_aiCeiling) return null;
        long free = Math.Max(0, (long)_aiCeiling - (long)_aiUsed + current);
        return $"{slot.Name} needs {Gb(installedSize)}, but only {Gb(free)} of the {Gb((long)_aiCeiling)} " +
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
        if (slot == AiSlot) _aiQueue.Insert(0, slot);
        else _aiQueue.Add(slot);
        StartNextPiece();
    }

    private static void BeginPieceInstall(AddonSlot slot)
    {
        _aiInstalling = slot;
        slot.Busy = true;
        string what = slot == AiSlot ? "local search" : slot.Name;
        // An update installs beside the running copy (the store keeps it until
        // the next start). A new Core takes over then; a new piece at once.
        string? update = UpdateVersion(slot);
        bool coreRunning = slot == AiSlot && AiSlot.Chrome is not null;
        SetLocalSearchStatus(string.Join(" ", _aiBatchNotes.Append(
            update is null ? $"Downloading {what}…" : $"Downloading {what} {update}…")));
        slot.Phase = new AddonPhase(AddonPhaseKind.Downloading);
        IProgress<AddonPhase> progress = PhaseReporter(slot);
        RefreshLocalSearch();
        long ceiling = (long)_aiCeiling;
        _ = Task.Run(async () =>
        {
            string message;
            try
            {
                await DownloadAndInstall(slot, manifest =>
                {
                    // The signed size, checked again before the archive is
                    // requested; mv_addon_install refuses it a third time.
                    long size = manifest.TryGetProperty("installed_size", out JsonElement s) ? s.GetInt64() : 0;
                    (ulong used, ulong cap) = AddonNative.FamilyUsage(AiFamily);
                    _aiUsed = used;
                    _aiCeiling = cap;
                    if (BudgetRefusal(slot, size) is string no) throw new OverBudgetException(no);
                }, progress).ConfigureAwait(false);
                string capital = $"{char.ToUpperInvariant(what[0])}{what[1..]}";
                message = update is null ? $"{capital} installed."
                    : coreRunning ? $"Local search {update} is installed. It takes over the next time MediaViewer starts."
                    : $"{capital} updated to {update}.";
            }
            catch (OverBudgetException ex)
            {
                message = ex.Message;
            }
            catch (AddonNotPublishedException)
            {
                message = "";
                DispatcherQueueControllerTryEnqueue(() => slot.Offer = new AddonOffer(OfferKind.NotPublished, 0));
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or MediaViewerException
                                           or InvalidDataException or TaskCanceledException or JsonException)
            {
                System.Diagnostics.Debug.WriteLine(ex.Message);
                message = ex is MediaViewerException { Status: MvStatus.UnsupportedFormat } && ceiling > 0
                    ? "This piece would take local search over its 3 GB, so it was not installed."
                    : ex is MediaViewerException or InvalidDataException
                        ? "The download did not verify, so nothing was installed."
                        : $"{char.ToUpperInvariant(what[0])}{what[1..]} could not be downloaded. Check the connection and try again.";
            }
            catch (Exception ex)
            {
                // Anything else still finishes this install, so the queue moves on.
                System.Diagnostics.Debug.WriteLine(ex);
                message = $"{char.ToUpperInvariant(what[0])}{what[1..]} could not be installed.";
            }
            FinishAiChange(slot, message, load: slot == AiSlot, install: true);
        });
    }

    // ---- NVIDIA detection -----------------------------------------------------------------

    private static readonly Guid IidDxgiFactory1 = new("770aae78-f26f-4dba-a829-253c83d1b387");

    [DllImport("dxgi.dll")]
    private static extern int CreateDXGIFactory1(in Guid riid, out IntPtr factory);

    /// <summary>
    /// Worker. Whether any adapter is NVIDIA's (DXGI vendor 0x10DE), for the
    /// "NVIDIA acceleration" piece: offered on hardware that can use it, never
    /// auto-installed (plan/17). Raw vtable calls; the shell owns no DXGI
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
