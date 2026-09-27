// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using Microsoft.UI.Dispatching;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// The AI pack's chrome entry point (plan/17 "UI and commands"). Owns the
/// search panel, the management panel Settings embeds, the people window, the
/// command-bar pill's text, and the search the viewer's result list came from
/// (its clip matches are the scrub bar's dots and what N / Shift+N walk),
/// and the gallery search bar's contents search and index control.
/// </summary>
/// <remarks>
/// Threads: every member runs on the chrome's UI thread. Calls the header
/// marks [worker-thread] (roots, people, thumbnails, clip matches) go through
/// <see cref="Task.Run(Action)"/>; status is [no-block] and is read on
/// AI_STATUS events at most 4 Hz, or polled at 4 Hz only while a panel is
/// visible. Rule 6: no path, query or name reaches a log from here.
/// </remarks>
public sealed class AiChrome : IAddonChrome, ISearchChrome, IGallerySearchChrome
{
    private IAddonHost2? _host;
    private AiApi? _api;
    private Look? _look;
    private SearchWindow? _window;
    private ManagePanel? _panel;
    private PeopleWindow? _people;
    private DispatcherQueue? _queue;
    private DispatcherQueueTimer? _statusThrottle;
    private long _lastStatusTick;
    private MvAiStatus _status;
    private bool _statusValid;

    // The folder the viewer has open and whether the index covers it.
    private string? _folder;
    private uint _coverage;

    // The search behind the viewer's result list, and its clips' matches.
    private ulong _activeSearch;
    private HashSet<string> _activeClips = new(StringComparer.OrdinalIgnoreCase);
    private string? _matchPath;
    private long[] _matches = Array.Empty<long>();
    private int _matchRequest;

    private readonly Dictionary<ulong, Action<MvStatus, long>> _searchWaiters = new();

    // The gallery search bar: its index control, the folder it shows, and
    // which contents query is the latest (an older answer opens nothing).
    private GalleryControl? _galleryControl;
    private string? _galleryFolder;
    private int _galleryQuery;

    internal AiApi Api => _api ?? throw new InvalidOperationException("not attached");
    internal IAddonHost2 Host => _host ?? throw new InvalidOperationException("not attached");
    internal Look Look => _look ?? throw new InvalidOperationException("not attached");
    internal MvAiStatus Status => _status;
    internal bool StatusValid => _statusValid;
    internal string? Folder => _folder;
    internal uint Coverage => _coverage;

    /// <summary>Raised on the UI thread after a status read.</summary>
    internal event Action? StatusChanged;

    // ---- IAddonChrome ----------------------------------------------------------------

    public void Attach(IAddonHost host, IntPtr interfaceTable)
    {
        // The AI chrome needs Milestone H's host; an older app never loads it
        // (host_api 2), so this is a belt to the manifest's braces.
        _host = host as IAddonHost2 ?? throw new InvalidOperationException("this chrome needs IAddonHost2");
        _api = new AiApi(interfaceTable);
        _look = new Look(_host);
        _host.ThemeChanged += OnThemeChanged;
        _queue = DispatcherQueue.GetForCurrentThread();
        _statusThrottle = _queue?.CreateTimer();
        if (_statusThrottle is not null)
        {
            _statusThrottle.Interval = TimeSpan.FromMilliseconds(250);
            _statusThrottle.IsRepeating = false;
            _statusThrottle.Tick += (_, _) => ReadStatus();
        }
        ReadStatus();
        string? folder = _host.CurrentFolder;
        if (folder is not null) OnFolderChanged(folder);
    }

    /// <summary>The search panel (Settings' "Open search", the pill).</summary>
    public void Open(string? sourceRoot)
    {
        _ = sourceRoot;
        ShowSearch(null);
    }

    public void ImportNow(string pathsJson) => _ = pathsJson;  // Import's; nothing here

