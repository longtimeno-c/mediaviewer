// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace MediaViewer.Shared;

/// <summary>A face in a person's sheet: person_review_json's row (person_faces_json's, with the
/// review fields at their "nothing to go by" values, from an older pack).</summary>
public sealed record ReviewFace(
    ulong Face, string Path, long PtsMs, double[] Box, double Score, bool Pinned, double Quality,
    double Support, double Rival, ulong RivalId, string RivalName, bool Check, bool DoubtfulPin);

/// <summary>Where rejected faces went: a person and how many of them.</summary>
public sealed record RejectedTo(ulong Id, string Name, long Faces);

/// <summary>faces_reject_json's answer.</summary>
/// <summary>A face that left on "Not …" (the clip's frames that went too included): where it was, and its pin.</summary>
public sealed record RejectedFace(ulong Face, ulong From, bool Pinned);

public sealed record RejectOutcome(long Unassigned, long Spread, IReadOnlyList<RejectedTo> To)
{
    /// <summary>Every face that left; empty from a pack before 2026-11-02's "faces".</summary>
    public IReadOnlyList<RejectedFace> Left { get; init; } = Array.Empty<RejectedFace>();
}

/// <summary>An unassigned face clear enough to name, and who it most resembles (0: nobody near).</summary>
public sealed record UnknownFace(
    ulong Face, string Path, long PtsMs, double[] Box, double Quality, ulong Suggest, string SuggestName, double Score);

public sealed record UnknownPage(long Total, IReadOnlyList<UnknownFace> Faces);

/// <summary>"Might also be": an unnamed person who resembles this one.</summary>
public sealed record SuggestedPerson(ulong Id, double Score, long Faces, ulong CoverFace, string CoverPath, long CoverMs, double[] CoverBox);

/// <summary>"Might also be": an unknown face that resembles this person.</summary>
public sealed record SuggestedFace(ulong Face, string Path, long PtsMs, double[] Box, double Score);

public sealed record Suggestions(IReadOnlyList<SuggestedPerson> People, IReadOnlyList<SuggestedFace> Faces)
{
    public bool IsEmpty => People.Count == 0 && Faces.Count == 0;
    public static readonly Suggestions None = new(Array.Empty<SuggestedPerson>(), Array.Empty<SuggestedFace>());
}

/// <summary>
/// A face "Refine faces" moves out (person_refine_json): where it would go (dry) or went.
/// <see cref="To"/> 0 is nobody; <see cref="Group"/> above 0 (a look only) starts a new person.
/// </summary>
public sealed record RefineFace(ulong Face, string Path, long PtsMs, double[] Box, string Why, ulong To, long Group, string ToName);

/// <summary>person_refine_json's answer.</summary>
public sealed record RefineOutcome(bool Dry, long Removed, IReadOnlyList<RefineFace> Faces)
{
    /// <summary>How many faces left (or would): the list when the pack sent one, else its count.</summary>
    public long Count => Faces.Count > 0 ? Faces.Count : Removed;
}

/// <summary>A person "Merge duplicates" merges, with its cover and the token person_merge_undo takes (0: none).</summary>
public sealed record DedupePerson(ulong Id, string Name, long Faces, ulong CoverFace, string CoverPath, long CoverMs, double[] CoverBox,
                                  ulong Undo);

/// <summary>One merge: <see cref="From"/> into the person who survives.</summary>
public sealed record DedupeMerge(DedupePerson Into, IReadOnlyList<DedupePerson> From);

/// <summary>people_dedupe_json's answer.</summary>
public sealed record DedupeOutcome(bool Dry, long Merged, long Moved, IReadOnlyList<DedupeMerge> Merges)
{
    /// <summary>A look with nothing to do: no merge, and no face the check would move.</summary>
    public bool IsEmpty => Merges.Count == 0 && Merged == 0 && Moved == 0;
}

/// <summary>A person a face can move to (the "Move to…" picker).</summary>
public sealed record PickPerson(ulong Id, string Name, long Faces);

