// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;

namespace MediaViewer.Updater;

/// <summary>A strict x.y.z release version. Prerelease tags are not a v1 channel.</summary>
public readonly record struct ReleaseVersion(int Major, int Minor, int Patch) : IComparable<ReleaseVersion>
{
    public static bool TryParse(string? text, out ReleaseVersion version)
    {
        version = default;
        if (string.IsNullOrEmpty(text)) return false;
        string[] parts = text.Split('.');
        if (parts.Length != 3) return false;
        var n = new int[3];
        for (int i = 0; i < 3; i++)
        {
            string p = parts[i];
            if (p.Length == 0 || p.Length > 9 || (p.Length > 1 && p[0] == '0')) return false;
            foreach (char c in p)
            {
                if (c < '0' || c > '9') return false;
            }
            n[i] = int.Parse(p, NumberStyles.None, CultureInfo.InvariantCulture);
        }
        version = new ReleaseVersion(n[0], n[1], n[2]);
        return true;
    }

    public int CompareTo(ReleaseVersion other)
    {
        int c = Major.CompareTo(other.Major);
        if (c != 0) return c;
        c = Minor.CompareTo(other.Minor);
        return c != 0 ? c : Patch.CompareTo(other.Patch);
    }

    public static bool operator <(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) < 0;
    public static bool operator >(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) > 0;
    public static bool operator <=(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) <= 0;
    public static bool operator >=(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) >= 0;

    public override string ToString() => $"{Major}.{Minor}.{Patch}";
}

public sealed record ManifestPackage(string File, string Sha256, long Size, string Kind);

/// <summary>The signed update manifest (schema 1). Only ever built from verified bytes.</summary>
public sealed record UpdateManifest(
    int Schema,
    string Channel,
    ReleaseVersion Version,
    ReleaseVersion MinVersion,
    IReadOnlyList<ReleaseVersion> Blocklist,
    DateTimeOffset ReleasedAt,
    IReadOnlyList<ManifestPackage> Packages)
{
    public ManifestPackage? FindPackage(string fileName) =>
        Packages.FirstOrDefault(p => string.Equals(p.File, fileName, StringComparison.OrdinalIgnoreCase));

    /// <summary>Strict parse. Any missing, mistyped or out-of-range field is null.</summary>
    internal static UpdateManifest? Parse(ReadOnlySpan<byte> utf8)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(utf8.ToArray());
            JsonElement root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return null;
            if (!root.TryGetProperty("schema", out JsonElement schema) || schema.ValueKind != JsonValueKind.Number ||
                !schema.TryGetInt32(out int schemaValue)) return null;
            string? channel = Str(root, "channel");
            if (channel is null) return null;
            if (!ReleaseVersion.TryParse(Str(root, "version"), out ReleaseVersion version)) return null;
            if (!ReleaseVersion.TryParse(Str(root, "min_version"), out ReleaseVersion minVersion)) return null;
            if (!root.TryGetProperty("blocklist", out JsonElement block) || block.ValueKind != JsonValueKind.Array) return null;
            var blocklist = new List<ReleaseVersion>();
            foreach (JsonElement b in block.EnumerateArray())
            {
                if (b.ValueKind != JsonValueKind.String || !ReleaseVersion.TryParse(b.GetString(), out ReleaseVersion bv)) return null;
                blocklist.Add(bv);
            }
            if (!DateTimeOffset.TryParse(Str(root, "released_at"), CultureInfo.InvariantCulture,
                    DateTimeStyles.RoundtripKind, out DateTimeOffset released)) return null;
            if (!root.TryGetProperty("packages", out JsonElement pk) || pk.ValueKind != JsonValueKind.Array) return null;
            var packages = new List<ManifestPackage>();
            foreach (JsonElement p in pk.EnumerateArray())
            {
                if (p.ValueKind != JsonValueKind.Object) return null;
                string? file = Str(p, "file");
                string? sha = Str(p, "sha256");
                string? kind = Str(p, "kind");
                if (string.IsNullOrWhiteSpace(file) || file.IndexOfAny(new[] { '/', '\\', ':' }) >= 0) return null;
                if (!Hex.TryDecode(sha, 32, out _)) return null;
                if (kind is not ("full" or "delta")) return null;
                if (!p.TryGetProperty("size", out JsonElement size) || size.ValueKind != JsonValueKind.Number ||
                    !size.TryGetInt64(out long sizeValue) || sizeValue <= 0) return null;
                packages.Add(new ManifestPackage(file, sha!.ToLowerInvariant(), sizeValue, kind));
            }
            return new UpdateManifest(schemaValue, channel, version, minVersion, blocklist, released, packages);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static string? Str(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out JsonElement e) && e.ValueKind == JsonValueKind.String ? e.GetString() : null;
}

