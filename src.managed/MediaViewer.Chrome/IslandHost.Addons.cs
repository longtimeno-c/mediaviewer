// SPDX-License-Identifier: GPL-2.0-or-later
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// Add-ons (plan/18 "Add-ons: how Import is installed"): Settings → Add-ons,
/// the download, the one-time card hint, and loading an installed add-on's
/// chrome into its own <see cref="AssemblyLoadContext"/>.
/// </summary>
/// <remarks>
/// The download is a plain GET of fixed release-asset URLs on the update
/// channel: no query, no cookies, no identifier, nothing about the user's
/// files (rule 6). The core verifies the signed manifest before the archive
/// is even requested, the archive's SHA-256 before it is opened, and every
/// extracted file (and that there is nothing extra) before it is installed;
/// it verifies again at every load. With nothing installed, nothing here
/// writes a byte, and Import's commands do not exist.
/// </remarks>
public static partial class IslandHost
{
    private const string ImportId = "import";
    private const string ImportInterface = "mv.import.1";
    private const string AddonChannelBase = UpdateReleaseBase + "mediaviewer-addon-import-win-x64";
    private const string UpdateReleaseBase = "https://github.com/longtimeno-c/mediaviewer/releases/latest/download/";

    private static bool _addonsStarted;
    private static bool _addonBusy;
    private static AddonState _importState = new(false, "", "", 0, false);
    private static IAddonChrome? _importChrome;
    private static AddonLoadContext? _importAlc;
    private static StackPanel? _addonRow;
    private static TextBlock? _addonStatus;
    private static Button? _importHint;
    private static Button? _importHintDismiss;
    private static TextBlock? _importProgressLabel;

    private sealed record AddonState(bool Installed, string Version, string State, long Size, bool Loaded);

    private static readonly HttpClient AddonHttp = CreateAddonClient();

    private static HttpClient CreateAddonClient()
    {
        var client = new HttpClient(new HttpClientHandler { UseCookies = false }) { Timeout = TimeSpan.FromMinutes(5) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MediaViewer");
        return client;
    }

    /// <summary>Once a session is borrowed: load an installed add-on, watch for cards.</summary>
    private static void StartAddons()
    {
        if (_addonsStarted || _folderSession is null) return;
        _addonsStarted = true;
        MediaViewerSession session = _folderSession;
        _ = Task.Run(() =>
        {
            AddonState state = ReadImportState();
            try { AddonNative.WatchVolumes(session); }
            catch (MediaViewerException ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _importState = state;
                if (state.Installed && state.State == "ok") LoadImport();
                RefreshAddonRow();
            });
        });
    }

    // Worker: hashes every installed file.
    private static AddonState ReadImportState()
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(AddonNative.InstalledJson());
            foreach (JsonElement a in doc.RootElement.EnumerateArray())
            {
                if (a.GetProperty("id").GetString() != ImportId) continue;
                return new AddonState(true, a.GetProperty("version").GetString() ?? "",
                    a.GetProperty("state").GetString() ?? "invalid", a.GetProperty("size").GetInt64(),
                    a.GetProperty("loaded").GetBoolean());
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        return new AddonState(false, "", "", 0, false);
    }

    // ---- loading -------------------------------------------------------------

    /// <summary>Resolves the add-on's own dependencies from its folder; shares
    /// MediaViewer.Interop (IAddonHost) and WinUI with the default context.</summary>
    private sealed class AddonLoadContext(string mainAssembly) : AssemblyLoadContext("addon-import", isCollectible: true)
    {
        private readonly AssemblyDependencyResolver _resolver = new(mainAssembly);

        protected override Assembly? Load(AssemblyName name)
        {
            // One copy of the contract and of WinUI in the process.
            if (name.Name is "MediaViewer.Interop" || (name.Name?.StartsWith("Microsoft.", StringComparison.Ordinal) ?? false)
                || (name.Name?.StartsWith("WinRT", StringComparison.Ordinal) ?? false))
            {
                return null;
            }
            string? path = _resolver.ResolveAssemblyToPath(name);
            return path is null ? null : LoadFromAssemblyPath(path);
        }
    }

