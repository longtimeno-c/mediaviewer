// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// Slots (docs/design/25 §7, PR 59): what MediaViewer's own add-ons add to pages
/// the app already has, read from the core's registry (<see cref="SlotNative"/>).
/// The Mac twin is SlotStore.swift and SlotItems.swift.
/// </summary>
/// <remarks>
/// Everything here is a table: the contributions are read when an add-on
/// attaches or detaches, trees and rows when the core says they changed (one
/// listener, marshalled here). Nothing is asked of an add-on per frame, per
/// tile or per key: a tile's badges are a dictionary lookup, and typing posts
/// a query and returns. With no such add-on installed every list is empty and
/// nothing is drawn. Add-ons that fill slots load on a worker after the
/// first-party ones (never ahead of the window's first pixel) and are removed
/// off the UI thread, since detaching waits for a call the add-on is in.
/// </remarks>
public static partial class IslandHost
{
    private sealed record SlotItem(string Addon, string Id, string Slot, string Label, string Symbol, string Target);
    private sealed record SlotBadge(string Key, string Label, string Symbol);
    private sealed record SlotSectionDef(string Addon, string Id, bool Location);
    private sealed record SlotAddonRow(string Id, string Name, string Version, string Description, string State, bool Loaded);

    // One UI-thread hop per burst, as the Mac host does: a provider posting
    // progress 50 times re-reads (and re-filters the gallery) once, not 50 times.
    private static int _slotPendingBits;
    private static readonly SlotNative.ChangedCallback SlotListener = (_, bits) =>
    {
        if (Interlocked.Or(ref _slotPendingBits, (int)bits) != 0) return;
        if (_dispatcher is null)
        {
            // No UI thread to hop to: drop the burst so a later one still wakes it.
            Interlocked.Exchange(ref _slotPendingBits, 0);
            return;
        }
        DispatcherQueueControllerTryEnqueue(() =>
            OnSlotsChanged((SlotChange)Interlocked.Exchange(ref _slotPendingBits, 0)));
    };

    private static bool _slotsListening;
    private static List<SlotItem> _slotItems = new();
    private static List<SlotSectionDef> _slotSections = new();
    private static readonly Dictionary<string, SlotBadge> SlotBadgeDefs = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, string> SlotAddonDirs = new(StringComparer.Ordinal);
    private static Dictionary<string, List<SlotBadge>> _slotBadgesByPath = new(StringComparer.Ordinal);
    private static string _slotInfoJson = "";
    private static string _slotInfoKey = "";
    private static string _slotBadgeFolder = "";
    private static ulong _slotFileGeneration;
    private static HashSet<string> _slotFileHits = new(StringComparer.Ordinal);
    private static string _slotFileNote = "";
    private static List<(string Path, string Why)> _slotLocalRows = new();
    private static List<(string Label, string Text)> _slotStatus = new();
    private static string _slotListTitle = "";
    private static readonly List<SlotAddonRow> SlotAddonRows = new();
    private static readonly HashSet<string> SlotAddonsLoaded = new(StringComparer.Ordinal);
    private static StackPanel? _slotBarPanel;
    private static StackPanel? _slotPathPanel;
    private static StackPanel? _slotSettingsPanel;
    private static event Action? SlotBadgesChanged;

    private static void EnsureSlotListener()
    {
        if (_slotsListening) return;
        _slotsListening = SlotNative.SetListener(SlotListener);
        if (_slotsListening) OnSlotsChanged((SlotChange)0x3F);
    }

    // ---- loading the add-ons that fill slots --------------------------------------

