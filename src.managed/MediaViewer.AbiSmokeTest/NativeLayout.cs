// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C# mirrors of native structs, compared field for field with the layout
// the native compiler gave them (tools/abi-layout prints it). A hand-copied
// size or offset is what drifted before (MvAiStatus pinned at 1336 for a 1640
// byte struct, 2026-10-05..15); this reads the native numbers instead.

using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.AbiSmokeTest;

static class NativeLayout
{
    /// <summary>"mv_ai_status" → 1640, "mv_ai_status.struct_size" → (0, 4).</summary>
    public sealed record Dump(Dictionary<string, int> Sizes, Dictionary<string, (int Offset, int Size)> Fields);

    public static Dump Read(string path)
    {
        var sizes = new Dictionary<string, int>();
        var fields = new Dictionary<string, (int, int)>();
        foreach (string line in File.ReadAllLines(path))
        {
            string[] p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (p.Length == 2) sizes[p[0]] = int.Parse(p[1]);
            else if (p.Length == 3) fields[p[0]] = (int.Parse(p[1]), int.Parse(p[2]));
        }
        return new Dump(sizes, fields);
    }

    /// <summary>
    /// Every native field has a C# field of the same offset and size and the
    /// other way round, and the sizes match. Names map snake_case to
    /// PascalCase, dropping a "_utf8" suffix (active_root_utf8 → ActiveRoot).
    /// Returns one line per mismatch; empty when the mirror holds.
    /// </summary>
    public static List<string> Compare(Dump dump, string native, Type managed)
    {
        var problems = new List<string>();
        if (!dump.Sizes.TryGetValue(native, out int nativeSize))
        {
            problems.Add($"{native} is not in the native dump");
            return problems;
        }
        int managedSize = Marshal.SizeOf(managed);
        if (managedSize != nativeSize)
            problems.Add($"{managed.Name} is {managedSize} bytes, {native} is {nativeSize}");

        var csFields = managed.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .ToDictionary(f => f.Name);
        var matched = new HashSet<string>();
        string prefix = native + ".";
        foreach (var (key, (offset, size)) in dump.Fields)
        {
            if (!key.StartsWith(prefix, StringComparison.Ordinal)) continue;
            string cName = key[prefix.Length..];
            string csName = Pascal(cName);
            if (!csFields.TryGetValue(csName, out FieldInfo? f))
            {
                problems.Add($"{native}.{cName} (offset {offset}) has no {managed.Name}.{csName}");
                continue;
            }
            matched.Add(csName);
            int csOffset = (int)Marshal.OffsetOf(managed, csName);
            int csSize = FieldSize(f.FieldType);
            if (csOffset != offset || csSize != size)
                problems.Add($"{managed.Name}.{csName} is at {csOffset} ({csSize} bytes), " +
                             $"{native}.{cName} at {offset} ({size} bytes)");
        }
        foreach (string name in csFields.Keys.Where(n => !matched.Contains(n)))
            problems.Add($"{managed.Name}.{name} has no field in {native}");
        return problems;
    }

    static int FieldSize(Type t) => Marshal.SizeOf(t.IsEnum ? Enum.GetUnderlyingType(t) : t);

    static string Pascal(string snake)
    {
        if (snake.EndsWith("_utf8", StringComparison.Ordinal)) snake = snake[..^5];
        var sb = new StringBuilder(snake.Length);
        foreach (string part in snake.Split('_', StringSplitOptions.RemoveEmptyEntries))
            sb.Append(char.ToUpperInvariant(part[0])).Append(part, 1, part.Length - 1);
        return sb.ToString();
    }
}
