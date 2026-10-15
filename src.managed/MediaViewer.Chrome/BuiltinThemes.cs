// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The themes Settings → Appearance ships with nothing installed
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
        string Effect, uint EffectRgb, double Intensity, double Speed, double Size);

    internal static bool Moves(string effect) => effect != "dots" && effect != "none";

    // The effect's number for the native empty window (issue #408): its index
    // here, which is shell::home_effect's order (src/shell/home_theme.h).
    internal static int EffectIndex(string effect) => Math.Max(0, Array.IndexOf(Effects, effect));

    private static readonly string[] Tokens =
        { "canvas", "surface", "title", "body", "disabled", "hairline", "accent" };

    private static readonly string[] Effects =
    {
        "none", "dots", "rain", "starfield", "fireflies", "petals",
        "synapse", "embers", "sparkles", "perlin-flow",
    };

    // id, name, dark|light, seven #rrggbb tokens, then the ground effect, its
    // #rrggbb, an intensity, a speed and a size, each 0.01–1.00. The effect
    // stays in the chrome. The JSON the token cache stores is the seven colours
    // only. Opaque; the hosts append ff. The Mac twin's rows are these exact
    // literals. Retrowave's title is lifted from #e94560 so it clears 4.5 : 1.
    private static readonly string[] Rows =
    {
        "mvbuiltin dark Dark dark #282c34 #111111 #9cdef2 #7aa9ba #618491 #355a66 #e06c75 none #9cdef2 1.00 1.00 1.00",
        "mvbuiltin light Light light #f3f5f7 #ffffff #26313a #5c646b #7d858b #cbd3da #b34d5c dots #26313a 1.00 1.00 1.00",
        "mvbuiltin midnight Midnight dark #0d1117 #161b22 #c9d1d9 #8b949e #6e7681 #30363d #f85149 rain #ffffff 0.50 1.00 1.00",
        "mvbuiltin cyberpunk Cyberpunk dark #10121a #171a26 #9bdde0 #6c989d #53747a #514b78 #d45aa8 synapse #62c4c7 0.55 1.00 1.00",
        "mvbuiltin retrowave Retrowave dark #1a1a2e #16213e #ed5e76 #c65169 #9e455b #533483 #e94560 embers #e94560 1.00 1.00 1.00",
        "mvbuiltin forest Forest dark #1b2a1b #142414 #a8d5a2 #81a67d #62805f #3d6b3d #7cb871 petals #a8d5a2 1.00 1.00 1.00",
        "mvbuiltin starfield Starfield dark #0b1a2c #091422 #64d2ff #6aa3b6 #5e8090 #1e5074 #4facfe starfield #64d2ff 1.00 1.00 1.00",
        "mvbuiltin terminal Terminal dark #0b1110 #080d0c #b8e6c1 #5d8f6e #3f6a4c #28553a #6bd38b fireflies #b8e6c1 0.80 1.00 1.00",
        "mvbuiltin organs Organs dark #0a0406 #15080a #efe1c8 #918779 #6f665c #3a1519 #c83240 rain #451616 0.75 0.05 0.75",
        "mvbuiltin ume Ume dark #271d2b #1d1721 #f1d8e9 #b894a8 #7d6472 #66475f #d98bb7 petals #f5a0c0 1.00 1.00 1.00",
        "mvbuiltin gpt GPT dark #212121 #171717 #ececec #969696 #747474 #424242 #949494 none #ececec 1.00 1.00 1.00",
        "mvbuiltin claude Claude dark #1f1e1b #2b2926 #f5f1e8 #a29f99 #7e7b76 #514b43 #d97757 none #f5f1e8 1.00 1.00 1.00",
        "mvbuiltin cute Cute light #fff4f7 #fffafd #63394d #7e5a6b #9b7d8a #edc7d5 #d65f88 sparkles #ff8cb8 1.00 1.00 1.00",
        "mvbuiltin eclipse Eclipse dark #17191c #0f1113 #e8e4dc #91908c #6e6e6c #3c4248 #46c2b3 starfield #46c2b3 1.00 1.00 1.00",
        "mvbuiltin porcelain Porcelain light #edf0f2 #ffffff #252a30 #5c6770 #8b939c #c7cdd2 #3478c9 dots #252a30 1.00 1.00 1.00",
        "mvbuiltin arcade Arcade dark #15131d #0b1720 #f4e85c #978f42 #746d38 #305f68 #ff4f91 synapse #ff4f91 1.00 1.00 1.00",
        "mvbuiltin blueprint Blueprint dark #10263b #091a29 #e8f1f5 #8e9da8 #697988 #48758d #e6b84a dots #e6b84a 1.00 1.00 1.00",
        "mvbuiltin monolith Monolith dark #202020 #121212 #d8d8d2 #969693 #747471 #50504b #b6e05c perlin-flow #b6e05c 1.00 1.00 1.00",
        "mvbuiltin yoyo Yoyo dark #211f23 #141415 #dfdbd7 #999594 #757273 #3e5146 #e09d6c fireflies #b8e6c1 0.80 1.00 1.00",
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
            if (parts.Length != 16 || parts[0] != "mvbuiltin" || (parts[3] != "dark" && parts[3] != "light"))
                throw new InvalidOperationException(Rows[i]);
            bool dark = parts[3] == "dark";
            string effect = parts[11];
            if (Array.IndexOf(Effects, effect) < 0) throw new InvalidOperationException(effect);
            uint effectRgb = Rgb(parts[12]);
            double intensity = Unit(parts[13]);
            double speed = Unit(parts[14]);
            double size = Unit(parts[15]);
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
            items[i] = new Item(id, name, json, "mediaviewer.builtin/" + id,
                effect, effectRgb, intensity, speed, size);
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
