// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #153: the Windows Settings screen's half of a key remap.
//
// The island captures a key (IslandHost.Settings.cs EncodeCaptureKey), packs it with
// the row and the modifiers into chrome_cmd_rebind's argument, and native unpacks it
// (main.cpp), rebinds, and persists. The persist / reload round trip itself is native
// and tested in mv_tests (tests/test_command_contract.cpp "[remap]"); these pin the
// chrome's side of the wire: the key codes it hand-encodes and the packing.
using System.Text.RegularExpressions;

namespace MediaViewer.Chrome.Tests;

public class KeyRemapTests
{
    private static string SettingsSource() =>
        SourceText.StripComments(Repo.Read("src.managed", "MediaViewer.Chrome", "IslandHost.Settings.cs"));

    // VirtualKey names whose commands.h key is not their snake-case spelling.
    private static readonly Dictionary<string, string> KeyAliases = new(StringComparer.Ordinal)
    {
        ["Back"] = "backspace",
        ["Delete"] = "del",
    };

    [Fact]
    public void CaptureEncodesNamedKeysAsCommandsHDoes()
    {
        string body = SourceText.Block(SettingsSource(), @"private\s+static\s+int\s+EncodeCaptureKey\([^)]*\)");
        var arms = Regex.Matches(body, @"VirtualKey\.(\w+)\s*=>\s*(0x[0-9A-Fa-f]+)\s*,").ToList();
        Assert.True(arms.Count >= 26, $"found only {arms.Count} named-key arms in EncodeCaptureKey");
        var failures = new List<string>();
        foreach (Match arm in arms)
        {
            string vk = arm.Groups[1].Value;
            int encoded = SourceText.ParseInt(arm.Groups[2].Value);
            string name = KeyAliases.TryGetValue(vk, out string? alias) ? alias
                        : Regex.IsMatch(vk, @"^F\d+$") ? vk.ToLowerInvariant()  // key::f1 .. f12
                        : SourceText.Snake(vk);
            if (!Core.Key.TryGetValue(name, out int expected))
                failures.Add($"VirtualKey.{vk} => {arm.Groups[2].Value}: commands.h has no key::{name}");
            else if (expected != encoded)
                failures.Add($"VirtualKey.{vk} => {arm.Groups[2].Value}, but key::{name} is 0x{expected:X}");
        }
        // Every named key the table can bind (space .. f12) has an arm.
        var covered = arms.Select(a => SourceText.ParseInt(a.Groups[2].Value)).ToHashSet();
        foreach (var (name, value) in Core.Key)
        {
            if (value >= Core.Key["space"] && value <= Core.Key["f12"] && !covered.Contains(value))
                failures.Add($"key::{name} (0x{value:X}) cannot be captured in Settings");
        }
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }

    [Fact]
    public void CaptureEncodesTheKeypadDigitsAfterF12()
    {
        Match m = Regex.Match(SettingsSource(),
                              @"return\s+(0x[0-9A-Fa-f]+)\s*\+\s*\(v\s*-\s*\(int\)VirtualKey\.NumberPad0\)");
        Assert.True(m.Success, "EncodeCaptureKey's keypad arm moved");
        Assert.Equal(Core.Key["numpad0"], SourceText.ParseInt(m.Groups[1].Value));
        Assert.Equal(Core.Key["numpad0"] + 9, Core.Key["numpad9"]);
    }

    [Fact]
    public void CaptureModifierBitsAreCommandsHMods()
    {
        string text = SettingsSource();
        string header = Repo.Read("src", "shell", "commands.h");
        int Bit(string pattern) => SourceText.ParseInt(Regex.Match(text, pattern).Groups[1].Value);
        int Mod(string name) =>
            1 << SourceText.ParseInt(Regex.Match(header, name + @"\s*=\s*1\s*<<\s*(\d+)").Groups[1].Value);
        Assert.Equal(Mod("mod_ctrl"), Bit(@"Down\(VirtualKey\.Control\)\)\s*mods\s*\|=\s*(\d+)"));
        Assert.Equal(Mod("mod_shift"), Bit(@"Down\(VirtualKey\.Shift\)\)\s*mods\s*\|=\s*(\d+)"));
        Assert.Equal(Mod("mod_alt"), Bit(@"Down\(VirtualKey\.Menu\)\)\s*mods\s*\|=\s*(\d+)"));
    }

    [Fact]
    public void RebindPackingMatchesNativeUnpacking()
    {
        // C#: (row & 0xFF) | (k << 8) | ((mods & 7) << 20)
        Match cs = Regex.Match(SettingsSource(),
            @"int\s+packed\s*=\s*\(_capturingRow\s*&\s*(0x[0-9A-Fa-f]+)\)\s*\|\s*\(k\s*<<\s*(\d+)\)\s*\|\s*\(\(mods\s*&\s*(\d+)\)\s*<<\s*(\d+)\)");
        Assert.True(cs.Success, "the rebind packing in OnSettingsKeyDown moved");
        int rowMask = SourceText.ParseInt(cs.Groups[1].Value);
        int keyShift = int.Parse(cs.Groups[2].Value);
        int modMask = int.Parse(cs.Groups[3].Value);
        int modShift = int.Parse(cs.Groups[4].Value);

        // main.cpp: case chrome_cmd_rebind
        string main = Repo.Read("src", "shell", "main.cpp");
        string arm = main[main.IndexOf("case mv::shell::chrome_cmd_rebind:", StringComparison.Ordinal)..];
        Match row = Regex.Match(arm, @"row\s*=\s*packed\s*&\s*(0x[0-9A-Fa-f]+)");
        Match k = Regex.Match(arm, @"\(packed\s*>>\s*(\d+)\)\s*&\s*(0x[0-9A-Fa-f]+)");
        Match mods = Regex.Match(arm, @"mods\s*=\s*static_cast<std::uint8_t>\(\(packed\s*>>\s*(\d+)\)\s*&\s*(\d+)\)");
        Assert.True(row.Success && k.Success && mods.Success, "main.cpp's chrome_cmd_rebind unpacking moved");
        Assert.Equal(rowMask, SourceText.ParseInt(row.Groups[1].Value));
        Assert.Equal(keyShift, int.Parse(k.Groups[1].Value));
        Assert.Equal(modShift, int.Parse(mods.Groups[1].Value));
        Assert.Equal(modMask, int.Parse(mods.Groups[2].Value));
        int keyMask = SourceText.ParseInt(k.Groups[2].Value);

        // The fields do not overlap, and every key commands.h has fits its field.
        Assert.Equal(0, (rowMask << 0) & (keyMask << keyShift));
        Assert.Equal(0, (keyMask << keyShift) & (modMask << modShift));
        Assert.True(Core.Key["count"] - 1 <= keyMask, $"key::count {Core.Key["count"]} overflows the 0x{keyMask:X} key field");
        Assert.True(7 <= modMask, "three modifier bits");
        // The row field: mv_tests checks default_bindings().size() against it.
        Assert.Equal(0xFF, rowMask);
    }
}
