// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #153: the SwiftUI chrome's copies of the command ids agree with the core.
//
// The package's library calls the host through MVChromeBridge, whose functions are
// defined in src/shell/main_mac.mm and only resolve in the app's final link, so a
// test bundle cannot link it. These read the package's sources as text next to
// src/shell/commands.h, command_table.cpp and the describe_commands() snapshot
// mv_tests keeps (tests/data/command_table.snapshot), as the Windows chrome's
// tests do (src.managed/MediaViewer.Chrome.Tests).
//   cd src.swift/MediaViewerChrome && swift test
import Foundation
import XCTest

final class CommandContractTests: XCTestCase {
  // MARK: sources

  private static let repo: URL = {
    var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    while dir.path != "/" {
      if FileManager.default.fileExists(atPath: dir.appendingPathComponent("src/shell/commands.h").path) {
        return dir
      }
      dir = dir.deletingLastPathComponent()
    }
    fatalError("src/shell/commands.h not found above \(#filePath)")
  }()

  private static func read(_ relative: String) throws -> String {
    let text = try String(contentsOf: repo.appendingPathComponent(relative), encoding: .utf8)
    return text.replacingOccurrences(of: "\r\n", with: "\n")
  }

  /// Line and block comments out; string literals kept as they are.
  private static func stripComments(_ text: String) -> String {
    var out = ""
    out.reserveCapacity(text.count)
    let chars = Array(text)
    var i = 0
    while i < chars.count {
      let c = chars[i]
      let next: Character? = i + 1 < chars.count ? chars[i + 1] : nil
      if c == "/" && next == "/" {
        while i < chars.count && chars[i] != "\n" { i += 1 }
        continue
      }
      if c == "/" && next == "*" {
        i += 2
        while i + 1 < chars.count && !(chars[i] == "*" && chars[i + 1] == "/") { i += 1 }
        i += 2
        out.append(" ")
        continue
      }
      if c == "\"" {
        out.append(c)
        i += 1
        while i < chars.count && chars[i] != "\"" && chars[i] != "\n" {
          if chars[i] == "\\" && i + 1 < chars.count {
            out.append(chars[i])
            i += 1
          }
          out.append(chars[i])
          i += 1
        }
        if i < chars.count {
          out.append(chars[i])
          i += 1
        }
        continue
      }
      out.append(c)
      i += 1
    }
    return out
  }

  private static func matches(_ pattern: String, in text: String) -> [[String]] {
    guard let re = try? NSRegularExpression(pattern: pattern) else { return [] }
    let ns = text as NSString
    return re.matches(in: text, range: NSRange(location: 0, length: ns.length)).map { m in
      (0..<m.numberOfRanges).map { idx in
        let r = m.range(at: idx)
        return r.location == NSNotFound ? "" : ns.substring(with: r)
      }
    }
  }

  /// The text between the braces after the first match of `header`.
  private static func block(_ text: String, after header: String) -> String? {
    guard let re = try? NSRegularExpression(pattern: header),
          let m = re.firstMatch(in: text, range: NSRange(location: 0, length: (text as NSString).length)),
          let start = Range(m.range, in: text)
    else { return nil }
    guard let open = text[start.lowerBound...].firstIndex(of: "{") else { return nil }
    var depth = 0
    var i = open
    while i < text.endIndex {
      if text[i] == "{" {
        depth += 1
      } else if text[i] == "}" {
        depth -= 1
        if depth == 0 { return String(text[text.index(after: open)..<i]) }
      }
      i = text.index(after: i)
    }
    return nil
  }

  private static func parseInt(_ s: String) -> Int? {
    let t = s.trimmingCharacters(in: .whitespaces)
    if t.lowercased().hasPrefix("0x") { return Int(t.dropFirst(2), radix: 16) }
    return Int(t)
  }

