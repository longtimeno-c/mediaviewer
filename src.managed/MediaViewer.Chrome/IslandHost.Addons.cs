// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// Add-ons (plan/18 "Add-ons: how Import is installed"): Settings → Add-ons,
/// the download, the one-time card hint, and loading an installed add-on's
/// chrome into its own <see cref="AssemblyLoadContext"/>. Milestone H adds the
/// AI pack (plan/17 "The AI pack"): a second add-on with a chrome, and two
/// pieces of its family with none (IslandHost.LocalSearch.cs is its Settings).
/// </summary>
/// <remarks>
/// The download is a plain GET of fixed release-asset URLs on the update
/// channel: no query, no cookies, no identifier, nothing about the user's
/// files (rule 6). The core verifies the signed manifest before the archive
/// is even requested, the archive's SHA-256 before it is opened, and every
/// extracted file (and that there is nothing extra) before it is installed;
/// it verifies again at every load. With nothing installed, nothing here
/// writes a byte, and no add-on's commands exist.
/// </remarks>
public static partial class IslandHost
{
    private const string UpdateReleaseBase = "https://github.com/longtimeno-c/mediaviewer/releases/latest/download/";

    private sealed record AddonState(bool Installed, string Version, string State, long Size, bool Loaded);

    // Whether the release channel has a piece this build can install. Only
    // a manifest that verifies counts: the button never offers a download that
    // does not exist or that the core would refuse.
    private enum OfferKind { Unknown, Checking, Available, NotPublished, NeedsNewerApp, Unreachable }
    private sealed record AddonOffer(OfferKind Kind, long ArchiveSize, long InstalledSize = 0, string Version = "");

    // Dotted numeric versions ("0.1.10" > "0.1.9"), as the store compares them.
    private static bool IsNewer(string published, string installed)
    {
        int[] a = Array.ConvertAll(published.Split('.'), p => int.TryParse(p, out int n) ? n : 0);
        int[] b = Array.ConvertAll(installed.Split('.'), p => int.TryParse(p, out int n) ? n : 0);
        for (int i = 0; i < Math.Max(a.Length, b.Length); ++i)
        {
            int x = i < a.Length ? a[i] : 0, y = i < b.Length ? b[i] : 0;
            if (x != y) return x > y;
        }
        return false;
    }

    // A newer published version of an installed add-on or piece, or null.
    private static string? UpdateVersion(AddonSlot slot) =>
        slot.State.Installed && slot.Offer.Kind == OfferKind.Available && slot.Offer.Version.Length > 0 &&
        IsNewer(slot.Offer.Version, slot.State.Version) ? slot.Offer.Version : null;

    /// <summary>
    /// One add-on or family piece. Pieces (<see cref="Parent"/> set) have no
    /// native entry and no chrome: the parent's add-on finds them itself.
    /// </summary>
    private sealed class AddonSlot(string id, string name, string? iface, string? parent)
    {
        public string Id { get; } = id;
        public string Name { get; } = name;
        public string? Interface { get; } = iface;
        public string? Parent { get; } = parent;
        public string Channel => UpdateReleaseBase + $"mediaviewer-addon-{Id}-win-x64";
        public AddonState State { get; set; } = new(false, "", "", 0, false);
        public AddonOffer Offer { get; set; } = new(OfferKind.Unknown, 0);
        public bool ProbeRunning { get; set; }
        public bool Busy { get; set; }
        // Being removed (the AI family's rows say "Removing…"); Busy alone is
        // also a load in flight.
        public bool Removing { get; set; }
        // The install in progress (null: none) and its bar, kept and updated in
        // place: a rebuilt row would drop keyboard focus at every 1 %.
        public AddonPhase? Phase { get; set; }
        public AddonProgressView? Progress { get; set; }
        public IAddonChrome? Chrome { get; set; }
        public AddonLoadContext? Alc { get; set; }
        public bool Usable => State.Installed && State.State == "ok";
    }

    private static readonly AddonSlot ImportSlot = new("import", "Import", "mv.import.1", null);
    private static readonly AddonSlot AiSlot = new("ai", "Local search", AiApi.InterfaceId, null);
    private static readonly AddonSlot FacesSlot = new("ai-faces", "People", null, "ai");
    private static readonly AddonSlot CudaSlot = new("ai-cuda", "NVIDIA acceleration", null, "ai");
    // 2026-09-27: sounds (CLAP) and speech (Whisper) in videos.
    private static readonly AddonSlot AudioSlot = new("ai-audio", "Audio", null, "ai");
    private static readonly AddonSlot[] AddonSlots = { ImportSlot, AiSlot, FacesSlot, AudioSlot, CudaSlot };

    /// <summary>What an install is doing, for the bar under its button (the
    /// Mac's AddonChannel.Phase): a determinate download, then the checking
    /// and installing steps, which have no fraction to show.</summary>
    private enum AddonPhaseKind { Downloading, Checking, Installing }

