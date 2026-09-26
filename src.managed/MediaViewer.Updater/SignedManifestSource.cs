// SPDX-License-Identifier: GPL-2.0-or-later
using System.Net;
using System.Text.Json;
using Velopack;
using Velopack.Logging;
using Velopack.Sources;

namespace MediaViewer.Updater;

/// <summary>Fetches the manifest and its detached signature. Either may be null (missing).</summary>
public interface IManifestFetcher
{
    Task<(byte[]? Manifest, byte[]? Signature)> FetchAsync(CancellationToken cancel);
}

/// <summary>
/// Release assets of the newest GitHub release, over HTTPS with default
/// certificate validation. The request is a plain GET of two fixed asset URLs:
/// no query string, no cookies, nothing about the user's files (rule 6).
/// </summary>
/// <remarks>
/// On the preview channel (plan/12, 2026-09-26) the newest release may be a
/// prerelease, which /releases/latest never serves, so the fetcher first reads
/// the public release listing and takes the highest-versioned release that
/// carries a manifest. That only chooses which signed manifest is evaluated:
/// the signature, the channel and "newer than running" are checked exactly as
/// on the stable channel.
/// </remarks>
public sealed class GithubManifestFetcher(Func<bool>? preview = null) : IManifestFetcher
{
    private static readonly HttpClient Http = CreateClient();
    private static readonly string DownloadPrefix = UpdateKeys.GithubRepoUrl + "/releases/download/";

    private static HttpClient CreateClient()
    {
        var client = new HttpClient(new HttpClientHandler { UseCookies = false }) { Timeout = TimeSpan.FromSeconds(60) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MediaViewer-Updater");
        return client;
    }

    public async Task<(byte[]? Manifest, byte[]? Signature)> FetchAsync(CancellationToken cancel)
    {
        string baseUrl = UpdateKeys.GithubRepoUrl + "/releases/latest/download/";
        string manifestUrl = baseUrl + UpdateKeys.ManifestAssetName;
        string sigUrl = baseUrl + UpdateKeys.SignatureAssetName;
        if (preview?.Invoke() == true)
        {
            byte[]? listing = await Get(UpdateKeys.GithubReleasesApiUrl, cancel).ConfigureAwait(false);
            (string Manifest, string Signature)? newest = listing is null ? null : PickNewestRelease(listing);
            if (newest is null) return (null, null);
            (manifestUrl, sigUrl) = newest.Value;
        }
        byte[]? manifest = await Get(manifestUrl, cancel).ConfigureAwait(false);
        if (manifest is null) return (null, null);
        byte[]? sig = await Get(sigUrl, cancel).ConfigureAwait(false);
        return (manifest, sig);
    }

    /// <summary>
    /// From a GitHub release listing: the manifest and signature download URLs
    /// of the highest-versioned published release (stable or prerelease) that
    /// has both. Tags are v1.2.3, or v1.2.3.preview.N from before previews were
    /// signed. URLs outside this repository's release downloads are ignored.
    /// </summary>
    public static (string Manifest, string Signature)? PickNewestRelease(ReadOnlySpan<byte> listingJson)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(listingJson.ToArray());
            if (doc.RootElement.ValueKind != JsonValueKind.Array) return null;
            (string, string)? best = null;
            ReleaseVersion bestVersion = default;
            foreach (JsonElement r in doc.RootElement.EnumerateArray())
            {
                if (r.ValueKind != JsonValueKind.Object) continue;
                if (r.TryGetProperty("draft", out JsonElement draft) && draft.ValueKind == JsonValueKind.True) continue;
                if (!TagVersion(r, out ReleaseVersion version) || (best is not null && version <= bestVersion)) continue;
                string? m = AssetUrl(r, UpdateKeys.ManifestAssetName);
                string? s = AssetUrl(r, UpdateKeys.SignatureAssetName);
                if (m is null || s is null) continue;
                best = (m, s);
                bestVersion = version;
            }
            return best;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static bool TagVersion(JsonElement release, out ReleaseVersion version)
    {
        version = default;
        if (!release.TryGetProperty("tag_name", out JsonElement t) || t.ValueKind != JsonValueKind.String) return false;
        string tag = t.GetString() ?? "";
        if (!tag.StartsWith('v')) return false;
        tag = tag[1..];
        int suffix = tag.IndexOf(".preview.", StringComparison.Ordinal);
        if (suffix >= 0) tag = tag[..suffix];
        return ReleaseVersion.TryParse(tag, out version);
    }

    private static string? AssetUrl(JsonElement release, string name)
    {
        if (!release.TryGetProperty("assets", out JsonElement assets) || assets.ValueKind != JsonValueKind.Array) return null;
        foreach (JsonElement a in assets.EnumerateArray())
        {
            if (a.ValueKind != JsonValueKind.Object ||
                !a.TryGetProperty("name", out JsonElement n) || n.ValueKind != JsonValueKind.String ||
                n.GetString() != name) continue;
            if (!a.TryGetProperty("browser_download_url", out JsonElement u) || u.ValueKind != JsonValueKind.String) return null;
            string? url = u.GetString();
            return url is not null && url.StartsWith(DownloadPrefix, StringComparison.Ordinal) ? url : null;
        }
        return null;
    }

    private static async Task<byte[]?> Get(string url, CancellationToken cancel)
    {
        using HttpResponseMessage r = await Http.GetAsync(url, cancel).ConfigureAwait(false);
        if (r.StatusCode == HttpStatusCode.NotFound) return null;
        r.EnsureSuccessStatusCode();
        return await r.Content.ReadAsByteArrayAsync(cancel).ConfigureAwait(false);
    }
}

/// <summary>
/// The Velopack feed for the current update channel: stable releases only, or
/// prereleases as well on the preview channel. Chosen per request, so a channel
/// change takes effect at the next check. The signed manifest still decides the
/// one version that may be installed (<see cref="SignedManifestSource"/>).
/// </summary>
public sealed class ChannelSource(Func<bool> preview, IUpdateSource stable, IUpdateSource withPrereleases) : IUpdateSource
{
    private IUpdateSource Current => preview() ? withPrereleases : stable;

