// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the repository's sources as text: the core's enums (commands.h,
// chrome_host.h), the chromes' constant blocks (C#, Swift), and the per-mode
// describe_commands() snapshot mv_tests checks in.
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace MediaViewer.Chrome.Tests;

internal static class Repo
{
    private static readonly Lazy<string> RootPath = new(FindRoot);

    public static string Root => RootPath.Value;

    public static string Path(params string[] parts) =>
        System.IO.Path.Combine(new[] { Root }.Concat(parts).ToArray());

    public static string Read(params string[] parts)
    {
        string file = Path(parts);
        Assert.True(File.Exists(file), $"missing {file}");
        return File.ReadAllText(file).Replace("\r\n", "\n");
    }

    private static string FindRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(System.IO.Path.Combine(dir.FullName, "src", "shell", "commands.h")))
                return dir.FullName;
        }
        throw new InvalidOperationException(
            "src/shell/commands.h not found above " + AppContext.BaseDirectory + "; run from the repository");
    }
}

internal static class SourceText
{
    // Comments out, string and char literals kept (no enum here holds a // inside one).
    public static string StripComments(string text)
    {
        var sb = new StringBuilder(text.Length);
        for (int i = 0; i < text.Length; i++)
        {
            char c = text[i];
            if (c == '/' && i + 1 < text.Length && text[i + 1] == '/')
            {
                while (i < text.Length && text[i] != '\n') i++;
                if (i < text.Length) sb.Append('\n');
                continue;
            }
            if (c == '/' && i + 1 < text.Length && text[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < text.Length && !(text[i] == '*' && text[i + 1] == '/')) i++;
                i++;
                sb.Append(' ');
                continue;
            }
            if (c == '"')
            {
                int j = i + 1;
                while (j < text.Length && text[j] != '"' && text[j] != '\n')
                {
                    if (text[j] == '\\') j++;
                    j++;
                }
                sb.Append(text, i, Math.Min(j + 1, text.Length) - i);
                i = j;
                continue;
            }
            sb.Append(c);
        }
        return sb.ToString();
    }

    // The text between the braces that follow the first match of `header`.
    public static string Block(string text, string headerPattern)
    {
        Match m = Regex.Match(text, headerPattern);
        Assert.True(m.Success, $"no block matching /{headerPattern}/");
        int open = text.IndexOf('{', m.Index + m.Length - 1);
        Assert.True(open >= 0, $"no '{{' after /{headerPattern}/");
        int depth = 0;
        for (int i = open; i < text.Length; i++)
        {
            if (text[i] == '{') depth++;
            else if (text[i] == '}' && --depth == 0) return text.Substring(open + 1, i - open - 1);
        }
        throw new InvalidOperationException($"unbalanced block after /{headerPattern}/");
    }

    // A C++ enum's enumerators in order, with implicit values counted the way the
    // compiler does. Values must be integer literals; the core's enums are.
    public static IReadOnlyList<(string Name, int Value)> CppEnum(string header, string enumPattern)
    {
        string body = Block(StripComments(header), enumPattern);
        var list = new List<(string, int)>();
        int next = 0;
        foreach (string raw in body.Split(','))
        {
            string entry = raw.Trim();
            if (entry.Length == 0) continue;
            int eq = entry.IndexOf('=');
            string name = (eq < 0 ? entry : entry[..eq]).Trim();
            Assert.Matches("^[A-Za-z_][A-Za-z0-9_]*$", name);
            int value = eq < 0 ? next : ParseInt(entry[(eq + 1)..].Trim());
            list.Add((name, value));
            next = value + 1;
        }
        return list;
    }

