// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #153: the chromes' copies of the command ids agree with the core.
//
// chrome_host.h pins the C# values it knows with static_asserts and a checksum the
// Probe run compares, but only for the ids it lists, and only on Windows in a
// probe build. Nothing pinned the Swift copies. These read both chromes' constant
// blocks and resolve every constant against commands.h (command_id) or chrome_host.h
// (chrome_command) by name: a constant whose name resolves to a different number,
// or to nothing, fails here.
using System.Text.RegularExpressions;

namespace MediaViewer.Chrome.Tests;

public class CommandIdTests
{
    // Chrome constants spelled differently from the core's enumerator. Keep this
    // short: a new constant should use the core's name.
    private static readonly Dictionary<string, string> Aliases = new(StringComparer.Ordinal)
    {
        // C# Command
        ["ClipToolsFlyout"] = "clip_tools",
        // Swift EditCommand (EditStore.swift)
        ["rotateLeft"] = "rotate_ccw",
        ["rotateRight"] = "rotate_cw",
        ["flipH"] = "flip_horizontal",
        ["flipV"] = "flip_vertical",
        ["undo"] = "undo_edit",
        ["reset"] = "reset_edits",
    };

    internal sealed record Constant(string File, string Owner, string Name, int Value);

    internal static List<Constant> CSharpCommandConstants()
    {
        const string file = "src.managed/MediaViewer.Chrome/IslandHost.cs";
        string text = SourceText.StripComments(Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.cs"));
        string body = SourceText.Block(text, @"internal\s+static\s+class\s+Command\b");
        var list = new List<Constant>();
        foreach (Match m in Regex.Matches(body, @"public\s+const\s+int\s+(\w+)\s*=\s*([^;]+);"))
            list.Add(new Constant(file, "Command", m.Groups[1].Value, SourceText.ParseInt(m.Groups[2].Value)));
        return list;
    }

    // Every `enum XxxCommand { static let name: Int32 = N }` in the Swift chromes.
    internal static List<Constant> SwiftCommandConstants()
    {
        var list = new List<Constant>();
        string root = Repo.Path("src.swift");
        foreach (string path in Directory.EnumerateFiles(root, "*.swift", SearchOption.AllDirectories)
                                         .Where(p => !p.Contains($"{Path.DirectorySeparatorChar}.build{Path.DirectorySeparatorChar}"))
                                         .OrderBy(p => p, StringComparer.Ordinal))
        {
            string text = SourceText.StripComments(File.ReadAllText(path).Replace("\r\n", "\n"));
            foreach (Match e in Regex.Matches(text, @"\benum\s+(\w*Command)\s*\{"))
            {
                string owner = e.Groups[1].Value;
                string body = SourceText.Block(text, @"\benum\s+" + owner + @"\s*\{");
                foreach (Match m in Regex.Matches(body, @"static\s+let\s+(\w+)\s*:\s*Int32\s*=\s*(\d+)"))
                {
                    list.Add(new Constant(Path.GetRelativePath(Repo.Root, path).Replace('\\', '/'), owner,
                                          m.Groups[1].Value, int.Parse(m.Groups[2].Value)));
                }
            }
        }
        return list;
    }

    // command_id names a constant may stand for: its own name, its enum's prefix
    // (TrimCommand.keyframe -> trim_keyframe), or an alias.
    private static IEnumerable<string> Candidates(Constant c)
    {
        string snake = SourceText.Snake(c.Name);
        yield return snake;
        string prefix = c.Owner.EndsWith("Command", StringComparison.Ordinal) ? c.Owner[..^"Command".Length] : "";
        if (prefix.Length > 0) yield return SourceText.Snake(prefix) + "_" + snake;
        if (Aliases.TryGetValue(c.Name, out string? alias)) yield return alias;
    }

    // null when the constant resolves to its own value; else why not.
    private static string? Check(Constant c, bool allowChromeCommands)
    {
        var mismatches = new List<string>();
        foreach (string name in Candidates(c))
        {
            if (Core.CommandId.TryGetValue(name, out int id))
            {
                if (id == c.Value) return null;
                mismatches.Add($"command_id::{name} is {id}");
            }
            if (allowChromeCommands && Core.ChromeCommand.TryGetValue("chrome_cmd_" + name, out int cmd))
            {
                if (cmd == c.Value) return null;
                mismatches.Add($"chrome_cmd_{name} is {cmd}");
            }
        }
        string where = $"{c.File}: {c.Owner}.{c.Name} = {c.Value}";
        return mismatches.Count > 0
            ? $"{where}, but {string.Join(", ", mismatches)}"
            : $"{where} names no command_id{(allowChromeCommands ? " or chrome_command" : "")} " +
              "(use the core's name, or add an alias in CommandIdTests)";
    }

    [Fact]
    public void CommandsHeaderParses()
    {
        // Spot checks against the values commands.h comments pin.
        Assert.Equal(12, Core.CommandId["toggle_gallery"]);
        Assert.Equal(21, Core.CommandId["back"]);
        Assert.Equal(76, Core.CommandId["palette"]);
        Assert.Equal(0x100, Core.Key["space"]);
        Assert.Equal(0x11A, Core.Key["f12"]);
        Assert.Equal(0x11B, Core.Key["numpad0"]);
        Assert.Equal(1001, Core.ChromeCommand["chrome_cmd_rebind"]);
        foreach (int id in Core.ReservedNotifications)
            Assert.DoesNotContain(id, Core.CommandId.Values);
    }

    [Fact]
    public void CSharpCommandConstantsMatchTheCore()
    {
        List<Constant> constants = CSharpCommandConstants();
        Assert.True(constants.Count > 80, $"parsed only {constants.Count} constants from IslandHost.cs");
        var failures = constants.Select(c => Check(c, allowChromeCommands: true)).OfType<string>().ToList();
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }

    [Fact]
    public void EveryChromeCommandHasACSharpConstant()
    {
        var values = CSharpCommandConstants().Select(c => c.Value).ToHashSet();
        var missing = Core.ChromeCommand.Where(kv => !values.Contains(kv.Value))
                                        .Select(kv => $"{kv.Key} = {kv.Value}").ToList();
        Assert.True(missing.Count == 0,
                    "chrome_host.h commands the island cannot send or recognise:\n" + string.Join("\n", missing));
    }

    [Fact]
    public void CSharpConstantsAreUnique()
    {
        var dupes = CSharpCommandConstants().GroupBy(c => c.Value).Where(g => g.Count() > 1)
                                            .Select(g => $"{g.Key}: {string.Join(", ", g.Select(c => c.Name))}").ToList();
        Assert.True(dupes.Count == 0, string.Join("\n", dupes));
    }

    [Fact]
    public void CSharpChecksumListsTheIdsChromeHostChecksums()
    {
        // Command.Checksum() and chrome_command_checksum() must hash the same ids
        // in the same order; the Probe run compares the numbers, this names the
        // id that moved.
        string cs = SourceText.StripComments(Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.cs"));
        string csList = SourceText.Block(cs, @"internal\s+static\s+int\s+Checksum\(\)\s*\{\s*int\[\]\s+ids\s*=");
        var byName = CSharpCommandConstants().ToDictionary(c => c.Name, c => c.Value);
        var csIds = Regex.Matches(csList, @"\b(\w+)\b").Select(m => byName[m.Groups[1].Value]).ToList();

        string cpp = SourceText.StripComments(Repo.Read("src", "shell", "chrome_host.h"));
        string cppList = SourceText.Block(cpp, @"chrome_command_checksum\(\)\s*noexcept\s*\{\s*constexpr\s+int\s+ids\[\]\s*=");
        var cppIds = Regex.Matches(cppList, @"\b(chrome_cmd_\w+)\b").Select(m => Core.ChromeCommand[m.Groups[1].Value]).ToList();

        Assert.Equal(cppIds, csIds);
    }

    [Fact]
    public void SwiftCommandConstantsMatchTheCore()
    {
        List<Constant> constants = SwiftCommandConstants();
        // ViewCommand, EditCommand and TrimCommand today; a new enum is picked up.
        Assert.Contains(constants, c => c.Owner == "ViewCommand");
        Assert.Contains(constants, c => c.Owner == "EditCommand");
        Assert.Contains(constants, c => c.Owner == "TrimCommand");
        var failures = constants.Select(c => Check(c, allowChromeCommands: false)).OfType<string>().ToList();
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }

    [Fact]
    public void SwiftShortcutLookupsNameRealCommands()
    {
        // CommandBarView's key("Zoom in") looks a shortcut up by its palette name
        // (SettingsStore.keyLabel); a renamed command_info would blank the label.
        string path = Repo.Path("src.swift", "MediaViewerChrome", "Sources", "MediaViewerChrome");
        var failures = new List<string>();
        int seen = 0;
        foreach (string file in Directory.EnumerateFiles(path, "*.swift"))
        {
            string text = File.ReadAllText(file);
            foreach (Match m in Regex.Matches(text, @"(?:\bkey|SettingsStore\.keyLabel)\(""([^""]+)""\)"))
            {
                seen++;
                if (!Core.CommandNames.ContainsKey(m.Groups[1].Value))
                    failures.Add($"{Path.GetFileName(file)}: \"{m.Groups[1].Value}\" is no command_info name");
            }
        }
        Assert.True(seen > 5, $"found only {seen} shortcut lookups");
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }
}
