// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 56, contribution API 2 (docs/design/25 "Settings page", "Keymap"): what an
/// add-on declares rather than codes. The Windows twin of the Mac's settings
/// page and key layout row.
///
/// A settings page is a schema the core reads, checks and fills with the stored
/// values; this draws it, in the manifest's order, with the controls this island
/// can load (tools/check-winui-controls.ps1): a ToggleSwitch, a ComboBox, a
/// FakeInput with − / + buttons for a number, a FakeInput for text. Every read
/// and write is the core's and touches disk (it re-verifies the add-on), so it
/// runs on a worker; writes go one after another, in the order they were made.
///
/// A keymap pack is applied once by native (chrome_cmd_apply_keymap): the
/// defaults with the pack's keys written in, then the person's like any remap.
/// </summary>
public static partial class IslandHost
{
    private enum SettingKind { Toggle, Choice, Number, Text }

    private sealed class SettingItem
    {
        public SettingKind Kind;
        public string Key = "", Label = "", Help = "", EnabledBy = "";
        // The last value the core accepted: what a refused change goes back to.
        public bool Bool;
        public double Number;
        public string Str = "";
        public double Min, Max, Step = 1;
        public string Unit = "", Placeholder = "";
        public int MaxLength;
        public List<(string Value, string Label)> Options = new();
        // Why the last change was refused; "" when it was not.
        public string Message = "";
    }

    private sealed record SettingsSectionModel(string Title, string Help, List<SettingItem> Items);

    private sealed record AddonSettingsPage(string Id, string Title, List<SettingsSectionModel> Sections);

    // First-party add-ons' pages by slot id, and where each slot's page is drawn.
    private static Dictionary<string, AddonSettingsPage> _firstPartyPages = new(StringComparer.Ordinal);
    private static readonly List<(AddonSlot[] Slots, StackPanel Host)> FirstPartyHosts = new();

    // One write at a time, in order: two quick clicks of + must not land as -, +.
    private static Task _settingWrites = Task.CompletedTask;

    // ---- reading (worker) ----------------------------------------------------------

    private static double Num(JsonElement e, string name, double fallback) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Number &&
        v.TryGetDouble(out double d) && double.IsFinite(d) ? d : fallback;