public enum ManifestRejection
{
    None = 0,
    KeyNotConfigured,
    MissingSignature,
    BadSignature,
    Malformed,
    UnsupportedSchema,
    ChannelMismatch,
    Rollback,            // manifest version below the running version
    TargetBlocklisted,   // the manifest's own version is on its blocklist
    TargetFailedLocally, // this machine already rolled that version back
    NoFullPackage,
}

public enum UpdateUrgency
{
    Normal = 0,
    BelowMinimum,        // running version < min_version
    CurrentBlocklisted,  // running version is pulled
}

public sealed record ManifestDecision(
    ManifestRejection Rejection,
    UpdateManifest? Manifest,
    bool UpdateAvailable,
    UpdateUrgency Urgency)
{
    public bool Trusted => Rejection == ManifestRejection.None && Manifest is not null;

    internal static ManifestDecision Reject(ManifestRejection why, UpdateManifest? m = null) =>
        new(why, m, false, UpdateUrgency.Normal);
}

/// <summary>
/// Every rule the updater applies before it trusts a byte of the manifest
/// (plan/13 "Signing", "Rollback and the kill switch"). Pure: no I/O, no clock.
/// </summary>
public static class ManifestVerifier
{
    public const int SupportedSchema = 1;

    /// <summary>Ed25519, detached, over the exact manifest bytes.</summary>
    public static bool VerifySignature(ReadOnlySpan<byte> manifest, ReadOnlySpan<byte> signature, ReadOnlySpan<byte> publicKey)
    {
        if (signature.Length != 64 || publicKey.Length != 32) return false;
        try
        {
            var signer = new Ed25519Signer();
            signer.Init(false, new Ed25519PublicKeyParameters(publicKey.ToArray(), 0));
            byte[] m = manifest.ToArray();
            signer.BlockUpdate(m, 0, m.Length);
            return signer.VerifySignature(signature.ToArray());
        }
        catch (Exception)
        {
            return false;
        }
    }

    public static ManifestDecision Evaluate(
        ReadOnlySpan<byte> manifestBytes,
        ReadOnlySpan<byte> signature,
        ReadOnlySpan<byte> pinnedPublicKey,
        string expectedChannel,
        ReleaseVersion running,
        IReadOnlyCollection<ReleaseVersion>? failedLocally = null)
    {
        // 1. Signature first. Nothing below runs on unauthenticated bytes.
        if (pinnedPublicKey.Length != 32 || pinnedPublicKey.IndexOfAnyExcept((byte)0) < 0)
            return ManifestDecision.Reject(ManifestRejection.KeyNotConfigured);
        if (signature.IsEmpty) return ManifestDecision.Reject(ManifestRejection.MissingSignature);
        if (!VerifySignature(manifestBytes, signature, pinnedPublicKey))
            return ManifestDecision.Reject(ManifestRejection.BadSignature);

        // 2. Shape.
        UpdateManifest? m = UpdateManifest.Parse(manifestBytes);
        if (m is null) return ManifestDecision.Reject(ManifestRejection.Malformed);
        if (m.Schema != SupportedSchema) return ManifestDecision.Reject(ManifestRejection.UnsupportedSchema, m);
        if (!string.Equals(m.Channel, expectedChannel, StringComparison.Ordinal))
            return ManifestDecision.Reject(ManifestRejection.ChannelMismatch, m);

        // 3. Version policy.
        if (m.Version < running) return ManifestDecision.Reject(ManifestRejection.Rollback, m);
        if (m.Blocklist.Contains(m.Version)) return ManifestDecision.Reject(ManifestRejection.TargetBlocklisted, m);
        if (failedLocally is not null && failedLocally.Contains(m.Version))
            return ManifestDecision.Reject(ManifestRejection.TargetFailedLocally, m);
        if (!m.Packages.Any(p => p.Kind == "full")) return ManifestDecision.Reject(ManifestRejection.NoFullPackage, m);

        UpdateUrgency urgency = m.Blocklist.Contains(running) ? UpdateUrgency.CurrentBlocklisted
            : running < m.MinVersion ? UpdateUrgency.BelowMinimum
            : UpdateUrgency.Normal;
        return new ManifestDecision(ManifestRejection.None, m, m.Version > running, urgency);
    }

    /// <summary>Size and SHA-256 of a downloaded package against its signed entry.</summary>
    public static bool VerifyPackage(Stream content, ManifestPackage expected)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[1 << 16];
        long total = 0;
        int read;
        while ((read = content.Read(buffer, 0, buffer.Length)) > 0)
        {
            total += read;
            if (total > expected.Size) return false;
            sha.AppendData(buffer, 0, read);
        }
        if (total != expected.Size) return false;
        byte[] want = Hex.Decode(expected.Sha256);
        return CryptographicOperations.FixedTimeEquals(sha.GetHashAndReset(), want);
    }

    public static bool VerifyPackageFile(string path, ManifestPackage expected)
    {
        using FileStream fs = File.OpenRead(path);
        return VerifyPackage(fs, expected);
    }
}