/// <summary>
/// Correcting people (docs/design/17 "People"): the JSON the AI pack's correction
/// calls answer, parsed, and the sheet's words. Pure, no UI, so Chrome.Tests runs
/// it; the Mac's copy of the words is PeopleView.swift's. Nothing here logs.
/// </summary>
public static class PeopleWords
{
    public const string CheckNotSure = "Not sure";
    public const string CheckDoubtfulPin = "Pinned, but unlike the others";
    public const string SortDoubtful = "Doubtful first";
    public const string SortClearest = "Clearest";
    public const string MoveTo = "Move to…";
    public const string Unpin = "Unpin";
    public const string Undo = "Undo";
    public const string ShowPhoto = "Show photo";
    public const string ShowMore = "Show more";
    public const string Yes = "Yes";
    public const string Merge = "Merge";
    public const string Merged = "Merged";
    public const string Undone = "Undone";
    public const string OtherFaces = "Other faces";
    public const string CouldNotChange = "Couldn't change that. Try again.";
    public const string Apply = "Apply";
    public const string Cancel = "Cancel";
    public const string EveryFaceMatches = "Every face matches";
    public const string CouldNotRefine = "Couldn't refine";
    public const string NoDuplicates = "No duplicates found, and every face matches.";
    public const string CouldNotDedupe = "Couldn't check for duplicates. Try again.";

    // ---- the sheet's words --------------------------------------------------------------

    /// <summary>A check tile's caption: who it looks like, a pin that disagrees, or neither.</summary>
    public static string CheckCaption(ReviewFace f) =>
        f.RivalName.Length > 0 ? $"Looks like {f.RivalName}?" : f.DoubtfulPin ? CheckDoubtfulPin : CheckNotSure;

    public static string CheckHeader(int count) => $"Check these ({count})";

    /// <summary>"Yes, this is Anna"; "Yes, same person" for someone unnamed.</summary>
    public static string YesLabel(string name) => name.Length > 0 ? $"Yes, this is {name}" : "Yes, same person";

    /// <summary>"Not Anna"; "Not this person" for someone unnamed.</summary>
    public static string NotLabel(string name) => name.Length > 0 ? $"Not {name}" : "Not this person";

    /// <summary>"Might also be Anna"; "Might also be this person" for someone unnamed.</summary>
    public static string MightAlsoBe(string name) => name.Length > 0 ? $"Might also be {name}" : "Might also be this person";

    /// <summary>The People grid's entry: "Unknown faces (12)".</summary>
    public static string UnknownEntry(long total) => $"Unknown faces ({total:N0})";

    /// <summary>An unknown face's caption: "Anna?" (an unnamed suggestion: "Unnamed person?"); "" for none.</summary>
    public static string SuggestCaption(ulong suggest, string name) =>
        suggest == 0 ? "" : name.Length > 0 ? $"{name}?" : "Unnamed person?";

    public static string Faces(long n) => n == 1 ? "1 face" : $"{n:N0} faces";

    public static string Photos(long n) => n == 1 ? "1 photo" : $"{n:N0} photos";

    private static string Who(string name) => name.Length > 0 ? name : "someone unnamed";

    /// <summary>
    /// After "Not …": where the faces went. "2 now with Carl, 1 unknown", plus
    /// ", 3 frames of the same clip went too". <paramref name="rejected"/> is the
    /// selection's size, for an answer that names nowhere.
    /// </summary>
    public static string RejectSummary(RejectOutcome o, int rejected)
    {
        var parts = new List<string>();
        foreach (RejectedTo t in o.To)
        {
            if (t.Faces > 0) parts.Add($"{t.Faces:N0} now with {Who(t.Name)}");
        }
        if (o.Unassigned > 0) parts.Add($"{o.Unassigned:N0} unknown");
        string line = parts.Count > 0 ? string.Join(", ", parts) : Removed(rejected);
        if (o.Spread > 0)
            line += o.Spread == 1 ? ", 1 frame of the same clip went too" : $", {o.Spread:N0} frames of the same clip went too";
        return line;
    }

    /// <summary>"1 face removed" / "3 faces removed": a reject without an answer (an older pack).</summary>
    public static string Removed(int n) => n == 1 ? "1 face removed" : $"{n:N0} faces removed";

    /// <summary>"Moved 2 to Carl".</summary>
    public static string Moved(int n, string name) => $"Moved {n:N0} to {Who(name)}";

    /// <summary>
    /// Unknown faces said yes to, each to its own suggestion: "Moved 2 to Carl",
    /// "Moved 2 to Carl, 1 to Ben". <paramref name="moved"/> is in the order shown.
    /// </summary>
    public static string MovedEach(IReadOnlyList<(string Name, int Count)> moved)
    {
        if (moved.Count == 0) return "";
        if (moved.Count == 1) return Moved(moved[0].Count, moved[0].Name);
        return "Moved " + string.Join(", ", moved.Select(m => $"{m.Count:N0} to {Who(m.Name)}"));
    }

    /// <summary>"Added 1 to Anna" (a suggested face said yes to).</summary>
    public static string Added(int n, string name) => $"Added {n:N0} to {(name.Length > 0 ? name : "this person")}";

    // ---- Refine faces / Merge duplicates in detail (docs/design/17) ----------------------

