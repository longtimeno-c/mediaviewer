// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace MediaViewer.Shared;

/// <summary>phone_json: the catalogue on the NAS (docs/plans/mobile.md §6).</summary>
internal sealed record PhonePublishState(
    bool Running, string Phase, double Fraction, string Error, string Destination, long Published, long Assets)
{
    public static readonly PhonePublishState None = new(false, "", 0, "", "", 0, 0);
}

/// <summary>A paired phone: its name and the hex of its device key.</summary>
internal sealed record PhoneDevice(string Name, string Key);

/// <summary>A named folder the service shares; "Catalogue" is the catalogue's own.</summary>
internal sealed record PhoneFolder(string Id, string Label, string Path)
{
    public bool IsCatalogue => Label == "Catalogue";
}

/// <summary>phone_link_json: the service, the code, the phones (docs/plans/mobile.md §7.2–§7.5).</summary>
internal sealed record PhoneLinkState
{
    public bool Driftwire { get; init; } = true;
    public string Busy { get; init; } = "";
    public string Error { get; init; } = "";
    public bool Claimed { get; init; }
    public bool Reachable { get; init; } = true;
    public string Local { get; init; } = "";
    public string Public { get; init; } = "";
    public string Warning { get; init; } = "";
    public IReadOnlyList<PhoneDevice> Phones { get; init; } = Array.Empty<PhoneDevice>();
    public IReadOnlyList<PhoneFolder> Folders { get; init; } = Array.Empty<PhoneFolder>();
    public string Qr { get; init; } = "";
    public string QrLibrary { get; init; } = "";
    public string QrFingerprint { get; init; } = "";
    public long QrExpires { get; init; }
    public IReadOnlyList<string> Added { get; init; } = Array.Empty<string>();
    public bool Nearby { get; init; }
    public bool OtherPublisher { get; init; }
    public bool Mismatch { get; init; }
    public string OtherFingerprint { get; init; } = "";
    public string AskingName { get; init; } = "";
    public string AskingKey { get; init; } = "";
    public string AskingFingerprint { get; init; } = "";

    /// <summary>Whether two reads would draw the same card (records compare lists by reference).</summary>
    public bool SameAs(PhoneLinkState o) =>
        this with { Phones = o.Phones, Folders = o.Folders, Added = o.Added } == o &&
        Phones.SequenceEqual(o.Phones) && Folders.SequenceEqual(o.Folders) && Added.SequenceEqual(o.Added);
}

