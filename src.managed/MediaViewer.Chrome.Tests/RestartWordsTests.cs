// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #510: the add-on restart card says the same on both platforms. The C# words
// (src.managed/Shared/RestartWords.cs) are run here; the Swift copy
// (RestartNoticeView.swift's enum RestartWords) is read as text, and every
// string the C# class has must be one of its strings too, with each
// interpolation hole ({…} / \(…)) standing for any value.
using System.Text;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class RestartWordsTests
{
    [Fact]
    public void OneUpdateNamesItsVersion()
    {
        var one = new List<AddonUpdated> { new("Driftwire", "1.4.2") };
        Assert.Equal("Driftwire was updated to 1.4.2.", RestartWords.Title(one));
        Assert.Equal("Restart MediaViewer to use it. It opens again on this folder and file.", RestartWords.Body(1));
        Assert.Equal("Restart to finish updating Driftwire 1.4.2.", RestartWords.Marker(one));
        Assert.Equal("Driftwire was updated to 1.4.2. Restart MediaViewer to use it.", RestartWords.Tooltip(one));
    }

    [Fact]
    public void SeveralUpdatesMakeOneLine()
    {
        var three = new List<AddonUpdated> { new("Import", "1.3.0"), new("Local search", "0.4.0"), new("Places", "1.1.0") };
        Assert.Equal("Import, Local search and Places were updated.", RestartWords.Title(three));
        Assert.Equal("Restart MediaViewer to use them. It opens again on this folder and file.", RestartWords.Body(3));
        Assert.Equal("Restart to finish updating Import 1.3.0, Local search 0.4.0 and Places 1.1.0.", RestartWords.Marker(three));
        Assert.Equal("", RestartWords.Title(new List<AddonUpdated>()));
        Assert.Equal("", RestartWords.Marker(new List<AddonUpdated>()));
    }

    [Fact]
    public void WaitingSaysWhatItWaitsFor()
    {
        var what = new List<string> { RestartWords.Import, "a Driftwire transfer" };
        Assert.Equal("Waiting for an import and a Driftwire transfer to finish", RestartWords.WaitingTitle(what));
        Assert.Equal("Restart now. This stops an import and a Driftwire transfer.", RestartWords.AnywayTip(what));
    }

    [Fact]
    public void DriftwireNamesItsTransferAsTheCardExpects()
    {
        // The add-on's busy_post text reads inside "Waiting for … to finish".
        string engine = Repo.Read("src", "addons", "driftwire", "engine.h");
        Assert.Contains("kBusyWhat = \"a Driftwire transfer\"", engine);
    }

    [Fact]
    public void TheMacSaysTheSame()
    {
        string cs = SourceText.Block(SourceText.StripComments(Repo.Read("src.managed", "Shared", "RestartWords.cs")),
                                     @"public\s+static\s+class\s+RestartWords\b");
        string swiftFile = SourceText.StripComments(Repo.Read("src.swift", "MediaViewerChrome", "Sources", "MediaViewerChrome", "RestartNoticeView.swift"));
        int start = swiftFile.IndexOf("enum RestartWords", StringComparison.Ordinal);
        Assert.True(start >= 0, "RestartNoticeView.swift has no enum RestartWords");
        string swift = SourceText.Block(swiftFile[start..], @"enum\s+RestartWords\b");
        HashSet<string> mac = Literals(swift, swiftHoles: true);
        foreach (string s in Literals(cs, swiftHoles: false))
        {
            Assert.True(mac.Contains(s), $"RestartNoticeView.swift's RestartWords lacks \"{s}\" (RestartWords.cs has it)");
        }
    }

    // Every string literal, each interpolation hole as "{}". C#: "…", $"…" with
    // {…} holes; Swift: "…" with \(…) holes. A hole's own strings are skipped.
    private static HashSet<string> Literals(string code, bool swiftHoles)
    {
        var found = new HashSet<string>(StringComparer.Ordinal);
        for (int i = 0; i < code.Length; ++i)
        {
            if (code[i] != '"') continue;
            bool interpolated = !swiftHoles && i > 0 && code[i - 1] == '$';
            var text = new StringBuilder();
            int j = i + 1;
            for (; j < code.Length && code[j] != '"'; ++j)
            {
                char c = code[j];
                bool hole = swiftHoles ? c == '\\' && j + 1 < code.Length && code[j + 1] == '(' : interpolated && c == '{';
                if (hole)
                {
                    j = SkipHole(code, swiftHoles ? j + 1 : j);
                    text.Append("{}");
                }
                else if (c == '\\' && j + 1 < code.Length)
                {
                    text.Append(code[++j]);
                }
                else
                {
                    text.Append(c);
                }
            }
            found.Add(text.ToString());
            i = j;
        }
        return found;
    }

    // From an opening ( or {, the index of its match, past nested brackets and strings.
    private static int SkipHole(string code, int open)
    {
        int depth = 0;
        for (int k = open; k < code.Length; ++k)
        {
            char c = code[k];
            if (c == '"')
            {
                for (++k; k < code.Length && code[k] != '"'; ++k)
                {
                    if (code[k] == '\\') ++k;
                }
                continue;
            }
            if (c is '(' or '{') ++depth;
            else if (c is ')' or '}' && --depth == 0) return k;
        }
        return code.Length - 1;
    }
}