    /// <summary>A face in Refine's look: "To Ben", "To nobody", or "A new person" (a group of its own).</summary>
    public static string RefineCaption(RefineFace f) =>
        f.Group > 0 ? "A new person" : f.To == 0 ? "To nobody" : $"To {Who(f.ToName)}";

    /// <summary>Refine's look: "3 faces would leave Anna" ("this person" for someone unnamed).</summary>
    public static string RefineLookHeader(long n, string name) =>
        $"{Faces(n)} would leave {(name.Length > 0 ? name : "this person")}";

    /// <summary>After Refine applied: "1 face left" / "3 faces left"; nothing moved: "Every face matches".</summary>
    public static string RefineLeft(long n) => n == 0 ? EveryFaceMatches : n == 1 ? "1 face left" : $"{n:N0} faces left";

    /// <summary>Undo of a Refine: every face that left, once each, to go back to the person unpinned.</summary>
    public static List<ulong> RefinePutBack(RefineOutcome o) => o.Faces.Select(f => f.Face).Where(f => f != 0).Distinct().ToList();

    /// <summary>"1 person" / "3 people".</summary>
    public static string People(long n) => n == 1 ? "1 person" : $"{n:N0} people";

    /// <summary>"Merge 2 people into Anna": the person who survives counted with those merging into it.</summary>
    public static string MergeLine(DedupeMerge m) =>
        $"Merge {People(m.From.Count + 1)} into {(m.Into.Name.Length > 0 ? m.Into.Name : "one")}";

    /// <summary>A cover tile in Merge duplicates' look: "Anna · 40 faces", "Unnamed · 4 faces".</summary>
    public static string DedupeCaption(DedupePerson p) => $"{(p.Name.Length > 0 ? p.Name : "Unnamed")} · {Faces(p.Faces)}";

    /// <summary>Merge duplicates' look: "3 people would merge, 12 faces would move."</summary>
    public static string DedupeLookHeader(DedupeOutcome o)
    {
        long merged = o.Merged > 0 ? o.Merged : o.Merges.Sum(m => (long)m.From.Count);
        var parts = new List<string>();
        if (merged > 0) parts.Add($"{People(merged)} would merge");
        if (o.Moved > 0) parts.Add($"{Faces(o.Moved)} would move");
        return parts.Count == 0 ? NoDuplicates : string.Join(", ", parts) + ".";
    }

    /// <summary>After Merge duplicates: "3 people merged, 12 faces moved." (the Mac's PeopleView words).</summary>
    public static string DedupeSummary(long merged, long moved) =>
        merged == 0 && moved == 0 ? NoDuplicates : $"{People(merged)} merged, {Faces(moved)} moved.";

    /// <summary>Undo of Merge duplicates: every merge's token (none of the zeros), last first.</summary>
    public static List<ulong> DedupeUndoTokens(DedupeOutcome o)
    {
        var tokens = o.Merges.SelectMany(m => m.From).Select(p => p.Undo).Where(t => t != 0).ToList();
        tokens.Reverse();
        return tokens;
    }

    // ---- order -------------------------------------------------------------------------

    /// <summary>
    /// The sheet's two groups in the chosen order: the faces worth a look
    /// (check), then the rest. "Doubtful first" keeps the pack's order (doubtful
    /// first already); "Clearest" sorts each group by quality, best first (stable).
    /// </summary>
    public static (List<ReviewFace> Check, List<ReviewFace> Others) Group(IEnumerable<ReviewFace> faces, bool clearest)
    {
        IEnumerable<ReviewFace> ordered = clearest ? faces.OrderByDescending(f => f.Quality) : faces;
        var check = new List<ReviewFace>();
        var rest = new List<ReviewFace>();
        foreach (ReviewFace f in ordered) (f.Check ? check : rest).Add(f);
        return (check, rest);
    }

    /// <summary>
    /// The sheets' ← →: the next face from <paramref name="cursor"/> (-1: none
    /// yet, so the first, or the last going left), held at the ends.
    /// </summary>
    public static int Step(int cursor, int count, int step)
    {
        if (count <= 0) return -1;
        if (cursor < 0 || cursor >= count) return step < 0 ? count - 1 : 0;
        return Math.Clamp(cursor + step, 0, count - 1);
    }

    /// <summary>"Show more" under unknown faces: how many are left past <paramref name="shown"/>.</summary>
    public static string ShowMoreLabel(int shown, long total) =>
        total > shown ? $"{ShowMore} ({total - shown:N0} more)" : ShowMore;