/// <summary>
/// The Phone access card's reading of the pack's JSON and its words, shared
/// by the WinUI card (ManagePanel.Phone.cs) and Chrome.Tests. The words are
/// the Mac's (PhoneLinkView.swift, PhoneAccessView.swift); Chrome.Tests reads
/// the Swift as text so the two do not drift.
/// </summary>
internal static class PhoneWords
{
    public static PhonePublishState ParsePublish(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            JsonElement o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object) return PhonePublishState.None;
            return new PhonePublishState(Bool(o, "running", false), Str(o, "phase"), Num(o, "fraction"),
                                         Str(o, "error"), Str(o, "destination"), Int(o, "published"), Int(o, "assets"));
        }
        catch (JsonException)
        {
            return PhonePublishState.None;
        }
    }

    public static PhoneLinkState? ParseLink(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            JsonElement o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object) return null;
            var phones = new List<PhoneDevice>();
            foreach (JsonElement d in Items(o, "devices"))
                if (d.ValueKind == JsonValueKind.Object && Has(d, "name") && Has(d, "key"))
                    phones.Add(new PhoneDevice(Str(d, "name"), Str(d, "key")));
            var folders = new List<PhoneFolder>();
            foreach (JsonElement f in Items(o, "folders"))
                if (f.ValueKind == JsonValueKind.Object && Has(f, "id") && Has(f, "label") && Has(f, "path"))
                    folders.Add(new PhoneFolder(Str(f, "id"), Str(f, "label"), Str(f, "path")));
            var added = new List<string>();
            foreach (JsonElement a in Items(o, "added"))
                if (a.ValueKind == JsonValueKind.String) added.Add(a.GetString() ?? "");
            return new PhoneLinkState
            {
                Driftwire = Bool(o, "driftwire", true),
                Busy = Str(o, "busy"),
                Error = Str(o, "error"),
                Claimed = Bool(o, "claimed", false),
                Reachable = Bool(o, "reachable", true),
                Local = Str(o, "local"),
                Public = Str(o, "public"),
                Warning = Str(o, "warning"),
                Phones = phones,
                Folders = folders,
                Qr = Str(o, "qr"),
                QrLibrary = Str(o, "qr_library"),
                QrFingerprint = Str(o, "qr_fingerprint"),
                QrExpires = Int(o, "qr_expires"),
                Added = added,
                Nearby = Bool(o, "nearby", false),
                OtherPublisher = Bool(o, "other_publisher", false),
                Mismatch = Bool(o, "mismatch", false),
                OtherFingerprint = Str(o, "other_fingerprint"),
                AskingName = Str(o, "asking_name"),
                AskingKey = Str(o, "asking_key"),
                AskingFingerprint = Str(o, "asking_fingerprint"),
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // ---- the publish rows (PhoneAccessView.swift) ----

    public static string PublishStatus(PhonePublishState s, long nowUnix)
    {
        if (s.Running) return s.Phase == "sending" ? "Sending to the NAS" : "Building the catalogue";
        switch (s.Error)
        {
            case "unreachable": return "Couldn't reach that folder. It will need to be mounted.";
            case "other_library": return "That folder holds another library's catalogue.";
            case "no_space": return "Not enough space on the share.";
            case "no_index": return "Nothing is indexed yet.";
            case "no_folders": return "None of your search folders are on that NAS.";
            case "no_key_store": return "This copy of MediaViewer can't keep a signing key, so it can't publish.";
            case "failed": return "The catalogue could not be built.";
            case "cancelled": return "Stopped.";
        }
        if (s.Published <= 0) return "Not published yet.";
        // No key here: the one value to compare with a phone is the check code
        // beside the QR, and a second one would teach people to skip it.
        return $"Published {Ago(nowUnix - s.Published)} · {Math.Max(0, s.Assets):N0} files";
    }

    /// <summary>"just now", "5 minutes ago", "yesterday": how long since, in the largest unit.</summary>
    public static string Ago(long seconds)
    {
        if (seconds < 60) return "just now";
        long minutes = seconds / 60, hours = minutes / 60, days = hours / 24;
        if (minutes < 60) return minutes == 1 ? "1 minute ago" : $"{minutes} minutes ago";
        if (hours < 24) return hours == 1 ? "1 hour ago" : $"{hours} hours ago";
        if (days == 1) return "yesterday";
        if (days < 30) return $"{days} days ago";
        long months = days / 30;
        if (months < 12) return months == 1 ? "1 month ago" : $"{months} months ago";
        long years = days / 365;
        return years <= 1 ? "1 year ago" : $"{years} years ago";
    }

    // ---- the link rows (PhoneLinkView.swift) ----

    public static string? ErrorText(string error) => error switch
    {
        "" => null,
        "refused" => "The service said no. It takes a claim only from its own network, and only once.",
        "unreachable" => "Couldn't reach the service.",
        "bad_address" => "That isn't an address. Use name:port.",
        "too_many" => "Too many phones are paired. Remove one first.",
        "no_folder" => "Choose the folder on your NAS first.",
        "not_published" => "Publish the catalogue first.",
        "no_key_store" => "This copy of MediaViewer can't keep a signing key.",
        "revoke_waits" => "Removed here. The NAS will be told when it can be reached.",
        "no_driftwire" => "Driftwire isn't installed.",
        _ => "That didn't work.",
    };

    /// <summary>The work under way, for the progress line; null for the quick ones.</summary>
    public static string? BusyText(string busy) => busy switch
    {
        "claim" => "Claiming the service",
        "pair" => "Asking the NAS for a code",
        "refresh" => "Checking the NAS",
        "revoke" => "Removing the phone",
        "add_folder" or "remove_folder" => "Changing the shared folders",
        "set_addresses" => "Checking the address",
        "takeover_ask" or "takeover_allow" or "takeover_restart" => "Updating the library",
        _ => null,
    };

    public static string? WarningText(string warning) => warning switch
    {
        "private" => "This name points to a private address, so your phone can't reach it from outside.",
        "carrier" => "This name points to a carrier-grade address, so a forwarded port won't work. Ask your provider for a public address.",
        "unresolved" => "This name doesn't resolve yet.",
        _ => null,
    };

    /// <summary>
    /// What a new named folder is called unless the person says otherwise: its
    /// share's name, which is how a phone matches a catalogue's roots to the
    /// service's. On a Synology a share is the folder right under /volumeN.
    /// </summary>
    public static string ShareLabel(string path)
    {
        string[] parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length >= 2 && parts[0].StartsWith("volume", StringComparison.Ordinal) &&
            parts[0].Length > 6 && int.TryParse(parts[0].AsSpan(6), System.Globalization.NumberStyles.None,
                                                System.Globalization.CultureInfo.InvariantCulture, out _))
            return parts[1];
        return parts.Length > 0 ? parts[^1] : "";
    }

    public static string AddDetail(PhoneLinkState s) =>
        s.Added.Count > 0 ? $"Added {s.Added[^1]}." : "One scan pairs a phone with this PC and your NAS.";

    /// <summary>"Good for 4:59" under the code, from its expiry and now.</summary>
    public static string Countdown(long expiresUnix, long nowUnix)
    {
        long left = Math.Max(0, expiresUnix - nowUnix);
        return $"Good for {left / 60}:{left % 60:00}";
    }

    /// <summary>A phone's row: the first eight hex of its key.</summary>
    public static string KeyDetail(string key) => "Key " + (key.Length > 8 ? key[..8] : key);

    // ---- JSON ----

    private static bool Has(JsonElement o, string name) => o.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String;

    private static string Str(JsonElement o, string name) =>
        o.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static bool Bool(JsonElement o, string name, bool fallback) =>
        o.TryGetProperty(name, out JsonElement v) && v.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? v.GetBoolean() : fallback;

    private static double Num(JsonElement o, string name) =>
        o.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out double d) ? d : 0;

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