    // Worker, from StartAddons' read of what is installed: MediaViewer's own
    // add-ons that contribute screens or slots, other than Import and the AI
    // family (they have chromes of their own).
    private static void StartSlotAddons(MediaViewerSession session)
    {
        var found = new List<SlotAddonRow>();
        try
        {
            using JsonDocument doc = JsonDocument.Parse(AddonNative.InstalledJson());
            foreach (JsonElement a in doc.RootElement.EnumerateArray())
            {
                string id = JStr(a, "id");
                bool slots = a.TryGetProperty("slots", out JsonElement s) && s.ValueKind == JsonValueKind.True;
                if (!slots || id == "import" || id.StartsWith("ai", StringComparison.Ordinal) || JStr(a, "part_of").Length > 0) continue;
                found.Add(new SlotAddonRow(id, JStr(a, "name"), JStr(a, "version"), JStr(a, "description"), JStr(a, "state"), false));
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        var loaded = new List<string>();
        foreach (SlotAddonRow row in found)
        {
            if (row.State != "ok") continue;
            try
            {
                // Re-verifies, loads, and attaches what the manifest contributes.
                _ = AddonNative.Load(session, row.Id, SlotNative.Interface);
                loaded.Add(row.Id);
            }
            catch (MediaViewerException ex)
            {
                System.Diagnostics.Debug.WriteLine(ex.Message);
            }
        }
        DispatcherQueueControllerTryEnqueue(() =>
        {
            SlotAddonRows.Clear();
            SlotAddonRows.AddRange(found);
            SlotAddonsLoaded.Clear();
            foreach (string id in loaded) SlotAddonsLoaded.Add(id);
            EnsureSlotListener();
            RefreshSlotSettings();
        });
    }

    // Removing takes every item it added away at once, with no restart. Off the
    // UI thread: unloading detaches it, which waits for a call it is in.
    private static void RemoveSlotAddon(string id, bool keepData)
    {
        SlotAddonRows.RemoveAll(r => r.Id == id);
        SlotAddonsLoaded.Remove(id);
        RefreshSlotSettings();
        _ = Task.Run(() =>
        {
            try { AddonNative.Remove(id, keepData); }
            catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        });
    }

    // ---- the registry changed ----------------------------------------------------------

    private static void OnSlotsChanged(SlotChange bits)
    {
        if (bits.HasFlag(SlotChange.Contributions)) ReadSlotContributions();
        if (bits.HasFlag(SlotChange.Info)) ReadSlotInfo();
        if (bits.HasFlag(SlotChange.Screens)) ReadSlotScreens();
        if (bits.HasFlag(SlotChange.Search)) ReadSlotSearch();
        if (bits.HasFlag(SlotChange.Badges)) ReadSlotBadges();
        if (bits.HasFlag(SlotChange.Status)) ReadSlotStatus();
    }

    private static void ReadSlotContributions()
    {
        string json = SlotNative.ContributionsJson();
        _slotItems = new List<SlotItem>();
        _slotSections = new List<SlotSectionDef>();
        SlotBadgeDefs.Clear();
        SlotAddonDirs.Clear();
        SlotFilePrefixes.Clear();
        if (json.Length > 0)
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(json);
                JsonElement r = doc.RootElement;
                foreach (JsonElement a in JArr(r, "addons")) SlotAddonDirs[JStr(a, "id")] = JStr(a, "dir");
                foreach (JsonElement s in JArr(r, "sections"))
                    _slotSections.Add(new SlotSectionDef(JStr(s, "addon"), JStr(s, "id"), JFlag(s, "location")));
                foreach (JsonElement i in JArr(r, "items"))
                    _slotItems.Add(new SlotItem(JStr(i, "addon"), JStr(i, "id"), JStr(i, "slot"), JStr(i, "label"), JStr(i, "symbol"), JStr(i, "target")));
                foreach (JsonElement p in JArr(r, "providers"))
                {
                    if (!JFlag(p, "files")) continue;
                    foreach (JsonElement pre in JArr(p, "prefixes"))
                    {
                        if (pre.ValueKind == JsonValueKind.String) SlotFilePrefixes.Add(pre.GetString() ?? "");
                    }
                }
                foreach (JsonElement b in JArr(r, "badges"))
                    SlotBadgeDefs[JStr(b, "key")] = new SlotBadge(JStr(b, "key"), JStr(b, "label"), JStr(b, "symbol"));
            }
            catch (JsonException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        }
        // A new add-on may want the file on screen and the folder.
        _slotInfoKey = "";
        AskSlotInfo();
        string folder = _slotBadgeFolder;
        _slotBadgeFolder = "";
        if (SlotBadgeDefs.Count > 0 && folder.Length > 0) RequestSlotBadges(folder);
        else if (SlotBadgeDefs.Count == 0 && _slotBadgesByPath.Count > 0)
        {
            _slotBadgesByPath = new Dictionary<string, List<SlotBadge>>(StringComparer.Ordinal);
            SlotBadgesChanged?.Invoke();
        }
        RebuildSlotBar();
        RebuildSlotPathItems();
        // Gallery tiles carry badge / context wrappers only while something fills
        // them: when that changes, the realised tiles are made again.
        bool badges = SlotBadgeDefs.Count > 0;
        bool context = _slotItems.Any(i => i.Slot == "context");
        if (badges != _slotTileBadges || context != _slotTileContext)
        {
            _slotTileBadges = badges;
            _slotTileContext = context;
            RemakeGalleryTiles();
        }
    }

    private static bool _slotTileBadges;
    private static bool _slotTileContext;

    private static void RemakeGalleryTiles()
    {
        ItemsRepeater? repeater = _galleryRepeater;
        if (repeater?.ItemsSource is not { } source) return;
        try
        {
            repeater.ItemsSource = null;
            repeater.ItemsSource = source;
        }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
    }

    private static string? SlotImagePath(string addon, string src)
    {
        if (src.Length == 0 || src.Contains("..", StringComparison.Ordinal)) return null;
        return SlotAddonDirs.TryGetValue(addon, out string? dir) && dir.Length > 0
            ? Path.Combine(dir, src.Replace('/', Path.DirectorySeparatorChar)) : null;
    }

    // ---- info-pane sections ----------------------------------------------------------

    // The Metadata pane has a record (or was shown): ask once per file, only when
    // something contributes a section; one that needs a location is not asked
    // for a file without one (the core skips it).
    private static void AskSlotInfo()
    {
        if (!_metaPaneVisible || _metaSummary.Count == 0 || _slotSections.Count == 0)
        {
            if (_slotInfoKey.Length > 0)
            {
                _slotInfoKey = "";
                SlotNative.ClearInfo();
            }
            return;
        }
        string path = _selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex].Path : "";
        if (path.Length == 0) return;
        string location = _metaSummary.FirstOrDefault(r => r.Label == "Location").Value ?? "";
        // The sections' content width: the pane less its 14 px insets, so a surface is pixel-exact.
        double pane = (_metaPane?.Content as FrameworkElement)?.ActualWidth ?? 0;
        double width = Math.Max(64, Math.Round((pane > 0 ? pane : 320) / 8) * 8 - 28);
        string key = path + "\n" + location + "\n" + width.ToString(System.Globalization.CultureInfo.InvariantCulture);
        if (key == _slotInfoKey) return;
        _slotInfoKey = key;
        SlotNative.RequestInfo(path, location, SlotTheme(), (float)width, (float)SlotScale());
    }

