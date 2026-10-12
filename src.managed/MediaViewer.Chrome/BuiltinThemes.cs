// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The five themes Settings → Appearance ships with nothing installed
// (docs/design/25 "Themes", issue #359). Host palettes: compiled in, not an
// open add-on. The Mac twin is BuiltinThemes.swift. The rows are one literal
// each and identical in both files; tests/test_open_addons.cpp reads them
// and refuses a palette below the contrast floor.
//
// One palette, no font, so a choice applies at once (a typeface would wait
// for the next start). Each palette also names one ground effect
// (ThemeField.cs). That effect is not a token: it is drawn in the chrome
// only, and the photo canvas never sees it. mediaviewer. is reserved, so
// this selection key cannot be an add-on's.

namespace MediaViewer.Chrome;

internal static class BuiltinThemes
{
    internal readonly record struct Item(
        string Id, string Name, string Json, string Key,
        string Effect, uint EffectRgb, double Intensity);

    internal static bool Moves(string effect) => effect != "dots";

    private static readonly string[] Tokens =
        { "canvas", "surface", "title", "body", "disabled", "hairline", "accent" };

    private static readonly string[] Effects = { "rain", "starfield", "fireflies", "petals", "dots" };

    // id, name, dark|light, seven #rrggbb tokens, then the ground effect, its
    // #rrggbb and an intensity 0.01–1.00. The effect stays in the chrome. The
    // JSON the token cache stores is the seven colours only. Opaque; the hosts
    // append ff. The Mac twin's rows are these exact literals.
    private static readonly string[] Rows =
    {
        "mvbuiltin midnight Midnight dark #0d1117 #161b22 #c9d1d9 #8b949e #6e7681 #30363d #f85149 rain #ffffff 0.50",
        "mvbuiltin starfield Starfield dark #0b1a2c #091422 #64d2ff #6aa3b6 #5e8090 #1e5074 #4facfe starfield #64d2ff 1.00",
        "mvbuiltin terminal Terminal dark #0b1110 #080d0c #b8e6c1 #5d8f6e #3f6a4c #28553a #6bd38b fireflies #b8e6c1 0.80",
        "mvbuiltin ume Ume dark #271d2b #1d1721 #f1d8e9 #b894a8 #7d6472 #66475f #d98bb7 petals #f5a0c0 1.00",
        "mvbuiltin porcelain Porcelain light #edf0f2 #ffffff #252a30 #5c6770 #8b939c #c7cdd2 #3478c9 dots #252a30 1.00",
    };

    private static readonly Item[] Parsed = Parse();

    internal static IReadOnlyList<Item> All => Parsed;

    internal static Item? Find(string key)
    {
        foreach (Item item in Parsed)
        {
            if (item.Key == key) return item;
        }
        return null;
    }

    private static Item[] Parse()
    {
        var items = new Item[Rows.Length];
        for (int i = 0; i < Rows.Length; i++)
        {
            string[] parts = Rows[i].Split(' ');
            if (parts.Length != 14 || parts[0] != "mvbuiltin" || (parts[3] != "dark" && parts[3] != "light"))
                throw new InvalidOperationException(Rows[i]);
            bool dark = parts[3] == "dark";
            string effect = parts[11];
            if (Array.IndexOf(Effects, effect) < 0) throw new InvalidOperationException(effect);
            uint effectRgb = Rgb(parts[12]);
            double intensity = Unit(parts[13]);
            var palette = new System.Text.StringBuilder("{");
            for (int t = 0; t < Tokens.Length; t++)
            {
                string colour = parts[4 + t];
                if (colour.Length != 7 || colour[0] != '#') throw new InvalidOperationException(colour);
                if (t > 0) palette.Append(',');
                palette.Append('"').Append(Tokens[t]).Append("\":\"").Append(colour).Append("ff\"");
            }
            palette.Append('}');
            string body = palette.ToString();
            string id = parts[1];
            string name = parts[2];
            string json = "{\"addon\":\"mediaviewer.builtin\",\"id\":\"" + id + "\",\"name\":\"" + name
                          + "\",\"theme\":{\"font\":\"\",\"dark\":" + (dark ? body : "null")
                          + ",\"light\":" + (dark ? "null" : body) + "}}";
            items[i] = new Item(id, name, json, "mediaviewer.builtin/" + id, effect, effectRgb, intensity);
        }
        return items;
    }

    private static uint Rgb(string text)
    {
        if (text.Length != 7 || text[0] != '#') throw new InvalidOperationException(text);
        return Convert.ToUInt32(text.Substring(1), 16);
    }

    // "0.50" / "1.00". The dot is the separator on every machine.
    private static double Unit(string text)
    {
        string[] parts = text.Split('.');
        if (parts.Length != 2 || parts[1].Length != 2) throw new InvalidOperationException(text);
        if (!int.TryParse(parts[0], out int whole) || !int.TryParse(parts[1], out int frac)
            || whole < 0 || whole > 1 || frac < 0 || frac > 99)
            throw new InvalidOperationException(text);
        double value = whole + frac / 100.0;
        if (value <= 0 || value > 1) throw new InvalidOperationException(text);
        return value;
    }
}
