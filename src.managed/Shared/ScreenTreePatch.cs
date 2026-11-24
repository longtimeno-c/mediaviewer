// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An add-on screen's new tree against the one on show: when all that moved is
// a text node's words or a link picture's progress (a transfer's line and
// bar, ten times a second), the window updates those in place instead of
// being built again, which flickered and lost the hover. Anything else
// (a node added, a style, a reveal token) is a rebuild, as before. Plain
// .NET, apart from any element, so the tests can drive it.
using System.Text.Json;

namespace MediaViewer.Shared;

public static class ScreenTreePatch
{
    /// <summary>A text node's new words (Kind "text") or a link's new progress (Kind "link").</summary>
    public readonly record struct Change(string Id, string Kind, string Text, double Value);

    /// <summary>
    /// What to update in place to show <paramref name="after"/> where
    /// <paramref name="before"/> is shown (empty: nothing), or null when the
    /// two differ in more than that and the screen must be built again.
    /// The screens' "revision" is not compared.
    /// </summary>
    public static List<Change>? Changes(JsonElement before, JsonElement after)
    {
        var changes = new List<Change>();
        var ids = new Dictionary<string, int>(StringComparer.Ordinal);
        if (!Same(before, after, changes, ids, top: true)) return null;
        // A changed node is found by its id: one that is missing or repeated
        // cannot be told apart, so the screen is built again.
        foreach (Change c in changes)
        {
            if (c.Id.Length == 0 || ids.GetValueOrDefault(c.Id) != 1) return null;
        }
        return changes;
    }

    private static bool Same(JsonElement a, JsonElement b, List<Change> changes, Dictionary<string, int> ids, bool top)
    {
        if (a.ValueKind != b.ValueKind) return false;
        switch (a.ValueKind)
        {
            case JsonValueKind.Object:
            {
                var pa = a.EnumerateObject().ToList();
                var pb = b.EnumerateObject().ToList();
                if (pa.Count != pb.Count) return false;
                string type = a.TryGetProperty("type", out JsonElement t) && t.ValueKind == JsonValueKind.String
                    ? t.GetString() ?? "" : "";
                string id = b.TryGetProperty("id", out JsonElement i) && i.ValueKind == JsonValueKind.String
                    ? i.GetString() ?? "" : "";
                if (!top && id.Length > 0) ids[id] = ids.GetValueOrDefault(id) + 1;
                for (int k = 0; k < pa.Count; ++k)
                {
                    if (pa[k].Name != pb[k].Name) return false;
                    string name = pa[k].Name;
                    if (top && name == "revision") continue;
                    JsonElement va = pa[k].Value, vb = pb[k].Value;
                    if (type == "text" && name == "text" && va.ValueKind == JsonValueKind.String &&
                        vb.ValueKind == JsonValueKind.String)
                    {
                        string? now = vb.GetString();
                        if (va.GetString() != now) changes.Add(new Change(id, "text", now ?? "", 0));
                        continue;
                    }
                    if (type == "link" && name == "value" && va.ValueKind == JsonValueKind.Number &&
                        vb.ValueKind == JsonValueKind.Number)
                    {
                        double now = vb.GetDouble();
                        if (va.GetDouble() != now) changes.Add(new Change(id, "link", "", now));
                        continue;
                    }
                    if (!Same(va, vb, changes, ids, top: false)) return false;
                }
                return true;
            }
            case JsonValueKind.Array:
            {
                if (a.GetArrayLength() != b.GetArrayLength()) return false;
                using var ea = a.EnumerateArray();
                using var eb = b.EnumerateArray();
                while (ea.MoveNext() && eb.MoveNext())
                {
                    if (!Same(ea.Current, eb.Current, changes, ids, top: false)) return false;
                }
                return true;
            }
            default:
                return a.GetRawText() == b.GetRawText();
        }
    }
}