  /// commands.h `enum class command_id`, implicit values counted as the compiler does.
  private static func commandIds() throws -> [String: Int] {
    let header = stripComments(try read("src/shell/commands.h"))
    let body = try XCTUnwrap(block(header, after: #"enum\s+class\s+command_id\s*:[^{]*"#))
    var out: [String: Int] = [:]
    var next = 0
    for raw in body.split(separator: ",") {
      let entry = raw.trimmingCharacters(in: .whitespacesAndNewlines)
      if entry.isEmpty { continue }
      let parts = entry.split(separator: "=", maxSplits: 1).map {
        $0.trimmingCharacters(in: .whitespacesAndNewlines)
      }
      var value = next
      if parts.count == 2 { value = try XCTUnwrap(parseInt(parts[1]), entry) }
      out[parts[0]] = value
      next = value + 1
    }
    return out
  }

  /// "Zoom in" -> "zoom_in" from command_table.cpp kCommands.
  private static func commandNames() throws -> [String: String] {
    let table = stripComments(try read("src/shell/command_table.cpp"))
    let body = try XCTUnwrap(block(table, after: #"constexpr\s+command_info\s+kCommands\[\]\s*="#))
    var out: [String: String] = [:]
    for m in matches(#"\{\s*([a-z_0-9]+)\s*,\s*"([^"]*)""#, in: body) { out[m[2]] = m[1] }
    return out
  }

  /// zoomIn -> zoom_in, zoom200 -> zoom_200.
  private static func snake(_ name: String) -> String {
    var out = ""
    var previous: Character?
    for c in name {
      if let p = previous, (c.isUppercase && !p.isUppercase) || (c.isNumber && !p.isNumber) { out.append("_") }
      out.append(contentsOf: c.lowercased())
      previous = c
    }
    return out
  }

  private struct Constant {
    let file: String
    let owner: String
    let name: String
    let value: Int
  }

  /// Every `enum XxxCommand { static let name: Int32 = N }` in the package's sources.
  private static func constants() throws -> [Constant] {
    let dir = repo.appendingPathComponent("src.swift/MediaViewerChrome/Sources/MediaViewerChrome")
    let files = try FileManager.default.contentsOfDirectory(atPath: dir.path).filter { $0.hasSuffix(".swift") }.sorted()
    var out: [Constant] = []
    for file in files {
      let text = stripComments(try String(contentsOf: dir.appendingPathComponent(file), encoding: .utf8))
      for e in matches(#"\benum\s+(\w*Command)\s*\{"#, in: text) {
        guard let body = block(text, after: #"\benum\s+"# + e[1] + #"\s*\{"#) else { continue }
        for m in matches(#"static\s+let\s+(\w+)\s*:\s*Int32\s*=\s*(\d+)"#, in: body) {
          out.append(Constant(file: file, owner: e[1], name: m[1], value: Int(m[2]) ?? -1))
        }
      }
    }
    return out
  }

  /// Constants spelled differently from the core's enumerator (as CommandIdTests.cs).
  private static let aliases: [String: String] = [
    "rotateLeft": "rotate_ccw",
    "rotateRight": "rotate_cw",
    "flipH": "flip_horizontal",
    "flipV": "flip_vertical",
    "undo": "undo_edit",
    "reset": "reset_edits",
  ]

  // MARK: tests

  func testCommandConstantsMatchCommandsH() throws {
    let ids = try Self.commandIds()
    XCTAssertEqual(ids["toggle_gallery"], 12)
    XCTAssertEqual(ids["back"], 21)
    let constants = try Self.constants()
    XCTAssertTrue(constants.contains { $0.owner == "ViewCommand" })
    XCTAssertTrue(constants.contains { $0.owner == "EditCommand" })
    XCTAssertTrue(constants.contains { $0.owner == "TrimCommand" })
    for c in constants {
      let snake = Self.snake(c.name)
      var candidates = [snake]
      if c.owner.hasSuffix("Command") && c.owner != "Command" {
        candidates.append(Self.snake(String(c.owner.dropLast("Command".count))) + "_" + snake)
      }
      if let alias = Self.aliases[c.name] { candidates.append(alias) }
      let found = candidates.compactMap { name in ids[name].map { (name, $0) } }
      XCTAssertFalse(found.isEmpty, "\(c.file): \(c.owner).\(c.name) names no command_id (\(candidates))")
      if !found.isEmpty {
        XCTAssertTrue(found.contains { $0.1 == c.value },
                      "\(c.file): \(c.owner).\(c.name) = \(c.value), but \(found.map { "\($0.0) is \($0.1)" })")
      }
    }
  }

  func testShortcutLookupsNameRealCommands() throws {
    // CommandBarView's key("Zoom in") looks the label up by palette name.
    let names = try Self.commandNames()
    let dir = Self.repo.appendingPathComponent("src.swift/MediaViewerChrome/Sources/MediaViewerChrome")
    var seen = 0
    for file in try FileManager.default.contentsOfDirectory(atPath: dir.path) where file.hasSuffix(".swift") {
      let text = try String(contentsOf: dir.appendingPathComponent(file), encoding: .utf8)
      for m in Self.matches(#"(?:\bkey|SettingsStore\.keyLabel)\("([^"]+)"\)"#, in: text) {
        seen += 1
        XCTAssertNotNil(names[m[1]], "\(file): \"\(m[1])\" is no command_info name")
      }
    }
    XCTAssertGreaterThan(seen, 5)
  }

  func testSnapshotBindsTheGEnterWalk() throws {
    // tests/data/command_table.snapshot: "[mode]" headers, then
    // keys \t name \t id \t runnable. G opens the gallery; Enter opens the
    // selected cell there; Esc (`back`) is not listed, it walks out in every mode.
    let ids = try Self.commandIds()
    var mode = ""
    var rows: [(mode: String, keys: String, id: Int)] = []
    for line in try Self.read("tests/data/command_table.snapshot").split(separator: "\n") {
      // Rows always have tabs; a key can be "[" or "#", so only a tab-free line
      // is a comment or a "[mode]" header.
      let isRow = line.contains("\t")
      if line.hasPrefix("#") && !isRow { continue }
      if line.hasPrefix("[") && !isRow {
        mode = line.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        continue
      }
      let f = line.split(separator: "\t", omittingEmptySubsequences: false)
      XCTAssertEqual(f.count, 4, String(line))
      if f.count == 4 { rows.append((mode, String(f[0]), Int(f[2]) ?? -1)) }
    }
    XCTAssertTrue(rows.contains { $0.mode == "browse" && $0.keys == "G" && $0.id == ids["toggle_gallery"] })
    XCTAssertTrue(rows.contains { $0.mode == "gallery" && $0.keys == "Enter" && $0.id == ids["gallery_open_selected"] })
  }
}
