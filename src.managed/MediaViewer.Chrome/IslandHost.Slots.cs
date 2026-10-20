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
    // The installed-list read has landed. Until then the Places row does not
    // offer Install: an empty list is also "not read yet".
    private static bool _slotAddonsRead;
    // Places has no chrome of its own. This slot is the release channel only
    // (not in AddonSlots, so LoadAddon never looks for a DLL beside it).
    private static readonly AddonSlot PlacesChannel = new("places", "Places", null, null);
    private static string _placesMessage = "";
    // Every other installed slot add-on's release channel, so Settings offers
    // its update as it does Places' (Driftwire's is DriftwireSlot). Keyed by id.
    private static readonly Dictionary<string, AddonSlot> SlotChannels = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, string> SlotUpdateMessages = new(StringComparer.Ordinal);
    private static AddonSettingsPage? _placesPage;
    private static string _placesPageVersion = "";
    private static int _placesPageGen;
    private static bool _placesPageLoading;
    private static StackPanel? _slotBarPanel;
    private static StackPanel? _slotPathPanel;
    private static StackPanel? _slotSettingsPanel;
    private static event Action? SlotBadgesChanged;

    private static void EnsureSlotListener()
    {
        if (_slotsListening) return;
        _slotsListening = SlotNative.SetListener(SlotListener);
        if (_slotsListening) OnSlotsChanged((SlotChange)0x7F);
    }

    // ---- loading the add-ons that fill slots --------------------------------------

    // Worker: MediaViewer's own add-ons that contribute screens or slots, other
    // than Import and the AI family (they have chromes of their own). Every
    // window loads them (issue #402): each window is its own process, so a later
    // one shows the same sections, providers, badges and entries, and closing
    // the first takes nothing from it. `guest`: this process does not host the
    // add-ons, so it loads them without touching the store. Found by manifest
    // (no file hashed: a later window never hashes the AI pack to find Places);
    // the load verifies the one it loads.
    private static void StartSlotAddons(MediaViewerSession session, bool guest = false)
    {
        var found = new List<SlotAddonRow>();
        try
        {
            using JsonDocument doc = JsonDocument.Parse(AddonNative.SlotAddonsJson());
            foreach (JsonElement a in doc.RootElement.EnumerateArray())
            {
                found.Add(new SlotAddonRow(JStr(a, "id"), JStr(a, "name"), JStr(a, "version"), JStr(a, "description"), JStr(a, "state"), false));
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException or EntryPointNotFoundException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        var loaded = new List<string>();
        for (int i = 0; i < found.Count; ++i)
        {
            SlotAddonRow row = found[i];
            if (row.State != "ok") continue;
            try
            {
                // Re-verifies, loads, and attaches what the manifest contributes.
                _ = AddonNative.Load(session, row.Id, SlotNative.Interface, guest: guest);
                loaded.Add(row.Id);
            }
            catch (MediaViewerException ex)
            {
                System.Diagnostics.Debug.WriteLine(ex.Message);
                // Only the load hashed it: a copy that failed is "did not pass verification".
                if (ex.Status != MvStatus.UnsupportedFormat) found[i] = row with { State = "invalid" };
            }
        }
        DispatcherQueueControllerTryEnqueue(() =>
        {
            SlotAddonRows.Clear();
            SlotAddonRows.AddRange(found);
            SlotAddonsLoaded.Clear();
            foreach (string id in loaded) SlotAddonsLoaded.Add(id);
            _slotAddonsRead = true;
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
        SlotUpdateMessages.Remove(id);
        if (id == PlacesChannel.Id)
        {
            _placesPage = null;
            _placesPageVersion = "";
            _placesPageLoading = false;
            _placesPageGen++;
            _placesMessage = "";
        }
        RefreshSlotSettings();
        _ = Task.Run(() =>
        {
            try { AddonNative.Remove(id, keepData); }
            catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            // "Remove everything" forgets the settings page too (PR 56, as Import).
            if (!keepData)
            {
                try { AddonNative.SettingsForget(id); }
                catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            }
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
        if (bits.HasFlag(SlotChange.Notices)) ReadSlotNotices();  // IslandHost.Notices.cs
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
                NoteSlotWindowScreens(r);  // #410
            }
            catch (JsonException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        }
        else
        {
            SlotWindowScreens.Clear();
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
        AfterSlotContributions();  // #410: Settings' Open, a share waiting on Driftwire
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
        var section = new StackPanel { Spacing = Space(6), Padding = Pad(14, 10, 14, 14) };
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
            Spacing = Space(2),
            Margin = Pad(4),
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
        _slotBarPanel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(2), VerticalAlignment = VerticalAlignment.Center };
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
            t.Margin = Pad(6, 0, 6, 0);
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
        _slotPathPanel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(2), VerticalAlignment = VerticalAlignment.Center };
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

    // The add-ons' context-menu entries for one file, each invoked with the paths
    // a gallery tile for that file would give it (the marks when it is one).
    // Null when no add-on contributes any.
    private static MenuFlyout? SlotContextMenuFor(FolderItemVm vm)
    {
        // Issue #444: Driftwire's own "Send with Driftwire…" is the Share
        // submenu's first entry, one route through the share command.
        List<SlotItem> entries = _slotItems.Where(i => i.Slot == "context" && !IsShareRoutedSlotItem(i)).ToList();
        if (entries.Count == 0) return null;
        var menu = new MenuFlyout();
        foreach (SlotItem item in entries)
        {
            var mi = new MenuFlyoutItem { Text = item.Label };
            mi.Click += (_, _) => InvokeSlotItem(item, vm);
            menu.Items.Add(mi);
        }
        return menu;
    }

    // A gallery or filmstrip tile's context menu: the add-ons' entries, then
    // Share (issue #444; IslandHost.Share.cs). Shift+F10 or the menu key on a
    // focused tile too.
    private static void AttachSlotContextMenu(FrameworkElement tile, FolderItemVm vm)
    {
        tile.ContextRequested += (_, e) =>
        {
            Windows.Foundation.Point? point = e.TryGetPosition(tile, out Windows.Foundation.Point at) ? at : null;
            MenuFlyout menu = TileContextMenuFor(vm, tile, point);
            if (point is { } p)
            {
                menu.ShowAt(tile, new Microsoft.UI.Xaml.Controls.Primitives.FlyoutShowOptions { Position = p });
            }
            else
            {
                menu.ShowAt(tile);
            }
            e.Handled = true;
        };
    }

    /// <summary>
    /// Native, on a right-click on the canvas (issue #402): { int32 x; int32 y }, client
    /// pixels of the main window. Shows the add-ons' context-menu entries for the file
    /// on screen, as its gallery tile lists them. 0 shown; 1 nothing to show.
    /// </summary>
    public static int ShowCanvasMenu(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            int x = System.Runtime.InteropServices.Marshal.ReadInt32(arg);
            int y = System.Runtime.InteropServices.Marshal.ReadInt32(arg, 4);
            FolderItemVm? vm = _selectedIndex >= 0 && _selectedIndex < Items.Count ? Items[_selectedIndex] : null;
            // The bar sits at the client origin (SheetOrigin): its coordinates are
            // the client area's, in DIPs.
            if (vm is null || _source?.Content is not FrameworkElement anchor || anchor.XamlRoot is null) return 1;
            double scale = anchor.XamlRoot.RasterizationScale > 0 ? anchor.XamlRoot.RasterizationScale : 1;
            var position = new Windows.Foundation.Point(x / scale, y / scale);
            MenuFlyout menu = TileContextMenuFor(vm, anchor, position);
            menu.ShouldConstrainToRootBounds = false;  // over the canvas, outside the bar's island
            menu.Closed += (_, _) => RestoreCanvasFocus();
            menu.ShowAt(anchor, new Microsoft.UI.Xaml.Controls.Primitives.FlyoutShowOptions
            {
                Position = position,
            });
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ---- Settings -> Add-ons ------------------------------------------------------------

    private const string PlacesFallback =
        "Place names, an offline map and search by place. Nothing is sent anywhere.";

    private const string PlacesOptional =
        "Optional, preferred. The app works without it: a geotagged photo still shows its coordinates.";

    private static void AddSlotAddonsSettingsRows(StackPanel view)
    {
        _slotSettingsPanel = new StackPanel { Spacing = Space(10), Margin = Pad(0, 2, 0, 0) };
        view.Children.Add(_slotSettingsPanel);
        RefreshSlotSettings();
        // Opening Settings asks the channel, as Import does: Places for its
        // install or update, every other installed one for its update. A later
        // window does not install (the first one hosts the add-ons).
        if (_addonsElsewhere) return;
        ProbeOffer(PlacesChannel);
        foreach (SlotAddonRow row in SlotAddonRows)
        {
            if (row.Id != PlacesChannel.Id) ProbeOffer(SlotChannelFor(row));
        }
    }

    private static AddonSlot SlotChannelFor(SlotAddonRow row)
    {
        if (row.Id == PlacesChannel.Id) return PlacesChannel;
        if (row.Id == DriftwireId) return DriftwireSlot;
        if (!SlotChannels.TryGetValue(row.Id, out AddonSlot? slot))
        {
            slot = new AddonSlot(row.Id, row.Name, null, null);
            SlotChannels[row.Id] = slot;
        }
        return slot;
    }

    private static bool IsSlotChannel(AddonSlot slot) => SlotChannels.ContainsValue(slot);

    /// <summary>The installed row's channel has a newer signed version, or null.</summary>
    private static string? SlotUpdateVersion(AddonSlot channel, SlotAddonRow row) =>
        channel.Offer.Kind == OfferKind.Available && channel.Offer.Version.Length > 0 &&
        IsNewer(channel.Offer.Version, row.Version) ? channel.Offer.Version : null;

    private static string? _slotConfirming;

    // #442: one card per add-on (the Mac's grouping in AddonsView /
    // SlotAddonsSection): its name, what it does, its state, the install bar
    // while it installs, its buttons (Open, Update, Remove), its message and
    // its settings page, together, and nothing of it outside the card.
    private static (Border Card, StackPanel Body) AddonCard(string title)
    {
        var body = new StackPanel { Spacing = Space(6) };
        TextBlock name = Label(title);
        name.Foreground = Brush(Title);
        AutomationProperties.SetHeadingLevel(name, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level3);
        body.Children.Add(name);
        var card = new Border
        {
            Child = body,
            Padding = Pad(14),
            Background = Brush(ChromeColour.Surface),
            CornerRadius = Corner(RadiusCard),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
        };
        AutomationProperties.SetName(card, title);
        return (card, body);
    }

    private static string SlotStateText(SlotAddonRow row, bool loaded) => row.State switch
    {
        "ok" => loaded ? $"Installed, version {row.Version}." : $"Installed, version {row.Version}; starting…",
        "needs_update" => $"Version {row.Version} needs a newer MediaViewer.",
        _ => "The installed copy did not pass verification and is not loaded.",
    };

    private static void RefreshSlotSettings()
    {
        if (_slotSettingsPanel is null) return;
        _slotSettingsPanel.Children.Clear();
        (Border places, StackPanel placesBody) = AddonCard("Places");
        AddPlacesSettingsRow(placesBody);
        _slotSettingsPanel.Children.Add(places);
        // Probed once the panel is built: a probe refreshes this panel, so one
        // started inside the loop would rebuild it under the loop.
        var probe = new List<AddonSlot>();
        // #410: Driftwire, offered with one click while it is not installed.
        if (!DriftwireInstalled)
        {
            (Border card, StackPanel body) = AddonCard("Driftwire");
            BuildDriftwireOffer(body, null, titled: false);
            _slotSettingsPanel.Children.Add(card);
            if (DriftwireSlot.Offer.Kind == OfferKind.Unknown) probe.Add(DriftwireSlot);
        }
        foreach (SlotAddonRow row in SlotAddonRows)
        {
            if (row.Id == PlacesChannel.Id) continue;
            (Border card, StackPanel body) = AddonCard(row.Name);
            if (row.Description.Length > 0) body.Children.Add(Note(row.Description));
            bool loaded = SlotAddonsLoaded.Contains(row.Id);
            body.Children.Add(Note(SlotStateText(row, loaded)));
            AddonSlot channel = SlotChannelFor(row);
            if (!_addonsElsewhere && channel.Offer.Kind == OfferKind.Unknown) probe.Add(channel);
            if (channel.Busy && channel.Phase is not null)
            {
                body.Children.Add(ProgressFor(channel, 320));
                _slotSettingsPanel.Children.Add(card);
                continue;
            }
            // A later window shows what it adds (issue #402) but leaves removing
            // it to the first window, which owns the store.
            StackPanel buttons = _addonsElsewhere
                ? new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) }
                : SlotRemoveButtons(row);
            if (!_addonsElsewhere && _slotConfirming != row.Id)
            {
                SlotAddonRow target = row;
                if (row.State != "ok")
                {
                    buttons.Children.Insert(0, SettingsButton("Reinstall", () => StartSlotAddonInstall(target)));
                }
                if (SlotUpdateVersion(channel, row) is { } v)
                {
                    buttons.Children.Insert(0, SettingsButton($"Update to {v}, {MediaViewer.Shared.ByteSize.Text(channel.Offer.ArchiveSize)}",
                                                              () => StartSlotAddonInstall(target)));
                }
            }
            // #410: an add-on with a window of its own opens it from here, as
            // Import does ("Open Import").
            if (_slotConfirming != row.Id && loaded && SlotWindowScreens.TryGetValue(row.Id, out string? screen))
            {
                string id = row.Id;
                Button open = SettingsButton($"Open {row.Name}", () => SlotNative.OpenScreen(
                    id, screen, "", SlotTheme(), 0, 0, (float)SlotScale()));
                open.IsTabStop = true;
                buttons.Children.Insert(0, open);
            }
            if (buttons.Children.Count > 0) body.Children.Add(buttons);
            if (row.Id == DriftwireId && _driftwireMessage.Length > 0) body.Children.Add(Note(_driftwireMessage));
            if (SlotUpdateMessages.TryGetValue(row.Id, out string? updated) && updated.Length > 0) body.Children.Add(Note(updated));
            if (_addonsElsewhere) body.Children.Add(Note(AddonsElsewhereText));
            _slotSettingsPanel.Children.Add(card);
        }
        foreach (AddonSlot slot in probe) ProbeOffer(slot);
    }

    private static void AddPlacesSettingsRow(StackPanel into)
    {
        SlotAddonRow? installed = null;
        foreach (SlotAddonRow row in SlotAddonRows)
        {
            if (row.Id == PlacesChannel.Id) { installed = row; break; }
        }
        string described = installed?.Description ?? "";
        if (described.Length == 0) described = PlacesChannel.Offer.Description;
        into.Children.Add(Note(described.Length > 0 ? described : PlacesFallback));
        if (_addonsElsewhere)
        {
            into.Children.Add(Note(AddonsElsewhereText));
            return;
        }
        if (!_slotAddonsRead)
        {
            into.Children.Add(Note("Checking installed add-ons…"));
            return;
        }
        if (installed is null)
        {
            into.Children.Add(Note(PlacesOptional));
            AddPlacesOffer(into);
            return;
        }
        bool loaded = SlotAddonsLoaded.Contains(installed.Id);
        into.Children.Add(Note(SlotStateText(installed, loaded)));
        if (PlacesChannel.Busy && PlacesChannel.Phase is not null)
        {
            into.Children.Add(ProgressFor(PlacesChannel, 320));
        }
        else if (_slotConfirming == installed.Id)
        {
            into.Children.Add(SlotRemoveButtons(installed));
        }
        else
        {
            var buttons = SlotRemoveButtons(installed);
            if (PlacesChannel.Offer.Kind == OfferKind.Available && PlacesChannel.Offer.Version.Length > 0 &&
                IsNewer(PlacesChannel.Offer.Version, installed.Version))
            {
                buttons.Children.Insert(0, SettingsButton(
                    $"Update to {PlacesChannel.Offer.Version}, {MediaViewer.Shared.ByteSize.Text(PlacesChannel.Offer.ArchiveSize)}",
                    StartPlacesInstall));
            }
            if (installed.State != "ok") buttons.Children.Insert(0, SettingsButton("Reinstall", StartPlacesInstall));
            into.Children.Add(buttons);
        }
        if (_placesMessage.Length > 0) into.Children.Add(Note(_placesMessage));
        if (installed.State == "ok")
        {
            EnsurePlacesPage(installed.Version);
            if (_placesPage is not null) into.Children.Add(BuildSettingsPage(_placesPage));
        }
    }

    private static void AddPlacesOffer(StackPanel into)
    {
        if (PlacesChannel.Busy && PlacesChannel.Phase is not null)
        {
            into.Children.Add(ProgressFor(PlacesChannel, 320));
            return;
        }
        switch (PlacesChannel.Offer.Kind)
        {
            case OfferKind.Available:
                Button install = SettingsButton(
                    $"Install Places, {MediaViewer.Shared.ByteSize.Text(PlacesChannel.Offer.ArchiveSize)}", StartPlacesInstall);
                install.HorizontalAlignment = HorizontalAlignment.Left;
                install.IsEnabled = !PlacesChannel.Busy;
                into.Children.Add(install);
                break;
            case OfferKind.NotPublished:
                into.Children.Add(Note(
                    "Places is not published for download yet. It will be offered here once a release carries it."));
                break;
            case OfferKind.NeedsNewerApp:
                into.Children.Add(Note(
                    "The published Places add-on needs a newer MediaViewer. Update MediaViewer, then install it."));
                break;
            case OfferKind.Unreachable:
                into.Children.Add(Note("Could not reach the download server."));
                Button retry = SettingsButton("Try again", () => ProbeOffer(PlacesChannel));
                retry.HorizontalAlignment = HorizontalAlignment.Left;
                into.Children.Add(retry);
                break;
            default:
                into.Children.Add(Note("Checking for Places…"));
                break;
        }
        if (_placesMessage.Length > 0) into.Children.Add(Note(_placesMessage));
    }

    private static void EnsurePlacesPage(string version)
    {
        if (_placesPageVersion == version || _placesPageLoading) return;
        _placesPageLoading = true;
        int gen = ++_placesPageGen;
        _ = Task.Run(() =>
        {
            AddonSettingsPage? page = ReadSettingsPage(PlacesChannel.Id);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                if (gen != _placesPageGen) return;
                _placesPageLoading = false;
                _placesPage = page;
                _placesPageVersion = version;
                RefreshSlotSettings();
            });
        });
    }

    // Update (or reinstall) an installed slot add-on other than Places, as
    // Places does. #510: a loaded copy is reloaded on the new version at once,
    // with no restart, or once the transfer it reports has ended
    // (ReloadUpdatedSlotAddon; docs/design/18 "Updates").
    private static void StartSlotAddonInstall(SlotAddonRow row)
    {
        AddonSlot channel = SlotChannelFor(row);
        if (channel.Busy || !_slotAddonsRead || _addonsElsewhere || _folderSession is not { } session) return;
        bool wasLoaded = SlotAddonsLoaded.Contains(row.Id);
        string? update = SlotUpdateVersion(channel, row);
        string id = row.Id, name = row.Name;
        channel.Busy = true;
        channel.Phase = new AddonPhase(AddonPhaseKind.Downloading);
        SlotUpdateMessages.Remove(id);
        IProgress<AddonPhase> progress = PhaseReporter(channel);
        RefreshSlotSettings();
        _ = Task.Run(async () =>
        {
            string message;
            bool reload = false;
            try
            {
                await DownloadAndInstall(channel, null, progress).ConfigureAwait(false);
                if (wasLoaded && update is not null)
                {
                    message = "";  // ReloadUpdatedSlotAddon's, on the UI thread
                    reload = true;
                }
                else
                {
                    message = update is null ? $"{name} reinstalled." : $"{name} updated to {update}.";
                    StartSlotAddons(session);
                }
            }
            catch (AddonNotPublishedException)
            {
                message = "";
                DispatcherQueueControllerTryEnqueue(() => channel.Offer = new AddonOffer(OfferKind.NotPublished, 0));
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or MediaViewerException
                                           or InvalidDataException or TaskCanceledException or JsonException)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = ex is MediaViewerException or InvalidDataException
                    ? "The download did not verify, so nothing was installed."
                    : $"{name} could not be downloaded. Check the connection and try again.";
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = $"{name} could not be installed.";
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                channel.Busy = false;
                channel.Phase = null;
                channel.Progress = null;
                SlotUpdateMessages[id] = reload && update is not null ? ReloadUpdatedSlotAddon(id, name, update) : message;
                if (reload && update is not null)
                {
                    for (int i = 0; i < SlotAddonRows.Count; ++i)
                    {
                        if (SlotAddonRows[i].Id == id) SlotAddonRows[i] = SlotAddonRows[i] with { Version = update };
                    }
                }
                RefreshSlotSettings();
            });
        });
    }

    private static void StartPlacesInstall()
    {
        if (PlacesChannel.Busy || !_slotAddonsRead || _addonsElsewhere || _folderSession is null) return;
        bool wasLoaded = SlotAddonsLoaded.Contains(PlacesChannel.Id);
        string? update = null;
        foreach (SlotAddonRow row in SlotAddonRows)
        {
            if (row.Id == PlacesChannel.Id && PlacesChannel.Offer.Kind == OfferKind.Available &&
                IsNewer(PlacesChannel.Offer.Version, row.Version))
            {
                update = PlacesChannel.Offer.Version;
            }
        }
        PlacesChannel.Busy = true;
        PlacesChannel.Phase = new AddonPhase(AddonPhaseKind.Downloading);
        _placesMessage = "";
        IProgress<AddonPhase> progress = PhaseReporter(PlacesChannel);
        RefreshSlotSettings();
        MediaViewerSession session = _folderSession;
        _ = Task.Run(async () =>
        {
            string message;
            bool reload = false;
            try
            {
                await DownloadAndInstall(PlacesChannel, null, progress).ConfigureAwait(false);
                if (wasLoaded && update is not null)
                {
                    message = "";  // ReloadUpdatedSlotAddon's, on the UI thread
                    reload = true;
                }
                else
                {
                    message = update is null ? "Places installed." : $"Places updated to {update}.";
                    StartSlotAddons(session);
                }
            }
            catch (AddonNotPublishedException)
            {
                message = "";
                DispatcherQueueControllerTryEnqueue(() => PlacesChannel.Offer = new AddonOffer(OfferKind.NotPublished, 0));
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or MediaViewerException
                                           or InvalidDataException or TaskCanceledException or JsonException)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = ex is MediaViewerException or InvalidDataException
                    ? "The download did not verify, so nothing was installed."
                    : "Places could not be downloaded. Check the connection and try again.";
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = "Places could not be installed.";
            }
            string? installedVersion = update;
            DispatcherQueueControllerTryEnqueue(() =>
            {
                PlacesChannel.Busy = false;
                PlacesChannel.Phase = null;
                _placesMessage = reload && installedVersion is not null
                    ? ReloadUpdatedSlotAddon(PlacesChannel.Id, "Places", installedVersion)
                    : message;
                if (reload && installedVersion is not null)
                {
                    for (int i = 0; i < SlotAddonRows.Count; ++i)
                    {
                        if (SlotAddonRows[i].Id != PlacesChannel.Id) continue;
                        SlotAddonRows[i] = SlotAddonRows[i] with { Version = installedVersion };
                    }
                }
                _placesPage = null;
                _placesPageVersion = "";
                _placesPageLoading = false;
                RefreshSlotSettings();
            });
        });
    }

    private static StackPanel SlotRemoveButtons(SlotAddonRow row)
    {
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        if (_slotConfirming == row.Id)
        {
            // #510 / #507: say exactly what each choice deletes (the Mac's words).
            var confirm = new StackPanel { Spacing = Space(6) };
            TextBlock q = Note($"Remove {row.Name}? \"Remove everything\" also deletes its data folder and its settings. \"Remove, keep its data\" leaves both for a reinstall.");
            q.Foreground = Brush(Title);
            confirm.Children.Add(q);
            confirm.Children.Add(buttons);
            buttons.Children.Add(EditButton("Remove, keep its data", () => { _slotConfirming = null; RemoveSlotAddon(row.Id, true); }));
            buttons.Children.Add(EditButton("Remove everything", () => { _slotConfirming = null; RemoveSlotAddon(row.Id, false); }));
            buttons.Children.Add(EditButton("Cancel", () => { _slotConfirming = null; RefreshSlotSettings(); }));
            FocusableButtons(buttons);
            return confirm;
        }
        else
        {
            Button remove = EditButton("Remove…", () => { _slotConfirming = row.Id; RefreshSlotSettings(); });
            AutomationProperties.SetName(remove, $"Remove {row.Name}");
            buttons.Children.Add(remove);
        }
        FocusableButtons(buttons);
        return buttons;
    }

    private static void FocusableButtons(StackPanel buttons)
    {
        foreach (UIElement child in buttons.Children)
        {
            if (child is Button b)
            {
                b.AllowFocusOnInteraction = true;
                b.IsTabStop = true;
            }
        }
    }
}
