// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace MediaViewer.Shared;

/// <summary>Local search's phone_json: the catalogue on the NAS (docs/plans/mobile.md §6).</summary>
internal sealed record PhonePublishState(
    bool Running, string Phase, double Fraction, string Error, string Destination, long Published, long Assets)
{
    public static readonly PhonePublishState None = new(false, "", 0, "", "", 0, 0);
}

/// <summary>A paired phone: its name, the hex of its device key, and its access (NAS clock).</summary>
internal sealed record PhoneDevice(string Name, string Key)
{
    public bool Limited { get; init; }   // has an end
    public long Expires { get; init; }   // unix, 0 = no end
    public bool Expired { get; init; }
    public bool Guest { get; init; }     // paired by a guest code
    public bool Backup { get; init; } = true;  // may back up to the backup folders (docs/plans/mobile.md §10b.1, B2)
}

/// <summary>A named folder the service shares; "Catalogue" is the catalogue's own.</summary>
internal sealed record PhoneFolder(string Id, string Label, string Path)
{
    public bool IsCatalogue => Label == "Catalogue";
    // Phone backup (1.1): phones may create files here, under these sizes.
    public bool Backup { get; init; }
    public long Limit { get; init; }
    public long Reserve { get; init; }
    public long Used { get; init; }
    public long Room { get; init; }
    public bool Indexed { get; init; }   // Local search on this PC covers it (§10b.1, B7)
}

/// <summary>This PC as the server (docs/plans/self-hosted.md slice 6): what the host's own files
/// say, read by the link thread (phone/self_host.h). Never a path or an address.</summary>
internal sealed record PhoneHost
{
    public bool Installed { get; init; }  // the driftwire program is on this PC
    public bool SetUp { get; init; }      // a folder is shared
    public bool Running { get; init; }
    public bool AtLogin { get; init; }
    public long ServicePort { get; init; }
    public long RelayPort { get; init; }
    public long ConfiguredPort { get; init; }
    public long Folders { get; init; }
    public long Offline { get; init; }
    public string Name { get; init; } = "";
    public string Duckdns { get; init; } = "";
    public IReadOnlyList<string> Keepers { get; init; } = Array.Empty<string>();  // the router's and DuckDNS's lines
    // docs/plans/self-hosted.md "Tray": hosting turned off, and today's usage and the
    // phones connected in the notification-area menu's words (the core's).
    public bool Paused { get; init; }
    public IReadOnlyList<string> Stats { get; init; } = Array.Empty<string>();

    public bool SameAs(PhoneHost? o) => o is not null && this with { Keepers = o.Keepers, Stats = o.Stats } == o &&
                                        Keepers.SequenceEqual(o.Keepers) && Stats.SequenceEqual(o.Stats);
}

/// <summary>A folder the person picked for this PC to share, before Set up.</summary>
internal sealed record PhoneHostFolder(string Path, bool Backup)
{
    public string Label => Backup ? "Phone backup" : PhoneWords.ShareLabel(Path.Replace('\\', '/'));
}

/// <summary>mv_phone_state_json: the folder, the service, the code, the phones (docs/plans/phone-in-the-app.md).</summary>
internal sealed record PhoneLinkState
{
    public bool Starting { get; init; }   // the app has not said yet which window runs it
    public bool Secondary { get; init; }  // a later window's process: Phone access is the first's
    public bool Driftwire { get; init; } = true;  // false: no secret store for phone keys
    public string Folder { get; init; } = "";
    public string LibraryName { get; init; } = "";
    public bool LocalSearch { get; init; }
    public PhonePublishState? Catalogue { get; init; }  // Local search's rows, when it is loaded
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
    public string NasPassphrase { get; init; } = "";  // "set" | "none" | "broken" | "" (a NAS too old to say)
    public IReadOnlyList<string> NasFeatures { get; init; } = Array.Empty<string>();
    public long UnlockedSeconds { get; init; }        // left on this computer's unlock; 0 = locked
    public long LockedUntil { get; init; }            // unix; the NAS refuses tries until then
    public long QrForSeconds { get; init; }           // the code showing is a guest code for this long
    public string Relay { get; init; } = "";          // the person's own relay, "host:port"; its key stays on the NAS
    public PhoneHost? Host { get; init; }             // this PC as the server, once the link has read its files
    public string HostStep { get; init; } = "";       // "share", "name", "start", "claim" while a setup runs
    public string HostSaid { get; init; } = "";       // the driftwire program's own words for a step that failed