    private static void LoadImport()
    {
        if (_importChrome is not null || _folderSession is null || _addonBusy) return;
        MediaViewerSession session = _folderSession;
        _addonBusy = true;
        _ = Task.Run(() =>
        {
            try
            {
                // Native load re-verifies the signature and every file first.
                (IntPtr table, string chromePath) = AddonNative.Load(session, ImportId, ImportInterface);
                DispatcherQueueControllerTryEnqueue(() => AttachImportChrome(table, chromePath));
            }
            catch (MediaViewerException ex)
            {
                string why = ex.Status == MvStatus.UnsupportedFormat
                    ? "Import needs an update."
                    : "Import did not pass verification and was not loaded.";
                DispatcherQueueControllerTryEnqueue(() =>
                {
                    _addonBusy = false;
                    SetAddonStatus(why);
                });
            }
        });
    }

    private static void AttachImportChrome(IntPtr table, string chromePath)
    {
        try
        {
            _importAlc = new AddonLoadContext(chromePath);
            Assembly asm = _importAlc.LoadFromAssemblyPath(chromePath);
            Type? entry = asm.GetExportedTypes().FirstOrDefault(t => typeof(IAddonChrome).IsAssignableFrom(t) && !t.IsAbstract);
            if (entry is null || Activator.CreateInstance(entry) is not IAddonChrome chrome)
            {
                throw new InvalidOperationException("no IAddonChrome in the add-on");
            }
            chrome.Attach(new AddonHost(), table);
            _importChrome = chrome;
            _importState = _importState with { Loaded = true };
            Send(Command.AddonState, 1);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            SetAddonStatus("Import could not start.");
            UnloadImportChrome();
            try { AddonNative.Unload(ImportId); } catch (MediaViewerException) { }
        }
        finally
        {
            _addonBusy = false;
            RefreshAddonRow();
        }
    }

