// SPDX-License-Identifier: GPL-2.0-or-later
using System.Diagnostics;
using Velopack;
using Velopack.Sources;

namespace MediaViewer.Updater;

public enum UpdatePhase
{
    Inert,       // not a Velopack install (dev build) or not started
    Idle,
    Checking,
    Downloading,
    Ready,       // staged in packages\, verified; "Update ready — restart"
    Failed,
}

public sealed record UpdateStatus(UpdatePhase Phase, string? Version, UpdateUrgency Urgency, string? RolledBackFrom);

/// <summary>
/// plan/13 Part 1 behaviour on top of Velopack. One background thread at
/// below-normal priority does every network and disk operation; the chrome
/// only reads <see cref="Status"/> and calls the two apply methods.
/// </summary>
/// <remarks>
/// Never interrupts: nothing here shows a window, and nothing restarts except
/// <see cref="RequestRestart"/>, which the user clicks. A staged update that is
/// ignored is applied when the app exits (<see cref="ApplyOnExit"/>).
/// </remarks>
public sealed class UpdateService
{
    public static readonly TimeSpan CheckInterval = TimeSpan.FromHours(6);
    public static readonly TimeSpan FirstCheckDelay = TimeSpan.FromSeconds(30);

    private readonly Func<bool> _autoCheck;
    private readonly Action<string> _log;
    private readonly AutoResetEvent _wake = new(false);
    private readonly object _gate = new();
    private Thread? _thread;
    private volatile bool _stopping;
    private UpdateManager? _manager;
    private VelopackAsset? _staged;
    private ReleaseVersion _running;
    private string? _root;
    private bool _applyLaunched;

    public UpdateService(Func<bool> autoCheck, Action<string> log)
    {
        _autoCheck = autoCheck;
        _log = log;
    }

    public UpdateStatus Status { get; private set; } = new(UpdatePhase.Inert, null, UpdateUrgency.Normal, null);

    /// <summary>Raised on the updater thread. The chrome marshals to its dispatcher.</summary>
    public event Action<UpdateStatus>? StatusChanged;

    public void Start()
    {
        lock (_gate)
        {
            if (_thread is not null) return;
            _thread = new Thread(Run)
            {
                IsBackground = true,
                Priority = ThreadPriority.BelowNormal,
                Name = "MediaViewer updater",
            };
            _thread.Start();
        }
    }

    /// <summary>Settings toggled auto-check on: check soon rather than in 6 h.</summary>
    public void Poke() => _wake.Set();

    public void Stop()
    {
        _stopping = true;
        _wake.Set();
    }

    private void SetStatus(UpdateStatus s)
    {
        Status = s;
        try { StatusChanged?.Invoke(s); }
        catch (Exception ex) { _log("updater: status handler threw " + ex.GetType().Name); }
    }

    private void Run()
    {
        try
        {
            if (!Initialise()) return;
            TimeSpan wait = FirstCheckDelay;
#if MV_UPDATER_DEV
            if (int.TryParse(Environment.GetEnvironmentVariable("MV_UPDATE_CHECK_DELAY_MS"), out int ms) && ms >= 0)
                wait = TimeSpan.FromMilliseconds(ms);
#endif
            while (!_stopping)
            {
                _wake.WaitOne(wait);
                if (_stopping) break;
                wait = CheckInterval;
                if (!_autoCheck()) continue;
                CheckAndStage();
            }
        }
        catch (Exception ex)
        {
            // Never let the updater take the viewer down. Type only: exception
            // messages can carry paths (rule 6).
            _log("updater: stopped after " + ex.GetType().Name);
            SetStatus(Status with { Phase = UpdatePhase.Failed });
        }
    }

    private bool Initialise()
    {
        string? exe = Environment.ProcessPath;
        string? current = exe is null ? null : Path.GetDirectoryName(exe);
        string? root = current is null ? null : Path.GetDirectoryName(current);
        if (root is null || !File.Exists(Path.Combine(root, "Update.exe")) ||
            !File.Exists(Path.Combine(current!, "sq.version")))
        {
            _log("updater: inert — not running from a Velopack install (dev build)");
            return false;
        }

        // Hooks (--veloapp-*) never reach here: the native host exits on them
        // before loading .NET. Empty args so Run() only sets up the locator;
        // no auto-apply-and-restart at startup (plan/13: never interrupt).
        VelopackApp.Build().SetArgs(Array.Empty<string>()).SetAutoApplyOnStartup(false).Run();

        _root = root;
        string? pinnedHex = UpdateKeys.ProductionPublicKeyHex;
        IManifestFetcher fetcher = new GithubManifestFetcher();
        IUpdateSource inner = new GithubSource(UpdateKeys.GithubRepoUrl, null, false);
#if MV_UPDATER_DEV
        string? devDir = Environment.GetEnvironmentVariable("MV_UPDATE_FEED_DIR");
        string? devKey = Environment.GetEnvironmentVariable("MV_UPDATE_DEV_PUBKEY");
        if (!string.IsNullOrEmpty(devDir))
        {
            fetcher = new DirectoryManifestFetcher(devDir);
            inner = new SimpleFileSource(new DirectoryInfo(devDir));
            _log("updater: DEV BUILD — local feed");
        }
        if (!string.IsNullOrEmpty(devKey)) pinnedHex = devKey;
#endif
        byte[] key = Hex.TryDecode(pinnedHex, 32, out byte[] k) ? k : new byte[32];

        var probe = new UpdateManager(inner, new UpdateOptions { ExplicitChannel = UpdateKeys.Channel });
        if (!probe.IsInstalled || probe.CurrentVersion is null ||
            !ReleaseVersion.TryParse(probe.CurrentVersion.ToString(), out _running))
        {
            _log("updater: inert — Velopack reports no installed version");
            return false;
        }

        List<ReleaseVersion> failed = TrialState.FailedVersions(root);
        string? rolledBack = TrialState.TakeUnreportedRollback(root);
        var source = new SignedManifestSource(inner, fetcher, key, _running, failed);
        _source = source;
        _manager = new UpdateManager(source, new UpdateOptions { ExplicitChannel = UpdateKeys.Channel });
        _log($"updater: installed {_running}");
        SetStatus(new UpdateStatus(UpdatePhase.Idle, null, UpdateUrgency.Normal, rolledBack));
        return true;
    }