    public Task<VelopackAssetFeed> GetReleaseFeed(IVelopackLogger logger, string? appId, string channel,
        Guid? stagingId = null, VelopackAsset? latestLocalRelease = null) =>
        Current.GetReleaseFeed(logger, appId, channel, stagingId, latestLocalRelease);

    // Either GitHub source downloads any GitHub asset: the entry carries its release.
    public Task DownloadReleaseEntry(IVelopackLogger logger, VelopackAsset releaseEntry, string localFile,
        Action<int> progress, CancellationToken cancelToken = default) =>
        Current.DownloadReleaseEntry(logger, releaseEntry, localFile, progress, cancelToken);
}

/// <summary>A local directory feed (tests and the dev-build e2e). Same signature rules.</summary>
public sealed class DirectoryManifestFetcher(string directory) : IManifestFetcher
{
    public Task<(byte[]? Manifest, byte[]? Signature)> FetchAsync(CancellationToken cancel)
    {
        string m = Path.Combine(directory, UpdateKeys.ManifestAssetName);
        string s = Path.Combine(directory, UpdateKeys.SignatureAssetName);
        byte[]? manifest = File.Exists(m) ? File.ReadAllBytes(m) : null;
        byte[]? sig = File.Exists(s) ? File.ReadAllBytes(s) : null;
        return Task.FromResult((manifest, sig));
    }
}

/// <summary>
/// Wraps a Velopack source so Velopack only ever sees packages the signed
/// manifest names. The manifest is verified before the inner feed is even
/// requested; every downloaded package is checked against its signed SHA-256
/// and size before Velopack moves it into packages\.
/// </summary>
public sealed class SignedManifestSource : IUpdateSource
{
    private readonly IUpdateSource _inner;
    private readonly IManifestFetcher _fetcher;
    private readonly byte[] _publicKey;
    private readonly ReleaseVersion _running;
    private readonly IReadOnlyCollection<ReleaseVersion> _failedLocally;

    public SignedManifestSource(IUpdateSource inner, IManifestFetcher fetcher, byte[] publicKey,
        ReleaseVersion running, IReadOnlyCollection<ReleaseVersion> failedLocally)
    {
        _inner = inner;
        _fetcher = fetcher;
        _publicKey = publicKey;
        _running = running;
        _failedLocally = failedLocally;
    }

    /// <summary>The decision from the most recent feed request.</summary>
    public ManifestDecision? LastDecision { get; private set; }

    public async Task<VelopackAssetFeed> GetReleaseFeed(IVelopackLogger logger, string? appId, string channel,
        Guid? stagingId = null, VelopackAsset? latestLocalRelease = null)
    {
        (byte[]? manifest, byte[]? sig) = await _fetcher.FetchAsync(CancellationToken.None).ConfigureAwait(false);
        ManifestDecision decision = manifest is null
            ? ManifestDecision.Reject(ManifestRejection.Malformed)
            : ManifestVerifier.Evaluate(manifest, sig ?? Array.Empty<byte>(), _publicKey, UpdateKeys.Channel,
                _running, _failedLocally);
        LastDecision = decision;
        if (!decision.Trusted || !decision.UpdateAvailable)
        {
            logger.Info($"updater: manifest not actionable ({decision.Rejection}, available={decision.UpdateAvailable})");
            return new VelopackAssetFeed();
        }

        // Only now does the unsigned Velopack index get read.
        VelopackAssetFeed feed = await _inner.GetReleaseFeed(logger, appId, channel, null, latestLocalRelease)
            .ConfigureAwait(false);
        return new VelopackAssetFeed { Assets = FilterAssets(feed.Assets, decision.Manifest!) };
    }

    /// <summary>Keeps assets for the signed version whose name, kind and size match an entry.</summary>
    public static VelopackAsset[] FilterAssets(IEnumerable<VelopackAsset> assets, UpdateManifest manifest)
    {
        string version = manifest.Version.ToString();
        return assets.Where(a =>
        {
            ManifestPackage? p = a.FileName is null ? null : manifest.FindPackage(a.FileName);
            if (p is null || a.Version is null) return false;
            if (!string.Equals(a.Version.ToString(), version, StringComparison.Ordinal)) return false;
            string kind = a.Type == VelopackAssetType.Full ? "full" : a.Type == VelopackAssetType.Delta ? "delta" : "";
            return kind == p.Kind && a.Size == p.Size;
        }).ToArray();
    }

    public async Task DownloadReleaseEntry(IVelopackLogger logger, VelopackAsset releaseEntry, string localFile,
        Action<int> progress, CancellationToken cancelToken = default)
    {
        ManifestPackage expected = LastDecision?.Trusted == true
            ? LastDecision.Manifest!.FindPackage(releaseEntry.FileName)
              ?? throw new InvalidOperationException("updater: package not in the signed manifest")
            : throw new InvalidOperationException("updater: no verified manifest for this download");

        await _inner.DownloadReleaseEntry(logger, releaseEntry, localFile, progress, cancelToken).ConfigureAwait(false);
        if (!ManifestVerifier.VerifyPackageFile(localFile, expected))
        {
            try { File.Delete(localFile); } catch (IOException) { }
            throw new InvalidDataException("updater: package SHA-256 or size does not match the signed manifest");
        }
    }
}
