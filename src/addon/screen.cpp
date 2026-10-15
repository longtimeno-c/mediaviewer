// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include "addon/contrib.h"

namespace mv::addon::screen {
namespace {

struct node_type_info {
  node_type t;
  const char* name;
};

constexpr std::array<node_type_info, 19> kTypes{{
    {node_type::stack, "stack"},
    {node_type::grid, "grid"},
    {node_type::scroll, "scroll"},
    {node_type::split, "split"},
    {node_type::text, "text"},
    {node_type::image, "image"},
    {node_type::thumbnail, "thumbnail"},
    {node_type::thumbnail_grid, "thumbnail_grid"},
    {node_type::list, "list"},
    {node_type::table, "table"},
    {node_type::button, "button"},
    {node_type::toggle, "toggle"},
    {node_type::choice, "choice"},
    {node_type::slider, "slider"},
    {node_type::number, "number"},
    {node_type::text_field, "text_field"},
    {node_type::progress, "progress"},
    {node_type::separator, "separator"},
    {node_type::surface, "surface"},
}};

bool container(node_type t) noexcept {
  return t == node_type::stack || t == node_type::grid || t == node_type::scroll ||
         t == node_type::split;
}

bool needs_label(node_type t) noexcept {
  switch (t) {
    case node_type::image:
    case node_type::thumbnail:
    case node_type::thumbnail_grid:
    case node_type::list:
    case node_type::table:
    case node_type::button:
    case node_type::toggle:
    case node_type::choice:
    case node_type::slider:
    case node_type::number:
    case node_type::text_field:
    case node_type::progress:
    case node_type::surface:
      return true;
    default:
      return false;
  }
}

bool id_text(std::string_view s) noexcept {
  if (s.empty() || s.size() > 64) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == ':';
  });
}

// Text a person reads: valid UTF-8, bounded, no control characters but a
// newline or tab where a paragraph is allowed.
bool plain(std::string_view s, std::size_t max, bool multiline = false) noexcept {
  if (s.size() > max || !json::detail::valid_utf8(s)) return false;
  return std::all_of(s.begin(), s.end(), [multiline](char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u == '\n' || u == '\t') return multiline;
    return u >= 0x20 && u != 0x7F;
  });
}

std::optional<double> num(const json::value& v) noexcept {
  if (v.k != json::kind::number) return std::nullopt;
  const double d = v.is_integer ? static_cast<double>(v.i) : v.d;
  if (!std::isfinite(d)) return std::nullopt;
  return d;
}

bool int_in(const json::value& v, std::int64_t lo, std::int64_t hi) noexcept {
  return v.k == json::kind::number && v.is_integer && v.i >= lo && v.i <= hi;
}

bool num_in(const json::value& v, double lo, double hi) noexcept {
  const auto d = num(v);
  return d && *d >= lo && *d <= hi;
}

bool one_of(const json::value& v, std::initializer_list<std::string_view> allowed) noexcept {
  if (v.k != json::kind::string) return false;
  return std::find(allowed.begin(), allowed.end(), v.s) != allowed.end();
}

bool str_max(const json::value& v, std::size_t max, bool multiline = false) noexcept {
  return v.k == json::kind::string && plain(v.s, max, multiline);
}

// An image the add-on ships in its own package: a safe relative path to a PNG
// or JPEG. The host resolves it inside the add-on's verified folder.
bool package_image(const json::value& v) noexcept {
  if (v.k != json::kind::string || v.s.empty() || v.s.size() > 256) return false;
  const std::string& s = v.s;
  if (s.front() == '/' || s.find('\\') != std::string::npos || s.find(':') != std::string::npos ||
      s.find("..") != std::string::npos) {
    return false;
  }
  auto ends = [&s](std::string_view ext) {
    if (s.size() < ext.size()) return false;
    for (std::size_t i = 0; i < ext.size(); ++i) {
      char c = s[s.size() - ext.size() + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != ext[i]) return false;
    }
    return true;
  };
  return ends(".png") || ends(".jpg") || ends(".jpeg");
}