    /// <summary>The NAS takes guest codes, time limits and a passphrase.</summary>
    public bool NasHasFeatures => NasFeatures.Contains("passphrase");

    /// <summary>The NAS can wait at the person's own relay (docs/plans/nas-relay.md).</summary>
    public bool NasHasRelay => NasFeatures.Contains("relay");
    /// <summary>The NAS service takes phone backups (1.1): backup folders and the per-phone switch.</summary>
    public bool NasHasBackup => NasFeatures.Contains("backup");

    /// <summary>Whether two reads would draw the same card (records compare lists by reference).</summary>
    // The catalogue's fraction moves on every read while it publishes: the card
    // updates its progress line in place, so that alone is not a change.
    public bool SameAs(PhoneLinkState o) =>
        this with { Phones = o.Phones, Folders = o.Folders, Added = o.Added, NasFeatures = o.NasFeatures, Host = o.Host,
                    // The unlock counts down each second; the card says minutes, so only those count.
                    UnlockedSeconds = (UnlockedSeconds + 59) / 60 == (o.UnlockedSeconds + 59) / 60
                        ? o.UnlockedSeconds : UnlockedSeconds,
                    Catalogue = Catalogue is { } c ? c with { Fraction = o.Catalogue?.Fraction ?? 0 } : null } == o &&
        Phones.SequenceEqual(o.Phones) && Folders.SequenceEqual(o.Folders) && Added.SequenceEqual(o.Added) &&
        NasFeatures.SequenceEqual(o.NasFeatures) && (Host is null ? o.Host is null : Host.SameAs(o.Host));
}

