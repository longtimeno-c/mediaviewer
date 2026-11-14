// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace MediaViewer.Shared;

/// <summary>A volume on a drive, as the driftwire program lists it.</summary>
internal sealed record DriveVolume(string Label, long Size, long Used, long Folders, bool Mounted);

/// <summary>A drive on this PC (`host drives list --json`). `Name` is what the person types to
/// erase it (unique in the list); `Refused` says why it cannot be erased, empty when it can.</summary>
internal sealed record DriveDisk
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string Model { get; init; } = "";
    public long Size { get; init; }
    public long Used { get; init; }
    public long Folders { get; init; }
    public string Facts { get; init; } = "";
    public bool System { get; init; }
    public bool Internal { get; init; }
    public bool Image { get; init; }
    public string Smart { get; init; } = "";
    public string Refused { get; init; } = "";
    public IReadOnlyList<DriveVolume> Volumes { get; init; } = Array.Empty<DriveVolume>();
}

internal sealed record DriveMember(string Id, string Status, bool Missing);

/// <summary>A combined drive and its health.</summary>
internal sealed record DriveSet
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string Kind { get; init; } = "";   // mirror | span | stripe | parity
    public string State { get; init; } = "";  // healthy | rebuilding | degraded | offline | failing
    public long Percent { get; init; }
    public long Size { get; init; }
    public string Line { get; init; } = "";
    public IReadOnlyList<DriveMember> Members { get; init; } = Array.Empty<DriveMember>();

    /// <summary>A mirror (or parity set) that lost a member takes a new drive; a stripe or span cannot.</summary>
    public bool CanTakeADrive => Kind is "mirror" or "parity" && State is "degraded" or "failing";
}

internal sealed record DriveListing
{
    public string Os { get; init; } = "";
    public bool PoolOk { get; init; }
    public string Error { get; init; } = "";
    public string NotABackup { get; init; } = "";
    public IReadOnlyList<DriveDisk> Disks { get; init; } = Array.Empty<DriveDisk>();
    public IReadOnlyList<DriveSet> Sets { get; init; } = Array.Empty<DriveSet>();
}

/// <summary>One of the five ways to use the picked drives (drives::options_for), always in the
/// order separate, mirror, span, stripe, parity; a row the OS cannot do says why.</summary>
internal sealed record DriveOption
{
    public string Choice { get; init; } = "";
    public string Title { get; init; } = "";
    public bool Offered { get; init; }
    public string WhyNot { get; init; } = "";
    public bool Recommended { get; init; }
    public bool Warning { get; init; }
    public long Space { get; init; }
    public string SpaceWords { get; init; } = "";
    public string DeadWords { get; init; } = "";
    public string SpeedWords { get; init; } = "";

    /// <summary>Every choice but Separate erases the drives.</summary>
    public bool Erases => Choice != "separate";
}

internal sealed record DriveChoices(string NotABackup, IReadOnlyList<DriveOption> Options);