    private readonly record struct AddonPhase(AddonPhaseKind Kind, long Done = 0, long Total = 0)
    {
        // Null until the first bytes arrive, or while the size is unknown:
        // the bar is indeterminate (animated) rather than a frozen 0 %.
        public double? Fraction => Kind == AddonPhaseKind.Downloading && Total > 0 && Done > 0
            ? Math.Min(1.0, (double)Done / Total) : null;

        public string Text => Kind switch
        {
            AddonPhaseKind.Downloading when Done <= 0 => "Connecting…",
            AddonPhaseKind.Downloading => Total > 0 ? $"{MbText(Done)} of {MbText(Total)}" : MbText(Done),
            AddonPhaseKind.Checking => "Checking the download…",
            _ => "Installing…",
        };
    }

    // "412 MB", "1.08 GB": decimal units, as Explorer's download sizes.
    private static string MbText(long bytes) => bytes >= 1_000_000_000
        ? $"{bytes / 1e9:0.00} GB"
        : $"{Math.Max(0, (long)Math.Round(bytes / 1e6))} MB";

    /// <summary>An install's bar: "412 MB of 1.08 GB" and the percentage
    /// under it while downloading, an indeterminate bar after.</summary>
    private sealed class AddonProgressView
    {
        private readonly ProgressBar _bar;
        private readonly TextBlock _text;
        private readonly TextBlock _percent;

        public AddonProgressView(double width)
        {
            _bar = new ProgressBar { Minimum = 0, Maximum = 1, Height = 4 };
            _text = Label("");
            _text.FontSize = 12;
            _percent = Label("");
            _percent.FontSize = 12;
            _percent.HorizontalAlignment = HorizontalAlignment.Right;
            var lines = new Grid();
            lines.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            lines.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            lines.Children.Add(_text);
            Grid.SetColumn(_percent, 1);
            lines.Children.Add(_percent);
            Root = new StackPanel { Spacing = 4, Width = width, HorizontalAlignment = HorizontalAlignment.Left };
            Root.Children.Add(_bar);
            Root.Children.Add(lines);
        }

        public StackPanel Root { get; }

        public void Show(AddonPhase p)
        {
            double? f = p.Fraction;
            // Whatever the phase the bar shows activity: indeterminate (the
            // animated dots) while no fraction is known, during checking and
            // installing too (owner report, 2026-09-28). With animations off
            // in Windows it is a still, dimmed full bar instead.
            bool still = f is null && !HostAnimationsEnabled();
            _bar.IsIndeterminate = f is null && !still;
            _bar.Opacity = still ? 0.45 : 1;
            if (f is double v) _bar.Value = v;
            else if (still) _bar.Value = 1;
            _text.Text = p.Text;
            _percent.Text = f is double pct ? $"{Math.Floor(pct * 100):0} %" : "";
            AutomationProperties.SetName(Root, f is double a ? $"{p.Text}, {Math.Floor(a * 100):0} percent" : p.Text);
        }
    }

    // The slot's bar, moved into the row being built.
    private static FrameworkElement ProgressFor(AddonSlot slot, double width)
    {
        slot.Progress ??= new AddonProgressView(width);
        if (slot.Progress.Root.Parent is Panel old) old.Children.Remove(slot.Progress.Root);
        slot.Progress.Show(slot.Phase ?? new AddonPhase(AddonPhaseKind.Downloading));
        return slot.Progress.Root;
    }

    // UI thread: Progress<T> made here reports on the dispatcher.
    private static IProgress<AddonPhase> PhaseReporter(AddonSlot slot) => new Progress<AddonPhase>(p =>
    {
        if (!slot.Busy) return;  // a late report after the install finished
        slot.Phase = p;
        slot.Progress?.Show(p);
    });

    private static bool _addonsStarted;
    // The first installed-state read has landed (StartAddons). It verifies
    // every installed file, seconds for the AI pack; until then Settings says
    // it is checking and offers no Install: the channel probe answering first
    // made it offer a download of what was already installed (owner report,
    // 2026-09-27).
    private static bool _addonStatesRead;
    private static StackPanel? _addonRow;
    private static TextBlock? _addonStatus;
    private static Button? _importHint;
    private static Button? _importHintDismiss;
    private static TextBlock? _importProgressLabel;
    private static bool _hintAfterProbe;

    private static readonly HttpClient AddonHttp = CreateAddonClient();

