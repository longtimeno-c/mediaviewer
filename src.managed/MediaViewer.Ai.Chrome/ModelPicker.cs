// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// The model tiers as settings_json carries them (#482, docs/plans/model-tiers.md
/// T4): every tier in display order with its models, licences and sizes, the
/// tier in force, and the local check of this machine ("machine": computed by
/// the pack, never sent anywhere).
/// </summary>
internal static class ModelTiers
{
    /// <summary>The pieces a tier's sizes are split by; "ai-large" is the Maxxed tier's picture tower.</summary>
    public const string Core = "ai", Faces = "ai-faces", Audio = "ai-audio", Docs = "ai-docs", Large = "ai-large";

    internal sealed record Tier(
        string Id, string Name, string Emoji, string For,
        string[] Pictures, string[] Speech, string[] Sounds, string[] Faces, string[] Documents,
        string[] Licences, Dictionary<string, long> Download, Dictionary<string, long> Disk,
        double? PhotosPerSecondAccelerated, double? PhotosPerSecondCpu)
    {
        public string Title => Emoji.Length > 0 ? $"{Emoji} {Name}" : Name;

        /// <summary>Pictures, speech, sounds, faces, documents, joined " · ".</summary>
        public string ModelLine => string.Join(" · ", Pictures.Concat(Speech).Concat(Sounds).Concat(Faces).Concat(Documents));

        /// <summary>The Maxxed tier's picture tower is a piece of its own.</summary>
        public bool NeedsLarge => Disk.ContainsKey(Large) || Download.ContainsKey(Large);

        public bool HasAudio => Sounds.Length > 0 || Speech.Length > 0;
    }

    internal sealed record Machine(string Chip, long RamBytes, string Accelerator, string Recommended);

    internal sealed record Choice(string Current, string Default, List<Tier> Tiers, Machine Machine)
    {
        public Tier? Find(string id) => Tiers.Find(t => t.Id == id);

        /// <summary>The local check's pick, else the pack's default, else the first.</summary>
        public Tier Recommended => Find(Machine.Recommended) ?? Find(Default) ?? Tiers[0];
    }

    /// <summary>[no-block] settings_json; null from a pack before tiers, or when it cannot be read.</summary>
    public static Choice? Read(AiApi api)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(api.SettingsJson());
            return Parse(doc.RootElement);
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException) { return null; }
    }

    public static Choice? Parse(JsonElement root)
    {
        try
        {
            if (!root.TryGetProperty("tiers", out JsonElement tiers) || tiers.ValueKind != JsonValueKind.Object ||
                !tiers.TryGetProperty("profiles", out JsonElement profiles) || profiles.ValueKind != JsonValueKind.Object)
            {
                return null;
            }
            var list = new List<Tier>();
            foreach (JsonProperty p in profiles.EnumerateObject())  // display order
            {
                JsonElement t = p.Value;
                if (t.ValueKind != JsonValueKind.Object) continue;
                JsonElement summary = t.TryGetProperty("summary", out JsonElement s) && s.ValueKind == JsonValueKind.Object ? s : default;
                JsonElement models = summary.ValueKind == JsonValueKind.Object && summary.TryGetProperty("models", out JsonElement m) ? m : default;
                double? accelerated = null, cpu = null;
                if (summary.ValueKind == JsonValueKind.Object && summary.TryGetProperty("photos_per_s", out JsonElement pps) &&
                    pps.ValueKind == JsonValueKind.Object)
                {
                    accelerated = Number(pps, "accelerated");
                    cpu = Number(pps, "cpu");
                }
                list.Add(new Tier(
                    p.Name,
                    Str(t, "name") is { Length: > 0 } name ? name : p.Name,
                    Str(t, "emoji"),
                    Str(t, "for"),
                    Strings(models, "pictures"), Strings(models, "speech"), Strings(models, "sounds"),
                    Strings(models, "faces"), Strings(models, "documents"),
                    Strings(summary, "licences"),
                    Sizes(summary, "download_bytes"), Sizes(summary, "disk_bytes"),
                    accelerated, cpu));
            }
            if (list.Count == 0) return null;
            JsonElement machine = root.TryGetProperty("machine", out JsonElement mc) && mc.ValueKind == JsonValueKind.Object ? mc : default;
            long ram = machine.ValueKind == JsonValueKind.Object && machine.TryGetProperty("ram_bytes", out JsonElement r) &&
                       r.TryGetInt64(out long rb) ? rb : 0;
            return new Choice(
                Str(root, "tier"),
                Str(tiers, "default"),
                list,
                new Machine(Str(machine, "chip"), ram, Str(machine, "accelerator"), Str(machine, "recommended")));
        }
        catch (Exception ex) when (ex is InvalidOperationException or FormatException) { return null; }
    }

    private static string Str(JsonElement e, string key) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() ?? "" : "";

    private static double? Number(JsonElement e, string key) =>
        e.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out double d) && d > 0 ? d : null;

    // A model is its name; an object with a "name" is read too.
    private static string[] Strings(JsonElement e, string key)
    {
        if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(key, out JsonElement a) || a.ValueKind != JsonValueKind.Array)
            return Array.Empty<string>();
        var list = new List<string>();
        foreach (JsonElement x in a.EnumerateArray())
        {
            string? s = x.ValueKind == JsonValueKind.String ? x.GetString() : Str(x, "name");
            if (!string.IsNullOrEmpty(s)) list.Add(s);
        }
        return list.ToArray();
    }

    private static Dictionary<string, long> Sizes(JsonElement e, string key)
    {
        var sizes = new Dictionary<string, long>(StringComparer.Ordinal);
        if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(key, out JsonElement o) || o.ValueKind != JsonValueKind.Object)
            return sizes;
        foreach (JsonProperty p in o.EnumerateObject())
        {
            if (p.Value.TryGetInt64(out long n) && n > 0) sizes[p.Name] = n;
        }
        return sizes;
    }
}