    private static void ReadSlotInfo()
    {
        string json = SlotNative.InfoJson();
        // No early return on equal text: new surface pixels arrive with the same tree.
        _slotInfoJson = json;
        FillSlotSections();
    }

    // The pane's bottom area (BuildMetaPane): the add-ons' sections after all the
    // built-in metadata, each under its heading; empty when none answered.
    private static StackPanel? _metaSlots;

    // What the panel shows: the info generation (a new file), then each section's
    // address and tree revision, in order. Two children per section (rule, section).
    private static ulong _slotShownGeneration;
    private static readonly List<(ScreenAt At, long Revision)> SlotShownSections = new();
    private static StackPanel? _slotShownPanel;

    // Only what changed: a new file or a different set of sections rebuilds the
    // panel; a section whose tree moved is rebuilt alone, in place; a section whose
    // pixels alone moved keeps its elements and swaps the pixels (RefreshSurfaces),
    // so a pinch or drag on the map is not reset mid-gesture.
    private static void FillSlotSections()
    {
        StackPanel? rows = _metaSlots;
        if (rows is null) return;
        var sections = new List<(ScreenAt At, long Revision, string Title, JsonElement Tree)>();
        ulong generation = 0;
        JsonDocument? doc = null;
        try
        {
            if (_slotInfoJson.Length > 0)
            {
                doc = JsonDocument.Parse(_slotInfoJson);
                generation = (ulong)JNum(doc.RootElement, "generation", 0);
                foreach (JsonElement s in JArr(doc.RootElement, "sections"))
                {
                    if (!s.TryGetProperty("tree", out JsonElement tree) || tree.ValueKind != JsonValueKind.Object) continue;
                    sections.Add((new ScreenAt(JStr(s, "addon"), JStr(s, "id")), (long)JNum(s, "revision", 0),
                                  JStr(s, "title"), tree));
                }
            }
            bool sameSet = ReferenceEquals(rows, _slotShownPanel) && generation == _slotShownGeneration &&
                           sections.Count == SlotShownSections.Count &&
                           rows.Children.Count == sections.Count * 2 &&
                           sections.Select(x => x.At).SequenceEqual(SlotShownSections.Select(x => x.At));
            if (!sameSet)
            {
                foreach ((ScreenAt at, _) in SlotShownSections) ForgetSurfaces(at);
                rows.Children.Clear();
                SlotShownSections.Clear();
                foreach (var s in sections)
                {
                    rows.Children.Add(new Border { Height = 1, Background = Brush(Hairline) });
                    rows.Children.Add(BuildSlotSection(s.At, s.Title, s.Tree));
                    SlotShownSections.Add((s.At, s.Revision));
                }
                _slotShownPanel = rows;
                _slotShownGeneration = generation;
                return;
            }
            for (int i = 0; i < sections.Count; ++i)
            {
                var s = sections[i];
                if (s.Revision != SlotShownSections[i].Revision)
                {
                    rows.Children[i * 2 + 1] = BuildSlotSection(s.At, s.Title, s.Tree);
                    SlotShownSections[i] = (s.At, s.Revision);
                }
                else
                {
                    RefreshSurfaces(s.At);
                }
            }
        }
        catch (JsonException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        finally { doc?.Dispose(); }
    }

    private static UIElement BuildSlotSection(ScreenAt at, string title, JsonElement tree)
    {
        var section = new StackPanel { Spacing = 6, Padding = new Thickness(14, 10, 14, 14) };
        TextBlock heading = Text(title, Body, UiFontSize - 3);
        AutomationProperties.SetHeadingLevel(heading, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        section.Children.Add(heading);
        section.Children.Add(BuildScreenNode(tree, at));
        return section;
    }

    private static uint Rgba(ChromeColour role)
    {
        Windows.UI.Color c = Brush(role).Color;
        return (uint)c.R << 24 | (uint)c.G << 16 | (uint)c.B << 8 | c.A;
    }

    private static MvSlotTheme SlotTheme() => new()
    {
        Canvas = Rgba(Canvas),
        Surface = Rgba(ChromeColour.Surface),
        Title = Rgba(Title),
        Body = Rgba(Body),
        Disabled = Rgba(Body),
        Hairline = Rgba(Hairline),
        Accent = Rgba(TrimAccent),
        Dark = IslandTheme == ElementTheme.Dark ? 1u : 0u,
    };

    private static double SlotScale() => _metaPane?.Content?.XamlRoot?.RasterizationScale ?? 1.0;

    // ---- screens ------------------------------------------------------------------------

    private static void ReadSlotScreens()
    {
        string json = SlotNative.ScreensJson();
        if (json.Length == 0) return;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            SyncScreenWindows(doc.RootElement);
        }
        catch (JsonException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
    }

    // ---- search providers -----------------------------------------------------------

    private static void ReadSlotSearch()
    {
        ReadSlotSearchScope(SlotNative.ScopeFiles, out ulong fileGen, out List<(string, string)> fileRows,
                            out bool searching, out int read, out int total);
        if (fileGen == _slotFileGeneration && _slotFileGeneration != 0)
        {
            _slotFileHits = new HashSet<string>(fileRows.Select(r => r.Item1), StringComparer.Ordinal);
            _slotFileNote = searching && total > 0 ? $"Still reading tags — {read:N0} of {total:N0}"
                : fileRows.Count > 0 ? fileRows[0].Item2 : "";
            // The name pass again, with the providers' rows; the query is not re-sent.
            if (_gsOpen) ApplyNamesFilter(fromProviders: true);
        }
        ReadSlotSearchScope(SlotNative.ScopeLocal, out _, out List<(string, string)> localRows, out _, out _, out _);
        _slotLocalRows = localRows.Where(r => r.Item1.Length > 0).ToList();
        RebuildSlotPathItems();
    }

    private static void ReadSlotSearchScope(uint scope, out ulong generation, out List<(string, string)> rows,
                                            out bool searching, out int read, out int total)
    {
        generation = 0;
        rows = new List<(string, string)>();
        searching = false;
        read = 0;
        total = 0;
        string json = SlotNative.SearchJson(scope);
        if (json.Length == 0) return;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement r = doc.RootElement;
            generation = r.TryGetProperty("generation", out JsonElement g) ? g.GetUInt64() : 0;
            searching = JFlag(r, "searching");
            foreach (JsonElement p in JArr(r, "providers"))
            {
                if (JStr(p, "addon") == "ai") continue;
                read += (int)JNum(p, "read", 0);
                total += (int)JNum(p, "total", 0);
            }
            foreach (JsonElement row in JArr(r, "rows"))
            {
                if (JStr(row, "addon") == "ai") continue;
                rows.Add((JStr(row, "path"), JStr(row, "why")));
            }
        }
        catch (Exception ex) when (ex is JsonException or FormatException or InvalidOperationException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
    }

    /// <summary>File search typed: the providers hear the query on their workers; returns at once.</summary>
    private static void PostSlotFileSearch(string typed)
    {
        if (typed.Length == 0)
        {
            if (_slotFileGeneration == 0) return;
            _slotFileGeneration = 0;
            _slotFileHits = new HashSet<string>(StringComparer.Ordinal);
            _slotFileNote = "";
            SlotNative.Search(SlotNative.ScopeFiles, "", "");
            return;
        }
        _slotFileHits = new HashSet<string>(StringComparer.Ordinal);
        _slotFileGeneration = SlotNative.Search(SlotNative.ScopeFiles, typed, _openedFolder);
    }

    // A prefix a file-search provider declares ("place:") is that provider's alone:
    // names are not matched. Any other "word:" query is an ordinary name query
    // (with Places not installed, "place:lisbon" still searches names). The core
    // routes the same way (slot_registry::search): a digit after the prefix is a
    // date term and is not routed.
    private static readonly List<string> SlotFilePrefixes = new();

    private static bool SlotPrefixed(string typed)
    {
        if (SlotFilePrefixes.Count == 0) return false;
        string f = FoldName(typed.Trim());
        foreach (string pre in SlotFilePrefixes)
        {
            if (pre.Length == 0 || !f.StartsWith(pre, StringComparison.Ordinal)) continue;
            int after = pre.Length;
            while (after < f.Length && f[after] == ' ') ++after;
            if (after < f.Length && char.IsAsciiDigit(f[after])) continue;
            return true;
        }
        return false;
    }

    // ---- badges -------------------------------------------------------------------------

    /// <summary>The gallery shows <paramref name="folder"/>: one request per folder listing
    /// (a relist, e.g. files copied in, asks again: the new files need their badges).</summary>
    private static int _slotBadgeListing = -1;

    private static void RequestSlotBadges(string folder)
    {
        if (SlotBadgeDefs.Count == 0 || folder.Length == 0) return;
        if (folder == _slotBadgeFolder && _gsListing == _slotBadgeListing) return;
        _slotBadgeFolder = folder;
        _slotBadgeListing = _gsListing;
        SlotNative.RequestBadges(folder);
    }

    private static void ReadSlotBadges()
    {
        string json = SlotNative.BadgesJson();
        var map = new Dictionary<string, List<SlotBadge>>(StringComparer.Ordinal);
        if (json.Length > 0)
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(json);
                if (doc.RootElement.TryGetProperty("tiles", out JsonElement tiles) && tiles.ValueKind == JsonValueKind.Object)
                {
                    foreach (JsonProperty t in tiles.EnumerateObject())
                    {
                        var list = new List<SlotBadge>();
                        foreach (JsonElement k in t.Value.EnumerateArray())
                        {
                            if (SlotBadgeDefs.TryGetValue(k.GetString() ?? "", out SlotBadge? b)) list.Add(b);
                        }
                        if (list.Count > 0) map[t.Name] = list;
                    }
                }
            }
            catch (JsonException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        }
        _slotBadgesByPath = map;
        SlotBadgesChanged?.Invoke();
    }

    // A gallery tile's badges, kept current while the tile is realised: a table
    // lookup, never an add-on call (PR 59 verify: counted).
    private static UIElement WithSlotBadges(UIElement thumb, FolderItemVm vm, out Action unsubscribe)
    {
        var grid = new Grid();
        grid.Children.Add(thumb);
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 2,
            Margin = new Thickness(4),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Bottom,
            IsHitTestVisible = false,
        };
        grid.Children.Add(row);
        void Fill()
        {
            row.Children.Clear();
            if (!_slotBadgesByPath.TryGetValue(vm.Path, out List<SlotBadge>? badges))
            {
                AutomationProperties.SetHelpText(grid, "");
                return;
            }
            foreach (SlotBadge b in badges)
            {
                row.Children.Add(new Border
                {
                    Width = 18,
                    Height = 18,
                    CornerRadius = new CornerRadius(9),
                    Background = Brush(TrimAccent),
                    Child = new FontIcon { Glyph = SlotGlyph(b.Symbol), FontFamily = SymbolFont, FontSize = 10, Foreground = new SolidColorBrush(Colors.White) },
                });
            }
            AutomationProperties.SetHelpText(grid, string.Join(", ", badges.Select(b => b.Label)));
        }
        Fill();
        Action handler = Fill;
        SlotBadgesChanged += handler;
        unsubscribe = () => SlotBadgesChanged -= handler;
        return grid;
    }