    public void OnEvent(AddonCompletion e)
    {
        switch (e.Event)
        {
            case MvAddonEvent.AiStatus:
                RequestStatus();
                break;
            case MvAddonEvent.AiSearchDone:
                if (_searchWaiters.Remove(e.Id, out Action<MvStatus, long>? waiter)) waiter(e.Status, e.Payload);
                else _window?.OnSearchDone(e.Id, e.Status, e.Payload);
                break;
            case MvAddonEvent.AiRoots:
                RefreshCoverage();
                _panel?.RefreshRoots();
                _galleryControl?.Refresh();
                _window?.OnRootsChanged();
                RequestStatus();
                break;
            case MvAddonEvent.AiCompute:
                _panel?.RefreshSettings();
                RequestStatus();
                break;
            case MvAddonEvent.AiPeople:
                _panel?.RefreshPeople();
                _people?.Refresh();
                break;
        }
    }

    public void Shutdown()
    {
        _statusThrottle?.Stop();
        _window?.Dispose();
        _window = null;
        _people?.Close();
        _people = null;
        _panel?.Detach();
        _panel = null;
        _galleryControl?.Detach();
        _galleryControl = null;
        ++_galleryQuery;
        if (_host is not null)
        {
            _host.ThemeChanged -= OnThemeChanged;
            _host.SetIndexingStatus(null, false);
            _host.SetScrubMarkers(Array.Empty<long>(), -1);
        }
        if (_api is not null && _activeSearch != 0)
        {
            try { _api.SearchRelease(_activeSearch); } catch (MediaViewerException) { }
        }
        _activeSearch = 0;
        _searchWaiters.Clear();
        _api = null;
        _host = null;
    }

    // ---- ISearchChrome -----------------------------------------------------------------

    public bool RunCommand(int command)
    {
        if (_api is null || _host is null) return false;
        switch (command)
        {
            case SearchCommand.Open:
                ShowSearch(null);
                return true;
            case SearchCommand.Similar:
            {
                AddonCurrentItem item = _host.CurrentItem();
                if (item.Path.Length == 0) return false;
                ShowSearch(new SimilarTo(item.Path, item.IsVideo ? item.PositionMs : -1));
                return true;
            }
            case SearchCommand.NextMatch:
                return WalkMatch(+1);
            case SearchCommand.PrevMatch:
                return WalkMatch(-1);
            default:
                return false;
        }
    }

    public void OnFolderChanged(string? dir)
    {
        if (_api is null || string.IsNullOrEmpty(dir)) return;
        _folder = dir;
        try
        {
            // Both [no-block]: a covered folder queues its delta; the offer
            // lives in the search panel, never as a banner over the viewer.
            _api.NoteFolderOpened(dir);
            _coverage = _api.FolderCoverage(dir);
        }
        catch (MediaViewerException)
        {
            _coverage = 0;
        }
        _window?.OnFolderChanged();
    }

    public void OnItemChanged(string? path)
    {
        _matchPath = null;
        _matches = Array.Empty<long>();
        int request = ++_matchRequest;
        if (_api is null || _activeSearch == 0 || path is null || !_activeClips.Contains(path)) return;
        AiApi api = _api;
        ulong search = _activeSearch;
        _ = Task.Run(() =>
        {
            long[] ms;
            try { ms = api.ClipMatches(search, path); }
            catch (MediaViewerException) { return; }
            _queue?.TryEnqueue(() =>
            {
                if (request != _matchRequest || _host is null) return;
                _matchPath = path;
                _matches = ms;
                AddonCurrentItem now = _host.CurrentItem();
                _host.SetScrubMarkers(ms, Nearest(ms, now.PositionMs));
            });
        });
    }

    public object? BuildSettingsPanel()
    {
        if (_api is null || _host is null) return null;
        _panel?.Detach();
        _panel = new ManagePanel(this);
        return _panel.Root;
    }

    public void OnPiecesChanged()
    {
        if (_api is null) return;
        // ai-faces is picked up at once (FACES_READY, AI_PEOPLE); ai-cuda sets
        // settings_json's restart_needed, which the panel shows.
        try { _api.Reload(); }
        catch (MediaViewerException) { }
        _panel?.RefreshSettings();
        ReadStatus();
    }

    // ---- the search panel ------------------------------------------------------------------

    internal readonly record struct SimilarTo(string Path, long PtsMs)
    {
        public string Name => System.IO.Path.GetFileName(Path);
    }

    private void ShowSearch(SimilarTo? similar)
    {
        if (_api is null || _host is null) return;
        if (_window is null)
        {
            _window = new SearchWindow(this);
            _window.Hidden += () => ReadStatus();
        }
        _window.Present(similar);
        ReadStatus();
    }