// An absolute path to a user's file (a thumbnail's source). Never opened by
// the core here: the host's thumbnail cache does that on its own workers.
bool file_path(const json::value& v) noexcept {
  return v.k == json::kind::string && !v.s.empty() && v.s.size() <= 4096 && json::detail::valid_utf8(v.s) &&
         std::none_of(v.s.begin(), v.s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

bool string_array(const json::value& v, std::size_t max_count, std::size_t max_len) noexcept {
  if (v.k != json::kind::array || v.a.size() > max_count) return false;
  return std::all_of(v.a.begin(), v.a.end(), [max_len](const json::value& e) {
    return e.k == json::kind::string && plain(e.s, max_len);
  });
}

// A list of objects each with the given string fields (required / optional),
// ids unique. Counts toward the tree's item budget.
bool rows(const json::value& v, std::initializer_list<std::string_view> required,
          std::initializer_list<std::string_view> optional, std::string_view unique_key,
          std::size_t& items, bool path_rows = false) {
  if (v.k != json::kind::array) return false;
  items += v.a.size();
  if (items > kMaxItems) return false;
  std::set<std::string_view> seen;
  for (const json::value& r : v.a) {
    if (r.k != json::kind::object) return false;
    for (const auto& [key, value] : r.o) {
      const bool req = std::find(required.begin(), required.end(), key) != required.end();
      const bool opt = std::find(optional.begin(), optional.end(), key) != optional.end();
      if (!req && !opt) return false;
      if (path_rows && key == "path") {
        if (!file_path(value)) return false;
      } else if (!str_max(value, key == "id" ? 256 : kMaxLabel)) {
        return false;
      }
    }
    for (std::string_view key : required) {
      if (!r.find(key)) return false;
    }
    if (!unique_key.empty()) {
      const std::string* k = r.str(unique_key);
      if (k && !seen.insert(*k).second) return false;
    }
  }
  return true;
}

error check_props(node& n, std::size_t& items) {
  const json::value& p = n.props;
  for (const auto& [key, v] : p.o) {
    bool ok = false;
    // Every node.
    if (key == "label" || key == "help") {
      ok = str_max(v, kMaxLabel);
    } else if (key == "disabled" || key == "hidden") {
      ok = v.k == json::kind::boolean;
    } else {
      switch (n.type) {
        case node_type::stack:
          ok = (key == "axis" && one_of(v, {"vertical", "horizontal"})) ||
               (key == "spacing" && int_in(v, 0, 64)) ||
               (key == "align" && one_of(v, {"start", "center", "end", "fill"}));
          break;
        case node_type::grid:
          ok = (key == "columns" && int_in(v, 1, 12)) || (key == "spacing" && int_in(v, 0, 64));
          break;
        case node_type::scroll:
          ok = (key == "axis" && one_of(v, {"vertical", "horizontal"})) ||
               (key == "height" && int_in(v, 0, 4096));
          break;
        case node_type::split:
          ok = (key == "axis" && one_of(v, {"vertical", "horizontal"})) ||
               (key == "fraction" && num_in(v, 0.05, 0.95));
          break;
        case node_type::text:
          ok = (key == "text" && str_max(v, kMaxText, true)) ||
               (key == "style" &&
                one_of(v, {"body", "title", "heading", "secondary", "caption", "mono"})) ||
               (key == "copyable" && v.k == json::kind::boolean) || (key == "lines" && int_in(v, 0, 100));
          break;
        case node_type::image:
          ok = (key == "src" && package_image(v)) || (key == "width" && int_in(v, 0, 4096)) ||
               (key == "height" && int_in(v, 0, 4096));
          break;
        case node_type::thumbnail:
          ok = (key == "path" && file_path(v)) || (key == "size" && int_in(v, 16, 512));
          break;
        case node_type::thumbnail_grid:
          ok = (key == "items" && rows(v, {"path"}, {"id", "label"}, "path", items, true)) ||
               (key == "tile" && int_in(v, 48, 512)) ||
               (key == "selection" && string_array(v, kMaxItems, 4096)) ||
               (key == "multi" && v.k == json::kind::boolean);
          break;
        case node_type::list:
          ok = (key == "items" && rows(v, {"id", "text"}, {"detail"}, "id", items)) ||
               (key == "selection" && string_array(v, kMaxItems, 256)) ||
               (key == "multi" && v.k == json::kind::boolean);
          break;
        case node_type::table:
          if (key == "columns") {
            ok = v.k == json::kind::array && !v.a.empty() && v.a.size() <= 32;
            std::size_t ignored = 0;
            ok = ok && rows(v, {"id", "title"}, {}, "id", ignored);
          } else if (key == "rows") {
            ok = v.k == json::kind::array;
            if (ok) {
              items += v.a.size();
              ok = items <= kMaxItems;
              std::set<std::string_view> seen;
              for (const json::value& r : v.a) {
                if (!ok) break;
                const std::string* id = r.str("id");
                const json::value* cells = r.find("cells");
                ok = r.k == json::kind::object && r.o.size() == 2 && id && plain(*id, 256) &&
                     cells && string_array(*cells, 32, kMaxLabel) && seen.insert(*id).second;
              }
            }
          } else {
            ok = (key == "selection" && string_array(v, kMaxItems, 256)) ||
                 (key == "multi" && v.k == json::kind::boolean);
          }
          break;
        case node_type::button:
          ok = (key == "style" && one_of(v, {"plain", "primary", "destructive"})) ||
               (key == "symbol" && v.k == json::kind::string && known_symbol(v.s));
          break;
        case node_type::toggle:
          ok = key == "value" && v.k == json::kind::boolean;
          break;
        case node_type::choice:
          if (key == "options") {
            std::size_t ignored = 0;
            ok = v.k == json::kind::array && !v.a.empty() && v.a.size() <= 256 &&
                 rows(v, {"id", "text"}, {}, "id", ignored);
          } else {
            ok = key == "value" && str_max(v, 256);
          }
          break;
        case node_type::slider:
        case node_type::number:
          ok = (key == "min" || key == "max" || key == "step" || key == "value") &&
               num_in(v, -1e12, 1e12);
          break;
        case node_type::text_field:
          // "pick": "folder" (2026-10-15, Sharepoint): the host adds a
          // Choose... button that shows the system folder dialog and reports
          // the folder as the field's change; typing a path still works.
          ok = (key == "value" && str_max(v, kMaxText, true)) ||
               (key == "placeholder" && str_max(v, kMaxLabel)) ||
               (key == "multiline" && v.k == json::kind::boolean) ||
               (key == "pick" && one_of(v, {"folder"}));
          break;
        case node_type::progress:
          ok = key == "value" && num_in(v, -1.0, 1.0);
          break;
        case node_type::separator:
          ok = false;
          break;
        case node_type::surface:
          if (key == "pin") {
            ok = v.k == json::kind::object && v.o.size() == 2 && v.find("x") && v.find("y") &&
                 num_in(*v.find("x"), 0.0, 1.0) && num_in(*v.find("y"), 0.0, 1.0);
          } else if (key == "interactive") {
            // true, or the bounds of a live zoom relative to the posted pixels.
            ok = v.k == json::kind::boolean;
            if (v.k == json::kind::object) {
              ok = v.o.size() <= 2;
              for (const auto& [bk, bv] : v.o) {
                ok = ok && ((bk == "min_scale" && num_in(bv, 0.01, 1.0)) ||
                            (bk == "max_scale" && num_in(bv, 1.0, 1000.0)));
              }
            }
          } else if (key == "marks") {
            // Names the chrome draws over the pixels, in priority order.
            ok = v.k == json::kind::array && v.a.size() <= 64;
            for (const json::value& m : v.a) {
              if (!ok) break;
              const json::value* x = m.find("x");
              const json::value* y = m.find("y");
              const std::string* text = m.str("text");
              ok = m.k == json::kind::object && m.o.size() == 3 && x && y && text && num_in(*x, 0.0, 1.0) &&
                   num_in(*y, 0.0, 1.0) && plain(*text, 64);
            }
          } else {
            ok = (key == "surface" && v.k == json::kind::string && id_text(v.s)) ||
                 (key == "width" && int_in(v, 0, 4096)) || (key == "height" && int_in(v, 16, 1024)) ||
                 (key == "pin_label" && str_max(v, kMaxLabel));
          }
          break;
      }
    }
    if (!ok) return error::bad_prop;
  }
  // Required values.
  const auto has = [&p](std::string_view k) { return p.find(k) != nullptr; };
  switch (n.type) {
    case node_type::text:
      if (!has("text")) return error::bad_prop;
      break;
    case node_type::image:
      if (!has("src")) return error::bad_prop;
      break;
    case node_type::thumbnail:
      if (!has("path")) return error::bad_prop;
      break;
    case node_type::thumbnail_grid:
    case node_type::list:
      if (!has("items")) return error::bad_prop;
      break;
    case node_type::table:
      if (!has("columns") || !has("rows")) return error::bad_prop;
      break;
    case node_type::choice:
      if (!has("options")) return error::bad_prop;
      break;
    case node_type::slider: {
      if (!has("min") || !has("max")) return error::bad_prop;
      if (*num(*p.find("min")) >= *num(*p.find("max"))) return error::bad_prop;
      break;
    }
    case node_type::number:
      if (has("min") && has("max") && *num(*p.find("min")) > *num(*p.find("max"))) {
        return error::bad_prop;
      }
      break;
    case node_type::surface:
      if (!has("surface") || !has("height")) return error::bad_prop;
      break;
    default:
      break;
  }
  if (needs_label(n.type)) {
    const std::string* label = p.str("label");
    if (!label || label->find_first_not_of(' ') == std::string::npos) return error::missing_label;
  }
  return error::none;
}

error check_tree(node& n, int depth, std::size_t& nodes, std::size_t& items,
                 std::set<std::string>& ids) {
  if (depth > kMaxDepth || ++nodes > kMaxNodes) return error::too_large;
  if (!id_text(n.id)) return error::bad_node;
  if (!ids.insert(n.id).second) return error::duplicate_id;
  if (n.props.k != json::kind::object) return error::bad_prop;
  if (const error e = check_props(n, items); e != error::none) {
    return items > kMaxItems ? error::too_large : e;
  }
  if (!container(n.type) && !n.children.empty()) return error::bad_children;
  if (n.type == node_type::split && n.children.size() != 2) return error::bad_children;
  for (node& c : n.children) {
    if (const error e = check_tree(c, depth + 1, nodes, items, ids); e != error::none) return e;
  }
  return error::none;
}

// JSON → node, structure only (check_tree judges it).
error build(const json::value& v, node& out, int depth) {
  if (depth > kMaxDepth) return error::too_large;
  if (v.k != json::kind::object) return error::bad_node;
  const std::string* type = v.str("type");
  const std::string* id = v.str("id");
  if (!type || !id) return error::bad_node;
  const auto t = type_from(*type);
  if (!t) return error::bad_node;
  out.type = *t;
  out.id = *id;
  out.props = json::value{};
  out.props.k = json::kind::object;
  out.children.clear();
  for (const auto& [key, value] : v.o) {
    if (key == "type" || key == "id") continue;
    if (key == "children") {
      if (value.k != json::kind::array) return error::bad_children;
      if (value.a.size() > kMaxNodes) return error::too_large;
      out.children.reserve(value.a.size());
      for (const json::value& c : value.a) {
        node child;
        if (const error e = build(c, child, depth + 1); e != error::none) return e;
        out.children.push_back(std::move(child));
      }
      continue;
    }
    out.props.o.emplace_back(key, value);
  }
  return error::none;
}

node* find_mut(node& n, std::string_view id) noexcept {
  if (n.id == id) return &n;
  for (node& c : n.children) {
    if (node* hit = find_mut(c, id)) return hit;
  }
  return nullptr;
}

// The parent of `id` and the child's index there.
std::pair<node*, std::size_t> parent_of(node& n, std::string_view id) noexcept {
  for (std::size_t i = 0; i < n.children.size(); ++i) {
    if (n.children[i].id == id) return {&n, i};
    auto hit = parent_of(n.children[i], id);
    if (hit.first) return hit;
  }
  return {nullptr, 0};
}

const node* find_const(const node& n, std::string_view id) noexcept {
  if (n.id == id) return &n;
  for (const node& c : n.children) {
    if (const node* hit = find_const(c, id)) return hit;
  }
  return nullptr;
}

void write_node(json::writer& w, const node& n) {
  w.begin_object();
  w.key("type").string(type_name(n.type));
  w.key("id").string(n.id);
  for (const auto& [key, value] : n.props.o) {
    w.key(key);
    write_value(w, value);
  }
  if (!n.children.empty()) {
    w.key("children").begin_array();
    for (const node& c : n.children) write_node(w, c);
    w.end_array();
  }
  w.end_object();
}

void set_prop(json::value& props, std::string_view key, json::value v) {
  for (auto& [k, existing] : props.o) {
    if (k == key) {
      existing = std::move(v);
      return;
    }
  }
  props.o.emplace_back(std::string(key), std::move(v));
}

void surfaces_of(const node& n, std::vector<std::string>& out) {
  if (n.type == node_type::surface) {
    if (const std::string* s = n.props.str("surface")) out.push_back(*s);
  }
  for (const node& c : n.children) surfaces_of(c, out);
}

bool item_has(const json::value* items, std::string_view key, std::string_view value) {
  if (!items || items->k != json::kind::array) return false;
  return std::any_of(items->a.begin(), items->a.end(), [&](const json::value& r) {
    const std::string* s = r.str(key);
    return s && *s == value;
  });
}

}  // namespace

const char* type_name(node_type t) noexcept {
  for (const node_type_info& i : kTypes) {
    if (i.t == t) return i.name;
  }
  return "stack";
}

std::optional<node_type> type_from(std::string_view name) noexcept {
  for (const node_type_info& i : kTypes) {
    if (name == i.name) return i.t;
  }
  return std::nullopt;
}

bool interactive(node_type t) noexcept {
  switch (t) {
    case node_type::thumbnail_grid:
    case node_type::list:
    case node_type::table:
    case node_type::button:
    case node_type::toggle:
    case node_type::choice:
    case node_type::slider:
    case node_type::number:
    case node_type::text_field:
      return true;
    default:
      return false;
  }
}

const char* error_name(error e) noexcept {
  switch (e) {
    case error::none: return "ok";
    case error::not_json: return "not_json";
    case error::bad_node: return "bad_node";
    case error::bad_prop: return "bad_prop";
    case error::missing_label: return "missing_label";
    case error::duplicate_id: return "duplicate_id";
    case error::bad_children: return "bad_children";
    case error::too_large: return "too_large";
    case error::no_such_node: return "no_such_node";
    case error::bad_op: return "bad_op";
    case error::not_interactive: return "not_interactive";
    case error::bad_value: return "bad_value";
  }
  return "unknown";
}

void write_value(json::writer& w, const json::value& v) {
  switch (v.k) {
    case json::kind::null: w.null(); break;
    case json::kind::boolean: w.boolean(v.b); break;
    case json::kind::number:
      if (v.is_integer) {
        w.integer(v.i);
      } else {
        w.number(v.d);
      }
      break;
    case json::kind::string: w.string(v.s); break;
    case json::kind::array:
      w.begin_array();
      for (const json::value& e : v.a) write_value(w, e);
      w.end_array();
      break;
    case json::kind::object:
      w.begin_object();
      for (const auto& [k, e] : v.o) {
        w.key(k);
        write_value(w, e);
      }
      w.end_object();
      break;
  }
}

error parse_node(const json::value& v, node& out, int depth, std::size_t& nodes, std::size_t& items) {
  if (const error e = build(v, out, depth); e != error::none) return e;
  std::set<std::string> ids;
  return check_tree(out, depth, nodes, items, ids);
}

error tree::set(std::string_view json_text) {
  if (json_text.size() > kMaxJsonBytes) return error::too_large;
  const auto doc = json::parse(json_text, kMaxDepth * 2 + 8);
  if (!doc) return error::not_json;
  node n;
  std::size_t nodes = 0;
  std::size_t items = 0;
  if (const error e = parse_node(*doc, n, 0, nodes, items); e != error::none) return e;
  root_ = std::move(n);
  ++revision_;
  return error::none;
}

error tree::apply(std::string_view diff_json) {
  if (!root_) return error::no_such_node;
  if (diff_json.size() > kMaxJsonBytes) return error::too_large;
  const auto doc = json::parse(diff_json, kMaxDepth * 2 + 8);
  if (!doc) return error::not_json;
  if (doc->k != json::kind::array || doc->a.size() > kMaxNodes) return error::bad_op;
  node work = *root_;
  for (const json::value& op : doc->a) {
    const std::string* kind = op.str("op");
    if (!kind) return error::bad_op;
    if (*kind == "set") {
      const std::string* id = op.str("id");
      const json::value* props = op.find("props");
      if (!id || !props || props->k != json::kind::object || op.o.size() != 3) return error::bad_op;
      node* n = find_mut(work, *id);
      if (!n) return error::no_such_node;
      for (const auto& [key, value] : props->o) {
        if (key == "type" || key == "id" || key == "children") return error::bad_prop;
        if (value.k == json::kind::null) {
          auto& o = n->props.o;
          o.erase(std::remove_if(o.begin(), o.end(), [&key](const auto& m) { return m.first == key; }),
                  o.end());
        } else {
          set_prop(n->props, key, value);
        }
      }
    } else if (*kind == "replace") {
      const std::string* id = op.str("id");
      const json::value* nv = op.find("node");
      if (!id || !nv || op.o.size() != 3) return error::bad_op;
      node fresh;
      if (const error e = build(*nv, fresh, 0); e != error::none) return e;
      if (work.id == *id) {
        work = std::move(fresh);
      } else {
        auto [parent, index] = parent_of(work, *id);
        if (!parent) return error::no_such_node;
        parent->children[index] = std::move(fresh);
      }
    } else if (*kind == "insert") {
      const std::string* pid = op.str("parent");
      const auto index = op.integer("index");
      const json::value* nv = op.find("node");
      if (!pid || !index || !nv || op.o.size() != 4) return error::bad_op;
      node* parent = find_mut(work, *pid);
      if (!parent) return error::no_such_node;
      node fresh;
      if (const error e = build(*nv, fresh, 0); e != error::none) return e;
      if (*index < -1 || *index > static_cast<std::int64_t>(parent->children.size())) {
        return error::bad_op;
      }
      const auto at = *index < 0 ? parent->children.size() : static_cast<std::size_t>(*index);
      parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(at),
                              std::move(fresh));
    } else if (*kind == "remove") {
      const std::string* id = op.str("id");
      if (!id || op.o.size() != 2) return error::bad_op;
      if (work.id == *id) return error::bad_op;
      auto [parent, index] = parent_of(work, *id);
      if (!parent) return error::no_such_node;
      parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
      return error::bad_op;
    }
  }
  std::size_t nodes = 0;
  std::size_t items = 0;
  std::set<std::string> ids;
  if (const error e = check_tree(work, 0, nodes, items, ids); e != error::none) return e;
  root_ = std::move(work);
  ++revision_;
  return error::none;
}

