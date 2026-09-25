// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Security.Cryptography;
using System.Text;
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
//   manifest <releasesDir> <version> <minVersion> [blocklist,csv] [channel]
//                                                write <releasesDir>\mediaviewer-manifest.json from its *.nupkg
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
    Console.Error.WriteLine("usage: (no args) | keygen <dir> | manifest <dir> <ver> <min> [blocklist] [channel] | sign <file> <keyfile> | verify-release <dir>");
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
        string? releasedAt = null)
    {
        releasedAt ??= DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
        var sb = new StringBuilder();
        sb.Append("{\n  \"schema\": ").Append(schema).Append(",\n");
        sb.Append("  \"channel\": \"").Append(channel).Append("\",\n");
        sb.Append("  \"version\": \"").Append(version).Append("\",\n");
        sb.Append("  \"min_version\": \"").Append(min).Append("\",\n");
        sb.Append("  \"blocklist\": [").Append(string.Join(", ", blocklist.Select(b => $"\"{b}\""))).Append("],\n");
        sb.Append("  \"released_at\": \"").Append(releasedAt).Append("\",\n");
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
        var packages = Directory.EnumerateFiles(dir, $"*-{version}-*.nupkg").Select(f =>
        {
            using FileStream fs = File.OpenRead(f);
            string sha = Convert.ToHexString(SHA256.HashData(fs)).ToLowerInvariant();
            string kind = f.EndsWith("-delta.nupkg", StringComparison.OrdinalIgnoreCase) ? "delta" : "full";
            return (Path.GetFileName(f), sha, new FileInfo(f).Length, kind);
        }).ToList();
        if (packages.Count == 0) { Console.Error.WriteLine("no packages for " + version); return 1; }
        File.WriteAllText(Path.Combine(dir, UpdateKeys.ManifestAssetName),
            Dev.ManifestJson(version, min, block, channel, packages));
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
        Check(Signed(Json(schema: 2)).Rejection == ManifestRejection.UnsupportedSchema, "schema 2 rejects");
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

        Console.WriteLine($"{_passes} passed, {_failures} failed");
        return _failures == 0 ? 0 : 1;
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