    private static void UnloadImportChrome()
    {
        try { _importChrome?.Shutdown(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        _importChrome = null;
        _importAlc?.Unload();
        _importAlc = null;
        _importState = _importState with { Loaded = false };
        Send(Command.AddonState, 0);
    }

    /// <summary>The base chrome's services for the add-on (plan/18 IAddonHost).</summary>
    private sealed class AddonHost : IAddonHost
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
            if (_importProgressLabel is null) return;
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
            if (_importChrome is null) return 1;
            if (kind == 0)
            {
                using (JsonDocument doc = JsonDocument.Parse(json.Length == 0 ? "[]" : json))
                {
                    _lastMarks = doc.RootElement.EnumerateArray().Select(e => e.GetString() ?? "")
                        .Where(s => s.Length > 0).ToArray();
                }
                _importChrome.Open(null);
            }
            else
            {
                _importChrome.ImportNow(json);
            }
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    /// <summary>From the session drain (IslandHost.Filmstrip.cs).</summary>
    private static void OnAddonCompletion(AddonCompletion e)
    {
        if (_importChrome is not null)
        {
            try { _importChrome.OnEvent(e); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            return;
        }
        // Not installed: the base watch's card arrival may offer it, once.
        if (e.Event == MvAddonEvent.VolumeArrived && e.Payload == 1 && !_importState.Installed &&
            !ImportHintDismissed())
        {
            ShowImportHint(true);
        }
    }

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
        _importHint = TextButton("Card inserted — install Import (3 MB)?", () =>
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
        return panel;
    }

    private static void ShowImportHint(bool show)
    {
        if (_importHint is null || _importHintDismiss is null) return;
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
        if (!_importState.Installed)
        {
            _addonRow.Children.Add(SettingsButton("Install Import, 3 MB", StartImportInstall));
            return;
        }
        string line = _importState.State switch
        {
            "ok" => $"Installed, version {_importState.Version}.",
            "needs_update" => $"Version {_importState.Version} needs an update to work with this MediaViewer.",
            _ => "The installed copy did not pass verification and is not loaded. Reinstall it.",
        };
        _addonRow.Children.Add(Label(line));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (_importState.State != "ok") buttons.Children.Add(SettingsButton("Reinstall", StartImportInstall));
        if (_importChrome is not null) buttons.Children.Add(SettingsButton("Open Import", () => _importChrome?.Open(null)));
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
        if (_addonBusy) return;
        _addonBusy = true;
        UnloadImportChrome();
        SetAddonStatus("Removing Import…");
        _ = Task.Run(() =>
        {
            string done;
            try
            {
                AddonNative.Remove(ImportId, keepData);
                done = "Import removed.";
            }
            catch (MediaViewerException)
            {
                done = "Import will finish uninstalling the next time MediaViewer starts.";
            }
            AddonState state = ReadImportState();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _addonBusy = false;
                _importState = state;
                SetAddonStatus(done);
                RefreshAddonRow();
            });
        });
    }

    private static void StartImportInstall()
    {
        if (_addonBusy) return;
        _addonBusy = true;
        SetAddonStatus("Downloading Import…");
        _ = Task.Run(async () =>
        {
            string message;
            try
            {
                await DownloadAndInstallImport().ConfigureAwait(false);
                message = "Import installed.";
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or MediaViewerException
                                           or InvalidDataException or TaskCanceledException or JsonException)
            {
                System.Diagnostics.Debug.WriteLine(ex);
                message = ex is MediaViewerException or InvalidDataException
                    ? "The download did not verify, so nothing was installed."
                    : "Import could not be downloaded. Check the connection and try again.";
            }
            AddonState state = ReadImportState();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _addonBusy = false;
                _importState = state;
                SetAddonStatus(message);
                RefreshAddonRow();
                if (state.Installed && state.State == "ok") LoadImport();
            });
        });
    }

    // Worker. Manifest first (signature checked before anything else is
    // fetched), then the archive (size + SHA-256 before it is opened), then
    // extraction into staging, then the core's full verify-and-install.
    private static async Task DownloadAndInstallImport()
    {
        byte[] manifest = await GetBytes(AddonChannelBase + ".json").ConfigureAwait(false)
                          ?? throw new HttpRequestException("no manifest");
        byte[] sig = await GetBytes(AddonChannelBase + ".json.sig").ConfigureAwait(false)
                     ?? throw new InvalidDataException("no signature");
        using JsonDocument check = JsonDocument.Parse(AddonNative.CheckManifest(manifest, sig));
        JsonElement root = check.RootElement;
        if (!root.GetProperty("ok").GetBoolean())
        {
            throw new InvalidDataException("manifest: " + root.GetProperty("why").GetString());
        }
        JsonElement archive = root.GetProperty("archive");
        string archiveName = archive.GetProperty("path").GetString() ?? throw new InvalidDataException();
        string archiveSha = archive.GetProperty("sha256").GetString() ?? throw new InvalidDataException();
        long archiveSize = archive.GetProperty("size").GetInt64();

        string staging = AddonNative.MakeStaging();
        string zipPath = Path.Combine(Path.GetDirectoryName(staging)!, Path.GetFileName(staging) + ".zip");
        try
        {
            using (HttpResponseMessage r = await AddonHttp.GetAsync(UpdateReleaseBase + archiveName,
                       HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
            {
                r.EnsureSuccessStatusCode();
                await using Stream body = await r.Content.ReadAsStreamAsync().ConfigureAwait(false);
                await using FileStream file = new(zipPath, FileMode.CreateNew, FileAccess.Write);
                byte[] buffer = new byte[1 << 16];
                long total = 0;
                int n;
                while ((n = await body.ReadAsync(buffer).ConfigureAwait(false)) > 0)
                {
                    total += n;
                    if (total > archiveSize) throw new InvalidDataException("archive larger than signed");
                    await file.WriteAsync(buffer.AsMemory(0, n)).ConfigureAwait(false);
                }
            }
            if (new FileInfo(zipPath).Length != archiveSize || AddonNative.Sha256File(zipPath) != archiveSha)
            {
                throw new InvalidDataException("archive does not match the signed manifest");
            }
            // Authenticated bytes only from here. ExtractToDirectory refuses
            // entries that would land outside `staging`.
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

    private static async Task<byte[]?> GetBytes(string url)
    {
        using HttpResponseMessage r = await AddonHttp.GetAsync(url).ConfigureAwait(false);
        if (r.StatusCode == HttpStatusCode.NotFound) return null;
        r.EnsureSuccessStatusCode();
        return await r.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
    }
}