/// <summary>
/// "This computer" → Drives (docs/plans/self-host-drives.md §6): the reading of the link's
/// "drives" and "drives_choices" (the driftwire program's own JSON) and the rules the card
/// keeps before it sends anything. The Mac twin is PhoneDrivesModel.swift.
/// </summary>
internal static class PhoneDrives
{
    public static DriveListing? Listing(string json)
    {
        if (json.Length == 0) return null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object) return null;
            return new DriveListing
            {
                Os = Str(o, "os"),
                PoolOk = Bool(o, "pool_ok"),
                Error = Str(o, "error"),
                NotABackup = Str(o, "not_a_backup"),
                Disks = Items(o, "disks").Where(d => d.ValueKind == JsonValueKind.Object && Str(d, "id").Length > 0)
                    .Select(d => new DriveDisk
                    {
                        Id = Str(d, "id"),
                        Name = Str(d, "name"),
                        Model = Str(d, "model"),
                        Size = Int(d, "size"),
                        Used = Int(d, "used"),
                        Folders = Int(d, "folders"),
                        Facts = Str(d, "facts"),
                        System = Bool(d, "system"),
                        Internal = Bool(d, "internal"),
                        Image = Bool(d, "image"),
                        Smart = Str(d, "smart"),
                        Refused = Str(d, "refused"),
                        Volumes = Items(d, "volumes").Where(v => v.ValueKind == JsonValueKind.Object)
                            .Select(v => new DriveVolume(Str(v, "label"), Int(v, "size"), Int(v, "used"), Int(v, "folders"), Bool(v, "mounted")))
                            .ToList(),
                    }).ToList(),
                Sets = Items(o, "sets").Where(e => e.ValueKind == JsonValueKind.Object)
                    .Select(e => new DriveSet
                    {
                        Id = Str(e, "id"),
                        Name = Str(e, "name"),
                        Kind = Str(e, "kind"),
                        State = Str(e, "state"),
                        Percent = Int(e, "percent"),
                        Size = Int(e, "size"),
                        Line = Str(e, "line"),
                        Members = Items(e, "members").Where(m => m.ValueKind == JsonValueKind.Object)
                            .Select(m => new DriveMember(Str(m, "id"), Str(m, "status"), Bool(m, "missing"))).ToList(),
                    }).ToList(),
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static DriveChoices? Choices(string json)
    {
        if (json.Length == 0) return null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object) return null;
            return new DriveChoices(Str(o, "not_a_backup"),
                Items(o, "options").Where(e => e.ValueKind == JsonValueKind.Object && Str(e, "choice").Length > 0)
                    .Select(e => new DriveOption
                    {
                        Choice = Str(e, "choice"),
                        Title = Str(e, "title"),
                        Offered = Bool(e, "offered"),
                        WhyNot = Str(e, "why_not"),
                        Recommended = Bool(e, "recommended"),
                        Warning = Bool(e, "warning"),
                        Space = Int(e, "space"),
                        SpaceWords = Str(e, "space_words"),
                        DeadWords = Str(e, "dead_words"),
                        SpeedWords = Str(e, "speed_words"),
                    }).ToList());
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>The guard the program applies again (drives::confirmed): one typed name per
    /// drive, in order, each equal to that drive's name once trimmed.</summary>
    public static bool NamesMatch(IReadOnlyList<string> typed, IReadOnlyList<DriveDisk> disks)
    {
        if (disks.Count == 0 || typed.Count != disks.Count) return false;
        for (int i = 0; i < disks.Count; ++i)
        {
            if (disks[i].Name.Length == 0 || typed[i].Trim() != disks[i].Name) return false;
        }
        return true;
    }

    /// <summary>The combined drive's name (drives::good_label): 1 to 27 ASCII letters, digits,
    /// spaces, '-' or '_', not starting or ending with a space.</summary>
    public static bool GoodLabel(string label)
    {
        if (label.Length == 0 || label.Length > 27 || label[0] == ' ' || label[^1] == ' ') return false;
        return label.All(c => c is (>= '0' and <= '9') or (>= 'A' and <= 'Z') or (>= 'a' and <= 'z') or ' ' or '-' or '_');
    }

    /// <summary>The request that erases and combines (or, for Separate, shares each drive as it is).</summary>
    public static string CombineRequest(string choice, IReadOnlyList<DriveDisk> disks, string label, IReadOnlyList<string> typed)
    {
        var ids = new System.Text.Json.Nodes.JsonArray();
        foreach (DriveDisk d in disks) ids.Add(d.Id);
        var r = new System.Text.Json.Nodes.JsonObject { ["do"] = "host_drives_combine", ["choice"] = choice, ["ids"] = ids };
        if (choice != "separate")
        {
            r["label"] = label.Trim();
            var names = new System.Text.Json.Nodes.JsonArray();
            foreach (string t in typed) names.Add(t.Trim());
            r["names"] = names;
        }
        return r.ToJsonString();
    }

    public static string ChoicesRequest(IEnumerable<string> ids)
    {
        var list = new System.Text.Json.Nodes.JsonArray();
        foreach (string id in ids) list.Add(id);
        return new System.Text.Json.Nodes.JsonObject { ["do"] = "host_drives_choices", ["ids"] = list }.ToJsonString();
    }

    public static string ReplaceRequest(string set, string id, string typed) =>
        new System.Text.Json.Nodes.JsonObject
        {
            ["do"] = "host_drives_replace", ["set"] = set, ["ids"] = new System.Text.Json.Nodes.JsonArray(id), ["name"] = typed.Trim(),
        }.ToJsonString();

    /// <summary>The drives that could take a lost member's place: not refused, not already in the set.</summary>
    public static IReadOnlyList<DriveDisk> Replacements(DriveSet set, DriveListing listing)
    {
        var members = set.Members.Select(m => m.Id).ToHashSet();
        return listing.Disks.Where(d => d.Refused.Length == 0 && !members.Contains(d.Id)).ToList();
    }

    private static string Str(JsonElement o, string name) =>
        o.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static bool Bool(JsonElement o, string name) =>
        o.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.True;

    private static long Int(JsonElement o, string name)
    {
        if (!o.TryGetProperty(name, out JsonElement v) || v.ValueKind != JsonValueKind.Number) return 0;
        if (v.TryGetInt64(out long l)) return l;
        return v.TryGetDouble(out double d) ? (long)d : 0;
    }

    private static IEnumerable<JsonElement> Items(JsonElement o, string name) =>
        o.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray() : Enumerable.Empty<JsonElement>();
}