/// <summary>
/// "Choose your Local search models" (#482 T4; the Mac's sheet in AI.bundle):
/// the tiers with the local check's recommendation first and preselected,
/// People / Sounds / Documents as checkboxes, and what Hugging Face sees.
/// Download saves the tier and asks the host to install the pieces it needs
/// (<see cref="IAddonPieces"/>). Opened once when the chrome attaches with no
/// tier chosen, and from Settings → Local search → Models. A small owned
/// window over the app, as a sheet is on the Mac (PersonSheet's pattern; not a
/// ContentDialog, which needs a XamlRoot the island host does not give it).
/// </summary>
internal sealed class ModelPicker : Window
{
    private readonly AiChrome _chrome;
    private readonly Look _look;
    private readonly ModelTiers.Choice _choice;
    private readonly bool _accelerated;
    private string _selected;
    private readonly Dictionary<string, ToggleButton> _cards = new();
    private readonly Dictionary<string, TextBlock> _cardSizes = new();
    private readonly CheckBox _people;
    private readonly CheckBox _sounds;
    private readonly CheckBox _documents;
    private readonly TextBlock _totals;
    private readonly Button _go;
    private bool _closed;

    internal ModelPicker(AiChrome chrome, ModelTiers.Choice choice)
    {
        _chrome = chrome;
        _look = chrome.Look;
        _choice = choice;
        _accelerated = choice.Machine.Accelerator.Length > 0;
        // Re-opened with a tier chosen: on that one; the first time, on the recommendation.
        _selected = choice.Find(choice.Current)?.Id ?? choice.Recommended.Id;
        Title = "Choose your Local search models";

        if (AppWindow.Presenter is not OverlappedPresenter) AppWindow.SetPresenter(OverlappedPresenter.Create());
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.IsResizable = false;
            p.IsMaximizable = false;
            p.IsMinimizable = false;
        }
        AppWindow.IsShownInSwitchers = false;
        try { SystemBackdrop = new MicaBackdrop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        Native.Adopt(WinRT.Interop.WindowNative.GetWindowHandle(this), chrome.Host.MainWindow);
        Closed += (_, _) => _closed = true;

        var root = new Grid { Padding = new Thickness(20), RowSpacing = 12 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var head = new StackPanel { Spacing = 4 };
        TextBlock title = _look.Text("Choose your Local search models", 20, AddonColour.Title);
        title.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        AutomationProperties.SetHeadingLevel(title, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level1);
        head.Children.Add(title);
        head.Children.Add(_look.Text(MachineLine(choice.Machine), 13));
        root.Children.Add(head);

        // The tiers, the recommendation first, then the checkboxes and the footnote.
        var body = new StackPanel { Spacing = 8, Padding = new Thickness(0, 0, 12, 0) };
        ModelTiers.Tier recommended = choice.Recommended;
        body.Children.Add(Caption("★ RECOMMENDED", AddonColour.Accent));
        body.Children.Add(Card(recommended));
        List<ModelTiers.Tier> others = choice.Tiers.Where(t => t.Id != recommended.Id).ToList();
        if (others.Count > 0)
        {
            TextBlock other = Caption("Other options", AddonColour.Body);
            other.Margin = new Thickness(0, 8, 0, 0);
            body.Children.Add(other);
            foreach (ModelTiers.Tier t in others) body.Children.Add(Card(t));
        }

        TextBlock also = _look.Text("Also install", 14, AddonColour.Title);
        also.Margin = new Thickness(0, 10, 0, 0);
        body.Children.Add(also);
        _people = Check("People", ModelTiers.Faces, false);
        _sounds = Check("Sounds", ModelTiers.Audio, true);
        _documents = Check("Documents", ModelTiers.Docs, _chrome.DocumentFiles);
        var checks = new StackPanel { Spacing = 2 };
        checks.Children.Add(_people);
        checks.Children.Add(_sounds);
        checks.Children.Add(_documents);
        body.Children.Add(checks);

        TextBlock footnote = _look.Text(
            "Models download from Hugging Face. It sees your IP address and which public model files are fetched — " +
            "nothing about your photos or videos.", 12);
        footnote.Margin = new Thickness(0, 8, 0, 0);
        body.Children.Add(footnote);
        if (choice.Current.Length > 0)
        {
            body.Children.Add(_look.Text(
                "Switching re-indexes your folders; the current index keeps answering until the new one is ready.", 12));
        }
        var scroll = new ScrollViewer
        {
            Content = body,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        Grid.SetRow(scroll, 1);
        root.Children.Add(scroll);

        // Download size · disk size · Not now · Download (or Switch)
        var foot = new Grid { ColumnSpacing = 8 };
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _totals = _look.Text("", 12);
        _totals.VerticalAlignment = VerticalAlignment.Center;
        foot.Children.Add(_totals);
        Button later = _look.Button("Not now", Close);
        Grid.SetColumn(later, 1);
        foot.Children.Add(later);
        _go = _look.Button(choice.Current.Length > 0 ? "Switch" : "Download", Download, accent: true);
        Grid.SetColumn(_go, 2);
        foot.Children.Add(_go);
        Grid.SetRow(foot, 2);
        root.Children.Add(foot);

        root.KeyDown += OnKeyDown;
        Content = root;
        Update();
    }

    internal void Present()
    {
        AppWindow.MoveAndResize(Native.CentreOver(_chrome.Host.MainWindow, 640, 720));
        Activate();
        if (_cards.TryGetValue(_selected, out ToggleButton? card)) card.Loaded += (_, _) => card.Focus(FocusState.Programmatic);
    }

    // ---- building blocks -------------------------------------------------------------

    /// <summary>"We checked this PC: Intel Core i7-1260P · 16 GB · NVIDIA GPU".</summary>
    private static string MachineLine(ModelTiers.Machine m)
    {
        var parts = new List<string>();
        if (m.Chip.Length > 0) parts.Add(m.Chip);
        if (m.RamBytes > 0) parts.Add($"{Math.Round(m.RamBytes / (1024.0 * 1024 * 1024)):0} GB");
        parts.Add(m.Accelerator == "nvidia" ? "NVIDIA GPU" : m.Accelerator == "apple" ? "Apple silicon" : "CPU only");
        return "We checked this PC: " + string.Join(" · ", parts);
    }

    private TextBlock Caption(string text, AddonColour role)
    {
        TextBlock t = _look.Text(text, 11, role, wrap: false);
        t.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        t.CharacterSpacing = 40;
        return t;
    }

    /// <summary>
    /// A tier, chosen with a click (a toggle so the keyboard and a screen reader
    /// see a choice): its name, what it is for, its models, sizes, the time for
    /// 10,000 photos here, and its licences in small type.
    /// </summary>
    private ToggleButton Card(ModelTiers.Tier t)
    {
        var lines = new StackPanel { Spacing = 3 };
        var top = new Grid { ColumnSpacing = 12 };
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock name = _look.Text(t.Title, 15, AddonColour.Title, wrap: false);
        name.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        top.Children.Add(name);
        TextBlock size = _look.Text("", 12, AddonColour.Body, wrap: false);
        size.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(size, 1);
        top.Children.Add(size);
        _cardSizes[t.Id] = size;
        lines.Children.Add(top);
        if (t.For.Length > 0) lines.Children.Add(_look.Text(t.For, 12));
        if (t.ModelLine.Length > 0) lines.Children.Add(_look.Text(t.ModelLine, 12, AddonColour.Title));
        if (PerTenThousand(t) is string time) lines.Children.Add(_look.Text(time, 12));
        if (t.Licences.Length > 0) lines.Children.Add(_look.Text("Licences: " + string.Join(" · ", t.Licences), 11));

        var b = new ToggleButton
        {
            Content = lines,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Padding = new Thickness(14, 10, 14, 12),
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(1.5),
        };
        if (!Look.HighContrast())
        {
            SolidColorBrush surface = _look[AddonColour.Surface];
            Look.ToggleBrushes(b, "Background", surface, _look.LiveTint(AddonColour.Accent, 14), _look.LiveTint(AddonColour.Accent, 24),
                               _look.LiveTint(AddonColour.Accent, 22), _look.LiveTint(AddonColour.Accent, 30), _look.LiveTint(AddonColour.Accent, 38));
            Look.ToggleBrushes(b, "BorderBrush", _look[AddonColour.Hairline], _look[AddonColour.Hairline], _look[AddonColour.Hairline],
                               _look[AddonColour.Accent], _look[AddonColour.Accent], _look[AddonColour.Accent]);
        }
        AutomationProperties.SetName(b, t.Name);
        AutomationProperties.SetHelpText(b, t.For);
        string id = t.Id;
        b.Click += (_, _) => Select(id);
        _cards[id] = b;
        return b;
    }

    /// <summary>"About 25 min per 10,000 photos", accelerated where the local check found
    /// an accelerator; nothing when the tier has no measurement.</summary>
    private string? PerTenThousand(ModelTiers.Tier t)
    {
        double? rate = _accelerated ? t.PhotosPerSecondAccelerated : t.PhotosPerSecondCpu;
        if (rate is not > 0) return null;
        double seconds = 10000 / rate.Value;
        string span = seconds < 90 ? "a minute"
            : seconds < 90 * 60 ? $"{Math.Round(seconds / 60):0} min"
            : seconds < 10 * 3600 ? (Math.Round(seconds / 1800) / 2).ToString("0.#", CultureInfo.CurrentCulture) + " h"
            : $"{Math.Round(seconds / 3600):0} h";
        return $"About {span} per 10,000 photos here";
    }

    private CheckBox Check(string label, string piece, bool on)
    {
        bool installed = _chrome.Host.IsPieceInstalled(piece);
        var box = new CheckBox
        {
            Content = label,
            IsChecked = on || installed,
            FontFamily = _look.Font,
            FontSize = _look.FontSize - 1,
        };
        box.Checked += (_, _) => Update();
        box.Unchecked += (_, _) => Update();
        return box;
    }

    // ---- choice ----------------------------------------------------------------------

    private void Select(string id)
    {
        _selected = id;
        Update();
    }

    private ModelTiers.Tier Selected => _choice.Find(_selected) ?? _choice.Recommended;

    /// <summary>Checked: wanted, or installed (an installed piece's models download for the tier too).</summary>
    private static bool Wanted(CheckBox box) => box.IsChecked == true;

    /// <summary>The pieces this tier and the checkboxes take (Core always).</summary>
    private List<string> Pieces(ModelTiers.Tier t)
    {
        var pieces = new List<string> { ModelTiers.Core };
        if (t.NeedsLarge) pieces.Add(ModelTiers.Large);
        if (Wanted(_people)) pieces.Add(ModelTiers.Faces);
        if (Wanted(_sounds) && t.HasAudio) pieces.Add(ModelTiers.Audio);
        if (Wanted(_documents)) pieces.Add(ModelTiers.Docs);
        return pieces;
    }

    private (long Download, long Disk) Sizes(ModelTiers.Tier t)
    {
        long download = 0, disk = 0;
        foreach (string piece in Pieces(t))
        {
            download += t.Download.GetValueOrDefault(piece);
            disk += t.Disk.GetValueOrDefault(piece);
        }
        return (download, disk);
    }

    private static string SizeLine((long Download, long Disk) s) =>
        s.Download <= 0 && s.Disk <= 0 ? ""
        : s.Disk <= 0 ? $"{MediaViewer.Shared.ByteSize.Text(s.Download)} download"
        : $"{MediaViewer.Shared.ByteSize.Text(s.Download)} download · {MediaViewer.Shared.ByteSize.Text(s.Disk)} on disk";

    // The cards' selection and sizes, the checkboxes' sizes, and the button,
    // after any click: only words and states change, so focus stays put.
    private void Update()
    {
        ModelTiers.Tier tier = Selected;
        foreach ((string id, ToggleButton card) in _cards)
        {
            card.IsChecked = id == tier.Id;
            AutomationProperties.SetItemStatus(card, id == tier.Id ? "Selected" : "");
        }
        // Installed pieces stay checked (removing one is Settings' Remove…). The
        // sounds checkbox follows the tier: off for a tier with no sound models,
        // but a tier with speech alone (Whisper) still uses the Audio piece.
        _sounds.IsEnabled = tier.HasAudio && !_chrome.Host.IsPieceInstalled(ModelTiers.Audio);
        _sounds.Content = CheckLabel(tier.Sounds.Length > 0 || !tier.HasAudio ? "Sounds" : "Speech", tier, ModelTiers.Audio,
                                     tier.Sounds.Concat(tier.Speech));
        _people.IsEnabled = !_chrome.Host.IsPieceInstalled(ModelTiers.Faces);
        _people.Content = CheckLabel("People", tier, ModelTiers.Faces, tier.Faces);
        _documents.IsEnabled = !_chrome.Host.IsPieceInstalled(ModelTiers.Docs);
        _documents.Content = CheckLabel("Documents", tier, ModelTiers.Docs, tier.Documents);
        foreach ((string id, TextBlock size) in _cardSizes)
        {
            if (_choice.Find(id) is ModelTiers.Tier t) size.Text = SizeLine(Sizes(t));
        }
        (long download, long disk) = Sizes(tier);
        _totals.Text = download > 0
            ? $"Downloads {MediaViewer.Shared.ByteSize.Text(download)} from Hugging Face" +
              (disk > 0 ? $", uses {MediaViewer.Shared.ByteSize.Text(disk)} on disk" : "")
            : "";
        bool switching = _choice.Current.Length > 0 && tier.Id != _choice.Current;
        _go.Content = _choice.Current.Length == 0 || !switching && NewPieces(tier).Count > 0 ? "Download" : "Switch";
        _go.IsEnabled = _choice.Current.Length == 0 || switching || NewPieces(tier).Count > 0;
    }

    private string CheckLabel(string name, ModelTiers.Tier tier, string piece, IEnumerable<string> models)
    {
        string label = name;
        string list = string.Join(" · ", models);
        if (list.Length > 0) label += $" ({list})";
        long bytes = tier.Download.GetValueOrDefault(piece);
        if (bytes > 0) label += $" · {MediaViewer.Shared.ByteSize.Text(bytes)}";
        if (_chrome.Host.IsPieceInstalled(piece)) label += " · installed";
        return label;
    }

    /// <summary>The pieces to install: those the tier and the checkboxes take that are not installed.</summary>
    private List<string> NewPieces(ModelTiers.Tier tier) =>
        Pieces(tier).Where(p => p != ModelTiers.Core && !_chrome.Host.IsPieceInstalled(p)).ToList();

    private void Download()
    {
        if (_closed) return;
        ModelTiers.Tier tier = Selected;
        List<string> pieces = NewPieces(tier);
        if (tier.Id != _choice.Current)
        {
            // A JSON string: the engine saves it, fetches the tier's models and
            // re-indexes with them while the old index answers. [no-block]
            try { _chrome.Api.SetSetting("tier", JsonSerializer.Serialize(tier.Id)); }
            catch (MediaViewerException) { }
        }
        if (pieces.Contains("ai-faces"))
        {
            // Ticking People is the People opt-in too, not only its piece.
            try { _chrome.Api.SetSetting("faces", "1"); }
            catch (MediaViewerException) { }
        }
        if (pieces.Count > 0)
        {
            try { RequestPieces(_chrome.Host, pieces); }
            catch (TypeLoadException)
            {
                // A host older than IAddonPieces: Settings' Install buttons remain.
            }
        }
        _chrome.TierChosen();
        Close();
    }

    // Its own method so that on a host whose Interop has no IAddonPieces, only
    // this call fails to load (caught above), as AiChrome.ReportPill.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static void RequestPieces(IAddonHost2 host, List<string> pieces)
    {
        if (host is IAddonPieces install) install.InstallPieces(pieces);
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        switch (e.Key)
        {
            case VirtualKey.Escape:
                Close();
                e.Handled = true;
                break;
            // Download is the default button; a focused button or card keeps its own Enter.
            case VirtualKey.Enter when FocusManager.GetFocusedElement(Content.XamlRoot) is not ButtonBase && _go.IsEnabled:
                Download();
                e.Handled = true;
                break;
        }
    }
}