    /// <summary>Worker: the page with its values, or null when there is none.</summary>
    private static AddonSettingsPage? ReadSettingsPage(string addonId)
    {
        string? json;
        try { json = AddonNative.SettingsJson(addonId); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return null;
        }
        if (json is null) return null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;
            var sections = new List<SettingsSectionModel>();
            if (root.TryGetProperty("sections", out JsonElement ss) && ss.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement s in ss.EnumerateArray())
                {
                    var items = new List<SettingItem>();
                    if (s.TryGetProperty("items", out JsonElement its) && its.ValueKind == JsonValueKind.Array)
                    {
                        foreach (JsonElement it in its.EnumerateArray())
                        {
                            if (ParseSettingItem(it) is SettingItem item) items.Add(item);
                        }
                    }
                    sections.Add(new SettingsSectionModel(Str(s, "title"), Str(s, "help"), items));
                }
            }
            string title = Str(root, "title");
            return new AddonSettingsPage(addonId, title.Length == 0 ? addonId : title, sections);
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException)
        {
            System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            return null;
        }
    }

    // An item of a type this chrome does not know is left out (a newer schema).
    private static SettingItem? ParseSettingItem(JsonElement it)
    {
        SettingKind? kind = Str(it, "type") switch
        {
            "toggle" => SettingKind.Toggle,
            "choice" => SettingKind.Choice,
            "number" => SettingKind.Number,
            "text" => SettingKind.Text,
            _ => null,
        };
        string key = Str(it, "key");
        if (kind is null || key.Length == 0) return null;
        var item = new SettingItem
        {
            Kind = kind.Value,
            Key = key,
            Label = Str(it, "label"),
            Help = Str(it, "help"),
            EnabledBy = Str(it, "enabled_by"),
        };
        if (item.Label.Length == 0) item.Label = key;
        // "value" is the stored one, folded in by the core; "default" when none is.
        JsonElement value = it.TryGetProperty("value", out JsonElement v) && v.ValueKind != JsonValueKind.Null
            ? v : it.TryGetProperty("default", out JsonElement d) ? d : default;
        switch (item.Kind)
        {
            case SettingKind.Toggle:
                item.Bool = value.ValueKind == JsonValueKind.True;
                break;
            case SettingKind.Number:
                item.Min = Num(it, "min", double.MinValue);
                item.Max = Num(it, "max", double.MaxValue);
                item.Step = Num(it, "step", 1);
                if (item.Step <= 0) item.Step = 1;
                item.Unit = Str(it, "unit");
                item.Number = value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out double n)
                    ? n : Math.Max(item.Min, Math.Min(item.Max, 0));
                break;
            case SettingKind.Choice:
                if (it.TryGetProperty("options", out JsonElement os) && os.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement o in os.EnumerateArray())
                    {
                        string ov = Str(o, "value");
                        string ol = Str(o, "label");
                        item.Options.Add((ov, ol.Length == 0 ? ov : ol));
                    }
                }
                item.Str = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
                break;
            case SettingKind.Text:
                item.MaxLength = (int)Math.Clamp(Num(it, "max_length", 0), 0, int.MaxValue);
                item.Placeholder = Str(it, "placeholder");
                item.Str = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
                break;
        }
        return item;
    }

    // ---- first-party add-ons (Import, the AI family) ---------------------------------

    /// <summary>Where a first-party add-on's page goes in its Settings item;
    /// empty until the page is read.</summary>
    private static StackPanel FirstPartyPageHost(params AddonSlot[] slots)
    {
        var host = new StackPanel { Spacing = Space(8) };
        FirstPartyHosts.RemoveAll(h => h.Slots.SequenceEqual(slots));  // Settings rebuilt
        FirstPartyHosts.Add((slots, host));
        FillFirstPartyHost(slots, host);
        return host;
    }

    private static void FillFirstPartyHost(AddonSlot[] slots, StackPanel host)
    {
        host.Children.Clear();
        // A later window manages no add-on (IslandHost.Addons.cs): its store is the first window's.
        if (_addonsElsewhere) return;
        foreach (AddonSlot slot in slots)
        {
            if (slot.Usable && _firstPartyPages.TryGetValue(slot.Id, out AddonSettingsPage? page))
                host.Children.Add(BuildSettingsPage(page));
        }
        host.Visibility = host.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>Settings opened, or what is installed changed: the pages of the
    /// first-party add-ons whose manifest declares one, read on a worker.</summary>
    private static void RefreshFirstPartyPages()
    {
        if (!_addonStatesRead || _addonsElsewhere) return;
        string[] ids = AddonSlots.Where(s => s.Usable && s.State.Settings).Select(s => s.Id).ToArray();
        if (ids.Length == 0 && _firstPartyPages.Count == 0) return;
        _ = Task.Run(() =>
        {
            var pages = new Dictionary<string, AddonSettingsPage>(StringComparer.Ordinal);
            foreach (string id in ids)
            {
                if (ReadSettingsPage(id) is AddonSettingsPage page) pages[id] = page;
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _firstPartyPages = pages;
                foreach ((AddonSlot[] slots, StackPanel host) in FirstPartyHosts) FillFirstPartyHost(slots, host);
            });
        });
    }

    // ---- drawing a page ---------------------------------------------------------------

    private static FrameworkElement BuildSettingsPage(AddonSettingsPage page)
    {
        var view = new StackPanel { Spacing = Space(8), Margin = Pad(0, 8, 0, 4) };
        TextBlock heading = WrappedLabel(page.Title).WithForeground(Brush(Title));
        heading.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        view.Children.Add(heading);
        // enabled_by: the toggles of this page by key, and what each one enables.
        var toggles = new Dictionary<string, ToggleSwitch>(StringComparer.Ordinal);
        var dependents = new List<(string By, Action<bool> Enable)>();
        foreach (SettingsSectionModel section in page.Sections)
        {
            if (section.Title.Length > 0)
                view.Children.Add(Label(section.Title).WithForeground(Brush(Title)));
            if (section.Help.Length > 0) view.Children.Add(WrappedLabel(section.Help));
            foreach (SettingItem item in section.Items)
            {
                view.Children.Add(BuildSettingRow(page, item, toggles, dependents));
            }
        }
        foreach ((string by, Action<bool> enable) in dependents)
        {
            if (!toggles.TryGetValue(by, out ToggleSwitch? toggle)) continue;
            enable(toggle.IsOn);
            toggle.Toggled += (_, _) => enable(toggle.IsOn);
        }
        AutomationProperties.SetName(view, page.Title);
        return view;
    }

    private static FrameworkElement BuildSettingRow(AddonSettingsPage page, SettingItem item,
        Dictionary<string, ToggleSwitch> toggles, List<(string By, Action<bool> Enable)> dependents)
    {
        var row = new Grid { Padding = Pad(14), ColumnSpacing = Space(24),
            Background = Brush(ChromeColour.Surface), CornerRadius = Corner(8) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var labels = new StackPanel { Spacing = Space(4), VerticalAlignment = VerticalAlignment.Center };
        TextBlock name = WrappedLabel(item.Label).WithForeground(Brush(Title));
        labels.Children.Add(name);
        if (item.Help.Length > 0)
        {
            TextBlock help = WrappedLabel(item.Help);
            help.FontSize = TypeSize(12);
            labels.Children.Add(help);
        }
        row.Children.Add(labels);

        // Why the core refused the last change, under the row's text.
        TextBlock refused = WrappedLabel(item.Message).WithForeground(Brush(Title));
        refused.FontSize = TypeSize(12);
        refused.Margin = Pad(0, 6, 0, 0);
        refused.Visibility = item.Message.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        Grid.SetRow(refused, 1);
        Grid.SetColumnSpan(refused, 2);
        row.Children.Add(refused);
        void Refused(string message)
        {
            item.Message = message;
            refused.Text = message;
            refused.Visibility = message.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        var controls = new List<Control>();
        FrameworkElement control = item.Kind switch
        {
            SettingKind.Toggle => ToggleControl(page, item, Refused, controls, toggles),
            SettingKind.Choice => ChoiceControl(page, item, Refused, controls),
            SettingKind.Number => NumberControl(page, item, Refused, controls),
            _ => TextControl(page, item, Refused, controls),
        };
        control.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetName(control, item.Label);
        if (item.Help.Length > 0) AutomationProperties.SetHelpText(control, item.Help);
        Grid.SetColumn(control, 1);
        row.Children.Add(control);

        if (item.EnabledBy.Length > 0 && item.EnabledBy != item.Key)
        {
            dependents.Add((item.EnabledBy, on =>
            {
                foreach (Control c in controls) c.IsEnabled = on;
                labels.Opacity = on ? 1 : 0.5;
            }));
        }
        return row;
    }

    private static FrameworkElement ToggleControl(AddonSettingsPage page, SettingItem item, Action<string> refused,
        List<Control> controls, Dictionary<string, ToggleSwitch> toggles)
    {
        var toggle = new ToggleSwitch
        {
            IsOn = item.Bool,
            Header = null,
            OnContent = "",
            OffContent = "",
            MinWidth = 0,
            Width = 48,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
        };
        bool reverting = false;
        toggle.Toggled += (_, _) =>
        {
            if (reverting) return;
            bool on = toggle.IsOn;
            WriteSetting(page, item, on ? "true" : "false", refused,
                ok: () => item.Bool = on,
                revert: () =>
                {
                    reverting = true;
                    toggle.IsOn = item.Bool;  // dependents follow through Toggled
                    reverting = false;
                });
        };
        controls.Add(toggle);
        toggles[item.Key] = toggle;
        return toggle;
    }

    private static FrameworkElement ChoiceControl(AddonSettingsPage page, SettingItem item, Action<string> refused,
        List<Control> controls)
    {
        var box = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            MinWidth = 180,
        };
        foreach ((string Value, string Label) option in item.Options) box.Items.Add(option.Label);
        int Index() => item.Options.FindIndex(o => o.Value == item.Str);
        box.SelectedIndex = Index();
        bool reverting = false;
        box.SelectionChanged += (_, _) =>
        {
            int i = box.SelectedIndex;
            if (reverting || i < 0 || i >= item.Options.Count || item.Options[i].Value == item.Str) return;
            string chosen = item.Options[i].Value;
            WriteSetting(page, item, JsonSerializer.Serialize(chosen), refused,
                ok: () => item.Str = chosen,
                revert: () =>
                {
                    reverting = true;
                    box.SelectedIndex = Index();
                    reverting = false;
                });
        };
        controls.Add(box);
        return box;
    }

    // At most three decimals, as the core stores them.
    private static double Round3(double v) => Math.Round(v, 3, MidpointRounding.AwayFromZero);

    private static string NumberText(double v) => Round3(v).ToString("0.###", CultureInfo.CurrentCulture);

    private static string NumberJson(double v) => Round3(v).ToString("0.###", CultureInfo.InvariantCulture);

    private static bool TryReadNumber(string text, out double v) =>
        (double.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.CurrentCulture, out v) ||
         double.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out v)) &&
        double.IsFinite(v);

    private static FrameworkElement NumberControl(AddonSettingsPage page, SettingItem item, Action<string> refused,
        List<Control> controls)
    {
        var line = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        var field = new FakeInput("", 96);
        field.SetText(NumberText(item.Number));
        // What the field shows and the last write asked for; − / + step from it,
        // so two quick clicks make two steps before the first is answered.
        double shown = item.Number;
        string range = item.Min > double.MinValue && item.Max < double.MaxValue
            ? $"Between {NumberText(item.Min)} and {NumberText(item.Max)}{(item.Unit.Length > 0 ? " " + item.Unit : "")}."
            : "That is not a number.";

        void Show(double v)
        {
            shown = v;
            field.SetText(NumberText(v));
        }

        void Commit(double v)
        {
            v = Round3(v);
            if (v == shown)
            {
                Show(v);  // unchanged, or − / + at the end of the range; "15.0" reads "15"
                return;
            }
            Show(v);
            WriteSetting(page, item, NumberJson(v), refused,
                ok: () => item.Number = v,
                revert: () => Show(item.Number));
        }

        void CommitTyped()
        {
            if (!TryReadNumber(field.Text, out double v) || v < item.Min || v > item.Max)
            {
                // Invalid text goes back to the value in use.
                if (field.Text.Trim() != NumberText(shown)) refused(range);
                Show(shown);
                return;
            }
            Commit(v);
        }

        field.Submitted += CommitTyped;
        field.LostFocus += (_, _) => CommitTyped();
        Button less = SettingsButton("−", () => Commit(Math.Max(item.Min, shown - item.Step)));
        Button more = SettingsButton("+", () => Commit(Math.Min(item.Max, shown + item.Step)));
        AutomationProperties.SetName(less, $"Less {item.Label}");
        AutomationProperties.SetName(more, $"More {item.Label}");
        line.Children.Add(less);
        line.Children.Add(field);
        line.Children.Add(more);
        if (item.Unit.Length > 0)
        {
            TextBlock unit = Label(item.Unit);
            unit.VerticalAlignment = VerticalAlignment.Center;
            line.Children.Add(unit);
        }
        controls.Add(less);
        controls.Add(field);
        controls.Add(more);
        return line;
    }

    private static FrameworkElement TextControl(AddonSettingsPage page, SettingItem item, Action<string> refused,
        List<Control> controls)
    {
        var field = new FakeInput(item.Placeholder, 220) { MaxUtf8Bytes = item.MaxLength };
        field.SetText(item.Str);
        void Commit()
        {
            string text = field.Text;
            if (text == item.Str) return;
            WriteSetting(page, item, JsonSerializer.Serialize(text), refused,
                ok: () => item.Str = text,
                revert: () => field.SetText(item.Str));
        }
        field.Submitted += Commit;
        field.LostFocus += (_, _) => Commit();
        controls.Add(field);
        return field;
    }

    // ---- writing --------------------------------------------------------------------

    /// <summary>
    /// Stores one value on a worker, after any write still in flight. Accepted:
    /// <paramref name="ok"/> records it as the value in use. Refused: the core's
    /// reason goes under the row and <paramref name="revert"/> puts the control
    /// back to the last value it accepted. UI thread in and out.
    /// </summary>
    private static void WriteSetting(AddonSettingsPage page, SettingItem item, string valueJson,
        Action<string> refused, Action ok, Action revert)
    {
        string id = page.Id, key = item.Key;
        _settingWrites = _settingWrites.ContinueWith(_ =>
        {
            bool accepted = false;
            string message = "";
            try
            {
                using JsonDocument doc = JsonDocument.Parse(AddonNative.SettingSet(id, key, valueJson));
                accepted = doc.RootElement.TryGetProperty("ok", out JsonElement o) && o.ValueKind == JsonValueKind.True;
                message = Str(doc.RootElement, "message");
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException
                                           or DllNotFoundException or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                if (accepted)
                {
                    ok();
                    refused("");
                }
                else
                {
                    refused(message.Length > 0 ? message : "This change could not be saved.");
                    revert();
                }
            });
        }, CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
    }

    // ---- Settings → Keyboard shortcuts → Key layout --------------------------------------

    private static FrameworkElement? _keymapRow;
    private static ComboBox? _keymapPicker;
    private static TextBlock? _keymapNote;
    private static List<(string Addon, string Pack, string Name)> _keymapChoices = new();

    private static FrameworkElement BuildKeymapRow()
    {
        _keymapPicker = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            MinWidth = 240,
        };
        AutomationProperties.SetName(_keymapPicker, "Key layout");
        Button apply = SettingsButton("Apply", ApplyKeymap);
        AutomationProperties.SetName(apply, "Apply key layout");
        var line = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        TextBlock label = Label("Key layout").WithForeground(Brush(Title));
        label.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(label);
        line.Children.Add(_keymapPicker);
        line.Children.Add(apply);
        _keymapNote = WrappedLabel("Shortcuts from an add-on, in place of the default ones.");
        _keymapNote.FontSize = TypeSize(12);
        var row = new StackPanel { Spacing = Space(4), Margin = Pad(0, 0, 0, 8) };
        row.Children.Add(line);
        row.Children.Add(_keymapNote);
        _keymapRow = row;
        RefreshKeymapRow();
        return row;
    }

    /// <summary>Every keymap of every installed, usable open add-on; the row is
    /// hidden when there is none.</summary>
    private static void RefreshKeymapRow()
    {
        if (_keymapRow is null || _keymapPicker is null) return;
        var choices = new List<(string Addon, string Pack, string Name)>();
        foreach (OpenAddon addon in _openAddons)
        {
            if (addon.State != "ok") continue;
            foreach (OpenKeymap pack in addon.Keymaps) choices.Add((addon.Id, pack.Id, $"{pack.Name} ({addon.Name})"));
        }
        (string Addon, string Pack, string Name)? was =
            _keymapPicker.SelectedIndex >= 0 && _keymapPicker.SelectedIndex < _keymapChoices.Count
                ? _keymapChoices[_keymapPicker.SelectedIndex] : null;
        _keymapChoices = choices;
        _keymapPicker.Items.Clear();
        foreach ((string Addon, string Pack, string Name) choice in choices) _keymapPicker.Items.Add(choice.Name);
        int keep = was is null ? -1 : choices.FindIndex(c => c.Addon == was.Value.Addon && c.Pack == was.Value.Pack);
        _keymapPicker.SelectedIndex = choices.Count == 0 ? -1 : Math.Max(0, keep);
        _keymapRow.Visibility = choices.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static void ApplyKeymap()
    {
        if (_keymapPicker is null) return;
        int i = _keymapPicker.SelectedIndex;
        if (i < 0 || i >= _keymapChoices.Count) return;
        (string addon, string pack, string name) = _keymapChoices[i];
        CancelKeyCapture(restoreFocus: false);
        _ = Task.Run(() =>
        {
            string? json = null;
            try { json = AddonNative.OpenKeymapJson(addon, pack); }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                if (_keymapNote is null) return;
                if (json is null)
                {
                    _keymapNote.Text = $"{name} is no longer installed, or did not pass verification.";
                    RefreshOpenAddons();
                    return;
                }
                // Native takes the parked text into 128 KB, NUL included
                // (chrome_host::take_parked_text; TakeTreePath refuses a longer one).
                if (System.Text.Encoding.UTF8.GetByteCount(json) >= 128 * 1024)
                {
                    _keymapNote.Text = $"{name} is too large to apply.";
                    return;
                }
                // Native applies it to the live table, saves it, and republishes the
                // table: the list below and ? update the way a remap does.
                _treePending = json;
                Send(Command.ApplyKeymap);
                _keymapNote.Text = $"Applied {name}. Reset puts back the default keys.";
            });
        });
    }
}
