// SPDX-License-Identifier: GPL-2.0-or-later
namespace MediaViewer.Updater;

/// <summary>
/// The update-manifest signing key pinned into the app (plan/13 "Signing").
/// </summary>
/// <remarks>
/// <para><b>PRODUCTION PUBLIC KEY — PLACEHOLDER. The owner replaces this
/// constant before the first external build</b> (procedure:
/// tools/package/update-signing.md). It is 32 bytes of raw Ed25519 public key,
/// lowercase hex.</para>
/// <para>While it is the all-zero placeholder, <see cref="ManifestVerifier"/>
/// rejects every manifest with <see cref="ManifestRejection.KeyNotConfigured"/>,
/// so an unconfigured build can never trust an update — it fails closed.</para>
/// <para>Only the public half is ever in the tree. The private key lives with
/// the release signer, never in git, never in CI logs.</para>
/// </remarks>
public static class UpdateKeys
{
    /// <summary>PLACEHOLDER — replace with the release Ed25519 public key (64 hex chars).</summary>
    public const string ProductionPublicKeyHex =
        "0000000000000000000000000000000000000000000000000000000000000000";

    public const string Channel = "win";
    public const string PackId = "MediaViewer";
    public const string GithubRepoUrl = "https://github.com/longtimeno-c/mediaviewer";
    public const string ManifestAssetName = "mediaviewer-manifest.json";
    public const string SignatureAssetName = "mediaviewer-manifest.json.sig";

    public static byte[] ProductionPublicKey => Hex.Decode(ProductionPublicKeyHex);
}

internal static class Hex
{
    public static byte[] Decode(string hex)
    {
        if (hex.Length % 2 != 0) throw new FormatException("odd hex length");
        var bytes = new byte[hex.Length / 2];
        for (int i = 0; i < bytes.Length; i++)
            bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
        return bytes;
    }

    public static bool TryDecode(string? hex, int expectedBytes, out byte[] bytes)
    {
        bytes = Array.Empty<byte>();
        if (hex is null || hex.Length != expectedBytes * 2) return false;
        foreach (char c in hex)
        {
            if (!Uri.IsHexDigit(c)) return false;
        }
        bytes = Decode(hex);
        return true;
    }

    public static string Encode(ReadOnlySpan<byte> bytes) => Convert.ToHexString(bytes).ToLowerInvariant();
}