    internal void RefreshCoverage()
    {
        if (_api is null || _folder is null) return;
        try { _coverage = _api.FolderCoverage(_folder); }
        catch (MediaViewerException) { _coverage = 0; }
    }

    /// <summary>The panel opened a result list: its search becomes the viewer's.</summary>
    internal void SetActiveSearch(ulong search, IEnumerable<string> clipPaths)
    {
        if (_activeSearch != 0 && _activeSearch != search && _api is not null)
        {
            try { _api.SearchRelease(_activeSearch); } catch (MediaViewerException) { }
        }
        _activeSearch = search;
        _activeClips = new HashSet<string>(clipPaths, StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>Releases a search the panel no longer shows, unless the viewer's list uses it.</summary>
    internal void ReleaseSearch(ulong search)
    {
        if (search == 0 || search == _activeSearch || _api is null) return;
        _searchWaiters.Remove(search);
        try { _api.SearchRelease(search); } catch (MediaViewerException) { }
    }

    /// <summary>A search whose answer goes to <paramref name="done"/> instead of the panel.</summary>
    internal void AwaitSearch(ulong search, Action<MvStatus, long> done) => _searchWaiters[search] = done;

    /// <summary>When <paramref name="search"/> answers, its results become the viewer's
    /// listing (the people window's "Show their photos").</summary>
    internal void OpenSearchAsList(ulong search, string title) => OpenSearchAsList(search, title, null, null);

    /// <summary>
    /// As above; <paramref name="wanted"/> false (a newer gallery query) opens
    /// nothing, and <paramref name="done"/> hears what happened (the gallery bar).
    /// </summary>
    private void OpenSearchAsList(ulong search, string title, Func<bool>? wanted,
                                  Action<GallerySearchOutcome>? done)
    {
        AwaitSearch(search, (status, count) =>
        {
            if (status != MvStatus.Ok || count <= 0 || _api is null || (wanted is not null && !wanted()))
            {
                ReleaseSearch(search);
                if (wanted is null || wanted())
                {
                    done?.Invoke(status == MvStatus.Ok && _api is not null
                        ? GallerySearchOutcome.NothingFound : GallerySearchOutcome.Failed);
                }
                return;
            }
            AiApi api = _api;
            _ = Task.Run(() =>
            {
                var paths = new List<string>();
                var moments = new List<long>();
                try
                {
                    for (uint i = 0; i < Math.Min(count, 1000); ++i)
                    {
                        MvAiResult r = api.ResultAt(search, i);
                        paths.Add(api.ResultPath(search, i));
                        moments.Add(r.Kind == MvAiKinds.Videos ? r.PtsMs : -1);
                    }
                }
                catch (MediaViewerException) { }
                _queue?.TryEnqueue(() =>
                {
                    if (wanted is not null && !wanted())
                    {
                        ReleaseSearch(search);
                        return;
                    }
                    if (_host is null || paths.Count == 0)
                    {
                        done?.Invoke(GallerySearchOutcome.Failed);
                        return;
                    }
                    SetActiveSearch(search, paths.Where((_, i) => moments[i] >= 0));
                    _host.OpenList(title, paths, moments, 0, gallery: true);
                    done?.Invoke(GallerySearchOutcome.Opened);
                });
            });
        });
    }

    // ---- IGallerySearchChrome (the gallery search bar) ------------------------------------

    public object? BuildGalleryIndexControl()
    {
        if (_api is null || _host is null) return null;
        _galleryControl?.Detach();
        _galleryControl = new GalleryControl(this);
        _galleryControl.SetFolder(_galleryFolder);
        return _galleryControl.Root;
    }

    public void SetGalleryFolder(string? folder)
    {
        _galleryFolder = folder;
        _galleryControl?.SetFolder(folder);
    }

    public void GalleryQuery(string text, string folder, Action<GallerySearchOutcome> done)
    {
        int query = ++_galleryQuery;
        string q = text.Trim();
        if (_api is null || _host is null || q.Length == 0 || folder.Length == 0)
        {
            done(GallerySearchOutcome.Failed);
            return;
        }
        uint coverage;
        try { coverage = _api.FolderCoverage(folder); }  // [no-block]
        catch (MediaViewerException)
        {
            done(GallerySearchOutcome.Failed);
            return;
        }
        if (coverage == 0)
        {
            done(GallerySearchOutcome.NotIndexed);
            return;
        }
        // The folder, and its subfolders when its root is recursive: whether
        // it is lives in roots_json, which is [worker-thread].
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            GalleryRoot? root = null;
            try { root = GalleryRoot.Covering(api.RootsJson(), folder); }
            catch (MediaViewerException) { }
            _queue?.TryEnqueue(() =>
            {
                if (query != _galleryQuery || _api is null) return;
                MvAiScope scope = root is { Recursive: true } ? MvAiScope.Tree : MvAiScope.Folder;
                ulong search;
                try
                {
                    // All kinds, no find bits: what the pack returns for the
                    // words, ranked as the panel ranks them.
                    search = _api.SearchText(q, folder, scope, MvAiKinds.All);
                }
                catch (MediaViewerException)
                {
                    done(GallerySearchOutcome.Failed);
                    return;
                }
                // The results' title is the query; the host shows "Search: <query>".
                OpenSearchAsList(search, q, () => query == _galleryQuery, done);
            });
        });
    }

