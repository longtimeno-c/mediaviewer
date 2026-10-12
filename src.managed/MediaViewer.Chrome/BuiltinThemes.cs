// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The five themes Settings → Appearance ships with nothing installed
// (docs/design/25 "Themes", issue #359). Host palettes: compiled in, not an
// open add-on. The Mac twin is BuiltinThemes.swift. The rows are one literal
// each and identical in both files; tests/test_open_addons.cpp reads them
// and refuses a palette below the contrast floor.
//
// One palette, no font, so a choice applies at once (a typeface would wait
// for the next start). mediaviewer. is reserved, so this selection key
// cannot be an add-on's.

namespace MediaViewer.Chrome;

internal static class BuiltinThemes
{
    internal readonly record struct Item(string Id, string Name, string Json, string Key);

    private static readonly string[] Tokens =
        { "canvas", "surface", "title", "body", "disabled", "hairline", "accent" };

    // id, name, dark|light, then the seven tokens as #rrggbb. Opaque; the hosts append ff.
    private static readonly string[] Rows =
    {
        "mvbuiltin ember Ember dark #1c1814 #2b241e #f6f1e8 #cbbba8 #7e7468 #433b33 #e39a45",
        "mvbuiltin graphite Graphite dark #1b1c20 #27292f #f3f4f6 #b7bcc6 #6e7480 #3a3d46 #8eb6ea",
        "mvbuiltin harbour Harbour dark #12161c #1c2530 #e7eef6 #a9b8c9 #647484 #2e3b4c #6ec8e6",
        "mvbuiltin linen Linen light #f6f1e7 #fffdf8 #2a2218 #5c5348 #8f867a #e0d5c4 #a34724",
        "mvbuiltin studio Studio light #f2f3f5 #ffffff #1a1c20 #4c525a #8a9098 #d4d7de #1d5cba",
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
            if (parts.Length != 11 || parts[0] != "mvbuiltin" || (parts[3] != "dark" && parts[3] != "light"))
                throw new InvalidOperationException(Rows[i]);
            bool dark = parts[3] == "dark";
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
            items[i] = new Item(id, name, json, "mediaviewer.builtin/" + id);
        }
        return items;
    }
}
