// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Generic;
using System.Linq;
namespace MediaViewer.Shared;

/// <summary>
/// docs/design/16 "Drag and drop into folders": the island's side of a drop on
/// a folder (IslandHost.DragDrop.cs). What a drop becomes is shell/file_drop.h's
/// rule, asked of native over WM_COPYDATA; this packs the question, reads the
/// answer and words it. Pure, so MediaViewer.Chrome.Tests checks it (and that
/// the refusal words are file_drop.h's own).
/// </summary>
public static class DropWords
{
    // WM_COPYDATA dwData: ask what a drop would do / do it (main.cpp).
    public const long VerdictCopyData = 0x4D560003;
    public const long DropCopyData = 0x4D560002;

    // Flags, as the Mac's DropFlags: 1 dragged from this window, 2 the copy
    // key (Ctrl), 4 the move key (Shift).
    public static int Flags(bool fromApp, bool ctrl, bool shift) =>
        (fromApp ? 1 : 0) | (ctrl ? 2 : 0) | (shift ? 4 : 0);

    /// <summary>flags, folder, then one path per line (main.cpp parse_drop_payload).</summary>
    public static string Payload(int flags, string target, IReadOnlyList<string> paths)
    {
        var parts = new List<string>(paths.Count + 2) { flags.ToString(System.Globalization.CultureInfo.InvariantCulture), target };
        parts.AddRange(paths);
        return string.Join("\n", parts);
    }

    public enum Verb { None = 0, Copy = 1, Move = 2 }

    public enum Refusal
    {
        None = 0, Nothing, ReadOnlyTarget, RemoteTarget, IntoItself, AlreadyThere, RemoteSource,
    }

    public readonly record struct Verdict(Verb Verb, Refusal Refusal)
    {
        public bool Allowed => Verb != Verb.None;
        public bool RefusedVisibly => Verb == Verb.None && Refusal is not (Refusal.None or Refusal.Nothing);
    }

    /// <summary>verb | refusal &lt;&lt; 8; anything unknown is no drop.</summary>
    public static Verdict Read(long raw)
    {
        int verb = (int)(raw & 0xFF);
        int refusal = (int)((raw >> 8) & 0xFF);
        Verb v = verb is >= 0 and <= 2 ? (Verb)verb : Verb.None;
        Refusal r = refusal is >= 0 and <= 6 ? (Refusal)refusal : Refusal.Nothing;
        if (v == Verb.None && r == Refusal.None) r = Refusal.Nothing;
        return new Verdict(v, r);
    }

    /// <summary>shell/file_drop.h drop_refusal_words, word for word.</summary>
    public static string RefusalText(Refusal r) => r switch
    {
        Refusal.ReadOnlyTarget => "Read-only: nothing can be added here",
        Refusal.RemoteTarget => "On another computer: read-only from here",
        Refusal.IntoItself => "Can't put a folder inside itself",
        Refusal.AlreadyThere => "Already in this folder",
        Refusal.RemoteSource => "A folder on another computer can't be copied here",
        _ => "",
    };

    /// <summary>The drag's caption over a target ("Move 3 items to Trip").</summary>
    public static string Caption(Verdict v, int count, string target)
    {
        if (!v.Allowed) return RefusalText(v.Refusal);
        string word = v.Verb == Verb.Move ? "Move" : "Copy";
        if (count <= 0) return $"{word} to {target}";
        return count == 1 ? $"{word} 1 item to {target}" : $"{word} {count} items to {target}";
    }

    /// <summary>One click selects a folder tile, two open it (Finder, Explorer).</summary>
    public static bool OpensTile(int tapCount) => tapCount >= 2;

    /// <summary>ApplyBrowse's bits: Up, Back, Forward.</summary>
    public static (bool Up, bool Back, bool Forward) Moves(int bits) =>
        ((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0);

    /// <summary>A folder rested on with a drag opens after this long (ms).</summary>
    public const int SpringLoadMs = 700;
}
