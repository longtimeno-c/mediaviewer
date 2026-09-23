// SPDX-License-Identifier: GPL-2.0-or-later
namespace MediaViewer.Updater;

/// <summary>
/// The update-manifest signing key pinned into the app (plan/13 "Signing").
/// </summary>
/// <remarks>
/// <para><b>PRODUCTION PUBLIC KEY.</b> 32 bytes of raw Ed25519 public key,
/// lowercase hex. Pinned 2026-09-23; its private half signs every release
/// manifest (procedure: tools/package/update-signing.md).</para>
/// <para>An all-zero value is the placeholder, and <see cref="ManifestVerifier"/>
/// rejects every manifest with <see cref="ManifestRejection.KeyNotConfigured"/>
/// while it is set, so an unconfigured build fails closed. Rotating this key
/// means shipping a build carrying the new key, signed under the old one,
/// first — there is no in-band key update.</para>
/// <para>Only the public half is ever in the tree. The private key lives with
/// the release signer, never in git, never in CI logs.</para>
/// </remarks>
public static class UpdateKeys
{
    /// <summary>The release Ed25519 public key. Its private half is held by the
    /// release signer and is never in this repository (update-signing.md).</summary>
    public const string ProductionPublicKeyHex =
        "0451bfecfb6a26d9058fb09cfa7a9305dcf1cea7c87221e9185b0038b1cc908c";

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