    public static int ParseInt(string literal)
    {
        literal = literal.Trim();
        if (literal.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return int.Parse(literal[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        return int.Parse(literal, NumberStyles.Integer, CultureInfo.InvariantCulture);
    }

    // OneToOne / oneToOne / SetRating0 / zoom200 -> one_to_one / set_rating_0 / zoom_200.
    public static string Snake(string name)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < name.Length; i++)
        {
            char c = name[i];
            bool boundary = i > 0 &&
                ((char.IsUpper(c) && !char.IsUpper(name[i - 1])) ||
                 (char.IsDigit(c) && !char.IsDigit(name[i - 1])) ||
                 (char.IsUpper(c) && i + 1 < name.Length && char.IsLower(name[i + 1]) && char.IsUpper(name[i - 1])));
            if (boundary) sb.Append('_');
            sb.Append(char.ToLowerInvariant(c));
        }
        return sb.ToString();
    }
}

// The core's tables, parsed once.
internal static class Core
{
    private static readonly Lazy<Dictionary<string, int>> CommandIds = new(() =>
        SourceText.CppEnum(Repo.Read("src", "shell", "commands.h"), @"enum\s+class\s+command_id\s*:[^{]*")
                  .ToDictionary(e => e.Name, e => e.Value));

    private static readonly Lazy<Dictionary<string, int>> Keys = new(() =>
        SourceText.CppEnum(Repo.Read("src", "shell", "commands.h"), @"enum\s+class\s+key\s*:[^{]*")
                  .ToDictionary(e => e.Name, e => e.Value));

    private static readonly Lazy<Dictionary<string, int>> ChromeCommands = new(() =>
        SourceText.CppEnum(Repo.Read("src", "shell", "chrome_host.h"), @"enum\s+chrome_command\s*:[^{]*")
                  .ToDictionary(e => e.Name, e => e.Value));

    // command_id names, without the class prefix: "toggle_gallery" -> 12.
    public static IReadOnlyDictionary<string, int> CommandId => CommandIds.Value;
    // key names: "f12" -> 0x11A.
    public static IReadOnlyDictionary<string, int> Key => Keys.Value;
    // chrome_command names with their prefix: "chrome_cmd_rebind" -> 1001.
    public static IReadOnlyDictionary<string, int> ChromeCommand => ChromeCommands.Value;

    // "15 set_settings, 16 folder_ready: notifications" in command_id's body:
    // the island -> native notifications that keep their ids reserved there.
    public static readonly int[] ReservedNotifications = { 15, 16, 18, 19, 20 };

    // command_info palette names in command_table.cpp kCommands: "Zoom in" -> zoom_in.
    public static IReadOnlyDictionary<string, string> CommandNames => Names.Value;

    private static readonly Lazy<Dictionary<string, string>> Names = new(() =>
    {
        string body = SourceText.Block(SourceText.StripComments(Repo.Read("src", "shell", "command_table.cpp")),
                                       @"constexpr\s+command_info\s+kCommands\[\]\s*=");
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(body, @"\{\s*([a-z_0-9]+)\s*,\s*""((?:[^""\\]|\\.)*)"""))
            map[Regex.Unescape(m.Groups[2].Value)] = m.Groups[1].Value;
        return map;
    });
}

// tests/data/command_table.snapshot: describe_commands() by mode, written and
// checked by mv_tests (tests/test_command_contract.cpp).
internal sealed record SnapshotRow(string Mode, string Keys, string Name, int Id, bool Runnable);

internal static class Snapshot
{
    private static readonly Lazy<List<SnapshotRow>> RowsValue = new(Load);

    public static IReadOnlyList<SnapshotRow> Rows => RowsValue.Value;

    private static List<SnapshotRow> Load()
    {
        var rows = new List<SnapshotRow>();
        string mode = "";
        foreach (string line in Repo.Read("tests", "data", "command_table.snapshot").Split('\n'))
        {
            // Rows always have tabs; a key can be '[' or '#', so only a tab-free
            // line is a comment or a "[mode]" header.
            bool row = line.Contains('\t');
            if (line.Length == 0 || (line[0] == '#' && !row)) continue;
            if (line[0] == '[' && !row)
            {
                mode = line.Trim('[', ']');
                continue;
            }
            string[] f = line.Split('\t');
            Assert.True(f.Length == 4, $"snapshot line '{line}'");
            rows.Add(new SnapshotRow(mode, f[0], f[1], SourceText.ParseInt(f[2]), f[3] == "1"));
        }
        return rows;
    }
}