/// <summary>
/// Settings → Phone access: the reading of the app's state JSON (mv_phone_state_json)
/// and the card's words, shared by the WinUI card (IslandHost.Phone.cs) and
/// Chrome.Tests. The words are the Mac's (PhoneAccessView.swift, "this Mac" there
/// is "this PC" here); Chrome.Tests reads the Swift as text so the two do not drift.
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
                    phones.Add(new PhoneDevice(Str(d, "name"), Str(d, "key"))
                    {
                        Limited = Bool(d, "limited", false),
                        Expires = Int(d, "expires"),
                        Expired = Bool(d, "expired", false),
                        Guest = Bool(d, "guest", false),
                        Backup = Bool(d, "backup", !Bool(d, "guest", false)),
                    });
            var folders = new List<PhoneFolder>();
            foreach (JsonElement f in Items(o, "folders"))
                if (f.ValueKind == JsonValueKind.Object && Has(f, "id") && Has(f, "label") && Has(f, "path"))
                    folders.Add(new PhoneFolder(Str(f, "id"), Str(f, "label"), Str(f, "path"))
                    {
                        Backup = Bool(f, "backup", false),
                        Limit = Int(f, "limit"),
                        Reserve = Int(f, "reserve"),
                        Used = Int(f, "used"),
                        Room = Int(f, "room"),
                        Indexed = Bool(f, "indexed", false),
                    });
            var added = new List<string>();
            foreach (JsonElement a in Items(o, "added"))
                if (a.ValueKind == JsonValueKind.String) added.Add(a.GetString() ?? "");
            PhonePublishState? catalogue = o.TryGetProperty("catalogue", out JsonElement c) &&
                                           c.ValueKind == JsonValueKind.Object
                ? ParsePublish(c.GetRawText()) : null;
            return new PhoneLinkState
            {
                Starting = Bool(o, "starting", false),
                Secondary = Bool(o, "secondary", false),
                Folder = Str(o, "folder"),
                LibraryName = Str(o, "library_name"),
                LocalSearch = Bool(o, "local_search", false),
                Catalogue = catalogue,
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
                NasPassphrase = Str(o, "nas_passphrase"),
                NasFeatures = Items(o, "nas_features").Where(f => f.ValueKind == JsonValueKind.String)
                                                    .Select(f => f.GetString() ?? "").ToList(),
                UnlockedSeconds = Int(o, "unlocked_s"),
                LockedUntil = Int(o, "locked_until"),
                QrForSeconds = Int(o, "qr_for_s"),
                Relay = Str(o, "relay"),
                Host = o.TryGetProperty("host", out JsonElement h) && h.ValueKind == JsonValueKind.Object
                    ? new PhoneHost
                    {
                        Installed = Bool(h, "installed", false),
                        SetUp = Bool(h, "set_up", false),
                        Running = Bool(h, "running", false),
                        AtLogin = Bool(h, "at_login", false),
                        ServicePort = Int(h, "service_port"),
                        RelayPort = Int(h, "relay_port"),
                        ConfiguredPort = Int(h, "configured_port"),
                        Folders = Int(h, "folders"),
                        Offline = Int(h, "offline"),
                        Name = Str(h, "name"),
                        Duckdns = Str(h, "duckdns"),
                        Keepers = Items(h, "keepers").Where(k => k.ValueKind == JsonValueKind.String)
                                                     .Select(k => k.GetString() ?? "").ToList(),
                        Paused = h.TryGetProperty("paused", out JsonElement paused) && paused.ValueKind == JsonValueKind.True,
                        Stats = Items(h, "stats").Where(k => k.ValueKind == JsonValueKind.String)
                                                 .Select(k => k.GetString() ?? "").ToList(),
                    }
                    : null,
                HostStep = Str(o, "host_step"),
                HostSaid = Str(o, "host_said"),
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // ---- what the phone searches (PhoneAccessView.swift searchDetail) ----

    public static string SearchDetail(PhoneLinkState l, long nowUnix)
    {
        if (!l.LocalSearch || l.Catalogue is not { } s)
            return "Your phone searches file names and tags. Install Local search and index this library for words, faces, places and sounds.";
        if (s.Running) return s.Phase == "sending" ? "Sending to your server" : "Building the catalogue";
        switch (s.Error)
        {
            case "unreachable": return "Couldn't reach that folder. It will need to be mounted.";
            case "other_library": return "That folder holds another library's catalogue.";
            case "no_space": return "Not enough space on the share.";
            case "no_index": return "Nothing is indexed yet. Your phone searches file names and tags until it is.";
            case "no_folders": return "None of your search folders are on that server.";
            case "no_key_store": return "This copy of MediaViewer can't keep a signing key, so it can't publish.";
            case "failed": return "The catalogue could not be built.";
            case "cancelled": return "Stopped.";
        }
        if (s.Published <= 0) return "Smart search starts once the catalogue is published.";
        return $"Smart search is on. Published {Ago(nowUnix - s.Published)} · {Math.Max(0, s.Assets):N0} files";
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

    // ---- the rest of the card (PhoneAccessView.swift) ----

    public static string? ErrorText(string error) => error switch
    {
        "" => null,
        "refused" => "The service said no. It takes a claim only from its own network, and only once.",
        "unreachable" => "Couldn't reach the service.",
        "bad_address" => "That isn't an address. Use name:port.",
        "too_many" => "Too many phones are paired. Remove one first.",
        "no_folder" => "Choose the folder on your server first.",
        "not_published" => "Choose the folder on your server first.",
        "other_library" => "That folder holds another library's catalogue.",
        "no_key_store" => "This copy of MediaViewer can't keep a signing key.",
        "revoke_waits" => "Removed here. Your server will be told when it can be reached.",
        "no_driftwire" => "This copy of MediaViewer can't keep phone keys.",
        "no_local_search" => "Install Local search to publish a catalogue.",
        "needs_unlock" => "Enter the server passphrase to do that.",
        "passphrase" => "That is not the server passphrase.",
        "passphrase_short" => "A passphrase needs at least 10 characters.",
        "no_passphrase" => "This server has no passphrase yet. Enter a new one twice to choose it.",
        "slow_down" => "Try again in a moment.",
        "wrong_service" => "That is a different server from the one this computer claimed.",
        "nas_too_old" => "Update the service on your server to use this.",
        "no_driftwire_program" => InstallLine,
        "host_no_folders" => "Choose a folder for your phone to read.",
        "host_no_library" => "Choose a folder for your phone to read, not only a backup folder.",
        "host_twice" => "That folder is in the list twice.",
        "host_bad_name" => "That isn't a DuckDNS name. Use the name you chose there, like home or home.duckdns.org.",
        "host_bad_token" => "That isn't a DuckDNS token. Copy it from duckdns.org; it looks like 1a2b3c4d-…",
        "host_share" => "The folder couldn't be shared.",
        "host_name" => "DuckDNS didn't take the name.",
        "host_start" => "The server didn't start.",
        "host_not_listening" => "The server started but isn't answering yet. Another program may be using its port.",
        "host_no_network" => "Claimed, but this PC isn't on a network, so a phone can't reach it yet.",
        "host_failed" => "The driftwire program couldn't be run.",
        _ => "That didn't work.",
    };

    /// <summary>The error line, with what the state adds: the lockout's end, and a claim that went
    /// through on a NAS too old to keep a passphrase. `clock` turns a unix time into "14:20".</summary>
    public static string? ErrorText(PhoneLinkState s, Func<long, string> clock) => s.Error switch
    {
        "locked_out" => $"Too many tries. Try again at {clock(s.LockedUntil)}, or reset it on the server with driftwire serve passphrase reset.",
        "nas_too_old" when s.Claimed && s.NasPassphrase is "" or "none" =>
            "Claimed. Update the service on your server to protect it with a passphrase.",
        // The driftwire program's own words for a step it refused.
        "host_share" when s.HostSaid.Length > 0 => $"Couldn't share it: {s.HostSaid}",
        "host_name" when s.HostSaid.Length > 0 => $"DuckDNS: {s.HostSaid}",
        "host_start" when s.HostSaid.Length > 0 => $"The server didn't start: {s.HostSaid}",
        _ => ErrorText(s.Error),
    };

    /// <summary>The NAS passphrase row's line.</summary>
    public static string PassphraseDetail(PhoneLinkState s) => s.NasPassphrase switch
    {
        "set" => s.UnlockedSeconds > 0 ? $"Unlocked · {Math.Max(1, (s.UnlockedSeconds + 59) / 60)} min" : "Locked",
        "none" => "None. Choose one so nobody else on your network can claim or change this service.",
        "broken" => "Your server can't read it. Reset it on the server with driftwire serve passphrase reset.",
        _ => "Update the service on your server to use this.",
    };

    /// <summary>The durations a code or a phone's access can have, in the menus' order.</summary>
    public static readonly (string Label, long Seconds)[] Durations =
    {
        ("1 hour", 3600), ("24 hours", 86400), ("7 days", 7 * 86400), ("30 days", 30 * 86400),
    };
    public const string Unlimited = "Until I remove it";

    /// <summary>What Extend offers a timed phone: only lengths that end after its end now (a grant
    /// counts from now, so a shorter one would cut its access), then Until I remove it.</summary>
    public static IReadOnlyList<(string Label, long Seconds)> ExtendDurations(PhoneDevice d, long nowUnix) =>
        Durations.Where(x => !d.Limited || d.Expires == 0 || nowUnix + x.Seconds > d.Expires).ToList();

    // ---- requests (the JSON link.cpp reads: numbers and booleans as such) ----

    public static string ClaimRequest(string local, string publicName, string passphrase, string again)
    {
        var o = new System.Text.Json.Nodes.JsonObject { ["do"] = "claim", ["local"] = local, ["public"] = publicName };
        if (passphrase.Length > 0)
        {
            o["passphrase"] = passphrase;
            o["new_passphrase"] = again.Length > 0;  // twice: choose it; once: prove the NAS's
        }
        return o.ToJsonString();
    }

    public static string PairRequest(long forSeconds) =>
        new System.Text.Json.Nodes.JsonObject { ["do"] = "pair", ["for_s"] = forSeconds }.ToJsonString();

    public static string GrantRequest(string key, long forSeconds) =>
        new System.Text.Json.Nodes.JsonObject { ["do"] = "grant", ["key"] = key, ["for_s"] = forSeconds }.ToJsonString();

    /// <summary>A phone's backup flag (B2): on adds access, off never needs the passphrase.</summary>
    public static string PhoneBackupRequest(string key, bool on) =>
        new System.Text.Json.Nodes.JsonObject { ["do"] = "phone_backup", ["key"] = key, ["on"] = on }.ToJsonString();

    /// <summary>A backup folder's sizes, decimal bytes as the link takes them.</summary>
    public static string SetBackupRequest(string id, long limit, long reserve) =>
        new System.Text.Json.Nodes.JsonObject
        {
            ["do"] = "set_backup", ["id"] = id,
            ["limit"] = limit.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ["reserve"] = reserve.ToString(System.Globalization.CultureInfo.InvariantCulture),
        }.ToJsonString();

    /// <summary>A backup folder's line: its path, sizes, and whether this PC indexes it.</summary>
    public static string BackupDetail(PhoneFolder f) =>
        $"{f.Path} · {Gigabytes(f.Used)} of {Gigabytes(f.Limit)} GB used, keeps {Gigabytes(f.Reserve)} GB free. " +
        (f.Indexed
            ? "Indexed by this PC: what phones send reaches search after the next scan."
            : "Not in Local search on this PC yet: add this folder there so what phones send reaches search.");

    /// <summary>Bytes as GB for the fields: "500" or "1.5".</summary>
    public static string Gigabytes(long bytes)
    {
        double gb = bytes / 1e9;
        if (gb >= 10) return Math.Round(gb).ToString(System.Globalization.CultureInfo.InvariantCulture);
        return Math.Round(gb, 2).ToString("0.##", System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>"500" or "1,5" (GB) to bytes; null when it is not a number.</summary>
    public static long? Bytes(string gigabytes)
    {
        string t = gigabytes.Trim().Replace(',', '.');
        if (!double.TryParse(t, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double v) ||
            v < 0 || v >= 1e6 || double.IsNaN(v))
            return null;
        return (long)Math.Round(v * 1e9);
    }

    /// <summary>The relay's key is at least 16 characters, as driftwire-relay takes it.</summary>
    public const int RelayKeyMinimum = 16;

    public static string RelayRequest(string address, string key) =>
        new System.Text.Json.Nodes.JsonObject { ["do"] = "set_relay", ["address"] = address.Trim(), ["key"] = key }.ToJsonString();

    /// <summary>There is no public relay (owner, 2026-11-12): the row says so plainly.</summary>
    public static string RelayDetail(PhoneLinkState s) =>
        !s.NasHasRelay ? "Update the service on your server to reach it away from home without a forwarded port."
        : s.Relay.Length == 0
            ? "Away from home without a forwarded port, phones need a relay you run yourself (driftwire-relay on your own server). There is no public one."
            : $"Phones away from home go through {s.Relay} when they can't reach your server directly. It passes sealed bytes only.";

    public static string DurationText(long seconds) => seconds switch
    {
        3600 => "1 hour",
        86400 => "24 hours",
        _ when seconds % 86400 == 0 => $"{seconds / 86400} days",
        _ when seconds % 3600 == 0 => $"{seconds / 3600} hours",
        _ => $"{Math.Max(1, seconds / 60)} minutes",
    };

    /// <summary>"Guest code · access for 24 hours from when it is scanned".</summary>
    public static string GuestCodeLine(long seconds) =>
        $"Guest code · access for {DurationText(seconds)} from when it is scanned";

    /// <summary>A phone row's line: "Guest · Until Fri 14:00", "Guest · Expired yesterday", or its key.
    /// `when` turns a unix time into "Fri 14:00".</summary>
    public static string PhoneDetail(PhoneDevice d, long nowUnix, Func<long, string> when)
    {
        // Backup is said only where it is not the usual: a guest that may back up, a phone that may not.
        string lead = d.Guest ? (d.Backup ? "Guest · Backs up · " : "Guest · ") : (d.Backup ? "" : "No backup · ");
        if (!d.Limited || d.Expires == 0) return lead + KeyDetail(d.Key);
        if (d.Expired || d.Expires <= nowUnix) return lead + "Expired " + Ago(nowUnix - d.Expires);
        return lead + "Until " + when(d.Expires);
    }

    /// <summary>The work under way, for the progress line; null for the quick ones.</summary>
    public static string? BusyText(string busy) => busy switch
    {
        "claim" => "Claiming the service",
        "pair" => "Asking your server for a code",
        "refresh" => "Checking your server",
        "revoke" => "Removing the phone",
        "add_folder" or "remove_folder" => "Changing the shared folders",
        "set_addresses" => "Checking the address",
        "folder" => "Setting up the folder",
        "takeover_ask" or "takeover_allow" or "takeover_restart" => "Updating the library",
        "unlock" => "Unlocking",
        "set_passphrase" => "Setting the passphrase",
        "grant" => "Changing the phone's access",
        "host_start" => "Starting the server",
        "host_setup" => "Setting up this PC",
        _ => null,
    };

    /// <summary>The progress line's words, with the setup's step when one runs.</summary>
    public static string? BusyText(PhoneLinkState s) => s.Busy == "host_setup"
        ? s.HostStep switch
        {
            "share" => "Sharing the folders",
            "name" => "Naming it on DuckDNS",
            "start" => "Starting the server",
            "claim" => "Claiming it",
            _ => BusyText(s.Busy),
        }
        : BusyText(s.Busy);

    // ---- this PC as the server (docs/plans/self-hosted.md slice 6) ----

    /// <summary>How to get the driftwire program, which runs the server (the app does not carry it).
    /// The Mac's says Homebrew; this is install.ps1's one line (docs/design/28 "Installing").</summary>
    public const string InstallLine =
        "This PC needs the driftwire program to be the server. Install it once in PowerShell with irm https://longtimeno-c.github.io/mediaviewer/driftwire/install.ps1 | iex (no administrator), then choose Check again.";

    /// <summary>The status row's line for a server on this PC: never a path or an address.</summary>
    public static string HostDetail(PhoneHost h)
    {
        if (!h.Running)
            return h.AtLogin ? "Not running. It starts when you log in; Start runs it now."
                             : "Not running, and not set to start at login. Start runs it and keeps it starting at login.";
        var parts = new List<string> { h.Paused ? "Running, hosting off" : "Running" };
        if (!h.Paused) parts.Add(h.ServicePort > 0 ? $"port {h.ServicePort}" : "not listening yet");
        if (h.Duckdns.Length > 0) parts.Add($"{h.Duckdns}.duckdns.org");
        if (h.Offline > 0) parts.Add(h.Offline == 1 ? "1 drive not connected" : $"{h.Offline} drives not connected");
        string line = string.Join(" · ", parts);
        if (h.Stats.Count > 0) line += "\n" + string.Join("\n", h.Stats);
        return h.Keepers.Count > 0 ? line + "\n" + string.Join("\n", h.Keepers) : line;
    }

    /// <summary>One request for the whole setup: the folders phones read first (the first holds the
    /// library), then the backup folders; the DuckDNS name and token only together.</summary>
    public static string HostSetupRequest(IEnumerable<PhoneHostFolder> folders, string duckdns, string token)
    {
        var list = new System.Text.Json.Nodes.JsonArray();
        foreach (PhoneHostFolder f in folders.Where(f => !f.Backup).Concat(folders.Where(f => f.Backup)))
        {
            var o = new System.Text.Json.Nodes.JsonObject { ["path"] = f.Path, ["backup"] = f.Backup };
            if (f.Backup) o["label"] = f.Label;
            list.Add(o);
        }
        var r = new System.Text.Json.Nodes.JsonObject { ["do"] = "host_setup", ["folders"] = list };
        if (duckdns.Length > 0)
        {
            r["duckdns"] = duckdns;
            r["duckdns_token"] = token;
        }
        return r.ToJsonString();
    }

    /// <summary>Set up is offered once a folder phones read is chosen, and a DuckDNS name has its token.</summary>
    public static bool HostReady(IReadOnlyCollection<PhoneHostFolder> folders, string duckdns, string token, bool busy) =>
        !busy && folders.Any(f => !f.Backup) && (duckdns.Length == 0 || token.Length > 0);

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
        s.Added.Count > 0 ? $"Added {s.Added[^1]}." : "One scan pairs a phone with this PC and your server.";

    public static string FolderDetail(PhoneLinkState s) =>
        s.Folder.Length == 0 ? "Where your phone reads your library. Nothing is written there until you choose it." : s.Folder;

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
