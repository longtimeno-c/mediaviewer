// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using MediaViewer.Updater;
using Org.BouncyCastle.Crypto.Generators;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;
using Org.BouncyCastle.Security;
using Velopack;
using Velopack.Logging;
using Velopack.Sources;

// Modes:
//   (none)                                       run the tests
//   keygen <dir>                                 dev keypair: <dir>\dev.key (private hex), dev.pub (public hex)
//   manifest <releasesDir> <version> <minVersion> [blocklist,csv] [channel] [notesFile]
//                                                write <releasesDir>\mediaviewer-manifest.json from its *.nupkg;
//                                                notesFile (UTF-8, "-" for none) is the #620 "What's new"
//   sign <file> <privateKeyHexFile>              write <file>.sig (Ed25519, detached, exact bytes)
//   verify-release <releasesDir>                 verify with the production key and check payload hashes
return args.Length switch
{
    0 => Tests.Run(),
    _ when args[0] == "keygen" && args.Length == 2 => Tool.Keygen(args[1]),
    _ when args[0] == "manifest" && args.Length >= 4 => Tool.Manifest(args),
    _ when args[0] == "sign" && args.Length == 3 => Tool.Sign(args[1], args[2]),
    _ when args[0] == "verify-release" && args.Length == 2 => Tool.VerifyRelease(args[1]),
    _ => Usage(),
};

static int Usage()
{
    Console.Error.WriteLine("usage: (no args) | keygen <dir> | manifest <dir> <ver> <min> [blocklist] [channel] [notesFile] | sign <file> <keyfile> | verify-release <dir>");
    return 2;
}

internal static class Dev
{
    public static (byte[] Private, byte[] Public) NewKeyPair()
    {
        var gen = new Ed25519KeyPairGenerator();
        gen.Init(new Ed25519KeyGenerationParameters(new SecureRandom()));
        var pair = gen.GenerateKeyPair();
        return (((Ed25519PrivateKeyParameters)pair.Private).GetEncoded(),
                ((Ed25519PublicKeyParameters)pair.Public).GetEncoded());
    }

    public static byte[] Sign(byte[] data, byte[] privateKey)
    {
        var signer = new Ed25519Signer();
        signer.Init(true, new Ed25519PrivateKeyParameters(privateKey, 0));
        signer.BlockUpdate(data, 0, data.Length);
        return signer.GenerateSignature();
    }

    public static string ManifestJson(string version, string min, string[] blocklist, string channel,
        IEnumerable<(string File, string Sha, long Size, string Kind)> packages, int schema = 1,
        string? releasedAt = null, string? notes = null)
    {
        releasedAt ??= DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
        var sb = new StringBuilder();
        sb.Append("{\n  \"schema\": ").Append(schema).Append(",\n");
        sb.Append("  \"channel\": \"").Append(channel).Append("\",\n");
        sb.Append("  \"version\": \"").Append(version).Append("\",\n");
        sb.Append("  \"min_version\": \"").Append(min).Append("\",\n");
        sb.Append("  \"blocklist\": [").Append(string.Join(", ", blocklist.Select(b => $"\"{b}\""))).Append("],\n");
        sb.Append("  \"released_at\": \"").Append(releasedAt).Append("\",\n");
        // #620: JSON-escaped; readers before it ignore the member.
        if (notes is not null) sb.Append("  \"notes\": ").Append(JsonSerializer.Serialize(notes)).Append(",\n");
        sb.Append("  \"packages\": [\n");
        sb.Append(string.Join(",\n", packages.Select(p =>
            $"    {{\"file\": \"{p.File}\", \"sha256\": \"{p.Sha}\", \"size\": {p.Size}, \"kind\": \"{p.Kind}\"}}")));
        sb.Append("\n  ]\n}\n");
        return sb.ToString();
    }
}

internal static class Tool
{
    public static int Keygen(string dir)
    {
        Directory.CreateDirectory(dir);
        (byte[] priv, byte[] pub) = Dev.NewKeyPair();
        File.WriteAllText(Path.Combine(dir, "dev.key"), Convert.ToHexString(priv).ToLowerInvariant());
        File.WriteAllText(Path.Combine(dir, "dev.pub"), Convert.ToHexString(pub).ToLowerInvariant());
        Console.WriteLine(Convert.ToHexString(pub).ToLowerInvariant());
        return 0;
    }

