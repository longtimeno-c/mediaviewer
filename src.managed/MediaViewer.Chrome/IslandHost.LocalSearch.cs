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
    private static bool _aiUsageRead;
    private static bool _confirmCoreRemove;
    private static ScaleTransform? _budgetFill;
    private static double _budgetShown;

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
            // An older core without 0.13 result listings.
            _pendingListGallery = null;
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return;
        }
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
    }

    private static void OnSearchChromeDetached()
    {
        _aiSettingsPanel = null;
        HostSetIndexingPill(null, false);
        HostSetScrubMarkers(Array.Empty<long>(), -1);
        _pendingListGallery = null;
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
        TextBlock heading = SettingsSection("Local search");
        _localSearchHeading = heading;
        view.Children.Add(heading);
        _localSearchPanel = new StackPanel { Spacing = 8 };
        _localSearchStatus = WrappedLabel("");
        view.Children.Add(_localSearchPanel);
        view.Children.Add(_localSearchStatus);
        RefreshLocalSearch();
        // Opening Settings asks the channel for what is not installed, like
        // Import: the person is looking at what can be installed. The GETs
        // carry nothing about them.
        foreach (AddonSlot piece in new[] { AiSlot, FacesSlot, AudioSlot })
        {
            if (!piece.State.Installed && piece.Offer.Kind is OfferKind.Unknown or OfferKind.Unreachable) ProbeOffer(piece);
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
                _aiUsageRead = true;
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
                    install.IsEnabled = !AiSlot.Busy;
                    install.HorizontalAlignment = HorizontalAlignment.Left;
                    card.Children.Add(install);
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
        _localSearchPanel.Children.Add(BuildBudgetBar());

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
        var sizeText = Label(size > 0 ? Gb(size) : "");
        sizeText.FontSize = 12;
        sizeText.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(sizeText, 1);
        row.Children.Add(sizeText);

        FrameworkElement action;
        if (slot.State.Installed)
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
            remove.IsEnabled = !slot.Busy;
            AutomationProperties.SetName(remove, "Remove " + title);
            action = remove;
        }
        else if (slot.Parent is not null && !AiSlot.State.Installed)
        {
            action = Small("Install Core first");
        }
        else if (slot.Offer.Kind == OfferKind.Available)
        {
            Button install = SettingsButton("Install", () => StartPieceInstall(slot));
            install.IsEnabled = !slot.Busy && !AnyAiBusy();
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

    // Used / 3 GB, animated when a piece comes or goes.
    private static FrameworkElement BuildBudgetBar()
    {
        var panel = new StackPanel { Spacing = 4, Margin = new Thickness(0, 4, 0, 0) };
        if (!_aiUsageRead || _aiCeiling == 0) return panel;
        double fraction = Math.Clamp((double)_aiUsed / _aiCeiling, 0, 1);
        _budgetFill = new ScaleTransform { ScaleX = _budgetShown };
        var fill = new Border
        {
            Background = Brush(ChromeColour.TrimAccent),
            CornerRadius = new CornerRadius(3),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            RenderTransform = _budgetFill,
        };
        var track = new Grid
        {
            Height = 6,
            CornerRadius = new CornerRadius(3),
            Background = Brush(Hairline),
            Children = { fill },
        };
        panel.Children.Add(track);
        panel.Children.Add(Small($"{Gb((long)_aiUsed)} of {Gb((long)_aiCeiling)} used by local search " +
                                 "(the search index is your data and does not count)."));
        AutomationProperties.SetName(track, $"Local search uses {Gb((long)_aiUsed)} of {Gb((long)_aiCeiling)}");
        if (Math.Abs(fraction - _budgetShown) > 0.0005)
        {
            if (!HostAnimationsEnabled())
            {
                _budgetFill.ScaleX = fraction;
            }
            else
            {
                var grow = new DoubleAnimation
                {
                    From = _budgetShown,
                    To = fraction,
                    Duration = new Duration(TimeSpan.FromMilliseconds(420)),
                    EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
                };
                Storyboard.SetTarget(grow, _budgetFill);
                Storyboard.SetTargetProperty(grow, "ScaleX");
                var board = new Storyboard();
                board.Children.Add(grow);
                board.Begin();
            }
            _budgetShown = fraction;
        }
        return panel;
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
        if (AnyAiBusy()) return;
        _confirmCoreRemove = false;
        AiSlot.Busy = true;
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
        if (AnyAiBusy()) return;
        slot.Busy = true;
        SetLocalSearchStatus($"Removing {slot.Name}…");
        RefreshLocalSearch();
        _ = Task.Run(() =>
        {
            string done;
            try
            {
                AddonNative.Remove(slot.Id, keepData);
                done = $"{slot.Name} removed.";
            }
            catch (MediaViewerException)
            {
                done = $"{slot.Name} will finish uninstalling the next time MediaViewer starts.";
            }
            FinishAiChange(slot, done, load: false);
        });
    }

    // Worker in, UI out: re-read what is installed and the family's room.
    private static void FinishAiChange(AddonSlot slot, string message, bool load)
    {
        Dictionary<string, AddonState> states = ReadAddonStates();
        (ulong used, ulong ceiling) = (0, 0);
        try { (used, ceiling) = AddonNative.FamilyUsage(AiFamily); }
        catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        DispatcherQueueControllerTryEnqueue(() =>
        {
            slot.Busy = false;
            ApplyAddonStates(states);
            _aiUsed = used;
            _aiCeiling = ceiling;
            _aiUsageRead = true;
            SetLocalSearchStatus(message);
            RefreshLocalSearch();
            if (load && AiSlot.Usable && AiSlot.Chrome is null) LoadAddon(AiSlot);
            // A piece came or went under a loaded pack: it picks People up at
            // once; NVIDIA acceleration waits for a restart (its panel says so).
            if (slot.Parent is not null && SearchChrome is ISearchChrome search)
            {
                try { search.OnPiecesChanged(); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            }
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

    private static void StartPieceInstall(AddonSlot slot)
    {
        if (AnyAiBusy()) return;
        if (BudgetRefusal(slot, slot.Offer.InstalledSize) is string refused)
        {
            SetLocalSearchStatus(refused);
            return;
        }
        slot.Busy = true;
        string what = slot == AiSlot ? "local search" : slot.Name;
        SetLocalSearchStatus($"Downloading {what}…");
        RefreshLocalSearch();
        long ceiling = (long)_aiCeiling;
        var progress = new Progress<double>(f => SetLocalSearchStatus($"Downloading {what}… {f * 100:0}%"));
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
                message = $"{char.ToUpperInvariant(what[0])}{what[1..]} installed.";
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
            FinishAiChange(slot, message, load: slot == AiSlot);
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
