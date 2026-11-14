// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The algorithm follows Project Nayuki's QR Code generator library
// (https://www.nayuki.io/page/qr-code-generator-library), which is
// Copyright (c) Project Nayuki, MIT License: the tables, the Reed-Solomon
// steps and the module placement are its, reduced to what a phone's pairing
// code needs (byte mode, one error-correction level). Its notice:
//
//   Copyright (c) Project Nayuki. (MIT License)
//   https://www.nayuki.io/page/qr-code-generator-library
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of
//   this software and associated documentation files (the "Software"), to deal in
//   the Software without restriction, including without limitation the rights to
//   use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
//   the Software, and to permit persons to whom the Software is furnished to do so,
//   subject to the following conditions:
//   - The above copyright notice and this permission notice shall be included in
//     all copies or substantial portions of the Software.
//   - The Software is provided "as is", without warranty of any kind, express or
//     implied, including but not limited to the warranties of merchantability,
//     fitness for a particular purpose and noninfringement. In no event shall the
//     authors or copyright holders be liable for any claim, damages or other
//     liability, whether in an action of contract, tort or otherwise, arising from,
//     out of or in connection with the Software or the use or other dealings in the
//     Software.
namespace MediaViewer.Shared;

/// <summary>
/// A QR code for the Phone access card's pairing code (docs/plans/mobile.md
/// §7.4): byte mode, error correction M (what the Mac's CIQRCodeGenerator is
/// asked for), the smallest version that holds the text, the mask with the
/// lowest penalty. Plain .NET, so Chrome.Tests runs it on any OS.
/// </summary>
internal sealed class QrCode
{
    /// <summary>Modules per side (21 for version 1 … 177 for version 40).</summary>
    public int Size { get; }
    public int Version { get; }
    public int Mask { get; private set; }
    private readonly bool[,] _modules;     // [y, x], true = dark
    private readonly bool[,] _function;    // finder, timing, alignment, format, version

    public bool this[int x, int y] => x >= 0 && y >= 0 && x < Size && y < Size && _modules[y, x];

    // Error correction M, by version (index 0 unused).
    private static readonly int[] EccPerBlock =
    {
        -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
        26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    };
    private static readonly int[] EccBlocks =
    {
        -1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
        17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49,
    };
    private const int FormatBitsM = 0;  // L = 1, M = 0, Q = 3, H = 2

    /// <summary>The code for <paramref name="text"/> as UTF-8; null when it is too long for version 40.</summary>
    public static QrCode? Encode(string text) => Encode(System.Text.Encoding.UTF8.GetBytes(text));

    public static QrCode? Encode(byte[] data)
    {
        for (int version = 1; version <= 40; ++version)
        {
            int capacityBits = DataCodewords(version) * 8;
            int countBits = version <= 9 ? 8 : 16;
            long used = 4L + countBits + data.Length * 8L;
            if (data.Length < (1 << countBits) && used <= capacityBits)
                return new QrCode(version, Codewords(data, version, countBits, capacityBits));
        }
        return null;
    }

    private static byte[] Codewords(byte[] data, int version, int countBits, int capacityBits)
    {
        var bits = new List<bool>(capacityBits);
        void Append(int value, int count)
        {
            for (int i = count - 1; i >= 0; --i) bits.Add(((value >> i) & 1) != 0);
        }
        Append(0b0100, 4);  // byte mode
        Append(data.Length, countBits);
        foreach (byte b in data) Append(b, 8);
        Append(0, Math.Min(4, capacityBits - bits.Count));  // terminator
        Append(0, (8 - bits.Count % 8) % 8);
        for (int pad = 0xEC; bits.Count < capacityBits; pad ^= 0xEC ^ 0x11) Append(pad, 8);
        var bytes = new byte[bits.Count / 8];
        for (int i = 0; i < bits.Count; ++i)
            if (bits[i]) bytes[i >> 3] |= (byte)(0x80 >> (i & 7));
        return bytes;
    }

    private QrCode(int version, byte[] dataCodewords)
    {
        Version = version;
        Size = version * 4 + 17;
        _modules = new bool[Size, Size];
        _function = new bool[Size, Size];
        DrawFunctionPatterns();
        DrawCodewords(AddEccAndInterleave(dataCodewords));

        // The mask with the lowest penalty, ties to the lowest number.
        int best = 0;
        long bestPenalty = long.MaxValue;
        for (int m = 0; m < 8; ++m)
        {
            ApplyMask(m);
            DrawFormatBits(m);
            long p = Penalty();
            if (p < bestPenalty)
            {
                best = m;
                bestPenalty = p;
            }
            ApplyMask(m);  // XOR again: undone
        }
        Mask = best;
        ApplyMask(best);
        DrawFormatBits(best);
    }