    public void IndexGalleryFolder(string folder, bool recursive)
    {
        if (_api is null || folder.Length == 0) return;
        try { _api.IndexFolder(folder, recursive); }
        catch (MediaViewerException) { return; }
        RefreshCoverage();
        ReadStatus();
        _galleryControl?.Refresh();
    }

    internal void OpenPeople()
    {
        if (_api is null || _host is null) return;
        if (_people is null)
        {
            _people = new PeopleWindow(this);
            _people.Closed += (_, _) => _people = null;
        }
        _people.Present();
    }

    // ---- N / Shift+N ------------------------------------------------------------------------

    private bool WalkMatch(int direction)
    {
        if (_host is null || _matches.Length == 0) return false;
        AddonCurrentItem item = _host.CurrentItem();
        if (!item.IsVideo || !string.Equals(item.Path, _matchPath, StringComparison.OrdinalIgnoreCase)) return false;
        // Half a frame's slack either side, so the moment just landed on is
        // not "next" again.
        const long Slack = 40;
        int target = -1;
        if (direction > 0)
        {
            for (int i = 0; i < _matches.Length; ++i)
            {
                if (_matches[i] > item.PositionMs + Slack) { target = i; break; }
            }
        }
        else
        {
            for (int i = _matches.Length - 1; i >= 0; --i)
            {
                if (_matches[i] < item.PositionMs - Slack) { target = i; break; }
            }
        }
        if (target < 0) return false;
        // Exact: the keyframe before, decoded forward; a paused clip stays paused.
        _host.SeekVideo(_matches[target], exact: true);
        _host.SetScrubMarkers(_matches, target);
        return true;
    }

    private static int Nearest(long[] ms, long position)
    {
        if (ms.Length == 0) return -1;
        int best = 0;
        long bestDelta = long.MaxValue;
        for (int i = 0; i < ms.Length; ++i)
        {
            long d = Math.Abs(ms[i] - Math.Max(0, position));
            if (d < bestDelta) { bestDelta = d; best = i; }
        }
        return best;
    }

    // ---- status -----------------------------------------------------------------------------

    // AI_STATUS can arrive per asset; the line changes at most 4 times a second.
    private void RequestStatus()
    {
        long now = Environment.TickCount64;
        if (now - _lastStatusTick >= 250 || _statusThrottle is null)
        {
            ReadStatus();
            return;
        }
        if (!_statusThrottle.IsRunning) _statusThrottle.Start();
    }

    /// <summary>[no-block] status read; the pill, the footer and Settings follow.</summary>
    internal void ReadStatus()
    {
        if (_api is null || _host is null) return;
        _lastStatusTick = Environment.TickCount64;
        try
        {
            _status = _api.Status();
            _statusValid = true;
        }
        catch (MediaViewerException)
        {
            _statusValid = false;
            _host.SetIndexingStatus(null, false);
            return;
        }
        _host.SetIndexingStatus(Look.PillVisible(_status) ? Look.StatusLine(_status) : null, Look.Busy(_status));
        StatusChanged?.Invoke();
    }

    private void OnThemeChanged()
    {
        _look?.Refresh();
        _window?.OnThemeChanged();
        _people?.OnThemeChanged();
    }
}