    /// <summary>
    /// Undo for faces taken out of a person: put back with the pin each had
    /// before. Pinned and unpinned go back as two calls.
    /// </summary>
    public static (List<ulong> Pinned, List<ulong> Unpinned) ByPin(IEnumerable<ulong> faces, IReadOnlySet<ulong> pinnedBefore)
    {
        var pinned = new List<ulong>();
        var unpinned = new List<ulong>();
        foreach (ulong f in faces) (pinnedBefore.Contains(f) ? pinned : unpinned).Add(f);
        return (pinned, unpinned);
    }

    /// <summary>
    /// The "Move to…" picker's list: named people first (by name), then the
    /// unnamed by faces; <paramref name="query"/> narrows by name (any part,
    /// any case). <paramref name="except"/> is the person the faces are in.
    /// </summary>
    public static List<PickPerson> PickerOrder(IEnumerable<PickPerson> people, string query, ulong except)
    {
        string q = query.Trim();
        return people
            .Where(p => p.Id != except)
            .Where(p => q.Length == 0 || p.Name.Contains(q, StringComparison.CurrentCultureIgnoreCase))
            .OrderBy(p => p.Name.Length > 0 ? 0 : 1)
            .ThenBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase)
            .ThenByDescending(p => p.Faces)
            .ToList();
    }

    /// <summary>A picker row: the name, or "Unnamed (3 photos)".</summary>
    public static string PickName(PickPerson p) => p.Name.Length > 0 ? p.Name : $"Unnamed ({Photos(p.Faces)})";

    // ---- the pack's JSON ---------------------------------------------------------------

    /// <summary>person_review_json, or person_faces_json from an older pack (no review fields).</summary>
    public static List<ReviewFace> ParseReview(string json)
    {
        var list = new List<ReviewFace>();
        using JsonDocument doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Array) return list;
        foreach (JsonElement f in doc.RootElement.EnumerateArray())
        {
            ulong id = U64(f, "face");
            if (id == 0) continue;
            list.Add(new ReviewFace(
                id, Str(f, "path"), I64(f, "pts_ms", -1), Box(f, "box"), Dbl(f, "score", 0),
                Bool(f, "pinned"), Dbl(f, "quality", 0), Dbl(f, "support", -1), Dbl(f, "rival", -1),
                U64(f, "rival_id"), Str(f, "rival_name"), Bool(f, "check"), Bool(f, "doubtful_pin")));
        }
        return list;
    }

    public static RejectOutcome ParseReject(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement r = doc.RootElement;
        if (r.ValueKind != JsonValueKind.Object) return new RejectOutcome(0, 0, Array.Empty<RejectedTo>());
        var to = new List<RejectedTo>();
        if (r.TryGetProperty("to", out JsonElement a) && a.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement t in a.EnumerateArray())
            {
                ulong id = U64(t, "id");
                if (id != 0) to.Add(new RejectedTo(id, Str(t, "name"), I64(t, "faces", 0)));
            }
        }
        var left = new List<RejectedFace>();
        if (r.TryGetProperty("faces", out JsonElement fs) && fs.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement f in fs.EnumerateArray())
            {
                ulong face = U64(f, "face");
                if (face != 0) left.Add(new RejectedFace(face, U64(f, "from"), Bool(f, "pinned")));
            }
        }
        return new RejectOutcome(I64(r, "unassigned", 0), I64(r, "spread", 0), to) { Left = left };
    }

    /// <summary>Undo of a reject: each (person, pinned) group of faces to put back, the clip's frames included.</summary>
    public static IReadOnlyList<(ulong Person, bool Pinned, List<ulong> Faces)> PutBackGroups(RejectOutcome o) =>
        o.Left.GroupBy(f => (f.From, f.Pinned))
            .OrderBy(g => g.Key.From).ThenBy(g => g.Key.Pinned)
            .Select(g => (g.Key.From, g.Key.Pinned, g.Select(f => f.Face).ToList()))
            .ToList();

    public static UnknownPage ParseUnknown(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement r = doc.RootElement;
        var faces = new List<UnknownFace>();
        if (r.ValueKind != JsonValueKind.Object) return new UnknownPage(0, faces);
        if (r.TryGetProperty("faces", out JsonElement a) && a.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement f in a.EnumerateArray())
            {
                ulong id = U64(f, "face");
                if (id == 0) continue;
                faces.Add(new UnknownFace(id, Str(f, "path"), I64(f, "pts_ms", -1), Box(f, "box"), Dbl(f, "quality", 0),
                                          U64(f, "suggest"), Str(f, "suggest_name"), Dbl(f, "score", 0)));
            }
        }
        return new UnknownPage(Math.Max(I64(r, "total", 0), faces.Count), faces);
    }

    public static Suggestions ParseSuggest(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement r = doc.RootElement;
        if (r.ValueKind != JsonValueKind.Object) return Suggestions.None;
        var people = new List<SuggestedPerson>();
        var faces = new List<SuggestedFace>();
        if (r.TryGetProperty("people", out JsonElement pa) && pa.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement p in pa.EnumerateArray())
            {
                ulong id = U64(p, "id");
                if (id == 0) continue;
                people.Add(new SuggestedPerson(id, Dbl(p, "score", 0), I64(p, "faces", 0), U64(p, "cover_face"),
                                               Str(p, "cover_path"), I64(p, "cover_ms", -1), Box(p, "cover_box")));
            }
        }
        if (r.TryGetProperty("faces", out JsonElement fa) && fa.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement f in fa.EnumerateArray())
            {
                ulong id = U64(f, "face");
                if (id == 0) continue;
                faces.Add(new SuggestedFace(id, Str(f, "path"), I64(f, "pts_ms", -1), Box(f, "box"), Dbl(f, "score", 0)));
            }
        }
        return new Suggestions(people, faces);
    }

    public static RefineOutcome ParseRefine(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement r = doc.RootElement;
        var faces = new List<RefineFace>();
        if (r.ValueKind != JsonValueKind.Object) return new RefineOutcome(false, 0, faces);
        if (r.TryGetProperty("faces", out JsonElement a) && a.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement f in a.EnumerateArray())
            {
                ulong id = U64(f, "face");
                if (id == 0) continue;
                faces.Add(new RefineFace(id, Str(f, "path"), I64(f, "pts_ms", -1), Box(f, "box"), Str(f, "why"),
                                         U64(f, "to"), I64(f, "group", 0), Str(f, "to_name")));
            }
        }
        return new RefineOutcome(Bool(r, "dry"), I64(r, "removed", 0), faces);
    }

    public static DedupeOutcome ParseDedupe(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement r = doc.RootElement;
        var merges = new List<DedupeMerge>();
        if (r.ValueKind != JsonValueKind.Object) return new DedupeOutcome(false, 0, 0, merges);
        if (r.TryGetProperty("merges", out JsonElement a) && a.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement m in a.EnumerateArray())
            {
                ulong into = U64(m, "into");
                if (into == 0) continue;
                var from = new List<DedupePerson>();
                if (m.TryGetProperty("from", out JsonElement fa) && fa.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement f in fa.EnumerateArray())
                    {
                        ulong id = U64(f, "id");
                        if (id != 0) from.Add(DedupeOne(f, id, "name", "faces"));
                    }
                }
                if (from.Count > 0) merges.Add(new DedupeMerge(DedupeOne(m, into, "into_name", "into_faces"), from));
            }
        }
        return new DedupeOutcome(Bool(r, "dry"), I64(r, "merged", 0), I64(r, "moved", 0), merges);
    }

    private static DedupePerson DedupeOne(JsonElement e, ulong id, string name, string faces) =>
        new(id, Str(e, name), I64(e, faces, 0), U64(e, "cover_face"), Str(e, "cover_path"), I64(e, "cover_ms", -1),
            Box(e, "cover_box"), U64(e, "undo"));

    /// <summary>people_json's rows as picker entries.</summary>
    public static List<PickPerson> ParsePeople(string json)
    {
        var list = new List<PickPerson>();
        using JsonDocument doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Array) return list;
        foreach (JsonElement p in doc.RootElement.EnumerateArray())
        {
            ulong id = U64(p, "id");
            if (id != 0) list.Add(new PickPerson(id, Str(p, "name"), I64(p, "faces", 0)));
        }
        return list;
    }

    /// <summary>A [x, y, w, h] box in 0..1 of the picture; the whole picture when missing.</summary>
    public static double[] Box(JsonElement e, string key)
    {
        if (!e.TryGetProperty(key, out JsonElement b) || b.ValueKind != JsonValueKind.Array || b.GetArrayLength() != 4)
            return new[] { 0.0, 0.0, 1.0, 1.0 };
        var box = new double[4];
        int i = 0;
        foreach (JsonElement v in b.EnumerateArray()) box[i++] = v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;
        return box;
    }

    private static ulong U64(JsonElement e, string key) =>
        e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number && v.TryGetUInt64(out ulong u) ? u : 0;

    private static long I64(JsonElement e, string key, long fallback) =>
        e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out long l) ? l : fallback;

    private static double Dbl(JsonElement e, string key, double fallback) =>
        e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : fallback;

    private static bool Bool(JsonElement e, string key) =>
        e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.True;

    private static string Str(JsonElement e, string key) =>
        e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
}