    // ---- sizes ----

    private static int RawDataModules(int ver)
    {
        int result = (16 * ver + 128) * ver + 64;
        if (ver >= 2)
        {
            int align = ver / 7 + 2;
            result -= (25 * align - 10) * align - 55;
            if (ver >= 7) result -= 36;
        }
        return result;
    }

    private static int DataCodewords(int ver) => RawDataModules(ver) / 8 - EccPerBlock[ver] * EccBlocks[ver];

    // ---- function patterns ----

    private void SetFunction(int x, int y, bool dark)
    {
        _modules[y, x] = dark;
        _function[y, x] = true;
    }

    private void DrawFunctionPatterns()
    {
        for (int i = 0; i < Size; ++i)
        {
            SetFunction(6, i, i % 2 == 0);
            SetFunction(i, 6, i % 2 == 0);
        }
        DrawFinder(3, 3);
        DrawFinder(Size - 4, 3);
        DrawFinder(3, Size - 4);
        int[] align = AlignmentPositions();
        int n = align.Length;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
                    DrawAlignment(align[i], align[j]);
        DrawFormatBits(0);  // reserves the area; drawn for real once the mask is chosen
        DrawVersion();
    }

    private void DrawFinder(int x, int y)
    {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx)
            {
                int dist = Math.Max(Math.Abs(dx), Math.Abs(dy));
                int xx = x + dx, yy = y + dy;
                if (xx >= 0 && xx < Size && yy >= 0 && yy < Size) SetFunction(xx, yy, dist != 2 && dist != 4);
            }
    }

    private void DrawAlignment(int x, int y)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                SetFunction(x + dx, y + dy, Math.Max(Math.Abs(dx), Math.Abs(dy)) != 1);
    }

    private int[] AlignmentPositions()
    {
        if (Version == 1) return Array.Empty<int>();
        int n = Version / 7 + 2;
        int step = (Version * 8 + n * 3 + 5) / (n * 4 - 4) * 2;
        var result = new int[n];
        result[0] = 6;
        for (int i = n - 1, pos = Size - 7; i >= 1; --i, pos -= step) result[i] = pos;
        return result;
    }

    private void DrawFormatBits(int mask)
    {
        int data = FormatBitsM << 3 | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = (data << 10 | rem) ^ 0x5412;
        bool Bit(int i) => ((bits >> i) & 1) != 0;

        for (int i = 0; i <= 5; ++i) SetFunction(8, i, Bit(i));
        SetFunction(8, 7, Bit(6));
        SetFunction(8, 8, Bit(7));
        SetFunction(7, 8, Bit(8));
        for (int i = 9; i < 15; ++i) SetFunction(14 - i, 8, Bit(i));

        for (int i = 0; i < 8; ++i) SetFunction(Size - 1 - i, 8, Bit(i));
        for (int i = 8; i < 15; ++i) SetFunction(8, Size - 15 + i, Bit(i));
        SetFunction(8, Size - 8, true);  // the dark module
    }

    private void DrawVersion()
    {
        if (Version < 7) return;
        int rem = Version;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        int bits = Version << 12 | rem;
        for (int i = 0; i < 18; ++i)
        {
            bool bit = ((bits >> i) & 1) != 0;
            int a = Size - 11 + i % 3, b = i / 3;
            SetFunction(a, b, bit);
            SetFunction(b, a, bit);
        }
    }

    // ---- data ----

    private byte[] AddEccAndInterleave(byte[] data)
    {
        int blocks = EccBlocks[Version];
        int eccLen = EccPerBlock[Version];
        int raw = RawDataModules(Version) / 8;
        int shortBlocks = blocks - raw % blocks;
        int shortLen = raw / blocks;
        byte[] divisor = ReedSolomonDivisor(eccLen);

        var all = new byte[blocks][];
        for (int i = 0, k = 0; i < blocks; ++i)
        {
            int datLen = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
            var block = new byte[shortLen + 1];
            Array.Copy(data, k, block, 0, datLen);
            byte[] ecc = ReedSolomonRemainder(data.AsSpan(k, datLen), divisor);
            k += datLen;
            // A short block keeps one empty slot before its ECC, skipped below.
            Array.Copy(ecc, 0, block, shortLen + 1 - eccLen, eccLen);
            all[i] = block;
        }

        var result = new byte[raw];
        int n = 0;
        for (int i = 0; i < all[0].Length; ++i)
            for (int j = 0; j < blocks; ++j)
                if (i != shortLen - eccLen || j >= shortBlocks)
                    result[n++] = all[j][i];
        return result;
    }

    private static byte[] ReedSolomonDivisor(int degree)
    {
        var result = new byte[degree];
        result[degree - 1] = 1;
        int root = 1;
        for (int i = 0; i < degree; ++i)
        {
            for (int j = 0; j < degree; ++j)
            {
                result[j] = Multiply(result[j], root);
                if (j + 1 < degree) result[j] ^= result[j + 1];
            }
            root = Multiply(root, 0x02);
        }
        return result;
    }

    private static byte[] ReedSolomonRemainder(ReadOnlySpan<byte> data, byte[] divisor)
    {
        var result = new byte[divisor.Length];
        foreach (byte b in data)
        {
            int factor = b ^ result[0];
            Array.Copy(result, 1, result, 0, result.Length - 1);
            result[^1] = 0;
            for (int i = 0; i < divisor.Length; ++i) result[i] ^= Multiply(divisor[i], factor);
        }
        return result;
    }

    // GF(2^8) over x^8 + x^4 + x^3 + x^2 + 1.
    private static byte Multiply(int x, int y)
    {
        int z = 0;
        for (int i = 7; i >= 0; --i)
        {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return (byte)z;
    }

    private void DrawCodewords(byte[] data)
    {
        int i = 0;
        for (int right = Size - 1; right >= 1; right -= 2)
        {
            if (right == 6) right = 5;  // skip the vertical timing column
            bool upward = ((right + 1) & 2) == 0;
            for (int vert = 0; vert < Size; ++vert)
                for (int j = 0; j < 2; ++j)
                {
                    int x = right - j;
                    int y = upward ? Size - 1 - vert : vert;
                    if (_function[y, x] || i >= data.Length * 8) continue;
                    _modules[y, x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                    ++i;
                }
        }
    }

    private void ApplyMask(int mask)
    {
        for (int y = 0; y < Size; ++y)
            for (int x = 0; x < Size; ++x)
            {
                bool invert = mask switch
                {
                    0 => (x + y) % 2 == 0,
                    1 => y % 2 == 0,
                    2 => x % 3 == 0,
                    3 => (x + y) % 3 == 0,
                    4 => (x / 3 + y / 2) % 2 == 0,
                    5 => x * y % 2 + x * y % 3 == 0,
                    6 => (x * y % 2 + x * y % 3) % 2 == 0,
                    _ => ((x + y) % 2 + x * y % 3) % 2 == 0,
                };
                if (invert && !_function[y, x]) _modules[y, x] = !_modules[y, x];
            }
    }

    // ---- choosing the mask (ISO/IEC 18004 §7.8.3) ----

    private long Penalty()
    {
        long result = 0;
        for (int pass = 0; pass < 2; ++pass)  // rows, then columns
        {
            for (int a = 0; a < Size; ++a)
            {
                bool runColour = false;
                int run = 0;
                for (int b = 0; b < Size; ++b)
                {
                    bool c = pass == 0 ? _modules[a, b] : _modules[b, a];
                    if (b > 0 && c == runColour) ++run;
                    else
                    {
                        if (run >= 5) result += 3 + (run - 5);
                        runColour = c;
                        run = 1;
                    }
                }
                if (run >= 5) result += 3 + (run - 5);
                result += 40 * FinderLike(a, pass == 0);
            }
        }
        for (int y = 0; y < Size - 1; ++y)
            for (int x = 0; x < Size - 1; ++x)
            {
                bool c = _modules[y, x];
                if (c == _modules[y, x + 1] && c == _modules[y + 1, x] && c == _modules[y + 1, x + 1]) result += 3;
            }
        int dark = 0;
        foreach (bool m in _modules) if (m) ++dark;
        int total = Size * Size;
        int k = (Math.Abs(dark * 20 - total * 10) + total - 1) / total - 1;
        result += k * 10L;
        return result;
    }

    // 1:1:3:1:1 dark-light-dark-light-dark with four light modules on either
    // side (outside the symbol counts as light).
    private int FinderLike(int line, bool row)
    {
        bool At(int i) => i >= 0 && i < Size && (row ? _modules[line, i] : _modules[i, line]);
        int count = 0;
        for (int i = -4; i < Size; ++i)
        {
            bool core = At(i) && !At(i + 1) && At(i + 2) && At(i + 3) && At(i + 4) && !At(i + 5) && At(i + 6);
            if (!core) continue;
            bool before = !At(i - 1) && !At(i - 2) && !At(i - 3) && !At(i - 4);
            bool after = !At(i + 7) && !At(i + 8) && !At(i + 9) && !At(i + 10);
            if (before || after) ++count;
        }
        return count;
    }
}
