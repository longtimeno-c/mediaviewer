// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Security.Cryptography;
using System.Text;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

/// <summary>
/// The Phone access card's QR on Windows (Shared/QrCode.cs). The matrices
/// pinned here were read back by Apple's CIDetector when the encoder was
/// written (2026-11-12, 19 payloads, versions 1 to 40 and non-ASCII text, all
/// decoded exactly); a change to the encoder that moves a module fails here.
/// </summary>
public class QrCodeTests
{
    private static string Matrix(QrCode q)
    {
        var sb = new StringBuilder();
        for (int y = 0; y < q.Size; ++y)
        {
            for (int x = 0; x < q.Size; ++x) sb.Append(q[x, y] ? '1' : '0');
            sb.Append('\n');
        }
        return Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(sb.ToString())));
    }

    [Theory]
    [InlineData("hello", 1, 7, "D9E93C47DF2FD3419021124B6F90CDE9ADD476DDA7D7B9E721F3B2463D312960")]
    [InlineData("mediaviewer://pair#eyJ2IjoxLCJsaWJyYXJ5X2lkIjoiMDEyMyJ9", 4, 1,
                "EFAB0B5E7E736915C0DAE4FB6187AD984C23D93359D6A1C3F451FD1A78B2E2A1")]
    public void Matrix_is_the_one_a_scanner_read_back(string text, int version, int mask, string sha)
    {
        QrCode q = QrCode.Encode(text)!;
        Assert.Equal(version, q.Version);
        Assert.Equal(version * 4 + 17, q.Size);
        Assert.Equal(mask, q.Mask);
        Assert.Equal(sha, Matrix(q));
    }

    [Theory]
    [InlineData(14, 1)]     // version 1-M holds 14 bytes
    [InlineData(15, 2)]
    [InlineData(2331, 40)]  // version 40-M holds 2,331
    public void Smallest_version_that_holds_the_bytes(int bytes, int version) =>
        Assert.Equal(version, QrCode.Encode(new byte[bytes])!.Version);

    [Fact]
    public void Too_long_is_null() => Assert.Null(QrCode.Encode(new byte[2332]));

    [Fact]
    public void Finder_patterns_sit_in_three_corners()
    {
        QrCode q = QrCode.Encode("mediaviewer://pair#x")!;
        foreach ((int cx, int cy) in new[] { (3, 3), (q.Size - 4, 3), (3, q.Size - 4) })
        {
            Assert.True(q[cx, cy]);           // the 3×3 centre
            Assert.False(q[cx + 2, cy]);      // the light ring
            Assert.True(q[cx + 3, cy]);       // the dark ring
        }
        Assert.False(q[-1, 0]);  // outside reads as light (the quiet zone)
    }
}