    public static int Manifest(string[] a)
    {
        string dir = a[1], version = a[2], min = a[3];
        // "-" means no blocklist. Windows PowerShell 5.1 drops an empty string
        // argument to a native command entirely, which silently shifted the
        // channel into this slot and produced blocklist ["win"] -- a manifest
        // every client rejects, because a blocklist entry must parse as x.y.z.
        string blockArg = a.Length > 4 && a[4] != "-" ? a[4] : "";
        string[] block = blockArg.Length > 0 ? blockArg.Split(',') : Array.Empty<string>();
        string channel = a.Length > 5 ? a[5] : "win";
        // #620: the "What's new" tools/package/whats_new.py wrote; "-" or an empty file is none.
        string? notes = null;
        if (a.Length > 6 && a[6] != "-")
        {
            notes = File.ReadAllText(a[6], Encoding.UTF8).Replace("\r\n", "\n").TrimEnd('\n');
            if (notes.Length == 0) notes = null;
            else if (!ManifestNotes.IsValid(notes))
            {
                Console.Error.WriteLine("What's new does not meet the manifest rule (whats_new.py)");
                return 1;
            }
        }
        var packages = Directory.EnumerateFiles(dir, $"*-{version}-*.nupkg").Select(f =>
        {
            using FileStream fs = File.OpenRead(f);
            string sha = Convert.ToHexString(SHA256.HashData(fs)).ToLowerInvariant();
            string kind = f.EndsWith("-delta.nupkg", StringComparison.OrdinalIgnoreCase) ? "delta" : "full";
            return (Path.GetFileName(f), sha, new FileInfo(f).Length, kind);
        }).ToList();
        if (packages.Count == 0) { Console.Error.WriteLine("no packages for " + version); return 1; }
        File.WriteAllText(Path.Combine(dir, UpdateKeys.ManifestAssetName),
            Dev.ManifestJson(version, min, block, channel, packages, notes: notes));
        return 0;
    }

    public static int Sign(string file, string keyFile)
    {
        byte[] key = Convert.FromHexString(File.ReadAllText(keyFile).Trim());
        File.WriteAllBytes(file + ".sig", Dev.Sign(File.ReadAllBytes(file), key));
        return 0;
    }

    public static int VerifyRelease(string dir)
    {
        byte[] manifest = File.ReadAllBytes(Path.Combine(dir, UpdateKeys.ManifestAssetName));
        byte[] signature = File.ReadAllBytes(Path.Combine(dir, UpdateKeys.SignatureAssetName));
        var decision = ManifestVerifier.Evaluate(manifest, signature, UpdateKeys.ProductionPublicKey,
            UpdateKeys.Channel, new ReleaseVersion(0, 0, 0));
        if (!decision.Trusted)
        {
            Console.Error.WriteLine("Release manifest rejected: " + decision.Rejection);
            return 1;
        }
        foreach (var package in decision.Manifest!.Packages)
        {
            if (!ManifestVerifier.VerifyPackageFile(Path.Combine(dir, package.File), package))
            {
                Console.Error.WriteLine("Release package hash/size mismatch: " + package.File);
                return 1;
            }
        }
        Console.WriteLine("Release manifest and packages verified with the pinned production key.");
        return 0;
    }
}

internal static class Tests
{
    private static int _failures;
    private static int _passes;

    private static void Check(bool ok, string name)
    {
        if (ok) { _passes++; Console.WriteLine("PASS " + name); }
        else { _failures++; Console.WriteLine("FAIL " + name); }
    }

    private static readonly ReleaseVersion V010 = new(0, 1, 0);
    private static readonly ReleaseVersion V011 = new(0, 1, 1);

    private static readonly (string, string, long, string)[] Pkg =
    {
        ("MediaViewer-0.1.1-full.nupkg", new string('a', 64), 1234, "full"),
        ("MediaViewer-0.1.1-delta.nupkg", new string('b', 64), 99, "delta"),
    };