    private SignedManifestSource? _source;

    private void CheckAndStage()
    {
        UpdateManager mgr = _manager!;
        SetStatus(Status with { Phase = UpdatePhase.Checking });
        UpdateInfo? info;
        try
        {
            info = mgr.CheckForUpdates();
        }
        catch (Exception ex)
        {
            _log("updater: check failed " + ex.GetType().Name);
            SetStatus(Status with { Phase = _staged is null ? UpdatePhase.Idle : UpdatePhase.Ready });
            return;
        }
        ManifestDecision? d = _source!.LastDecision;
        if (d is not null && !d.Trusted) _log("updater: manifest rejected: " + d.Rejection);
        if (info is null)
        {
            SetStatus(Status with { Phase = _staged is null ? UpdatePhase.Idle : UpdatePhase.Ready });
            return;
        }

        VelopackAsset target = info.TargetFullRelease;
        if (_staged is not null && _staged.Version == target.Version)
        {
            SetStatus(Status with { Phase = UpdatePhase.Ready });
            return;
        }

        SetStatus(Status with { Phase = UpdatePhase.Downloading, Version = target.Version.ToString() });
        try
        {
            KeepPriorPackage();
            mgr.DownloadUpdates(info);
        }
        catch (Exception ex)
        {
            _log("updater: download rejected " + ex.GetType().Name);
            SetStatus(Status with { Phase = UpdatePhase.Failed, Version = null });
            return;
        }
        lock (_gate) _staged = target;
        _log($"updater: staged {target.Version}");
        SetStatus(new UpdateStatus(UpdatePhase.Ready, target.Version.ToString(),
            d?.Urgency ?? UpdateUrgency.Normal, Status.RolledBackFrom));
    }

    /// <summary>
    /// Velopack's download deletes every other package in packages\. Before it
    /// runs, copy the running version's full package aside so a failed start
    /// can reinstall it. Exactly one prior version is kept.
    /// </summary>
    private void KeepPriorPackage()
    {
        string packages = Path.Combine(_root!, "packages");
        string rollback = TrialState.RollbackDir(_root!);
        string? full = Directory.Exists(packages)
            ? Directory.EnumerateFiles(packages, $"*-{_running}-full.nupkg").FirstOrDefault()
            : null;
        if (full is null)
        {
            _log("updater: no full package for the running version; rollback unavailable for this update");
            return;
        }
        Directory.CreateDirectory(rollback);
        string dest = Path.Combine(rollback, Path.GetFileName(full));
        foreach (string old in Directory.EnumerateFiles(rollback))
        {
            if (!string.Equals(old, dest, StringComparison.OrdinalIgnoreCase)) File.Delete(old);
        }
        if (!File.Exists(dest)) File.Copy(full, dest);
    }

    private string? PriorPackageName()
    {
        string rollback = TrialState.RollbackDir(_root!);
        return Directory.Exists(rollback)
            ? Directory.EnumerateFiles(rollback, $"*-{_running}-full.nupkg").Select(Path.GetFileName).FirstOrDefault()
            : null;
    }

    private bool LaunchApply(bool restart, string[]? restartArgs)
    {
        VelopackAsset? staged;
        lock (_gate)
        {
            staged = _staged;
            if (staged is null || _manager is null || _applyLaunched) return false;
            _applyLaunched = true;
        }
        try
        {
            if (ReleaseVersion.TryParse(staged.Version.ToString(), out ReleaseVersion target))
                TrialState.Arm(_root!, target, _running, PriorPackageName());
            // silent: no progress window. Update.exe waits for this process to
            // exit before it touches current\ — locked DLLs are never replaced
            // under a running viewer.
            _manager.WaitExitThenApplyUpdates(staged, silent: true, restart: restart, restartArgs: restartArgs);
            _log(restart ? "updater: apply-and-restart armed" : "updater: apply-on-exit armed");
            return true;
        }
        catch (Exception ex)
        {
            _log("updater: could not launch Update.exe " + ex.GetType().Name);
            lock (_gate) _applyLaunched = false;
            return false;
        }
    }

    /// <summary>The user clicked "Update ready — restart". The host exits next.</summary>
    public bool RequestRestart(string[] restartArgs) => LaunchApply(true, restartArgs);

    /// <summary>Natural exit with an update staged: apply it after this process is gone, no restart.</summary>
    public bool ApplyOnExit() => LaunchApply(false, null);

    [Conditional("DEBUG")]
    internal void DebugDump() => _log($"updater: phase={Status.Phase} staged={_staged?.Version}");
}