    // ---- items ---------------------------------------------------------------------------

    private static List<string> SlotTargetPaths(SlotItem item, FolderItemVm? tile = null)
    {
        switch (item.Target)
        {
            case "folder":
                return _openedFolder.Length > 0 ? new List<string> { _openedFolder } : new List<string>();
            case "file":
            {
                string path = tile?.Path ?? (_selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex].Path : "");
                return path.Length > 0 ? new List<string> { path } : new List<string>();
            }
            case "files":
            {
                List<string> marked = _lastMarks.ToList();
                FolderItemVm? at = tile ?? (_selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex] : null);
                if (marked.Count > 0 && (at is null || marked.Contains(at.Path))) return marked;
                return at is null ? new List<string>() : new List<string> { at.Path };
            }
            default:
                return new List<string>();
        }
    }

    private static void InvokeSlotItem(SlotItem item, FolderItemVm? tile = null) =>
        SlotNative.Invoke(item.Addon, item.Id, JsonList(SlotTargetPaths(item, tile)));

    // The add-ons' command-bar buttons and status text, after the chrome's own.
    private static UIElement BuildSlotBarItems()
    {
        _slotBarPanel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        RebuildSlotBar();
        return _slotBarPanel;
    }

    private static void RebuildSlotBar()
    {
        if (_slotBarPanel is null) return;
        _slotBarPanel.Children.Clear();
        foreach (SlotItem item in _slotItems.Where(i => i.Slot == "bar"))
        {
            Button b = PathIconButton(SlotGlyph(item.Symbol), () => InvokeSlotItem(item));
            if (b.Content is FontIcon icon) icon.FontFamily = SymbolFont;
            AutomationProperties.SetName(b, item.Label);
            ToolTipService.SetToolTip(b, item.Label);
            _slotBarPanel.Children.Add(b);
        }
        foreach ((string label, string text) in _slotStatus)
        {
            TextBlock t = Text(text, Body, UiFontSize - 3, maxLines: 1);
            t.VerticalAlignment = VerticalAlignment.Center;
            t.Margin = new Thickness(6, 0, 6, 0);
            ToolTipService.SetToolTip(t, label);
            AutomationProperties.SetName(t, $"{label}: {text}");
            _slotBarPanel.Children.Add(t);
        }
    }

    private static void ReadSlotStatus()
    {
        _slotStatus = new List<(string, string)>();
        string json = SlotNative.StatusJson();
        if (json.Length > 0)
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(json);
                foreach (JsonElement s in doc.RootElement.EnumerateArray()) _slotStatus.Add((JStr(s, "label"), JStr(s, "text")));
            }
            catch (JsonException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        }
        RebuildSlotBar();
    }

    // The add-ons' path-bar icons, after the search icon; and, under a Local
    // search result list, what the other local-scope providers matched.
    private static UIElement BuildSlotPathItems()
    {
        _slotPathPanel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        RebuildSlotPathItems();
        return _slotPathPanel;
    }

    private static void RebuildSlotPathItems()
    {
        if (_slotPathPanel is null) return;
        _slotPathPanel.Children.Clear();
        foreach (SlotItem item in _slotItems.Where(i => i.Slot == "path"))
        {
            Button b = PathIconButton(SlotGlyph(item.Symbol), () => InvokeSlotItem(item));
            if (b.Content is FontIcon icon) icon.FontFamily = SymbolFont;
            AutomationProperties.SetName(b, item.Label);
            ToolTipService.SetToolTip(b, item.Label);
            _slotPathPanel.Children.Add(b);
        }
        if (_listOpen && _slotLocalRows.Count > 0)
        {
            string why = _slotLocalRows.Select(r => r.Why).Distinct().Count() == 1 ? _slotLocalRows[0].Why : "";
            Button more = EditButton(why.Length > 0 ? $"+ {_slotLocalRows.Count} in {why}" : $"+ {_slotLocalRows.Count} more from add-ons",
                AddSlotRowsToList, tip: "Add what other Local search providers matched to these results");
            AutomationProperties.SetName(more, $"Add {_slotLocalRows.Count} more results from add-ons");
            _slotPathPanel.Children.Add(more);
        }
        MeasurePathFixed();
    }

    // docs/design/25 §7: a Local search query went to the other local-scope
    // providers too (HostOpenList); one click adds their rows to the list.
    private static void AddSlotRowsToList()
    {
        var paths = Items.Select(i => i.Path).ToList();
        var seen = new HashSet<string>(paths, StringComparer.Ordinal);
        foreach ((string path, _) in _slotLocalRows)
        {
            if (seen.Add(path)) paths.Add(path);
        }
        string title = _slotListTitle;
        _slotLocalRows = new List<(string, string)>();
        SlotNative.Search(SlotNative.ScopeLocal, "", "");
        HostOpenList(title, paths, null, 0, true);
    }

    // A gallery tile's context menu: the add-ons' entries (Shift+F10 or the
    // menu key on a focused tile too). Nothing when none contributes any.
    private static void AttachSlotContextMenu(FrameworkElement tile, FolderItemVm vm)
    {
        tile.ContextRequested += (_, e) =>
        {
            List<SlotItem> entries = _slotItems.Where(i => i.Slot == "context").ToList();
            if (entries.Count == 0) return;
            var menu = new MenuFlyout();
            foreach (SlotItem item in entries)
            {
                var mi = new MenuFlyoutItem { Text = item.Label };
                mi.Click += (_, _) => InvokeSlotItem(item, vm);
                menu.Items.Add(mi);
            }
            if (e.TryGetPosition(tile, out Windows.Foundation.Point at))
            {
                menu.ShowAt(tile, new Microsoft.UI.Xaml.Controls.Primitives.FlyoutShowOptions { Position = at });
            }
            else
            {
                menu.ShowAt(tile);
            }
            e.Handled = true;
        };
    }

    // ---- Settings -> Add-ons ------------------------------------------------------------

    private static void AddSlotAddonsSettingsRows(StackPanel view)
    {
        _slotSettingsPanel = new StackPanel { Spacing = 6, Margin = new Thickness(0, 12, 0, 0) };
        view.Children.Add(_slotSettingsPanel);
        RefreshSlotSettings();
    }

    private static string? _slotConfirming;

    private static void RefreshSlotSettings()
    {
        if (_slotSettingsPanel is null) return;
        _slotSettingsPanel.Children.Clear();
        foreach (SlotAddonRow row in SlotAddonRows)
        {
            _slotSettingsPanel.Children.Add(Label(row.Name));
            if (row.Description.Length > 0) _slotSettingsPanel.Children.Add(WrappedLabel(row.Description));
            bool loaded = SlotAddonsLoaded.Contains(row.Id);
            _slotSettingsPanel.Children.Add(WrappedLabel(row.State switch
            {
                "ok" => loaded ? $"Installed, version {row.Version}." : $"Installed, version {row.Version}; starting…",
                "needs_update" => $"Version {row.Version} needs a newer MediaViewer.",
                _ => "The installed copy did not pass verification and is not loaded.",
            }));
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            if (_slotConfirming == row.Id)
            {
                buttons.Children.Add(EditButton("Remove, keep its data", () => { _slotConfirming = null; RemoveSlotAddon(row.Id, true); }));
                buttons.Children.Add(EditButton("Remove everything", () => { _slotConfirming = null; RemoveSlotAddon(row.Id, false); }));
                buttons.Children.Add(EditButton("Cancel", () => { _slotConfirming = null; RefreshSlotSettings(); }));
            }
            else
            {
                Button remove = EditButton("Remove…", () => { _slotConfirming = row.Id; RefreshSlotSettings(); });
                AutomationProperties.SetName(remove, $"Remove {row.Name}");
                buttons.Children.Add(remove);
            }
            foreach (UIElement child in buttons.Children)
            {
                if (child is Button b)
                {
                    b.AllowFocusOnInteraction = true;
                    b.IsTabStop = true;
                }
            }
            _slotSettingsPanel.Children.Add(buttons);
        }
    }
}