    public static int Run()
    {
        (byte[] priv, byte[] pub) = Dev.NewKeyPair();
        (_, byte[] otherPub) = Dev.NewKeyPair();

        byte[] Json(string version = "0.1.1", string min = "0.1.0", string[]? block = null, string channel = "win", int schema = 1) =>
            Encoding.UTF8.GetBytes(Dev.ManifestJson(version, min, block ?? Array.Empty<string>(), channel, Pkg, schema));

        ManifestDecision Eval(byte[] m, byte[]? sig, byte[]? key = null, ReleaseVersion? running = null,
            IReadOnlyCollection<ReleaseVersion>? failed = null) =>
            ManifestVerifier.Evaluate(m, sig ?? Array.Empty<byte>(), key ?? pub, "win", running ?? V010, failed);

        // --- signature ---
        byte[] good = Json();
        byte[] goodSig = Dev.Sign(good, priv);
        ManifestDecision ok = Eval(good, goodSig);
        Check(ok.Trusted && ok.UpdateAvailable && ok.Urgency == UpdateUrgency.Normal && ok.Manifest!.Version == V011,
            "valid signature passes and offers 0.1.1");
        Check(ok.Manifest!.Packages.Count == 2 && ok.Manifest.FindPackage("mediaviewer-0.1.1-FULL.nupkg") is not null,
            "packages parsed");

        for (int i = 0; i < good.Length; i += Math.Max(1, good.Length / 7))
        {
            byte[] t = (byte[])good.Clone();
            t[i] ^= 0x01;
            Check(Eval(t, goodSig).Rejection == ManifestRejection.BadSignature, $"tampered byte {i} rejects");
        }
        byte[] appended = good.Concat(new byte[] { (byte)' ' }).ToArray();
        Check(Eval(appended, goodSig).Rejection == ManifestRejection.BadSignature, "appended whitespace rejects");
        byte[] badSig = (byte[])goodSig.Clone();
        badSig[10] ^= 0x80;
        Check(Eval(good, badSig).Rejection == ManifestRejection.BadSignature, "tampered signature rejects");
        Check(Eval(good, goodSig, otherPub).Rejection == ManifestRejection.BadSignature, "wrong key rejects");
        Check(Eval(good, null).Rejection == ManifestRejection.MissingSignature, "missing signature rejects");
        Check(Eval(good, goodSig.Take(63).ToArray()).Rejection == ManifestRejection.BadSignature, "short signature rejects");
        Check(Eval(good, goodSig, new byte[32]).Rejection == ManifestRejection.KeyNotConfigured, "all-zero key fails closed");
        Check(Eval(good, goodSig, UpdateKeys.ProductionPublicKey).Rejection is ManifestRejection.KeyNotConfigured
                                                                          or ManifestRejection.BadSignature,
            "shipped key never accepts a dev-signed manifest");

        // --- content, all correctly signed ---
        ManifestDecision Signed(byte[] m, ReleaseVersion? running = null, IReadOnlyCollection<ReleaseVersion>? failed = null) =>
            Eval(m, Dev.Sign(m, priv), null, running, failed);

        Check(Signed(Json(channel: "mac")).Rejection == ManifestRejection.ChannelMismatch, "channel mismatch rejects");
        Check(Signed(Json(channel: "WIN")).Rejection == ManifestRejection.ChannelMismatch, "channel is case-sensitive");
        Check(Signed(Json(version: "0.0.9")).Rejection == ManifestRejection.Rollback, "downgrade rejects");
        Check(Signed(Json(), new ReleaseVersion(0, 2, 0)).Rejection == ManifestRejection.Rollback, "older than running rejects");
        ManifestDecision same = Signed(Json(), V011);
        Check(same.Trusted && !same.UpdateAvailable, "same version: trusted, nothing to do");
        Check(Signed(Json(block: new[] { "0.1.1" })).Rejection == ManifestRejection.TargetBlocklisted, "blocklisted target rejects");
        ManifestDecision pulled = Signed(Json(block: new[] { "0.1.0" }));
        Check(pulled.Trusted && pulled.UpdateAvailable && pulled.Urgency == UpdateUrgency.CurrentBlocklisted,
            "blocklisted current: offers newest good version, flagged");
        ManifestDecision below = Signed(Json(min: "0.1.1"));
        Check(below.Trusted && below.Urgency == UpdateUrgency.BelowMinimum, "below min_version flagged");
        Check(Signed(Json(), failed: new[] { V011 }).Rejection == ManifestRejection.TargetFailedLocally,
            "version rolled back on this machine rejects");
        Check(Signed(Json(schema: 3)).Rejection == ManifestRejection.UnsupportedSchema, "schema 3 rejects");
        Check(Signed(Json(schema: 0)).Rejection == ManifestRejection.UnsupportedSchema, "schema 0 rejects");

        // --- #620: "What's new" ---
        NotesTests(priv, pub);
        Check(Signed(Encoding.UTF8.GetBytes("{\"schema\":1}")).Rejection == ManifestRejection.Malformed, "signed but incomplete rejects");
        Check(Signed(Encoding.UTF8.GetBytes("not json")).Rejection == ManifestRejection.Malformed, "signed garbage rejects");
        Check(Signed(Encoding.UTF8.GetBytes(Dev.ManifestJson("0.1.1", "0.1.0", Array.Empty<string>(), "win",
            new[] { ("..\\evil.nupkg", new string('a', 64), 1L, "full") }))).Rejection == ManifestRejection.Malformed,
            "path in package name rejects");
        Check(Signed(Encoding.UTF8.GetBytes(Dev.ManifestJson("0.1.1", "0.1.0", Array.Empty<string>(), "win",
            new[] { ("x.nupkg", "abc", 1L, "full") }))).Rejection == ManifestRejection.Malformed, "short sha256 rejects");
        Check(Signed(Encoding.UTF8.GetBytes(Dev.ManifestJson("0.1.1", "0.1.0", Array.Empty<string>(), "win",
            new[] { ("x.nupkg", new string('a', 64), 1L, "delta") }))).Rejection == ManifestRejection.NoFullPackage,
            "delta-only manifest rejects");
        Check(Signed(Json(version: "0.1.1-beta")).Rejection == ManifestRejection.Malformed, "prerelease version rejects");

        // --- package hash ---
        byte[] payload = RandomNumberGenerator.GetBytes(4096);
        string sha = Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
        var entry = new ManifestPackage("p.nupkg", sha, payload.Length, "full");
        Check(ManifestVerifier.VerifyPackage(new MemoryStream(payload), entry), "matching package passes");
        byte[] flipped = (byte[])payload.Clone();
        flipped[2000] ^= 1;
        Check(!ManifestVerifier.VerifyPackage(new MemoryStream(flipped), entry), "package sha256 mismatch rejects");
        Check(!ManifestVerifier.VerifyPackage(new MemoryStream(payload.Take(4095).ToArray()), entry), "truncated package rejects");
        Check(!ManifestVerifier.VerifyPackage(new MemoryStream(payload.Concat(new byte[] { 0 }).ToArray()), entry), "oversized package rejects");

        // --- the Velopack source wrapper ---
        SourceTests(priv, pub).GetAwaiter().GetResult();

        // --- versions ---
        Check(ReleaseVersion.TryParse("1.10.0", out var a) && ReleaseVersion.TryParse("1.9.9", out var b) && a > b, "numeric compare");
        Check(!ReleaseVersion.TryParse("01.0.0", out _) && !ReleaseVersion.TryParse("1.0", out _), "strict parse");

        // --- the preview channel's release pick ---
        PreviewChannelTests().GetAwaiter().GetResult();

        Console.WriteLine($"{_passes} passed, {_failures} failed");
        return _failures == 0 ? 0 : 1;
    }