const node* tree::find(std::string_view id) const noexcept {
  return root_ ? find_const(*root_, id) : nullptr;
}

std::string tree::json() const {
  if (!root_) return "null";
  json::writer w;
  write_node(w, *root_);
  return w.take();
}

std::vector<std::string> tree::surface_ids() const {
  std::vector<std::string> out;
  if (root_) surfaces_of(*root_, out);
  return out;
}

error tree::check_event(std::string_view node_id, std::uint32_t kind, std::string_view value) {
  if (!root_) return error::no_such_node;
  node* n = find_mut(*root_, node_id);
  if (!n) return error::no_such_node;
  // A surface takes zoom, pan and double-click only when its tree says so.
  const bool live_surface =
      n->type == node_type::surface && n->props.find("interactive") &&
      (n->props.find("interactive")->k == json::kind::object || n->props.boolean("interactive").value_or(false));
  if (!interactive(n->type) && !live_surface) return error::not_interactive;
  if (const auto d = n->props.boolean("disabled"); d && *d) return error::not_interactive;
  std::optional<json::value> v;
  if (!value.empty()) {
    v = json::parse(value, 4);
    if (!v) return error::bad_value;
  }
  if (live_surface) {
    if (!v || v->k != json::kind::object) return error::bad_value;
    const auto field = [&v](std::string_view key, double lo, double hi) {
      const json::value* f = v->find(key);
      return f && num_in(*f, lo, hi);
    };
    switch (kind) {
      case ev_zoom:
        return v->o.size() == 3 && field("scale", 0.05, 20.0) && field("x", 0.0, 1.0) && field("y", 0.0, 1.0)
                   ? error::none
                   : error::bad_value;
      case ev_pan:
        return v->o.size() == 2 && field("dx", -10.0, 10.0) && field("dy", -10.0, 10.0) ? error::none
                                                                                       : error::bad_value;
      case ev_double:
        return v->o.size() == 2 && field("x", 0.0, 1.0) && field("y", 0.0, 1.0) ? error::none
                                                                               : error::bad_value;
      default:
        return error::bad_value;
    }
  }
  switch (n->type) {
    case node_type::button:
      return kind == ev_click && !v ? error::none : error::bad_value;
    case node_type::toggle:
      if (kind != ev_change || !v || v->k != json::kind::boolean) return error::bad_value;
      set_prop(n->props, "value", *v);
      return error::none;
    case node_type::choice:
      if (kind != ev_change || !v || v->k != json::kind::string ||
          !item_has(n->props.find("options"), "id", v->s)) {
        return error::bad_value;
      }
      set_prop(n->props, "value", *v);
      return error::none;
    case node_type::slider:
    case node_type::number: {
      if (kind != ev_change || !v) return error::bad_value;
      const auto d = num(*v);
      if (!d) return error::bad_value;
      const json::value* lo = n->props.find("min");
      const json::value* hi = n->props.find("max");
      if ((lo && *d < *num(*lo)) || (hi && *d > *num(*hi))) return error::bad_value;
      set_prop(n->props, "value", *v);
      return error::none;
    }
    case node_type::text_field:
      if ((kind != ev_change && kind != ev_submit) || !v || v->k != json::kind::string ||
          !plain(v->s, kMaxText, true)) {
        return error::bad_value;
      }
      set_prop(n->props, "value", *v);
      return error::none;
    case node_type::list:
    case node_type::table:
    case node_type::thumbnail_grid: {
      const char* key = n->type == node_type::thumbnail_grid ? "path" : "id";
      const json::value* items = n->props.find(n->type == node_type::table ? "rows" : "items");
      if (kind == ev_activate) {
        return v && v->k == json::kind::string && item_has(items, key, v->s) ? error::none
                                                                               : error::bad_value;
      }
      if (kind != ev_select || !v || v->k != json::kind::array) return error::bad_value;
      const auto multi = n->props.boolean("multi");
      if (v->a.size() > 1 && !(multi && *multi)) return error::bad_value;
      // One set of the node's keys, not a scan per selected item: Select All in a
      // 100,000-item grid is checked on the UI thread, under the registry lock.
      std::set<std::string_view> known;
      if (items && items->k == json::kind::array) {
        for (const json::value& r : items->a) {
          if (const std::string* s = r.str(key)) known.insert(*s);
        }
      }
      for (const json::value& e : v->a) {
        if (e.k != json::kind::string || known.count(e.s) == 0) return error::bad_value;
      }
      set_prop(n->props, "selection", *v);
      return error::none;
    }
    default:
      return error::not_interactive;
  }
}

}  // namespace mv::addon::screen
