// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Dispatching;

namespace MediaViewer.Import.Chrome;

/// <summary>
/// The add-on chrome's entry point (plan/18 "Windows chrome"). Owns the one
/// Import window and every running job's progress, so closing the window
/// keeps an import running with a small indicator in the main command bar,
/// and a notification when it finishes.
/// </summary>
public sealed class ImportChrome : IAddonChrome
{
    private IAddonHost? _host;
    private ImportApi? _api;
    private ImportWindow? _window;
    private DispatcherQueueTimer? _timer;
    private readonly HashSet<ulong> _jobs = new();
    private readonly Dictionary<ulong, string> _jobLabels = new();

    internal ImportApi Api => _api ?? throw new InvalidOperationException("not attached");
    internal IAddonHost Host => _host ?? throw new InvalidOperationException("not attached");

    public void Attach(IAddonHost host, IntPtr interfaceTable)
    {
        _host = host;
        _api = new ImportApi(interfaceTable);
        _timer = DispatcherQueue.GetForCurrentThread()?.CreateTimer();
        if (_timer is not null)
        {
            _timer.Interval = TimeSpan.FromMilliseconds(250);
            _timer.Tick += (_, _) => Tick();
        }
    }

    public void Open(string? sourceRoot)
    {
        if (_api is null || _host is null) return;
        if (_window is null)
        {
            _window = new ImportWindow(this);
            _window.Closed += (_, _) => _window = null;
        }
        _window.Show(sourceRoot, _host.MarkedPaths());
    }

    public void ImportNow(string pathsJson)
    {
        if (_api is null) return;
        try
        {
            ulong job = _api.ImportNow(pathsJson);
            Track(job, "marked files");
        }
        catch (MediaViewerException ex)
        {
            _host?.SetStatus("Import could not start: " + ex.Status);
        }
    }

    internal void Track(ulong job, string label)
    {
        _jobs.Add(job);
        _jobLabels[job] = label;
        _timer?.Start();
        Tick();
    }

    public void OnEvent(AddonCompletion e)
    {
        switch (e.Event)
        {
            case MvAddonEvent.VolumeArrived:
                if ((MvImportArrival)e.Payload == MvImportArrival.Auto)
                {
                    Track(e.Id, "auto-import");
                }
                else if ((MvImportArrival)e.Payload == MvImportArrival.Open)
                {
                    string root = Try(() => Api.ArrivalRoot(e.Id)) ?? "";
                    if (root.Length > 0) Open(root);
                }
                _window?.OnSourcesChanged();
                break;
            case MvAddonEvent.VolumeRemoved:
            case MvAddonEvent.ScanDone when e.Id == 0:
                _window?.OnSourcesChanged();
                break;
            case MvAddonEvent.ScanDone:
                _window?.OnScanDone(e.Id, e.Status);
                break;
            case MvAddonEvent.PlanReady:
                _window?.OnPlanReady(e.Id, e.Status);
                break;
            case MvAddonEvent.JobProgress:
                Tick();
                break;
            case MvAddonEvent.JobDone:
            case MvAddonEvent.VerifyDone:
                Tick();
                Finished(e.Id, (MvImportJobState)(uint)e.Payload);
                break;
        }
    }

    private void Finished(ulong job, MvImportJobState state)
    {
        bool mine = _jobs.Remove(job);
        _window?.OnJobDone(job, state);
        if (!mine || _host is null) return;
        string label = _jobLabels.GetValueOrDefault(job, "Import");
        string text = state switch
        {
            MvImportJobState.Done => "Finished. Every copy verified.",
            MvImportJobState.Failed => "Finished with files that could not be copied. Open Import to retry them.",
            MvImportJobState.Interrupted => "Interrupted. Reconnect the card or drive and resume in Import.",
            _ => "Cancelled. Nothing partial was left behind.",
        };
        try
        {
            using JsonDocument s = JsonDocument.Parse(Api.SummaryJson(job));
            if (s.RootElement.TryGetProperty("copied", out JsonElement copied) &&
                copied.TryGetProperty("files", out JsonElement files))
            {
                text = $"{files.GetInt64()} files copied. " + text;
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException) { }
        _host.Notify("Import: " + label, text);
        if (_jobs.Count == 0)
        {
            _timer?.Stop();
            _host.SetStatus(null);
        }
    }

    private void Tick()
    {
        if (_api is null || _host is null) return;
        string? line = null;
        foreach (ulong job in _jobs.ToArray())
        {
            MvImportProgress p;
            try { p = _api.Progress(job); }
            catch (MediaViewerException) { _jobs.Remove(job); continue; }
            _window?.OnProgress(job, p);
            if (p.State is MvImportJobState.Running or MvImportJobState.Paused or MvImportJobState.Queued)
            {
                double pct = p.BytesTotal == 0 ? 0 : 100.0 * p.BytesRead / p.BytesTotal;
                line = p.State == MvImportJobState.Paused
                    ? $"Import paused · {pct:0}%"
                    : $"Importing · {pct:0}%" + (p.EtaSeconds >= 0 ? $" · {Format.Eta(p.EtaSeconds)}" : "");
            }
        }
        _host.SetStatus(line);
        if (_jobs.Count == 0) _timer?.Stop();
    }

    public void Shutdown()
    {
        _timer?.Stop();
        _window?.Close();
        _window = null;
        _api = null;
        _host?.SetStatus(null);
        _host = null;
    }

    internal static T? Try<T>(Func<T> f) where T : class
    {
        try { return f(); }
        catch (MediaViewerException) { return null; }
    }
}

internal static class Format
{
    public static string Bytes(long b)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double v = b;
        int u = 0;
        while (v >= 1000 && u < units.Length - 1)
        {
            v /= 1000;
            ++u;
        }
        return u == 0 ? $"{b} B" : $"{v:0.0} {units[u]}";
    }

    public static string Eta(long seconds) => seconds switch
    {
        < 0 => "",
        < 60 => $"≈ {seconds} s",
        < 3600 => $"≈ {Math.Round(seconds / 60.0)} min",
        _ => $"≈ {seconds / 3600} h {seconds % 3600 / 60} min",
    };

    public static string Rate(double bytesPerSecond) =>
        bytesPerSecond <= 0 ? "" : $"{bytesPerSecond / 1_000_000:0} MB/s";
}