    private static byte[] WithNotes(string? notes, int schema = 2) =>
        Encoding.UTF8.GetBytes(Dev.ManifestJson("0.1.1", "0.1.0", Array.Empty<string>(), "win", Pkg, schema, notes: notes));

    private static byte[] Json1() => WithNotes(null, schema: 1);

    private static byte[] Inject(string member) =>
        Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(Json1()).Replace("\"packages\"", member + ",\n  \"packages\""));

    private static void NotesTests(byte[] priv, byte[] pub)
    {
        ManifestDecision Signed(byte[] m) =>
            ManifestVerifier.Evaluate(m, Dev.Sign(m, priv), pub, "win", V010);

        // Verify: a schema 1 manifest still updates, with no notes.
        ManifestDecision v1 = Signed(Json1());
        Check(v1.Trusted && v1.UpdateAvailable && v1.Manifest!.Schema == 1 && v1.Manifest.Notes is null,
            "notes: schema 1 without notes still updates");
        ManifestDecision v2 = Signed(WithNotes(null));
        Check(v2.Trusted && v2.UpdateAvailable && v2.Manifest!.Notes is null, "notes: schema 2 without notes updates");

        const string text = "Faster RAW previews\nHEIC \"quoted\" \\ café — 日本語 <b>\nand 3 more changes";
        ManifestDecision with2 = Signed(WithNotes(text));
        Check(with2.Trusted && with2.UpdateAvailable && with2.Manifest!.Notes == text,
            "notes: schema 2 carries them, escapes round-trip");
        ManifestDecision with1 = Signed(WithNotes(text, schema: 1));
        Check(with1.Trusted && with1.Manifest!.Notes == text, "notes: schema 1 may carry them (older readers ignore them)");

        // Verify: a manifest whose notes were altered fails the signature check.
        byte[] good = WithNotes("Faster RAW previews");
        byte[] sig = Dev.Sign(good, priv);
        Check(Signed(good).Trusted, "notes: the unaltered manifest passes");
        byte[] altered = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(good).Replace("Faster", "Slower"));
        Check(altered.Length == good.Length &&
              ManifestVerifier.Evaluate(altered, sig, pub, "win", V010).Rejection == ManifestRejection.BadSignature,
            "notes: altered notes fail the signature check");
        byte[] added = Inject("\"notes\": \"Free upgrade\"");
        Check(ManifestVerifier.Evaluate(added, Dev.Sign(Json1(), priv), pub, "win", V010).Rejection == ManifestRejection.BadSignature,
            "notes: notes added to a signed manifest fail the signature check");

        Check(Signed(WithNotes(new string('x', 400))).Trusted, "notes: 400 bytes pass");
        Check(Signed(WithNotes("One\nTwo\nThree")).Trusted, "notes: three lines pass");
        Check(Signed(WithNotes("\U0001F600 emoji")).Trusted, "notes: a surrogate pair passes");
        string[] bad =
        {
            "", new string('x', 401), new string('é', 201), "a\nb\nc\nd", "a\n\nb", " lead", "trail ", "a\r\nb",
            "tab\there", "bell\u0007", "del\u007f", "c1\u0085", "rtl\u202eevil", "iso\u2066x", "zero\u200bwidth",
            "bom\ufeff", "line\u2028sep", "https://example.com", "see WWW.example.com", "mailto:me", "ftp://x",
            "lone \ud800 surrogate",
        };
        for (int i = 0; i < bad.Length; i++)
        {
            Check(!ManifestNotes.IsValid(bad[i]), $"notes rule rejects case {i}");
            // A lone surrogate cannot be written as UTF-8 text; the escaped form is below.
            if (i == bad.Length - 1) continue;
            Check(Signed(WithNotes(bad[i])).Rejection == ManifestRejection.Malformed,
                $"notes: signed manifest with case {i} rejects");
        }
        Check(Signed(Inject("\"notes\": 7")).Rejection == ManifestRejection.Malformed, "notes: a non-string rejects");
        Check(Signed(Inject("\"notes\": null")).Rejection == ManifestRejection.Malformed, "notes: null rejects");
        Check(Signed(Inject("\"notes\": \"x\\ud800\"")).Rejection == ManifestRejection.Malformed,
            "notes: an escaped lone surrogate rejects");
    }


    private static string ReleaseJson(string tag, bool draft, bool prerelease, bool withManifest, string? host = null)
    {
        string dl = (host ?? UpdateKeys.GithubRepoUrl) + "/releases/download/" + tag + "/";
        string assets = withManifest
            ? $"{{\"name\":\"{UpdateKeys.ManifestAssetName}\",\"browser_download_url\":\"{dl}{UpdateKeys.ManifestAssetName}\"}}," +
              $"{{\"name\":\"{UpdateKeys.SignatureAssetName}\",\"browser_download_url\":\"{dl}{UpdateKeys.SignatureAssetName}\"}}"
            : $"{{\"name\":\"MediaViewer-Setup.exe\",\"browser_download_url\":\"{dl}MediaViewer-Setup.exe\"}}";
        return $"{{\"tag_name\":\"{tag}\",\"draft\":{(draft ? "true" : "false")},\"prerelease\":{(prerelease ? "true" : "false")},\"assets\":[{assets}]}}";
    }

    private static string? PickedTag(params string[] releases)
    {
        var picked = GithubManifestFetcher.PickNewestRelease(Encoding.UTF8.GetBytes("[" + string.Join(",", releases) + "]"));
        return picked is null ? null : picked.Value.Manifest.Split('/')[^2];
    }

    private static async Task PreviewChannelTests()
    {
        Check(PickedTag(ReleaseJson("v0.1.4", false, false, true), ReleaseJson("v0.1.5", false, true, true)) == "v0.1.5",
            "preview: a newer prerelease wins");
        Check(PickedTag(ReleaseJson("v0.1.6", false, false, true), ReleaseJson("v0.1.5", false, true, true)) == "v0.1.6",
            "preview: a newer stable release wins");
        Check(PickedTag(ReleaseJson("v0.1.9", true, true, true), ReleaseJson("v0.1.4", false, false, true)) == "v0.1.4",
            "preview: drafts are ignored");
        Check(PickedTag(ReleaseJson("v0.1.9", false, true, false), ReleaseJson("v0.1.4", false, false, true)) == "v0.1.4",
            "preview: a release without a manifest (old unsigned preview) is skipped");
        Check(PickedTag(ReleaseJson("v0.1.5.preview.123", false, true, true), ReleaseJson("v0.1.4", false, false, true)) == "v0.1.5.preview.123",
            "preview: legacy preview tags parse");
        Check(PickedTag(ReleaseJson("v0.1.9", false, true, true, "https://evil.example"), ReleaseJson("v0.1.4", false, false, true)) == "v0.1.4",
            "preview: asset URLs outside this repository are ignored");
        Check(PickedTag(ReleaseJson("nightly", false, true, true)) is null, "preview: unversioned tags are ignored");
        Check(GithubManifestFetcher.PickNewestRelease(Encoding.UTF8.GetBytes("{\"message\":\"rate limited\"}")) is null,
            "preview: a non-list response is no update");

        bool preview = false;
        var stable = new FakeInner(Array.Empty<VelopackAsset>(), Array.Empty<byte>());
        var pre = new FakeInner(Array.Empty<VelopackAsset>(), Array.Empty<byte>());
        var source = new ChannelSource(() => preview, stable, pre);
        await source.GetReleaseFeed(NullVelopackLogger.Instance, null, UpdateKeys.Channel);
        preview = true;
        await source.GetReleaseFeed(NullVelopackLogger.Instance, null, UpdateKeys.Channel);
        Check(stable.FeedCalls == 1 && pre.FeedCalls == 1, "channel source follows the setting per request");
    }

    private sealed class FakeFetcher(byte[]? m, byte[]? s) : IManifestFetcher
    {
        public Task<(byte[]? Manifest, byte[]? Signature)> FetchAsync(CancellationToken cancel) => Task.FromResult((m, s));
    }

    private sealed class FakeInner(VelopackAsset[] assets, byte[] download) : IUpdateSource
    {
        public int FeedCalls;

        public Task<VelopackAssetFeed> GetReleaseFeed(IVelopackLogger logger, string? appId, string channel,
            Guid? stagingId = null, VelopackAsset? latestLocalRelease = null)
        {
            FeedCalls++;
            return Task.FromResult(new VelopackAssetFeed { Assets = assets });
        }

        public Task DownloadReleaseEntry(IVelopackLogger logger, VelopackAsset releaseEntry, string localFile,
            Action<int> progress, CancellationToken cancelToken = default)
        {
            File.WriteAllBytes(localFile, download);
            return Task.CompletedTask;
        }
    }

    private static VelopackAsset Asset(string file, string version, long size, VelopackAssetType type) => new()
    {
        PackageId = "MediaViewer",
        FileName = file,
        Version = SemanticVersion.Parse(version),
        Size = size,
        Type = type,
        SHA1 = "",
        SHA256 = "",
    };

    private static async Task SourceTests(byte[] priv, byte[] pub)
    {
        byte[] pkg = RandomNumberGenerator.GetBytes(2048);
        string sha = Convert.ToHexString(SHA256.HashData(pkg)).ToLowerInvariant();
        byte[] manifest = Encoding.UTF8.GetBytes(Dev.ManifestJson("0.1.1", "0.1.0", Array.Empty<string>(), "win",
            new[] { ("MediaViewer-0.1.1-full.nupkg", sha, (long)pkg.Length, "full") }));
        byte[] sig = Dev.Sign(manifest, priv);
        VelopackAsset[] feed =
        {
            Asset("MediaViewer-0.1.1-full.nupkg", "0.1.1", pkg.Length, VelopackAssetType.Full),
            Asset("MediaViewer-0.1.1-full.nupkg", "0.1.1", pkg.Length + 1, VelopackAssetType.Full), // size lie
            Asset("MediaViewer-9.9.9-full.nupkg", "9.9.9", 10, VelopackAssetType.Full),             // unsigned version
            Asset("Evil-0.1.1-full.nupkg", "0.1.1", pkg.Length, VelopackAssetType.Full),            // unlisted file
        };
        var log = NullVelopackLogger.Instance;
        string tmp = Path.Combine(Path.GetTempPath(), "mv-updater-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tmp);
        try
        {
            // Bad signature: inner feed is never requested.
            byte[] badSig = (byte[])sig.Clone();
            badSig[0] ^= 1;
            var inner0 = new FakeInner(feed, pkg);
            var s0 = new SignedManifestSource(inner0, new FakeFetcher(manifest, badSig), pub, new ReleaseVersion(0, 1, 0),
                Array.Empty<ReleaseVersion>());
            VelopackAssetFeed f0 = await s0.GetReleaseFeed(log, "MediaViewer", "win");
            Check(f0.Assets.Length == 0 && inner0.FeedCalls == 0, "bad signature: empty feed, Velopack index never read");
            bool threw0 = false;
            try { await s0.DownloadReleaseEntry(log, feed[0], Path.Combine(tmp, "x"), _ => { }); }
            catch (InvalidOperationException) { threw0 = true; }
            Check(threw0 && !File.Exists(Path.Combine(tmp, "x")), "no download without a verified manifest");

            var s1 = new SignedManifestSource(new FakeInner(feed, pkg), new FakeFetcher(manifest, null), pub,
                new ReleaseVersion(0, 1, 0), Array.Empty<ReleaseVersion>());
            Check((await s1.GetReleaseFeed(log, "MediaViewer", "win")).Assets.Length == 0, "missing signature: empty feed");

            // Good: only the one signed asset survives.
            var s2 = new SignedManifestSource(new FakeInner(feed, pkg), new FakeFetcher(manifest, sig), pub,
                new ReleaseVersion(0, 1, 0), Array.Empty<ReleaseVersion>());
            VelopackAssetFeed f2 = await s2.GetReleaseFeed(log, "MediaViewer", "win");
            Check(f2.Assets.Length == 1 && f2.Assets[0].Size == pkg.Length && f2.Assets[0].Version.ToString() == "0.1.1",
                "feed filtered to signed name, version and size");
            string okFile = Path.Combine(tmp, "ok.nupkg");
            await s2.DownloadReleaseEntry(log, f2.Assets[0], okFile, _ => { });
            Check(File.Exists(okFile), "matching download kept");

            // Same manifest, tampered bytes served.
            byte[] evil = (byte[])pkg.Clone();
            evil[100] ^= 1;
            var s3 = new SignedManifestSource(new FakeInner(feed, evil), new FakeFetcher(manifest, sig), pub,
                new ReleaseVersion(0, 1, 0), Array.Empty<ReleaseVersion>());
            VelopackAssetFeed f3 = await s3.GetReleaseFeed(log, "MediaViewer", "win");
            string badFile = Path.Combine(tmp, "bad.nupkg");
            bool threw = false;
            try { await s3.DownloadReleaseEntry(log, f3.Assets[0], badFile, _ => { }); }
            catch (InvalidDataException) { threw = true; }
            Check(threw && !File.Exists(badFile), "download with sha256 mismatch throws and is deleted");

            // Downgrade manifest (running 0.2.0): empty feed.
            var s4 = new SignedManifestSource(new FakeInner(feed, pkg), new FakeFetcher(manifest, sig), pub,
                new ReleaseVersion(0, 2, 0), Array.Empty<ReleaseVersion>());
            Check((await s4.GetReleaseFeed(log, "MediaViewer", "win")).Assets.Length == 0 &&
                  s4.LastDecision!.Rejection == ManifestRejection.Rollback, "downgrade: empty feed");
        }
        finally
        {
            Directory.Delete(tmp, true);
        }
    }
}