    private static HttpClient CreateAddonClient()
    {
        var client = new HttpClient(new HttpClientHandler { UseCookies = false }) { Timeout = TimeSpan.FromMinutes(30) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MediaViewer");
        return client;
    }

    /// <summary>Once a session is borrowed: load installed add-ons, watch for cards.</summary>
    private static void StartAddons()
    {
        if (_addonsStarted || _folderSession is null) return;
        _addonsStarted = true;
        MediaViewerSession session = _folderSession;
        _ = Task.Run(() =>
        {
            Dictionary<string, AddonState> states = ReadAddonStates();
            try { AddonNative.WatchVolumes(session); }
            catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                ApplyAddonStates(states);
                if (ImportSlot.Usable) LoadAddon(ImportSlot);
                if (AiSlot.Usable) LoadAddon(AiSlot);
                RefreshAddonRow();
                RefreshLocalSearch();
            });
        });
    }

    // Worker: hashes every installed file. One read for every slot.
    private static Dictionary<string, AddonState> ReadAddonStates()
    {
        var states = new Dictionary<string, AddonState>(StringComparer.Ordinal);
        try
        {
            using JsonDocument doc = JsonDocument.Parse(AddonNative.InstalledJson());
            foreach (JsonElement a in doc.RootElement.EnumerateArray())
            {
                string id = a.GetProperty("id").GetString() ?? "";
                if (Array.FindIndex(AddonSlots, s => s.Id == id) < 0) continue;
                states[id] = new AddonState(true, a.GetProperty("version").GetString() ?? "",
                    a.GetProperty("state").GetString() ?? "invalid", a.GetProperty("size").GetInt64(),
                    a.GetProperty("loaded").GetBoolean());
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        return states;
    }

    // UI thread. The Loaded bit is the chrome's own truth, not the file listing's.
    private static void ApplyAddonStates(Dictionary<string, AddonState> states)
    {
        foreach (AddonSlot slot in AddonSlots)
        {
            AddonState next = states.GetValueOrDefault(slot.Id) ?? new AddonState(false, "", "", 0, false);
            slot.State = next with { Loaded = slot.Chrome is not null };
        }
        _addonStatesRead = true;
    }

    // ---- loading -------------------------------------------------------------

    /// <summary>Resolves the add-on's own dependencies from its folder; shares
    /// MediaViewer.Interop (IAddonHost) and WinUI with the chrome's context.</summary>
    /// <remarks>
    /// The shell loads this chrome through hostfxr's
    /// load_assembly_and_get_function_pointer, which puts it and its
    /// dependencies (Interop, WinUI, WinRT) in an isolated component context,
    /// not Default. Returning null would fall back to Default, which holds only
    /// the framework: MediaViewer.Interop is not found there (issue #58), and
    /// a second copy would not share IAddonChrome's type identity anyway.
    /// </remarks>
    private sealed class AddonLoadContext(string id, string mainAssembly)
        : AssemblyLoadContext("addon-" + id, isCollectible: true)
    {
        private static readonly AssemblyLoadContext HostContext =
            GetLoadContext(typeof(IAddonHost).Assembly) ?? Default;

        private readonly AssemblyDependencyResolver _resolver = new(mainAssembly);

        protected override Assembly? Load(AssemblyName name)
        {
            // One copy of the contract and of WinUI in the process.
            if (name.Name is "MediaViewer.Interop" || (name.Name?.StartsWith("Microsoft.", StringComparison.Ordinal) ?? false)
                || (name.Name?.StartsWith("WinRT", StringComparison.Ordinal) ?? false))
            {
                return HostContext.LoadFromAssemblyName(name);
            }
            string? path = _resolver.ResolveAssemblyToPath(name);
            return path is null ? null : LoadFromAssemblyPath(path);
        }
    }

    private static void LoadAddon(AddonSlot slot)
    {
        if (slot.Interface is null || slot.Chrome is not null || _folderSession is null || slot.Busy) return;
        MediaViewerSession session = _folderSession;
        slot.Busy = true;
        // The path bar's search icon shows while the pack starts.
        if (slot == AiSlot) OnSearchLoadChanged();
        _ = Task.Run(() =>
        {
            try
            {
                // Native load re-verifies the signature and every file first.
                (IntPtr table, string chromePath) = AddonNative.Load(session, slot.Id, slot.Interface);
                DispatcherQueueControllerTryEnqueue(() => AttachAddonChrome(slot, table, chromePath));
            }
            catch (MediaViewerException ex)
            {
                string why = ex.Status == MvStatus.UnsupportedFormat
                    ? $"{slot.Name} needs an update."
                    : $"{slot.Name} did not pass verification and was not loaded.";
                DispatcherQueueControllerTryEnqueue(() =>
                {
                    slot.Busy = false;
                    if (slot == AiSlot) OnSearchLoadChanged();
                    SetSlotStatus(slot, why);
                });
            }
        });
    }

    private static void AttachAddonChrome(AddonSlot slot, IntPtr table, string chromePath)
    {
        try
        {
            slot.Alc = new AddonLoadContext(slot.Id, chromePath);
            Assembly asm = slot.Alc.LoadFromAssemblyPath(chromePath);
            Type? entry = asm.GetExportedTypes().FirstOrDefault(t => typeof(IAddonChrome).IsAssignableFrom(t) && !t.IsAbstract);
            if (entry is null || Activator.CreateInstance(entry) is not IAddonChrome chrome)
            {
                throw new InvalidOperationException("no IAddonChrome in the add-on");
            }
            chrome.Attach(new AddonHost(slot), table);
            slot.Chrome = chrome;
            slot.State = slot.State with { Loaded = true };
            // Import is 0 / 1 on the wire (Milestone G), the AI pack 2 / 3.
            Send(Command.AddonState, slot == ImportSlot ? 1 : 3);
            if (slot == AiSlot) OnSearchChromeAttached();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            SetSlotStatus(slot, $"{slot.Name} could not start.");
            UnloadAddonChrome(slot);
            try { AddonNative.Unload(slot.Id); } catch (MediaViewerException) { }
        }
        finally
        {
            slot.Busy = false;
            // Attached (already shown) or failed: the search icon follows.
            if (slot == AiSlot) OnSearchLoadChanged();
            RefreshAddonRow();
            RefreshLocalSearch();
        }
    }

    private static void UnloadAddonChrome(AddonSlot slot)
    {
        bool had = slot.Chrome is not null;
        try { slot.Chrome?.Shutdown(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        slot.Chrome = null;
        slot.Alc?.Unload();
        slot.Alc = null;
        slot.State = slot.State with { Loaded = false };
        if (slot == AiSlot && had) OnSearchChromeDetached();
        Send(Command.AddonState, slot == ImportSlot ? 0 : 2);
    }

    private static void SetSlotStatus(AddonSlot slot, string text)
    {
        if (slot == ImportSlot) SetAddonStatus(text);
        else SetLocalSearchStatus(text);
    }

    /// <summary>The base chrome's services for an add-on (plan/18 IAddonHost; Milestone H IAddonHost2).</summary>
    private sealed class AddonHost(AddonSlot slot) : IAddonHost2
    {
        public void Post(Action action) => DispatcherQueueControllerTryEnqueue(action);

        public void OpenInViewer(string path) => DispatcherQueueControllerTryEnqueue(() =>
        {
            _treePending = path;
            Send(Command.OpenPath);
        });

        public IReadOnlyList<string> MarkedPaths() => _lastMarks;

        public void SetStatus(string? text) => DispatcherQueueControllerTryEnqueue(() =>
        {
            // Import's progress line; the AI pack has its own pill.
            if (slot != ImportSlot || _importProgressLabel is null) return;
            _importProgressLabel.Text = text ?? "";
            _importProgressLabel.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
        });

        public void Notify(string title, string body)
        {
            try
            {
                var toast = new Microsoft.Windows.AppNotifications.Builder.AppNotificationBuilder()
                    .AddText(title).AddText(body).BuildNotification();
                Microsoft.Windows.AppNotifications.AppNotificationManager.Default.Show(toast);
            }
            catch (Exception ex)
            {
                // Unpackaged without a registered AUMID: the summary in the
                // window is the notification.
                System.Diagnostics.Debug.WriteLine(ex.Message);
            }
        }

        // ---- IAddonHost2 (IslandHost.LocalSearch.cs) ----
        public IntPtr MainWindow => _themeWindow;
        public string? CurrentFolder => _openedFolder.Length == 0 ? null : _openedFolder;
        public AddonCurrentItem CurrentItem() => HostCurrentItem();

        public void OpenList(string title, IReadOnlyList<string> paths, IReadOnlyList<long>? momentsMs,
                             int selectIndex, bool gallery) => HostOpenList(title, paths, momentsMs, selectIndex, gallery);

        public void SeekVideo(long ms, bool exact) => HostSeekVideo(ms, exact);
        public void SetScrubMarkers(IReadOnlyList<long> ms, int currentIndex) => HostSetScrubMarkers(ms, currentIndex);
        public void SetIndexingStatus(string? text, bool busy) => HostSetIndexingPill(text, busy);
        public void ShowSettings() => HostShowLocalSearchSettings();
        public bool IsPieceInstalled(string pieceId) =>
            Array.Find(AddonSlots, s => s.Id == pieceId && s.Parent is not null)?.Usable ?? false;
        public uint Colour(AddonColour role) => HostColour(role);
        public string UiFontFamily => HostUiFontSource();
        public double UiFontSize => IslandHost.UiFontSize;
        public bool AnimationsEnabled => HostAnimationsEnabled();

        public event Action? ThemeChanged
        {
            add => HostThemeChanged += value;
            remove => HostThemeChanged -= value;
        }
    }

    private static IReadOnlyList<string> _lastMarks = Array.Empty<string>();

    /// <summary>
    /// Native: Ctrl+Shift+I (kind 0, the viewer's marks) or Ctrl+Shift+F7
    /// (kind 1, the files to import now). In: { int32 kind; int32 bytes; UTF-8 JSON }.
    /// </summary>
    public static int ShowImport(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            int kind = Marshal.ReadInt32(arg);
            int len = Marshal.ReadInt32(arg, 4);
            if (len < 0 || 8 + len > sizeBytes) return unchecked((int)0x80070057);
            byte[] bytes = new byte[len];
            Marshal.Copy(arg + 8, bytes, 0, len);
            string json = System.Text.Encoding.UTF8.GetString(bytes);
            return RunImportCommand(kind, json);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static int RunImportCommand(int kind, string json)
    {
        IAddonChrome? import = ImportSlot.Chrome;
        if (import is null) return 1;
        if (kind == 0)
        {
            using (JsonDocument doc = JsonDocument.Parse(json.Length == 0 ? "[]" : json))
            {
                _lastMarks = doc.RootElement.EnumerateArray().Select(e => e.GetString() ?? "")
                    .Where(s => s.Length > 0).ToArray();
            }
            import.Open(null);
        }
        else
        {
            import.ImportNow(json);
        }
        return 0;
    }

    // Mirrors mv::shell::addon_family (commands.h).
    private const int FamilyImport = 1;
    private const int FamilyAi = 2;

    /// <summary>
    /// Native: an add-on command from the router (Milestone H). In:
    /// { int32 family; int32 kind; int32 bytes; UTF-8 JSON }. Family 1 is
    /// Import (kinds as <see cref="ShowImport"/>), family 2 the AI pack
    /// (kinds are <see cref="SearchCommand"/>; the JSON is what is on screen,
    /// which the chrome can also read through IAddonHost2). Returns 0 when the
    /// command ran, 1 when there was nothing to do or no add-on to do it.
    /// </summary>
    public static int ShowAddon(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 12) return unchecked((int)0x80070057);
            int family = Marshal.ReadInt32(arg);
            int kind = Marshal.ReadInt32(arg, 4);
            int len = Marshal.ReadInt32(arg, 8);
            if (len < 0 || 12 + len > sizeBytes) return unchecked((int)0x80070057);
            byte[] bytes = new byte[len];
            Marshal.Copy(arg + 12, bytes, 0, len);
            string json = System.Text.Encoding.UTF8.GetString(bytes);
            return family switch
            {
                FamilyImport => RunImportCommand(kind, json),
                FamilyAi => AiSlot.Chrome is ISearchChrome search && search.RunCommand(kind) ? 0 : 1,
                _ => 1,
            };
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>From the session drain (IslandHost.Filmstrip.cs). Routed by
    /// kind: 1–7 are Import's, 20 and up the AI pack's.</summary>
    private static void OnAddonCompletion(AddonCompletion e)
    {
        if ((uint)e.Event >= (uint)MvAddonEvent.AiStatus)
        {
            if (AiSlot.Chrome is null) return;
            try { AiSlot.Chrome.OnEvent(e); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            return;
        }
        if (ImportSlot.Chrome is not null)
        {
            try { ImportSlot.Chrome.OnEvent(e); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            return;
        }
        // Not installed: the base watch's card arrival may offer it, once.
        if (e.Event == MvAddonEvent.VolumeArrived && e.Payload == 1 && !ImportSlot.State.Installed &&
            !ImportHintDismissed())
        {
            OfferImportHint();
        }
    }

    // Only when there is something to install. Checking the channel is a
    // network call like the update check, so it waits on the same switch:
    // with automatic checks off, a card arriving never reaches the network
    // and Import is offered from Settings only.
    private static void OfferImportHint()
    {
        // Not before the installed state is known: "not installed" is only
        // the default until then.
        if (!_addonStatesRead) return;
        if (ImportSlot.Offer.Kind == OfferKind.Available)
        {
            ShowImportHint(true);
            return;
        }
        if (!HasFlag(SettingFlag.UpdateAutoCheck)) return;
        if (ImportSlot.Offer.Kind is OfferKind.NotPublished or OfferKind.NeedsNewerApp) return;
        _hintAfterProbe = true;
        ProbeOffer(ImportSlot);
    }

    /// <summary>Fetches the signed manifest (two small GETs of fixed URLs, like
    /// the update check) and has the core verify it. UI thread in and out.</summary>
    private static void ProbeOffer(AddonSlot slot)
    {
        if (slot.ProbeRunning) return;
        slot.ProbeRunning = true;
        slot.Offer = new AddonOffer(OfferKind.Checking, 0);
        RefreshSlotUi(slot);
        _ = Task.Run(async () =>
        {
            AddonOffer offer = await ReadOffer(slot).ConfigureAwait(false);
            DispatcherQueueControllerTryEnqueue(() =>
            {
                slot.ProbeRunning = false;
                slot.Offer = offer;
                if (slot == ImportSlot && _hintAfterProbe)
                {
                    _hintAfterProbe = false;
                    if (offer.Kind == OfferKind.Available && _addonStatesRead && !ImportSlot.State.Installed &&
                        !ImportHintDismissed())
                    {
                        ShowImportHint(true);
                    }
                }
                RefreshSlotUi(slot);
            });
        });
    }

    private static void RefreshSlotUi(AddonSlot slot)
    {
        if (slot == ImportSlot) RefreshAddonRow();
        else RefreshLocalSearch();
    }

    // Worker.
    private static async Task<AddonOffer> ReadOffer(AddonSlot slot)
    {
        try
        {
            byte[]? manifest = await GetBytes(slot.Channel + ".json").ConfigureAwait(false);
            byte[]? sig = manifest is null ? null : await GetBytes(slot.Channel + ".json.sig").ConfigureAwait(false);
            if (manifest is null || sig is null) return new AddonOffer(OfferKind.NotPublished, 0);
            using JsonDocument check = JsonDocument.Parse(AddonNative.CheckManifest(manifest, sig));
            JsonElement root = check.RootElement;
            if (root.GetProperty("ok").GetBoolean())
            {
                long installed = root.TryGetProperty("installed_size", out JsonElement size) ? size.GetInt64() : 0;
                string version = root.TryGetProperty("version", out JsonElement v) ? v.GetString() ?? "" : "";
                return new AddonOffer(OfferKind.Available, root.GetProperty("archive").GetProperty("size").GetInt64(),
                    installed, version);
            }
            // A signed piece for a newer host API; anything else that does not
            // verify is, to this build, nothing to offer.
            return new AddonOffer(root.GetProperty("why").GetString() == "needs_update"
                ? OfferKind.NeedsNewerApp : OfferKind.NotPublished, 0);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or IOException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return new AddonOffer(OfferKind.Unreachable, 0);
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException or InvalidOperationException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return new AddonOffer(OfferKind.NotPublished, 0);
        }
    }

    private static string DownloadSize(long bytes) =>
        $"{Math.Max(1, (long)Math.Round(bytes / (1024.0 * 1024.0)))} MB";

    // ---- the one-time hint -----------------------------------------------------

    private static string HintMarker => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaViewer", "import-hint.dismissed");

    private static bool ImportHintDismissed()
    {
        try { return File.Exists(HintMarker); }
        catch (IOException) { return true; }
    }

    private static StackPanel BuildImportHint()
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        _importHint = TextButton("Card inserted — install Import?", () =>
        {
            ShowImportHint(false);
            StartImportInstall();
        });
        ToolTipService.SetToolTip(_importHint,
            "Import copies a card into your library, skips what is already there, and verifies every copy. An optional add-on.");
        _importHintDismiss = TextButton("Not now", () =>
        {
            ShowImportHint(false);
            // "It never appears again after Not now" (plan/18).
            _ = Task.Run(() =>
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(HintMarker)!);
                    File.WriteAllText(HintMarker, "");
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            });
        });
        _importHint.Visibility = Visibility.Collapsed;
        _importHintDismiss.Visibility = Visibility.Collapsed;
        _importProgressLabel = new TextBlock
        {
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        panel.Children.Add(_importHint);
        panel.Children.Add(_importHintDismiss);
        panel.Children.Add(_importProgressLabel);
        // Milestone H: the Local search pill (IslandHost.LocalSearch.cs),
        // collapsed unless the AI pack is indexing.
        panel.Children.Add(BuildIndexingPill());
        return panel;
    }

    private static void ShowImportHint(bool show)
    {
        if (_importHint is null || _importHintDismiss is null) return;
        if (show && _importHint.Content is TextBlock text)
        {
            text.Text = $"Card inserted — install Import ({DownloadSize(ImportSlot.Offer.ArchiveSize)})?";
        }
        Visibility v = show ? Visibility.Visible : Visibility.Collapsed;
        _importHint.Visibility = v;
        _importHintDismiss.Visibility = v;
    }

    // ---- Settings → Add-ons -----------------------------------------------------

    private static void AddAddonsSettingsRow(StackPanel view)
    {
        view.Children.Add(Heading("Add-ons"));
        _addonRow = new StackPanel { Spacing = 6 };
        _addonStatus = Label("");
        _addonStatus.TextWrapping = TextWrapping.Wrap;
        view.Children.Add(_addonRow);
        view.Children.Add(_addonStatus);
        RefreshAddonRow();
        // Opening Settings asks the channel, whatever the automatic-check
        // switch says: the person is looking at what can be installed, or
        // updated (plan/18).
        ProbeOffer(ImportSlot);
    }

    private static TextBlock WrappedLabel(string text)
    {
        TextBlock t = Label(text);
        t.TextWrapping = TextWrapping.Wrap;
        return t;
    }

    private static void SetAddonStatus(string text)
    {
        if (_addonStatus is not null) _addonStatus.Text = text;
    }

    private static void RefreshAddonRow()
    {
        if (_addonRow is null) return;
        _addonRow.Children.Clear();
        var title = Label("Import");
        _addonRow.Children.Add(title);
        var about = Label("Copy a card or folder into your library: skips what is already there by content, verifies every copy, sorts by date. Never deletes from the card.");
        about.TextWrapping = TextWrapping.Wrap;
        _addonRow.Children.Add(about);
        if (!_addonStatesRead)
        {
            // Quiet, and no button while what is installed is unknown.
            _addonRow.Children.Add(WrappedLabel("Checking installed add-ons…"));
            return;
        }
        AddonState state = ImportSlot.State;
        if (ImportSlot.Busy && ImportSlot.Phase is not null)
        {
            // Installing: the bar in place of the buttons.
            _addonRow.Children.Add(ProgressFor(ImportSlot, 320));
            return;
        }
        if (!state.Installed)
        {
            switch (ImportSlot.Offer.Kind)
            {
                case OfferKind.Available:
                    _addonRow.Children.Add(SettingsButton($"Install Import, {DownloadSize(ImportSlot.Offer.ArchiveSize)}",
                        StartImportInstall));
                    break;
                case OfferKind.NotPublished:
                    _addonRow.Children.Add(WrappedLabel("Import is not published for download yet. It will be offered here once a release carries it."));
                    break;
                case OfferKind.NeedsNewerApp:
                    _addonRow.Children.Add(WrappedLabel("The published Import needs a newer MediaViewer. Update MediaViewer, then install it."));
                    break;
                case OfferKind.Unreachable:
                    _addonRow.Children.Add(WrappedLabel("Could not reach the download server."));
                    _addonRow.Children.Add(SettingsButton("Try again", () => ProbeOffer(ImportSlot)));
                    break;
                default:
                    _addonRow.Children.Add(WrappedLabel("Checking for Import…"));
                    break;
            }
            return;
        }
        string line = state.State switch
        {
            "ok" => $"Installed, version {state.Version}.",
            "needs_update" => $"Version {state.Version} needs an update to work with this MediaViewer.",
            _ => "The installed copy did not pass verification and is not loaded. Reinstall it.",
        };
        _addonRow.Children.Add(Label(line));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (UpdateVersion(ImportSlot) is string newer)
        {
            buttons.Children.Add(SettingsButton($"Update to {newer}, {DownloadSize(ImportSlot.Offer.ArchiveSize)}",
                StartImportInstall));
        }
        if (state.State != "ok") buttons.Children.Add(SettingsButton("Reinstall", StartImportInstall));
        if (ImportSlot.Chrome is not null) buttons.Children.Add(SettingsButton("Open Import", () => ImportSlot.Chrome?.Open(null)));
        buttons.Children.Add(SettingsButton("Remove…", ConfirmImportRemove));
        _addonRow.Children.Add(buttons);
    }

    private static void ConfirmImportRemove()
    {
        if (_addonRow is null) return;
        _addonRow.Children.Clear();
        var q = Label("Remove Import? Its library index and import history (import.db) can stay, so a reinstall still knows what was imported.");
        q.TextWrapping = TextWrapping.Wrap;
        _addonRow.Children.Add(q);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(SettingsButton("Remove, keep history", () => RemoveImport(keepData: true)));
        buttons.Children.Add(SettingsButton("Remove everything", () => RemoveImport(keepData: false)));
        buttons.Children.Add(SettingsButton("Cancel", RefreshAddonRow));
        _addonRow.Children.Add(buttons);
    }

    private static void RemoveImport(bool keepData)
    {
        if (ImportSlot.Busy) return;
        ImportSlot.Busy = true;
        UnloadAddonChrome(ImportSlot);
        SetAddonStatus("Removing Import…");
        _ = Task.Run(() =>
        {
            string done;
            try
            {
                AddonNative.Remove(ImportSlot.Id, keepData);
                done = "Import removed.";
            }
            catch (MediaViewerException)
            {
                done = "Import will finish uninstalling the next time MediaViewer starts.";
            }
            Dictionary<string, AddonState> states = ReadAddonStates();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                ImportSlot.Busy = false;
                ApplyAddonStates(states);
                SetAddonStatus(done);
                RefreshAddonRow();
            });
        });
    }

    private static void StartImportInstall()
    {
        if (ImportSlot.Busy || !_addonStatesRead) return;
        ImportSlot.Busy = true;
        // An update of a running Import installs beside it and takes over at
        // the next start (the store keeps the running copy until then).
        string? update = UpdateVersion(ImportSlot);
        bool running = ImportSlot.Chrome is not null;
        SetAddonStatus(update is null ? "Downloading Import…" : $"Downloading Import {update}…");
        ImportSlot.Phase = new AddonPhase(AddonPhaseKind.Downloading);
        IProgress<AddonPhase> progress = PhaseReporter(ImportSlot);
        RefreshAddonRow();
        _ = Task.Run(async () =>
        {
            string message;
            try
            {
                await DownloadAndInstall(ImportSlot, null, progress).ConfigureAwait(false);
                message = update is null ? "Import installed."
                    : running ? $"Import {update} is installed. It takes over the next time MediaViewer starts."
                    : $"Import updated to {update}.";
            }
            catch (AddonNotPublishedException)
            {
                message = "";
                DispatcherQueueControllerTryEnqueue(() => ImportSlot.Offer = new AddonOffer(OfferKind.NotPublished, 0));
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or MediaViewerException
                                           or InvalidDataException or TaskCanceledException or JsonException)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = ex is MediaViewerException or InvalidDataException
                    ? "The download did not verify, so nothing was installed."
                    : "Import could not be downloaded. Check the connection and try again.";
            }
            Dictionary<string, AddonState> states = ReadAddonStates();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                ImportSlot.Busy = false;
                ImportSlot.Phase = null;
                ApplyAddonStates(states);
                SetAddonStatus(message);
                RefreshAddonRow();
                if (ImportSlot.Usable && ImportSlot.Chrome is null) LoadAddon(ImportSlot);
            });
        });
    }

    // Worker. Manifest first (signature checked before anything else is
    // fetched), then the archive (size + SHA-256 before it is opened), then
    // extraction into staging, then the core's full verify-and-install.
    // `admit` sees the verified manifest before the archive is requested and
    // may refuse it (the AI family's 3 GB rule) by throwing.
    private static async Task DownloadAndInstall(AddonSlot slot, Action<JsonElement>? admit,
                                                 IProgress<AddonPhase>? progress = null)
    {
        byte[] manifest = await GetBytes(slot.Channel + ".json").ConfigureAwait(false)
                          ?? throw new AddonNotPublishedException();
        byte[] sig = await GetBytes(slot.Channel + ".json.sig").ConfigureAwait(false)
                     ?? throw new AddonNotPublishedException();
        using JsonDocument check = JsonDocument.Parse(AddonNative.CheckManifest(manifest, sig));
        JsonElement root = check.RootElement;
        if (!root.GetProperty("ok").GetBoolean())
        {
            throw new InvalidDataException("manifest: " + root.GetProperty("why").GetString());
        }
        admit?.Invoke(root);
        JsonElement archive = root.GetProperty("archive");
        string archiveName = archive.GetProperty("path").GetString() ?? throw new InvalidDataException();
        string archiveSha = archive.GetProperty("sha256").GetString() ?? throw new InvalidDataException();
        long archiveSize = archive.GetProperty("size").GetInt64();

        string staging = AddonNative.MakeStaging();
        string zipPath = Path.Combine(Path.GetDirectoryName(staging)!, Path.GetFileName(staging) + ".zip");
        try
        {
            progress?.Report(new AddonPhase(AddonPhaseKind.Downloading, 0, archiveSize));
            using (HttpResponseMessage r = await AddonHttp.GetAsync(UpdateReleaseBase + archiveName,
                       HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
            {
                r.EnsureSuccessStatusCode();
                await using Stream body = await r.Content.ReadAsStreamAsync().ConfigureAwait(false);
                await using FileStream file = new(zipPath, FileMode.CreateNew, FileAccess.Write);
                byte[] buffer = new byte[1 << 16];
                long total = 0;
                long lastReport = 0;
                var clock = System.Diagnostics.Stopwatch.StartNew();
                int n;
                while ((n = await body.ReadAsync(buffer).ConfigureAwait(false)) > 0)
                {
                    total += n;
                    if (total > archiveSize) throw new InvalidDataException("archive larger than signed");
                    await file.WriteAsync(buffer.AsMemory(0, n)).ConfigureAwait(false);
                    // ~10 reports a second, each a hop to the UI thread. By
                    // time, not 1 % steps: 1 % of a gigabyte is 10 MB, which
                    // on a slow line held the bar at 0 % for many seconds.
                    long now = clock.ElapsedMilliseconds;
                    if (progress is not null && (now - lastReport >= 100 || total == archiveSize))
                    {
                        lastReport = now;
                        progress.Report(new AddonPhase(AddonPhaseKind.Downloading, total, archiveSize));
                    }
                }
            }
            // Size and SHA-256 of a gigabyte: seconds, with no fraction to show.
            progress?.Report(new AddonPhase(AddonPhaseKind.Checking));
            if (new FileInfo(zipPath).Length != archiveSize || AddonNative.Sha256File(zipPath) != archiveSha)
            {
                throw new InvalidDataException("archive does not match the signed manifest");
            }
            // Authenticated bytes only from here. ExtractToDirectory refuses
            // entries that would land outside `staging`.
            progress?.Report(new AddonPhase(AddonPhaseKind.Installing));
            ZipFile.ExtractToDirectory(zipPath, staging);
            File.WriteAllBytes(Path.Combine(staging, "manifest.json"), manifest);
            File.WriteAllBytes(Path.Combine(staging, "manifest.json.sig"), sig);
            AddonNative.Install(staging);  // verifies every file; consumes staging
        }
        finally
        {
            try { File.Delete(zipPath); } catch (IOException) { }
            try { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); } catch (IOException) { }
        }
    }

    // The channel has no such add-on (a 404): the row says so rather than blaming the connection.
    private sealed class AddonNotPublishedException : Exception { }

    private static async Task<byte[]?> GetBytes(string url)
    {
        using HttpResponseMessage r = await AddonHttp.GetAsync(url).ConfigureAwait(false);
        if (r.StatusCode == HttpStatusCode.NotFound) return null;
        r.EnsureSuccessStatusCode();
        return await r.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
    }
}
